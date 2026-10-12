#include "NothingBackend.h"

#include <algorithm>
#include <span>

#include "Log.h"

namespace {

constexpr uint8_t Sof = 0x55;
constexpr uint16_t ControlCrc = 0x0020;
constexpr uint16_t ControlOutgoing = 0x0160;
constexpr uint16_t ControlErrorMask = 0x001F;
constexpr int HeaderSize = 8;
constexpr int MaxPayload = 512;
constexpr uint16_t ControlPlainMask = 0xF01F;

constexpr uint16_t GetDeviceInfo = 0xC006;
constexpr uint16_t GetBattery = 0xC007;
constexpr uint16_t GetInEar = 0xC00E;
constexpr uint16_t GetNoiseControl = 0xC01E;
constexpr uint16_t GetEqualizer = 0xC01F;
constexpr uint16_t GetLowLatency = 0xC041;
constexpr uint16_t GetBassBoost = 0xC04E;
constexpr uint16_t GetListeningMode = 0xC050;

constexpr uint16_t SetInEar = 0xF004;
constexpr uint16_t SetNoiseControl = 0xF00F;
constexpr uint16_t SetEqualizer = 0xF010;
constexpr uint16_t SetListeningMode = 0xF01D;
constexpr uint16_t SetLowLatency = 0xF040;
constexpr uint16_t SetBassBoost = 0xF051;

constexpr uint16_t AnswerDeviceInfo = 0x4006;
constexpr uint16_t AnswerBattery = 0x4007;
constexpr uint16_t AnswerInEar = 0x400E;
constexpr uint16_t AnswerNoiseControl = 0x401E;
constexpr uint16_t AnswerEqualizer = 0x401F;
constexpr uint16_t AnswerLowLatency = 0x4041;
constexpr uint16_t AnswerBassBoost = 0x404E;
constexpr uint16_t AnswerListeningMode = 0x4050;
constexpr uint16_t EventBattery = 0xE001;
constexpr uint16_t EventNoiseControl = 0xE003;

constexpr uint8_t BatteryLeft = 0x02;
constexpr uint8_t BatteryRight = 0x03;
constexpr uint8_t BatteryCase = 0x04;
constexpr uint8_t BatteryMain = 0x06;
constexpr uint8_t BatteryLevelMask = 0x7F;
constexpr uint8_t BatteryChargingBit = 0x80;

constexpr uint8_t NoiseHigh = 0x01;
constexpr uint8_t NoiseMid = 0x02;
constexpr uint8_t NoiseLow = 0x03;
constexpr uint8_t NoiseAdaptive = 0x04;
constexpr uint8_t NoiseOff = 0x05;
constexpr uint8_t NoiseTransparency = 0x07;

constexpr uint8_t AncHigh = 1 << 0;
constexpr uint8_t AncMid = 1 << 1;
constexpr uint8_t AncLow = 1 << 2;
constexpr uint8_t AncAdaptive = 1 << 3;
constexpr uint8_t AncTransparency = 1 << 4;
constexpr uint8_t AncFull = AncHigh | AncMid | AncLow | AncAdaptive | AncTransparency;

constexpr uint8_t LowLatencyOn = 0x01;
constexpr uint8_t LowLatencyOff = 0x02;

constexpr qint64 DeviceInfoWaitMs = 1500;
constexpr qint64 StatusTimeoutMs = 10000;
constexpr qint64 NoisePollMs = 5000;
constexpr qint64 BatteryPollMs = 30000;

enum class EqFamily { None, Classic, ListeningMode };

struct Preset {
    int id;
    const char *key;
};

constexpr Preset ClassicPresets[] = {
    {0, "balanced"},
    {3, "more-bass"},
    {2, "more-treble"},
    {1, "voice"},
    {5, "custom"},
};

constexpr Preset Ear1Presets[] = {
    {0, "balanced"},
    {3, "more-bass"},
    {2, "more-treble"},
    {1, "voice"},
};

constexpr Preset ListeningPresets[] = {
    {3, "pop"},
    {1, "rock"},
    {5, "classical"},
    {2, "electronic"},
    {4, "enhance-vocals"},
    {6, "custom"},
};

constexpr Preset Buds2APresets[] = {
    {7, "dirac"},
    {3, "pop"},
    {1, "rock"},
    {5, "classical"},
    {2, "electronic"},
    {4, "enhance-vocals"},
    {6, "custom"},
};

const char *const StrengthKeys[] = {"low", "medium", "high"};
constexpr uint8_t StrengthCodes[] = {NoiseLow, NoiseMid, NoiseHigh};
constexpr uint8_t StrengthBits[] = {AncLow, AncMid, AncHigh};

uint8_t at(const QByteArray &p, int i)
{
    return i >= 0 && i < p.size() ? uint8_t(p.at(i)) : 0;
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

QJsonObject control(const QString &key, const QJsonValue &value)
{
    return QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("value"), value}};
}

}

struct NothingModelSpec {
    NothingModel model;
    const char *match;
    bool exact;
    const char *name;
    uint8_t anc;
    EqFamily eq;
    std::span<const Preset> presets;
    bool inEar;
    bool bassBoost;
    bool headphone;
};

namespace {

using Spec = NothingModelSpec;

const Spec Specs[] = {
    {NothingModel::CmfBudsPro2, "CMF Buds Pro 2", false, "CMF Buds Pro 2", AncFull, EqFamily::ListeningMode, ListeningPresets, true, true, false},
    {NothingModel::CmfBudsPro, "CMF Buds Pro", false, "CMF Buds Pro", AncHigh | AncMid | AncLow | AncTransparency, EqFamily::Classic, ClassicPresets, true, false, false},
    {NothingModel::CmfBuds2Plus, "CMF Buds 2 Plus", false, "CMF Buds 2 Plus", AncFull, EqFamily::ListeningMode, ListeningPresets, true, true, false},
    {NothingModel::CmfBuds2A, "CMF Buds 2a", false, "CMF Buds 2a", AncHigh | AncTransparency, EqFamily::ListeningMode, Buds2APresets, false, false, false},
    {NothingModel::CmfBuds2, "CMF Buds 2", false, "CMF Buds 2", AncFull, EqFamily::ListeningMode, ListeningPresets, true, true, false},
    {NothingModel::CmfBuds, "CMF Buds", false, "CMF Buds", AncHigh | AncTransparency, EqFamily::ListeningMode, ListeningPresets, true, true, false},
    {NothingModel::CmfNeckbandPro, "Neckband Pro", false, "CMF Neckband Pro", AncFull, EqFamily::Classic, ClassicPresets, false, true, false},
    {NothingModel::CmfHeadphonePro, "CMF Headphone Pro", false, "CMF Headphone Pro", AncFull, EqFamily::ListeningMode, ListeningPresets, false, false, true},
    {NothingModel::EarStick, "Ear (stick)", false, "Nothing Ear (stick)", AncHigh, EqFamily::Classic, ClassicPresets, true, false, false},
    {NothingModel::EarOpen, "Ear (open)", false, "Nothing Ear (open)", 0, EqFamily::Classic, ClassicPresets, false, false, false},
    {NothingModel::Ear1, "Ear (1)", false, "Nothing Ear (1)", AncHigh | AncLow | AncTransparency, EqFamily::Classic, Ear1Presets, true, false, false},
    {NothingModel::Ear2, "Ear (2)", false, "Nothing Ear (2)", AncFull, EqFamily::Classic, ClassicPresets, true, false, false},
    {NothingModel::EarA, "Ear (a)", false, "Nothing Ear (a)", AncFull, EqFamily::Classic, ClassicPresets, true, true, false},
    {NothingModel::Ear3A, "Ear (3a)", false, "Nothing Ear (3a)", AncFull, EqFamily::Classic, ClassicPresets, true, false, false},
    {NothingModel::Ear3, "Ear (3)", false, "Nothing Ear (3)", AncFull, EqFamily::Classic, ClassicPresets, true, true, false},
    {NothingModel::Headphone1Pro, "Headphone (1) Pro", false, "Nothing Headphone (1) Pro", AncFull, EqFamily::Classic, ClassicPresets, true, false, true},
    {NothingModel::Headphone1, "Headphone (1)", false, "Nothing Headphone (1)", AncFull, EqFamily::Classic, ClassicPresets, true, true, true},
    {NothingModel::HeadphoneA, "Headphone (a)", false, "Nothing Headphone (a)", AncFull, EqFamily::Classic, ClassicPresets, false, true, true},
    {NothingModel::Ear, "Nothing Ear", true, "Nothing Ear", AncFull, EqFamily::Classic, ClassicPresets, true, true, false},
    {NothingModel::Unknown, "", true, "Nothing", AncHigh | AncTransparency, EqFamily::None, {}, true, false, false},
};

}

NothingModel NothingBackend::modelFromName(const QString &name)
{
    const QString trimmed = name.trimmed();
    for (const Spec &s : Specs) {
        if (s.model == NothingModel::Unknown)
            continue;
        const QLatin1String match(s.match);
        if (s.exact ? trimmed.compare(match, Qt::CaseInsensitive) == 0 : trimmed.contains(match, Qt::CaseInsensitive))
            return s.model;
    }
    return NothingModel::Unknown;
}

QString NothingBackend::modelName(NothingModel model)
{
    for (const Spec &s : Specs) {
        if (s.model == model)
            return QString::fromLatin1(s.name);
    }
    return QStringLiteral("Nothing");
}

uint16_t NothingBackend::crc16(const QByteArray &data)
{
    uint16_t crc = 0xFFFF;
    for (const char c : data) {
        crc ^= uint8_t(c);
        for (int i = 0; i < 8; i++)
            crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0xA001) : uint16_t(crc >> 1);
    }
    return crc;
}

QByteArray NothingBackend::encodeFrame(uint16_t command, uint8_t operation, const QByteArray &payload)
{
    QByteArray frame;
    frame.append(char(Sof));
    frame.append(char(ControlOutgoing & 0xFF));
    frame.append(char(ControlOutgoing >> 8));
    frame.append(char(command & 0xFF));
    frame.append(char(command >> 8));
    frame.append(char(payload.size() & 0xFF));
    frame.append(char((payload.size() >> 8) & 0xFF));
    frame.append(char(operation));
    frame.append(payload);
    const uint16_t crc = crc16(frame);
    frame.append(char(crc & 0xFF));
    frame.append(char(crc >> 8));
    return frame;
}

NothingBackend::NothingBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
    , _model(modelFromName(device.name))
{
    _strength = (spec().anc & AncHigh) ? NoiseHigh : (spec().anc & AncMid) ? NoiseMid : NoiseLow;
}

const NothingModelSpec &NothingBackend::spec() const
{
    for (const Spec &s : Specs) {
        if (s.model == _model)
            return s;
    }
    return Specs[std::size(Specs) - 1];
}

QStringList NothingBackend::modes() const
{
    QStringList m;
    if (!_noise)
        return m;
    const uint8_t anc = spec().anc;
    if (anc & (AncHigh | AncMid | AncLow))
        m << ModeNc;
    if (anc & AncAdaptive)
        m << ModeAdaptive;
    if (anc & AncTransparency)
        m << ModeAmbient;
    m << ModeOff;
    return m;
}

QStringList NothingBackend::strengths() const
{
    QStringList s;
    for (size_t i = 0; i < std::size(StrengthBits); i++) {
        if (spec().anc & StrengthBits[i])
            s << QString::fromLatin1(StrengthKeys[i]);
    }
    return s;
}

uint8_t NothingBackend::strengthCode() const
{
    return _strength;
}

bool NothingBackend::start()
{
    _sinceStart.start();
    _sincePoll.start();
    qCInfo(lcHp) << "nothing model:" << modelName(_model) << (_model == NothingModel::Unknown ? "(unrecognized, using a minimal layout)" : "");
    send(GetDeviceInfo);
    return true;
}

void NothingBackend::send(uint16_t command, const QByteArray &payload)
{
    _operation = _operation >= 250 ? 1 : _operation + 1;
    _tx.append(encodeFrame(command, _operation, payload));
    qCDebug(lcHp) << "nothing tx" << Qt::hex << command << payload.toHex(' ');
}

void NothingBackend::sendQueries()
{
    _queriesSent = true;
    const Spec &s = spec();
    send(GetBattery);
    if (s.anc)
        send(GetNoiseControl);
    if (s.eq == EqFamily::Classic)
        send(GetEqualizer);
    else if (s.eq == EqFamily::ListeningMode)
        send(GetListeningMode);
    if (s.inEar)
        send(GetInEar);
    send(GetLowLatency);
    if (s.bassBoost)
        send(GetBassBoost);
}

bool NothingBackend::flush()
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

bool NothingBackend::receive()
{
    char buffer[1024];
    for (;;) {
        int received = 0;
        const MDRResult r = mdrConnectionRecv(_conn, buffer, sizeof(buffer), &received);
        if (r == MDR_RESULT_INPROGRESS)
            break;
        if (r != MDR_RESULT_OK)
            return fail(QString::fromUtf8(mdrConnectionGetLastError(_conn)));
        _rx.append(buffer, received);
    }

    for (;;) {
        const qsizetype start = _rx.indexOf(char(Sof));
        if (start < 0) {
            _rx.clear();
            break;
        }
        _rx.remove(0, start);
        if (_rx.size() < HeaderSize)
            break;
        const uint16_t ctrl = at(_rx, 1) | (at(_rx, 2) << 8);
        const uint16_t command = at(_rx, 3) | (at(_rx, 4) << 8);
        const int length = at(_rx, 5) | (at(_rx, 6) << 8);
        if (length > MaxPayload) {
            _rx.remove(0, 1);
            continue;
        }
        const bool hasCrc = ctrl & ControlCrc;
        if (!hasCrc && (ctrl & ControlPlainMask)) {
            _rx.remove(0, 1);
            continue;
        }
        const qsizetype total = HeaderSize + length + (hasCrc ? 2 : 0);
        if (_rx.size() < total)
            break;
        if (hasCrc) {
            const uint16_t crc = at(_rx, int(total) - 2) | (at(_rx, int(total) - 1) << 8);
            if (crc16(_rx.left(total - 2)) != crc) {
                _rx.remove(0, 1);
                continue;
            }
        }
        const QByteArray payload = _rx.mid(HeaderSize, length);
        _rx.remove(0, total);
        if (ctrl & ControlErrorMask)
            continue;
        qCDebug(lcHp) << "nothing rx" << Qt::hex << command << payload.toHex(' ');
        handle(command, payload);
    }
    return true;
}

bool NothingBackend::tick()
{
    if (!receive())
        return false;
    if (!_queriesSent && _sinceStart.elapsed() > DeviceInfoWaitMs)
        sendQueries();
    if (ready() && _sincePoll.elapsed() > NoisePollMs) {
        _sincePoll.restart();
        if (spec().anc)
            send(GetNoiseControl);
        if (_sinceStart.elapsed() - _lastBatteryPoll > BatteryPollMs) {
            _lastBatteryPoll = _sinceStart.elapsed();
            send(GetBattery);
        }
    }
    if (!flush())
        return false;
    if (!ready() && _sinceStart.elapsed() > StatusTimeoutMs)
        return fail(QStringLiteral("no status from the earbuds"));
    return true;
}

void NothingBackend::handle(uint16_t command, const QByteArray &p)
{
    switch (command) {
    case AnswerDeviceInfo:
        parseDeviceInfo(p);
        if (!_queriesSent)
            sendQueries();
        break;
    case AnswerBattery:
    case EventBattery:
        parseBattery(p);
        break;
    case AnswerNoiseControl:
    case EventNoiseControl:
        if (spec().anc)
            parseNoiseControl(p);
        break;
    case AnswerEqualizer:
        if (spec().eq == EqFamily::Classic && !p.isEmpty())
            _equalizer = at(p, 0);
        break;
    case AnswerListeningMode:
        if (spec().eq == EqFamily::ListeningMode && !p.isEmpty())
            _equalizer = at(p, 0);
        break;
    case AnswerInEar:
        if (spec().inEar && p.size() >= 3)
            _inEar = at(p, 2) == 1;
        break;
    case AnswerLowLatency:
        if (!p.isEmpty())
            _lowLatency = at(p, 0) == LowLatencyOn;
        break;
    case AnswerBassBoost:
        if (spec().bassBoost && p.size() >= 2) {
            _bassBoost = at(p, 0) == 1;
            _bassLevel = at(p, 1);
        }
        break;
    default:
        break;
    }
}

void NothingBackend::parseDeviceInfo(const QByteArray &p)
{
    const QList<QByteArray> lines = p.split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> fields = line.trimmed().split(',');
        if (fields.size() >= 3 && fields.at(1) == "2" && !fields.at(2).isEmpty()) {
            _firmware = QString::fromLatin1(fields.at(2));
            return;
        }
    }
}

void NothingBackend::parseBattery(const QByteArray &p)
{
    if (p.isEmpty())
        return;
    const int count = at(p, 0);
    if (p.size() < 1 + count * 2)
        return;
    Battery b;
    if (_battery && _battery->caseLevel >= 0 && !spec().headphone) {
        b.caseLevel = _battery->caseLevel;
        b.chargingCase = _battery->chargingCase;
    }
    for (int i = 0; i < count; i++) {
        const uint8_t part = at(p, 1 + i * 2);
        const uint8_t value = at(p, 2 + i * 2);
        const int level = value & BatteryLevelMask;
        const bool charging = value & BatteryChargingBit;
        if (level > 100)
            continue;
        switch (part) {
        case BatteryLeft:
            b.left = level;
            b.chargingLeft = charging;
            break;
        case BatteryRight:
            b.right = level;
            b.chargingRight = charging;
            break;
        case BatteryCase:
            b.caseLevel = level;
            b.chargingCase = charging;
            break;
        case BatteryMain:
            b.main = level;
            b.chargingMain = charging;
            break;
        default:
            break;
        }
    }
    _battery = b;
}

void NothingBackend::parseNoiseControl(const QByteArray &p)
{
    std::optional<uint8_t> mode;
    std::optional<uint8_t> level;
    for (int i = 0; i + 1 < p.size(); i += 3) {
        if (at(p, i) == 1)
            mode = at(p, i + 1);
        else if (at(p, i) == 2)
            level = at(p, i + 1);
    }
    if (!mode)
        return;
    switch (*mode) {
    case NoiseHigh:
    case NoiseMid:
    case NoiseLow:
        _strength = *mode;
        break;
    case NoiseAdaptive:
    case NoiseOff:
    case NoiseTransparency:
        if (level && (*level == NoiseHigh || *level == NoiseMid || *level == NoiseLow)) {
            const int index = int(std::find(std::begin(StrengthCodes), std::end(StrengthCodes), *level) - std::begin(StrengthCodes));
            if (spec().anc & StrengthBits[index])
                _strength = *level;
        }
        break;
    default:
        return;
    }
    _noise = *mode;
}

Snapshot NothingBackend::snapshot()
{
    Snapshot s;
    if (!_battery)
        return s;
    const Battery &b = *_battery;
    const Spec &sp = spec();

    const auto add = [&](const char *part, int level, bool charging) {
        if (level < 0)
            return;
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QString::fromLatin1(part)},
            {QStringLiteral("level"), level},
            {QStringLiteral("charging"), charging},
        });
    };
    if (b.main >= 0) {
        add("main", b.main, b.chargingMain);
        s.battery = b.main;
    } else {
        add("left", b.left, b.chargingLeft);
        add("right", b.right, b.chargingRight);
        add("case", b.caseLevel, b.chargingCase);
        if (b.left >= 0 && b.right >= 0)
            s.battery = std::min(b.left, b.right);
        else
            s.battery = std::max(b.left, b.right);
    }

    s.modes = modes();
    if (_noise) {
        switch (*_noise) {
        case NoiseHigh:
        case NoiseMid:
        case NoiseLow:
            s.mode = ModeNc;
            break;
        case NoiseAdaptive:
            s.mode = ModeAdaptive;
            break;
        case NoiseTransparency:
            s.mode = ModeAmbient;
            break;
        default:
            s.mode = ModeOff;
            break;
        }
    }

    QJsonObject o;
    o[QStringLiteral("model")] = modelName(_model);
    o[QStringLiteral("untested")] = true;
    if (!_firmware.isEmpty())
        o[QStringLiteral("firmware")] = _firmware;

    if (_equalizer && !sp.presets.empty()) {
        QJsonArray presets;
        int current = -1;
        for (const Preset &preset : sp.presets) {
            presets.append(QJsonObject{{QStringLiteral("id"), preset.id}, {QStringLiteral("key"), QString::fromLatin1(preset.key)}});
            if (preset.id == *_equalizer)
                current = preset.id;
        }
        o[QStringLiteral("equalizer")] = QJsonObject{
            {QStringLiteral("preset"), current},
            {QStringLiteral("presets"), presets},
        };
    }

    QJsonArray toggles;
    if (_inEar)
        toggles.append(control(sp.headphone ? QStringLiteral("wearDetection") : QStringLiteral("earDetection"), *_inEar));
    if (_lowLatency)
        toggles.append(control(QStringLiteral("lowLatency"), *_lowLatency));
    if (_bassBoost)
        toggles.append(control(QStringLiteral("bassBoost"), *_bassBoost));
    if (!toggles.isEmpty())
        o[QStringLiteral("toggles")] = toggles;

    const QStringList levels = strengths();
    if (_noise && levels.size() > 1) {
        const int index = int(std::find(std::begin(StrengthCodes), std::end(StrengthCodes), _strength) - std::begin(StrengthCodes));
        QJsonObject choice = control(QStringLiteral("ancStrength"), QString::fromLatin1(StrengthKeys[std::clamp(index, 0, 2)]));
        choice[QStringLiteral("options")] = QJsonArray::fromStringList(levels);
        o[QStringLiteral("choices")] = QJsonArray{choice};
    }

    s.settings = o;
    return s;
}

QDBusError NothingBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!modes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("These earbuds do not support mode %1").arg(mode));
    uint8_t code = NoiseOff;
    if (mode == ModeNc)
        code = strengthCode();
    else if (mode == ModeAdaptive)
        code = NoiseAdaptive;
    else if (mode == ModeAmbient)
        code = NoiseTransparency;
    QByteArray payload("\x01", 1);
    payload.append(char(code));
    payload.append(char(0));
    send(SetNoiseControl, payload);
    _noise = code;
    return {};
}

QDBusError NothingBackend::setAmbientLevel(int)
{
    return unsupported();
}

QDBusError NothingBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();
    const Spec &sp = spec();

    const QJsonObject eq = in.value(QStringLiteral("equalizer")).toObject();
    if (const QJsonValue v = eq.value(QStringLiteral("preset")); v.isDouble() && _equalizer) {
        const int id = v.toInt();
        const bool known = std::any_of(sp.presets.begin(), sp.presets.end(), [&](const Preset &p) { return p.id == id; });
        if (!known)
            return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Unknown equalizer preset %1").arg(id));
        QByteArray payload;
        payload.append(char(id));
        payload.append(char(0));
        send(sp.eq == EqFamily::ListeningMode ? SetListeningMode : SetEqualizer, payload);
        _equalizer = id;
    }

    const QJsonObject toggles = in.value(QStringLiteral("toggles")).toObject();
    const QString inEarKey = sp.headphone ? QStringLiteral("wearDetection") : QStringLiteral("earDetection");
    if (const QJsonValue v = toggles.value(inEarKey); v.isBool() && _inEar) {
        QByteArray payload("\x01\x01", 2);
        payload.append(char(v.toBool() ? 1 : 0));
        send(SetInEar, payload);
        _inEar = v.toBool();
    }
    if (const QJsonValue v = toggles.value(QStringLiteral("lowLatency")); v.isBool() && _lowLatency) {
        send(SetLowLatency, QByteArray(1, char(v.toBool() ? LowLatencyOn : LowLatencyOff)));
        _lowLatency = v.toBool();
    }
    if (const QJsonValue v = toggles.value(QStringLiteral("bassBoost")); v.isBool() && _bassBoost) {
        QByteArray payload;
        payload.append(char(v.toBool() ? 1 : 0));
        payload.append(char(_bassLevel));
        send(SetBassBoost, payload);
        _bassBoost = v.toBool();
    }

    const QJsonObject choices = in.value(QStringLiteral("choices")).toObject();
    if (const QJsonValue v = choices.value(QStringLiteral("ancStrength")); v.isString() && _noise) {
        const QString key = v.toString();
        const int index = int(std::find_if(std::begin(StrengthKeys), std::end(StrengthKeys), [&](const char *k) { return key == QLatin1String(k); })
                              - std::begin(StrengthKeys));
        if (index >= int(std::size(StrengthKeys)) || !(sp.anc & StrengthBits[index]))
            return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Unknown noise cancelling strength %1").arg(key));
        _strength = StrengthCodes[index];
        if (*_noise == NoiseHigh || *_noise == NoiseMid || *_noise == NoiseLow) {
            QByteArray payload("\x01", 1);
            payload.append(char(_strength));
            payload.append(char(0));
            send(SetNoiseControl, payload);
            _noise = _strength;
        }
    }
    return {};
}
