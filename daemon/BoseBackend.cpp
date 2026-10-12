#include "BoseBackend.h"

#include <algorithm>

#include "Log.h"

namespace {

constexpr uint8_t OpGet = 1;
constexpr uint8_t OpSetGet = 2;
constexpr uint8_t OpStatus = 3;
constexpr uint8_t OpError = 4;
constexpr uint8_t OpStart = 5;
constexpr uint8_t OpResult = 6;
constexpr uint8_t OpProcessing = 7;

constexpr uint8_t BlockProductInfo = 0;
constexpr uint8_t BlockSettings = 1;
constexpr uint8_t BlockStatus = 2;
constexpr uint8_t BlockNotification = 9;
constexpr uint8_t BlockAudioModes = 31;

constexpr uint8_t FuncBmapVersion = 1;
constexpr uint8_t FuncProductId = 3;

constexpr uint8_t FuncVoicePrompts = 3;
constexpr uint8_t FuncStandbyTimer = 4;
constexpr uint8_t FuncCnc = 5;
constexpr uint8_t FuncAnr = 6;
constexpr uint8_t FuncEqualizer = 7;
constexpr uint8_t FuncMultipoint = 10;
constexpr uint8_t FuncSidetone = 11;
constexpr uint8_t FuncAutoPause = 24;
constexpr uint8_t FuncAutoAnswer = 27;

constexpr uint8_t FuncBattery = 2;

constexpr uint8_t FuncNotifyByBlock = 2;

constexpr uint8_t FuncGetAll = 1;
constexpr uint8_t FuncCurrentMode = 3;
constexpr uint8_t FuncModeConfig = 6;
constexpr uint8_t FuncAudioSettings = 10;

constexpr uint8_t VoicePromptsEnabled = 0x20;
constexpr uint8_t VoicePromptsLanguage = 0x1F;

constexpr uint8_t MultipointEnabled = 0x01;
constexpr uint8_t MultipointSupported = 0x02;
constexpr uint8_t MultipointDisableSupported = 0x04;

constexpr int AnrOff = 0;
constexpr int AnrHigh = 1;
constexpr int AnrWind = 2;
constexpr int AnrLow = 3;

constexpr int PromptQuiet = 0x0001;
constexpr int PromptAware = 0x0002;

constexpr int BatteryMain = 0;
constexpr int BatteryRight = 1;
constexpr int BatteryLeft = 2;
constexpr int BatteryCase = 3;
constexpr int BatteryBuds = 4;

constexpr int CncMax = 10;

constexpr qint64 RequestTimeoutMs = 1500;
constexpr qint64 GetAllTimeoutMs = 4000;
constexpr qint64 SettleTimeoutMs = 5000;
constexpr qint64 StatusTimeoutMs = 10000;
constexpr qint64 RefreshIntervalMs = 15000;

const BoseModel Models[] = {
    {0x400C, "QuietComfort 35", true, false, 0},
    {0x4020, "QuietComfort 35 II", true, false, 0},
    {0x4024, "Noise Cancelling Headphones 700", true, false, 3},
    {0x4039, "QuietComfort 45", true, false, 0},
    {0x4066, "QuietComfort Ultra Headphones", true, false, 0},
    {0x4075, "QuietComfort Headphones", true, false, 0},
    {0x4082, "QuietComfort Ultra Headphones (2nd Gen)", true, false, 0},
    {0x4083, "QuietComfort Headphones (2nd Gen)", true, false, 0},
    {0x402F, "QuietComfort Earbuds", true, true, 1},
    {0x4064, "QuietComfort Earbuds II", true, true, 0},
    {0x4072, "QuietComfort Ultra Earbuds", true, true, 0},
    {0x4062, "QuietComfort Ultra Earbuds (2nd Gen)", true, true, 0},
    {0x4068, "Ultra Open Earbuds", false, true, 0},
    {0x402D, "Sport Earbuds", false, true, 0},
    {0x403A, "Sport Open Earbuds", false, true, 0},
};

struct NamePattern {
    const char *pattern;
    uint16_t productId;
};

constexpr NamePattern NamePatterns[] = {
    {"QCULTRAEARBUDS", 0x4072},
    {"ULTRAOPEN", 0x4068},
    {"SPORTOPEN", 0x403A},
    {"SPORTEARBUDS", 0x402D},
    {"QCEARBUDSII", 0x4064},
    {"QCEARBUDS", 0x402F},
    {"QCULTRA", 0x4066},
    {"NC700", 0x4024},
    {"QC45", 0x4039},
    {"QC35II", 0x4020},
    {"QC35", 0x400C},
};

const char *const AnrNames[] = {"off", "high", "wind", "low"};
const char *const SidetoneNames[] = {"off", "high", "medium", "low"};
const char *const SpatialNames[] = {"off", "still", "motion"};
const char *const EqKeys[] = {"eqBass", "eqMid", "eqTreble"};

uint8_t at(const QByteArray &p, int i)
{
    return i >= 0 && i < p.size() ? uint8_t(p.at(i)) : 0;
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

QDBusError invalid(const QString &what)
{
    return QDBusError(QDBusError::InvalidArgs, what);
}

QString autoOffName(int minutes)
{
    return minutes == 0 ? QStringLiteral("never") : QStringLiteral("%1-min").arg(minutes);
}

template<std::size_t N>
int indexOf(const char *const (&names)[N], const QString &value)
{
    for (std::size_t i = 0; i < N; i++) {
        if (value == QLatin1String(names[i]))
            return int(i);
    }
    return -1;
}

QJsonObject toggle(const QString &key, bool value)
{
    return QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("value"), value}};
}

QJsonObject slider(const QString &key, int value, int min, int max)
{
    return QJsonObject{
        {QStringLiteral("key"), key},
        {QStringLiteral("value"), value},
        {QStringLiteral("min"), min},
        {QStringLiteral("max"), max},
    };
}

QJsonObject choice(const QString &key, const QString &value, const QStringList &options)
{
    return QJsonObject{
        {QStringLiteral("key"), key},
        {QStringLiteral("value"), value},
        {QStringLiteral("options"), QJsonArray::fromStringList(options)},
    };
}

}

const BoseModel *BoseBackend::modelForProductId(uint16_t productId)
{
    for (const auto &m : Models) {
        if (m.productId == productId)
            return &m;
    }
    return nullptr;
}

const BoseModel *BoseBackend::modelForName(const QString &name)
{
    const QString compact = QString(name).remove(QLatin1Char(' ')).remove(QLatin1Char('-')).toUpper();
    for (const auto &p : NamePatterns) {
        if (compact.contains(QLatin1String(p.pattern)))
            return modelForProductId(p.productId);
    }
    return nullptr;
}

QByteArray BoseBackend::packet(uint8_t block, uint8_t function, uint8_t op, const QByteArray &payload)
{
    QByteArray p;
    p.append(char(block));
    p.append(char(function));
    p.append(char(op & 0x0F));
    p.append(char(std::min<qsizetype>(payload.size(), 255)));
    p.append(payload.left(255));
    return p;
}

BoseBackend::BoseBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
    , _model(modelForName(device.name))
{
}

bool BoseBackend::start()
{
    _sinceStart.start();
    _sinceRefresh.start();
    enqueue(BlockProductInfo, FuncBmapVersion, OpGet);
    enqueue(BlockProductInfo, FuncProductId, OpGet);
    enqueue(BlockNotification, FuncNotifyByBlock, OpSetGet, QByteArray("\x01\x37", 2));
    enqueue(BlockStatus, FuncBattery, OpGet);
    for (const uint8_t f : {FuncVoicePrompts, FuncStandbyTimer, FuncCnc, FuncAnr, FuncEqualizer, FuncMultipoint, FuncSidetone,
                            FuncAutoPause, FuncAutoAnswer})
        enqueue(BlockSettings, f, OpGet);
    enqueue(BlockAudioModes, FuncCurrentMode, OpGet);
    return true;
}

bool BoseBackend::ready() const
{
    return !_batteries.isEmpty() && (_settled || _sinceStart.elapsed() > SettleTimeoutMs);
}

void BoseBackend::enqueue(uint8_t block, uint8_t function, uint8_t op, const QByteArray &payload)
{
    _queue.push_back(Request{block, function, op, payload});
}

void BoseBackend::pump()
{
    if (_inFlight) {
        const bool getAll = _inFlight->op == OpStart && _inFlight->function == FuncGetAll;
        if (_inFlightTimer.elapsed() < (getAll ? GetAllTimeoutMs : RequestTimeoutMs))
            return;
        qCDebug(lcHp) << "bose: no reply to" << _inFlight->block << _inFlight->function;
        _inFlight.reset();
    }
    if (_queue.empty()) {
        if (!_settled) {
            _settled = true;
            qCInfo(lcHp) << "bose model:" << modelName() << (_model ? "" : "(unrecognized)");
        }
        return;
    }
    _inFlight = _queue.front();
    _queue.pop_front();
    const QByteArray p = packet(_inFlight->block, _inFlight->function, _inFlight->op, _inFlight->payload);
    _tx.append(p);
    _inFlightTimer.start();
    qCDebug(lcHp) << "bose tx" << p.toHex(' ');
}

bool BoseBackend::flush()
{
    while (!_tx.isEmpty()) {
        int sent = 0;
        const MDRResult r = mdrConnectionSend(_conn, _tx.constData(), int(_tx.size()), &sent);
        if (r == MDR_RESULT_INPROGRESS)
            return true;
        if (r != MDR_RESULT_OK)
            return fail(QString::fromUtf8(mdrConnectionGetLastError(_conn)));
        _tx.remove(0, sent);
    }
    return true;
}

bool BoseBackend::receive()
{
    char buffer[1024];
    for (;;) {
        int received = 0;
        const MDRResult r = mdrConnectionRecv(_conn, buffer, sizeof(buffer), &received);
        if (r == MDR_RESULT_INPROGRESS)
            break;
        if (r != MDR_RESULT_OK)
            return fail(QString::fromUtf8(mdrConnectionGetLastError(_conn)));
        if (received <= 0)
            break;
        _rx.append(buffer, received);
    }

    while (_rx.size() >= 4) {
        const uint8_t op = uint8_t(_rx.at(2)) & 0x0F;
        if (op > OpProcessing) {
            _rx.remove(0, 1);
            continue;
        }
        const int length = uint8_t(_rx.at(3));
        if (_rx.size() < 4 + length)
            break;
        const uint8_t block = uint8_t(_rx.at(0));
        const uint8_t function = uint8_t(_rx.at(1));
        const QByteArray payload = _rx.mid(4, length);
        _rx.remove(0, 4 + length);
        qCDebug(lcHp) << "bose rx" << block << function << op << payload.toHex(' ');
        handle(block, function, op, payload);
    }
    return true;
}

bool BoseBackend::tick()
{
    if (!receive())
        return false;
    pump();
    if (!flush())
        return false;
    if (_batteries.isEmpty() && _sinceStart.elapsed() > StatusTimeoutMs)
        return fail(QStringLiteral("no battery status from the headphones"));
    if (_settled && _queue.empty() && !_inFlight && _sinceRefresh.elapsed() > RefreshIntervalMs)
        refresh();
    return true;
}

void BoseBackend::refresh()
{
    _sinceRefresh.start();
    enqueue(BlockStatus, FuncBattery, OpGet);
    if (_anr)
        enqueue(BlockSettings, FuncAnr, OpGet);
    if (_cncRaw)
        enqueue(BlockSettings, FuncCnc, OpGet);
    if (_currentMode)
        enqueue(BlockAudioModes, FuncCurrentMode, OpGet);
    if (_audioSettings)
        enqueue(BlockAudioModes, FuncAudioSettings, OpGet);
}

void BoseBackend::probeAudioModes()
{
    enqueue(BlockAudioModes, FuncAudioSettings, OpGet);
    enqueue(BlockAudioModes, FuncGetAll, OpStart);
}

void BoseBackend::handle(uint8_t block, uint8_t function, uint8_t op, const QByteArray &payload)
{
    if (_inFlight && _inFlight->block == block && _inFlight->function == function) {
        const bool getAll = _inFlight->op == OpStart && function == FuncGetAll;
        if (op == OpStatus || op == OpError || op == OpResult || (op == OpProcessing && !getAll))
            _inFlight.reset();
    }

    if (op == OpError) {
        qCDebug(lcHp) << "bose error on" << block << function << "code" << at(payload, 0);
        return;
    }
    if (op != OpStatus && op != OpResult)
        return;

    switch (block) {
    case BlockProductInfo:
        if (function == FuncProductId && payload.size() >= 2) {
            const uint16_t pid = uint16_t((at(payload, 0) << 8) | at(payload, 1));
            if (const BoseModel *m = modelForProductId(pid))
                _model = m;
            qCInfo(lcHp) << "bose product id" << Qt::hex << pid;
        }
        break;
    case BlockStatus:
        if (function == FuncBattery)
            parseBattery(payload);
        break;
    case BlockSettings:
        handleSettings(function, payload);
        break;
    case BlockAudioModes:
        handleAudioModes(function, op, payload);
        break;
    default:
        break;
    }
}

void BoseBackend::parseBattery(const QByteArray &p)
{
    if (p.isEmpty())
        return;
    QVector<Battery> batteries;
    if (p.size() >= 4 && p.size() % 4 == 0) {
        for (int i = 0; i + 3 < p.size(); i += 4) {
            const int level = at(p, i);
            if (level <= 100)
                batteries.append(Battery{at(p, i + 3), level});
        }
    } else if (at(p, 0) <= 100) {
        batteries.append(Battery{BatteryMain, at(p, 0)});
    }
    if (!batteries.isEmpty())
        _batteries = batteries;
}

void BoseBackend::handleSettings(uint8_t function, const QByteArray &p)
{
    if (p.isEmpty())
        return;
    switch (function) {
    case FuncVoicePrompts:
        _voicePrompts = at(p, 0);
        break;
    case FuncStandbyTimer:
        _autoOff = p.size() >= 2 ? at(p, 0) | (at(p, 1) << 8) : at(p, 0);
        break;
    case FuncCnc:
        if (p.size() >= 2 && at(p, 0) >= 2) {
            _cncSteps = at(p, 0);
            _cncRaw = std::min<int>(at(p, 1), _cncSteps - 1);
            if (*_cncRaw < _cncSteps - 1)
                _lastCncRaw = *_cncRaw;
        }
        break;
    case FuncAnr:
        if (at(p, 0) <= AnrLow) {
            _anr = at(p, 0);
            if (*_anr != AnrOff)
                _lastAnr = *_anr;
        }
        break;
    case FuncEqualizer:
        for (int i = 0; i + 3 < p.size(); i += 4) {
            const int band = at(p, i + 3);
            if (band < 0 || band >= _eq.size())
                continue;
            _eq[band] = EqBand{int8_t(at(p, i)), int8_t(at(p, i + 1)), int8_t(at(p, i + 2))};
        }
        break;
    case FuncMultipoint:
        _multipoint = at(p, 0);
        break;
    case FuncSidetone:
        if (p.size() >= 2 && at(p, 1) <= 3)
            _sidetone = at(p, 1);
        break;
    case FuncAutoPause:
        _autoPause = at(p, 0) != 0;
        break;
    case FuncAutoAnswer:
        _autoAnswer = at(p, 0) != 0;
        break;
    default:
        break;
    }
}

void BoseBackend::handleAudioModes(uint8_t function, uint8_t op, const QByteArray &p)
{
    switch (function) {
    case FuncCurrentMode:
        if (p.isEmpty())
            return;
        if (!_currentMode && op == OpStatus) {
            _currentMode = at(p, 0);
            probeAudioModes();
        }
        _currentMode = at(p, 0);
        break;
    case FuncModeConfig: {
        if (p.size() < 38)
            return;
        const bool editable = at(p, 3);
        const bool configured = at(p, 4);
        const QString name = QString::fromUtf8(p.mid(6, 32).split('\0').constFirst()).trimmed();
        const int index = at(p, 0);
        const auto existing = std::find_if(_audioModes.begin(), _audioModes.end(), [&](const AudioMode &m) {
            return m.index == index;
        });
        if ((editable && !configured) || name.isEmpty()) {
            if (existing != _audioModes.end())
                _audioModes.erase(existing);
            return;
        }
        const AudioMode mode{index, (at(p, 1) << 8) | at(p, 2), name};
        if (existing != _audioModes.end())
            *existing = mode;
        else
            _audioModes.append(mode);
        std::sort(_audioModes.begin(), _audioModes.end(), [](const AudioMode &a, const AudioMode &b) {
            return a.index < b.index;
        });
        break;
    }
    case FuncAudioSettings:
        if (p.size() >= 5) {
            AudioSettings s;
            s.cnc = std::min<int>(at(p, 0), CncMax);
            s.autoCnc = at(p, 1);
            s.spatial = std::min<int>(at(p, 2), 2);
            s.wind = at(p, 3) != 0;
            s.anc = at(p, 4) != 0;
            _audioSettings = s;
        }
        break;
    default:
        break;
    }
}

BoseNoise BoseBackend::noise() const
{
    if (_model && !_model->anc)
        return BoseNoise::None;
    if (_anr)
        return BoseNoise::Anr;
    if (_currentMode)
        return BoseNoise::AudioModes;
    if (_cncRaw && cncWritable())
        return BoseNoise::Cnc;
    return BoseNoise::None;
}

bool BoseBackend::earbuds() const
{
    if (_model)
        return _model->earbuds;
    return std::any_of(_batteries.begin(), _batteries.end(), [](const Battery &b) {
        return b.id == BatteryLeft || b.id == BatteryRight;
    });
}

QString BoseBackend::modelName() const
{
    if (_model)
        return QStringLiteral("Bose ") + QLatin1String(_model->name);
    return _device.name.isEmpty() ? QStringLiteral("Bose") : _device.name;
}

bool BoseBackend::cncWritable() const
{
    return _cncRaw && (!_model || _model->cncWrites > 0);
}

int BoseBackend::quietIndex() const
{
    for (const AudioMode &m : _audioModes) {
        if (m.prompt == PromptQuiet)
            return m.index;
    }
    return 0;
}

int BoseBackend::awareIndex() const
{
    for (const AudioMode &m : _audioModes) {
        if (m.prompt == PromptAware)
            return m.index;
    }
    return 1;
}

QStringList BoseBackend::modes() const
{
    switch (noise()) {
    case BoseNoise::Anr:
        return {ModeNc, ModeOff};
    case BoseNoise::Cnc:
        return {ModeNc, ModeAmbient};
    case BoseNoise::AudioModes:
        if (_audioSettings)
            return {ModeNc, ModeAmbient, ModeOff};
        return {ModeNc, ModeAmbient};
    case BoseNoise::None:
        break;
    }
    return {};
}

QString BoseBackend::currentMode() const
{
    switch (noise()) {
    case BoseNoise::Anr:
        return *_anr == AnrOff ? ModeOff : ModeNc;
    case BoseNoise::Cnc:
        return *_cncRaw >= _cncSteps - 1 ? ModeAmbient : ModeNc;
    case BoseNoise::AudioModes:
        if (*_currentMode == awareIndex())
            return ModeAmbient;
        if (_audioSettings && !_audioSettings->anc)
            return ModeOff;
        if (*_currentMode != quietIndex() && _audioSettings && _audioSettings->cnc >= CncMax)
            return ModeAmbient;
        return ModeNc;
    case BoseNoise::None:
        break;
    }
    return {};
}

QList<int> BoseBackend::autoOffOptions() const
{
    QList<int> options = _model && _model->productId == 0x4024 ? QList<int>{0, 5, 10, 20, 40, 60, 180} : QList<int>{0, 5, 20, 40, 60, 180};
    if (_autoOff && !options.contains(*_autoOff)) {
        options.append(*_autoOff);
        std::sort(options.begin(), options.end());
    }
    return options;
}

Snapshot BoseBackend::snapshot()
{
    Snapshot s;
    if (_batteries.isEmpty())
        return s;

    const auto find = [&](int id) -> int {
        for (const Battery &b : _batteries) {
            if (b.id == id)
                return b.level;
        }
        return -1;
    };
    const auto add = [&](const char *part, int level) {
        if (level < 0)
            return;
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QString::fromLatin1(part)},
            {QStringLiteral("level"), level},
            {QStringLiteral("charging"), false},
        });
    };
    const int left = find(BatteryLeft);
    const int right = find(BatteryRight);
    if (left >= 0 || right >= 0) {
        add("left", left);
        add("right", right);
        add("case", find(BatteryCase));
        const int buds = find(BatteryBuds);
        s.battery = buds >= 0 ? buds : (left < 0 ? right : right < 0 ? left : std::min(left, right));
    } else {
        const int main = find(BatteryMain) >= 0 ? find(BatteryMain) : _batteries.constFirst().level;
        add("main", main);
        s.battery = main;
    }

    s.modes = modes();
    s.mode = currentMode();

    QJsonObject o;
    o[QStringLiteral("model")] = modelName();
    o[QStringLiteral("untested")] = true;

    QJsonArray toggles;
    if (_voicePrompts)
        toggles.append(toggle(QStringLiteral("voicePrompts"), *_voicePrompts & VoicePromptsEnabled));
    if (_multipoint && (*_multipoint & MultipointSupported) && (*_multipoint & MultipointDisableSupported))
        toggles.append(toggle(QStringLiteral("multipoint"), *_multipoint & MultipointEnabled));
    if (_autoPause)
        toggles.append(toggle(earbuds() ? QStringLiteral("earDetection") : QStringLiteral("wearDetection"), *_autoPause));
    if (_autoAnswer)
        toggles.append(toggle(QStringLiteral("autoAnswer"), *_autoAnswer));
    if (_audioSettings && noise() == BoseNoise::AudioModes)
        toggles.append(toggle(QStringLiteral("windBlock"), _audioSettings->wind));
    if (!toggles.isEmpty())
        o[QStringLiteral("toggles")] = toggles;

    QJsonArray sliders;
    if (_audioSettings && noise() == BoseNoise::AudioModes)
        sliders.append(slider(QStringLiteral("ancStrength"), CncMax - _audioSettings->cnc, 0, CncMax));
    else if (cncWritable() && noise() != BoseNoise::None)
        sliders.append(slider(QStringLiteral("ancStrength"), _cncSteps - 1 - *_cncRaw, 0, _cncSteps - 1));
    for (int i = 0; i < _eq.size(); i++) {
        if (_eq[i])
            sliders.append(slider(QLatin1String(EqKeys[i]), _eq[i]->value, _eq[i]->min, _eq[i]->max));
    }
    if (!sliders.isEmpty())
        o[QStringLiteral("sliders")] = sliders;

    QJsonArray choices;
    if (noise() == BoseNoise::Anr)
        choices.append(choice(QStringLiteral("ancLevel"), QLatin1String(AnrNames[_lastAnr]),
                              {QLatin1String(AnrNames[AnrHigh]), QLatin1String(AnrNames[AnrLow]), QLatin1String(AnrNames[AnrWind])}));
    if (_audioSettings && noise() == BoseNoise::AudioModes)
        choices.append(choice(QStringLiteral("spatialAudio"), QLatin1String(SpatialNames[_audioSettings->spatial]),
                              {QLatin1String(SpatialNames[0]), QLatin1String(SpatialNames[1]), QLatin1String(SpatialNames[2])}));
    if (_currentMode && _audioModes.size() > 2) {
        QStringList names;
        QString current;
        for (const AudioMode &m : _audioModes) {
            names.append(m.name);
            if (m.index == *_currentMode)
                current = m.name;
        }
        choices.append(choice(QStringLiteral("audioMode"), current, names));
    }
    if (_sidetone)
        choices.append(choice(QStringLiteral("sidetone"), QLatin1String(SidetoneNames[*_sidetone]),
                              {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")}));
    if (_autoOff) {
        QStringList options;
        for (const int m : autoOffOptions())
            options.append(autoOffName(m));
        choices.append(choice(QStringLiteral("autoPowerOff"), autoOffName(*_autoOff), options));
    }
    if (!choices.isEmpty())
        o[QStringLiteral("choices")] = choices;

    s.settings = o;
    return s;
}

void BoseBackend::writeCnc(int raw)
{
    raw = std::clamp(raw, 0, _cncSteps - 1);
    QByteArray payload;
    payload.append(char(raw));
    payload.append(char(1));
    const int repeats = _model ? std::max(_model->cncWrites, 1) : 1;
    for (int i = 0; i < repeats; i++)
        enqueue(BlockSettings, FuncCnc, OpSetGet, payload);
    _cncRaw = raw;
    if (raw < _cncSteps - 1)
        _lastCncRaw = raw;
}

void BoseBackend::writeAudioSettings(const AudioSettings &st)
{
    QByteArray payload;
    payload.append(char(st.cnc));
    payload.append(char(0));
    payload.append(char(st.spatial));
    payload.append(char(st.wind));
    payload.append(char(st.anc));
    enqueue(BlockAudioModes, FuncAudioSettings, OpSetGet, payload);
    _audioSettings = st;
    _audioSettings->autoCnc = 0;
}

void BoseBackend::startMode(int index)
{
    QByteArray payload;
    payload.append(char(index));
    payload.append(char(0));
    enqueue(BlockAudioModes, FuncCurrentMode, OpStart, payload);
    _currentMode = index;
    if (_audioSettings)
        enqueue(BlockAudioModes, FuncAudioSettings, OpGet);
}

QDBusError BoseBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!modes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("These headphones do not support mode %1").arg(mode));

    switch (noise()) {
    case BoseNoise::Anr: {
        const int value = mode == ModeOff ? AnrOff : _lastAnr;
        enqueue(BlockSettings, FuncAnr, OpSetGet, QByteArray(1, char(value)));
        _anr = value;
        break;
    }
    case BoseNoise::Cnc:
        writeCnc(mode == ModeAmbient ? _cncSteps - 1 : _lastCncRaw);
        break;
    case BoseNoise::AudioModes:
        if (mode == ModeOff) {
            AudioSettings st = *_audioSettings;
            st.anc = false;
            writeAudioSettings(st);
        } else {
            startMode(mode == ModeAmbient ? awareIndex() : quietIndex());
        }
        break;
    case BoseNoise::None:
        return unsupported();
    }
    return {};
}

QDBusError BoseBackend::setAmbientLevel(int)
{
    return unsupported();
}

QDBusError BoseBackend::applyToggle(const QString &key, bool value)
{
    if (key == QLatin1String("voicePrompts") && _voicePrompts) {
        const uint8_t byte = (value ? VoicePromptsEnabled : 0) | (*_voicePrompts & VoicePromptsLanguage);
        enqueue(BlockSettings, FuncVoicePrompts, OpSetGet, QByteArray(1, char(byte)));
        _voicePrompts = uint8_t((*_voicePrompts & ~VoicePromptsEnabled) | (value ? VoicePromptsEnabled : 0));
        return {};
    }
    if (key == QLatin1String("multipoint") && _multipoint && (*_multipoint & MultipointSupported)
        && (*_multipoint & MultipointDisableSupported)) {
        enqueue(BlockSettings, FuncMultipoint, OpSetGet, QByteArray(1, char(value)));
        _multipoint = uint8_t((*_multipoint & ~MultipointEnabled) | (value ? MultipointEnabled : 0));
        return {};
    }
    if ((key == QLatin1String("earDetection") || key == QLatin1String("wearDetection")) && _autoPause) {
        enqueue(BlockSettings, FuncAutoPause, OpSetGet, QByteArray(1, char(value)));
        _autoPause = value;
        return {};
    }
    if (key == QLatin1String("autoAnswer") && _autoAnswer) {
        enqueue(BlockSettings, FuncAutoAnswer, OpSetGet, QByteArray(1, char(value)));
        _autoAnswer = value;
        return {};
    }
    if (key == QLatin1String("windBlock") && _audioSettings) {
        AudioSettings st = *_audioSettings;
        st.wind = value;
        writeAudioSettings(st);
        return {};
    }
    return invalid(QStringLiteral("Unknown toggle %1").arg(key));
}

QDBusError BoseBackend::applySlider(const QString &key, int value)
{
    if (key == QLatin1String("ancStrength")) {
        if (_audioSettings && noise() == BoseNoise::AudioModes) {
            AudioSettings st = *_audioSettings;
            st.cnc = CncMax - std::clamp(value, 0, CncMax);
            st.anc = true;
            writeAudioSettings(st);
            return {};
        }
        if (cncWritable()) {
            writeCnc(_cncSteps - 1 - std::clamp(value, 0, _cncSteps - 1));
            return {};
        }
        return unsupported();
    }
    for (int i = 0; i < _eq.size(); i++) {
        if (key != QLatin1String(EqKeys[i]))
            continue;
        if (!_eq[i])
            return unsupported();
        const int v = std::clamp(value, _eq[i]->min, _eq[i]->max);
        QByteArray payload;
        payload.append(char(int8_t(v)));
        payload.append(char(i));
        enqueue(BlockSettings, FuncEqualizer, OpSetGet, payload);
        _eq[i]->value = v;
        return {};
    }
    return invalid(QStringLiteral("Unknown slider %1").arg(key));
}

QDBusError BoseBackend::applyChoice(const QString &key, const QString &value)
{
    if (key == QLatin1String("autoPowerOff") && _autoOff) {
        int minutes = -1;
        for (const int m : autoOffOptions()) {
            if (autoOffName(m) == value)
                minutes = m;
        }
        if (minutes < 0)
            return invalid(QStringLiteral("Unknown auto power off value %1").arg(value));
        QByteArray payload;
        payload.append(char(minutes & 0xFF));
        if (minutes > 0xFF)
            payload.append(char(minutes >> 8));
        enqueue(BlockSettings, FuncStandbyTimer, OpSetGet, payload);
        _autoOff = minutes;
        return {};
    }
    if (key == QLatin1String("ancLevel") && noise() == BoseNoise::Anr) {
        const int level = indexOf(AnrNames, value);
        if (level <= AnrOff)
            return invalid(QStringLiteral("Unknown noise cancelling level %1").arg(value));
        enqueue(BlockSettings, FuncAnr, OpSetGet, QByteArray(1, char(level)));
        _anr = level;
        _lastAnr = level;
        return {};
    }
    if (key == QLatin1String("spatialAudio") && _audioSettings) {
        const int spatial = indexOf(SpatialNames, value);
        if (spatial < 0)
            return invalid(QStringLiteral("Unknown spatial audio mode %1").arg(value));
        AudioSettings st = *_audioSettings;
        st.spatial = spatial;
        writeAudioSettings(st);
        return {};
    }
    if (key == QLatin1String("audioMode") && _currentMode) {
        for (const AudioMode &m : _audioModes) {
            if (m.name == value) {
                startMode(m.index);
                return {};
            }
        }
        return invalid(QStringLiteral("Unknown audio mode %1").arg(value));
    }
    if (key == QLatin1String("sidetone") && _sidetone) {
        const int level = indexOf(SidetoneNames, value);
        if (level < 0)
            return invalid(QStringLiteral("Unknown sidetone level %1").arg(value));
        QByteArray payload;
        payload.append(char(1));
        payload.append(char(level));
        enqueue(BlockSettings, FuncSidetone, OpSetGet, payload);
        _sidetone = level;
        return {};
    }
    return invalid(QStringLiteral("Unknown choice %1").arg(key));
}

QDBusError BoseBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();
    QDBusError result;
    const auto keep = [&](const QDBusError &e) {
        if (e.isValid() && !result.isValid())
            result = e;
    };

    const QJsonObject toggles = in.value(QStringLiteral("toggles")).toObject();
    for (auto it = toggles.begin(); it != toggles.end(); ++it) {
        if (it.value().isBool())
            keep(applyToggle(it.key(), it.value().toBool()));
    }
    const QJsonObject sliders = in.value(QStringLiteral("sliders")).toObject();
    for (auto it = sliders.begin(); it != sliders.end(); ++it) {
        if (it.value().isDouble())
            keep(applySlider(it.key(), it.value().toInt()));
    }
    const QJsonObject choices = in.value(QStringLiteral("choices")).toObject();
    for (auto it = choices.begin(); it != choices.end(); ++it) {
        if (it.value().isString())
            keep(applyChoice(it.key(), it.value().toString()));
    }
    if (in.contains(QStringLiteral("equalizer")))
        keep(unsupported());
    return result;
}
