#include "HuaweiBackend.h"

#include <algorithm>

#include "Log.h"

namespace {

constexpr uint8_t Magic = 0x5A;
constexpr uint8_t FlagSingle = 0;
constexpr uint8_t FlagFirst = 1;
constexpr uint8_t FlagLast = 3;
constexpr int MinFrame = 6;
constexpr int MaxFrame = 4096;

constexpr uint16_t CmdDeviceInfo = 0x0107;
constexpr uint16_t CmdBattery = 0x0108;
constexpr uint16_t CmdBatteryPush = 0x0127;
constexpr uint16_t CmdAncPush = 0x2B03;
constexpr uint16_t CmdAncSet = 0x2B04;
constexpr uint16_t CmdWearSet = 0x2B10;
constexpr uint16_t CmdWearGet = 0x2B11;
constexpr uint16_t CmdAncGet = 0x2B2A;
constexpr uint16_t CmdMultipointSet = 0x2B2E;
constexpr uint16_t CmdMultipointGet = 0x2B2F;
constexpr uint16_t CmdEqualizerSet = 0x2B49;
constexpr uint16_t CmdEqualizerGet = 0x2B4A;
constexpr uint16_t CmdLowLatency = 0x2B6C;

constexpr int TlvStatus = 0x7F;

constexpr int AncOff = 0;
constexpr int AncCancelling = 1;
constexpr int AncAwareness = 2;
constexpr int AwarenessVoiceBoost = 1;
constexpr int AwarenessNormal = 2;
constexpr int LevelAuto = 0xFF;

constexpr qint64 StatusTimeoutMs = 10000;
constexpr qint64 RequestTimeoutMs = 2000;
constexpr qint64 SettleMs = 300;
constexpr qint64 LowLatencySettleMs = 1000;
constexpr qint64 BatteryPollMs = 60000;

enum Feature : uint32_t {
    FeatCancelling = 1 << 0,
    FeatAwareness = 1 << 1,
    FeatVoiceBoost = 1 << 2,
    FeatTws = 1 << 3,
    FeatWear = 1 << 4,
    FeatLowLatency = 1 << 5,
    FeatEqualizer = 1 << 6,
    FeatMultipoint = 1 << 7,
    FeatDualEngine = 1 << 8,
};

constexpr uint8_t LevelNormal = 1 << 0;
constexpr uint8_t LevelComfort = 1 << 1;
constexpr uint8_t LevelUltra = 1 << 2;
constexpr uint8_t LevelDynamic = 1 << 3;
constexpr uint8_t LevelAll = LevelNormal | LevelComfort | LevelUltra | LevelDynamic;

constexpr uint32_t FullTws = FeatCancelling | FeatAwareness | FeatVoiceBoost | FeatTws | FeatWear | FeatEqualizer | FeatMultipoint;

struct ModelSpec {
    HuaweiModel model;
    const char *name;
    const char *modelIds;
    const char *skus;
    uint32_t features;
    uint8_t levels;
    qint64 startDelayMs;
};

constexpr ModelSpec Models[] = {
    {HuaweiModel::FreeBudsPro5, "FreeBuds Pro 5", "00016B,00016D", "", FullTws | FeatLowLatency | FeatDualEngine, LevelAll, 0},
    {HuaweiModel::FreeBudsPro4, "FreeBuds Pro 4", "000157", "", FullTws, LevelAll, 0},
    {HuaweiModel::FreeBudsPro3, "FreeBuds Pro 3", "000149", "BTFT0018", FullTws, LevelAll, 0},
    {HuaweiModel::FreeBudsPro2, "FreeBuds Pro 2", "000131,00013F", "BTFT0006", FullTws, LevelAll, 0},
    {HuaweiModel::FreeBudsPro, "FreeBuds Pro", "00003C", "", FeatCancelling | FeatAwareness | FeatVoiceBoost | FeatTws | FeatMultipoint, LevelAll, 0},
    {HuaweiModel::FreeBudsStudio, "FreeBuds Studio", "00003B", "", FeatCancelling | FeatAwareness | FeatVoiceBoost | FeatWear, LevelAll, 0},
    {HuaweiModel::FreeBudsSe4, "FreeBuds SE 4", "000169", "", FeatCancelling | FeatAwareness | FeatTws | FeatEqualizer | FeatLowLatency, LevelAll, 0},
    {HuaweiModel::FreeBudsSe2, "FreeBuds SE 2", "000147", "", FeatTws | FeatEqualizer | FeatLowLatency, 0, 0},
    {HuaweiModel::FreeBudsSe, "FreeBuds SE", "00012B", "", FeatTws, 0, 0},
    {HuaweiModel::FreeBuds6i, "FreeBuds 6i", "000153", "",
     FeatCancelling | FeatAwareness | FeatTws | FeatWear | FeatLowLatency | FeatEqualizer | FeatMultipoint, LevelAll, 0},
    {HuaweiModel::FreeBuds5i, "FreeBuds 5i", "00013D,000145", "",
     FeatCancelling | FeatAwareness | FeatTws | FeatWear | FeatLowLatency | FeatEqualizer | FeatMultipoint, LevelAll, 0},
    {HuaweiModel::FreeBuds4i, "FreeBuds 4i", "000124", "", FeatCancelling | FeatAwareness | FeatTws | FeatWear, 0, 0},
    {HuaweiModel::FreeBuds6, "FreeBuds 6", "000155,000161", "BTFT0020",
     FeatCancelling | FeatTws | FeatWear | FeatEqualizer | FeatMultipoint, LevelNormal | LevelComfort | LevelDynamic, 0},
    {HuaweiModel::FreeBuds5, "FreeBuds 5", "000141", "BTFT0013",
     FeatCancelling | FeatTws | FeatWear | FeatEqualizer | FeatMultipoint, LevelNormal | LevelComfort | LevelDynamic, 0},
    {HuaweiModel::FreeBuds4, "FreeBuds 4", "000129", "", FeatCancelling | FeatTws, LevelNormal | LevelComfort, 0},
    {HuaweiModel::FreeBuds3, "FreeBuds 3", "000027", "", FeatTws, 0, 0},
    {HuaweiModel::FreeClip2, "FreeClip 2", "000167,000173", "", FeatTws | FeatWear | FeatEqualizer | FeatLowLatency | FeatMultipoint, 0, 0},
    {HuaweiModel::FreeClip, "FreeClip", "00014B", "", FeatTws, 0, 0},
    {HuaweiModel::FreeLacePro2, "FreeLace Pro 2", "00014D", "",
     FeatCancelling | FeatAwareness | FeatVoiceBoost | FeatEqualizer | FeatLowLatency | FeatMultipoint, LevelAll, 0},
    {HuaweiModel::FreeLacePro, "FreeLace Pro", "000117", "", FeatCancelling | FeatAwareness, LevelNormal | LevelComfort | LevelUltra, 2000},
    {HuaweiModel::HonorEarbuds2, "HONOR Earbuds 2", "", "", FeatCancelling | FeatAwareness | FeatTws | FeatWear, 0, 0},
};

constexpr ModelSpec UnknownModel = {HuaweiModel::Unknown, "FreeBuds", "", "", FeatTws, 0, 0};

const ModelSpec &specOf(HuaweiModel model)
{
    for (const auto &m : Models) {
        if (m.model == model)
            return m;
    }
    return UnknownModel;
}

bool containsWord(const QString &haystack, const QString &needle)
{
    qsizetype from = 0;
    for (;;) {
        const qsizetype i = haystack.indexOf(needle, from, Qt::CaseInsensitive);
        if (i < 0)
            return false;
        const qsizetype end = i + needle.size();
        if (end >= haystack.size() || !haystack.at(end).isLetterOrNumber())
            return true;
        from = i + 1;
    }
}

struct NamedPreset {
    int id;
    const char *key;
};

constexpr NamedPreset EqualizerPresets[] = {
    {0, "default"}, {1, "default"}, {5, "default"}, {2, "bass-boost"}, {3, "treble-boost"}, {9, "vocal"},
    {10, "lively"}, {11, "balanced"}, {12, "classical"}, {13, "movie"}, {14, "gaming"}, {15, "podcast"},
    {16, "punchy"}, {17, "adaptive"}, {18, "natural"}, {19, "concert"}, {25, "shooter"},
};

QString presetKey(int id)
{
    for (const auto &p : EqualizerPresets) {
        if (p.id == id)
            return QString::fromLatin1(p.key);
    }
    return QStringLiteral("preset-%1").arg(id);
}

QString text(const QByteArray &value)
{
    QByteArray v = value;
    while (v.endsWith('\0'))
        v.chop(1);
    return QString::fromUtf8(v).trimmed();
}

QByteArray bytes(std::initializer_list<int> values)
{
    QByteArray out;
    for (const int v : values)
        out.append(char(v));
    return out;
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

}

HuaweiModel HuaweiBackend::modelFromName(const QString &name)
{
    for (const auto &m : Models) {
        if (containsWord(name, QLatin1String(m.name)))
            return m.model;
    }
    return HuaweiModel::Unknown;
}

HuaweiModel HuaweiBackend::modelFromInfo(const QString &sku, const QString &modelId)
{
    for (const auto &m : Models) {
        if (!modelId.isEmpty() && QString::fromLatin1(m.modelIds).split(u',', Qt::SkipEmptyParts).contains(modelId, Qt::CaseInsensitive))
            return m.model;
    }
    for (const auto &m : Models) {
        if (!sku.isEmpty() && QString::fromLatin1(m.skus).split(u',', Qt::SkipEmptyParts).contains(sku, Qt::CaseInsensitive))
            return m.model;
    }
    return HuaweiModel::Unknown;
}

QString HuaweiBackend::modelName(HuaweiModel model)
{
    if (model == HuaweiModel::HonorEarbuds2)
        return QStringLiteral("HONOR Earbuds 2");
    return QStringLiteral("HUAWEI ") + QLatin1String(specOf(model).name);
}

uint16_t HuaweiBackend::crc16(const QByteArray &data)
{
    uint16_t crc = 0;
    for (const char c : data) {
        crc ^= uint16_t(uint8_t(c)) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

QByteArray HuaweiBackend::encodeFrame(const QByteArray &payload)
{
    const int length = int(payload.size()) + 1;
    QByteArray out;
    out.append(char(Magic));
    out.append(char((length >> 8) & 0xFF));
    out.append(char(length & 0xFF));
    out.append(char(FlagSingle));
    out.append(payload);
    const uint16_t crc = crc16(out);
    out.append(char(crc >> 8));
    out.append(char(crc & 0xFF));
    return out;
}

QByteArray HuaweiBackend::encodePacket(uint16_t id, const QList<std::pair<int, QByteArray>> &tlv)
{
    QByteArray payload;
    payload.append(char(id >> 8));
    payload.append(char(id & 0xFF));
    for (const auto &[type, value] : tlv) {
        payload.append(char(type));
        payload.append(char(value.size()));
        payload.append(value);
    }
    return encodeFrame(payload);
}

std::optional<HuaweiBackend::Packet> HuaweiBackend::decodePayload(const QByteArray &payload)
{
    if (payload.size() < 2)
        return std::nullopt;
    Packet p;
    p.id = uint16_t((uint8_t(payload.at(0)) << 8) | uint8_t(payload.at(1)));
    qsizetype i = 2;
    while (i + 1 < payload.size()) {
        const int type = uint8_t(payload.at(i));
        const int length = uint8_t(payload.at(i + 1));
        const qsizetype end = std::min<qsizetype>(i + 2 + length, payload.size());
        p.tlv.insert(type, payload.mid(i + 2, end - i - 2));
        i = end;
    }
    return p;
}

HuaweiBackend::HuaweiBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
    , _model(modelFromName(device.name))
{
}

bool HuaweiBackend::has(uint32_t feature) const
{
    return specOf(_model).features & feature;
}

QStringList HuaweiBackend::modes() const
{
    QStringList m;
    if (has(FeatCancelling))
        m << ModeNc;
    if (has(FeatAwareness))
        m << ModeAmbient;
    if (!m.isEmpty())
        m << ModeOff;
    return m;
}

QList<int> HuaweiBackend::ancLevels() const
{
    QList<int> out;
    const uint8_t mask = specOf(_model).levels;
    for (const int level : {1, 0, 2, 3}) {
        if (mask & (1 << level))
            out << level;
    }
    return out;
}

QString HuaweiBackend::ancLevelKey(int level) const
{
    switch (level) {
    case 0:
        return QStringLiteral("normal");
    case 1:
        return QStringLiteral("comfort");
    case 2:
        return QStringLiteral("ultra");
    case 3:
        return has(FeatDualEngine) ? QStringLiteral("dual-engine") : QStringLiteral("dynamic");
    default:
        return {};
    }
}

bool HuaweiBackend::start()
{
    _sinceStart.start();
    _sinceIdle.start();
    _sinceBattery.start();
    qCInfo(lcHp) << "huawei model from name:" << modelName(_model);
    read(CmdDeviceInfo, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 24, 27}, [this](bool) { queueFeatureReads(); },
         specOf(_model).startDelayMs);
    read(CmdBattery, {1, 2, 3});
    return true;
}

void HuaweiBackend::request(uint16_t id, const QList<std::pair<int, QByteArray>> &tlv, std::function<void(bool)> done, qint64 delayMs)
{
    _queue.push_back(Request{encodePacket(id, tlv), id, delayMs, std::move(done)});
}

void HuaweiBackend::read(uint16_t id, const QList<int> &types, std::function<void(bool)> done, qint64 delayMs)
{
    QList<std::pair<int, QByteArray>> tlv;
    for (const int t : types)
        tlv.append({t, QByteArray()});
    request(id, tlv, std::move(done), delayMs);
}

void HuaweiBackend::queueFeatureReads()
{
    qCInfo(lcHp) << "huawei model:" << modelName(_model) << _modelId;
    if (has(FeatCancelling | FeatAwareness))
        read(CmdAncGet, {1});
    if (has(FeatWear))
        read(CmdWearGet, {1});
    if (has(FeatLowLatency))
        read(CmdLowLatency, {2});
    if (has(FeatEqualizer))
        read(CmdEqualizerGet, {2});
    if (has(FeatMultipoint))
        read(CmdMultipointGet, {1});
}

void HuaweiBackend::pump()
{
    if (_inflight && _inflightTimer.elapsed() > RequestTimeoutMs) {
        qCDebug(lcHp) << "huawei request timed out" << Qt::hex << _inflight->reply;
        auto done = std::move(_inflight->done);
        _inflight.reset();
        _sinceIdle.restart();
        if (done)
            done(false);
    }
    if (_inflight || _queue.empty() || _sinceIdle.elapsed() < _queue.front().delayMs)
        return;
    _inflight = std::move(_queue.front());
    _queue.pop_front();
    _tx.append(_inflight->frame);
    _inflightTimer.start();
    qCDebug(lcHp) << "huawei tx" << _inflight->frame.toHex(' ');
}

bool HuaweiBackend::flush()
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

bool HuaweiBackend::receive()
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

    for (;;) {
        const qsizetype start = _rx.indexOf(char(Magic));
        if (start < 0) {
            _rx.clear();
            break;
        }
        _rx.remove(0, start);
        if (_rx.size() < 3)
            break;
        const int total = ((uint8_t(_rx.at(1)) << 8) | uint8_t(_rx.at(2))) + 5;
        if (total < MinFrame || total > MaxFrame) {
            _rx.remove(0, 1);
            continue;
        }
        if (_rx.size() < total)
            break;
        const QByteArray frame = _rx.left(total);
        const uint16_t crc = uint16_t((uint8_t(frame.at(total - 2)) << 8) | uint8_t(frame.at(total - 1)));
        if (crc16(frame.left(total - 2)) != crc) {
            _rx.remove(0, 1);
            continue;
        }
        _rx.remove(0, total);

        const uint8_t flag = uint8_t(frame.at(3));
        QByteArray payload;
        if (flag == FlagSingle) {
            payload = frame.mid(4, total - 6);
        } else if (flag >= FlagFirst && flag <= FlagLast && total > MinFrame) {
            if (flag == FlagFirst) {
                _fragments.clear();
                _fragmenting = true;
                _fragmentIndex = 0;
            }
            if (!_fragmenting || uint8_t(frame.at(4)) != _fragmentIndex) {
                _fragments.clear();
                _fragmenting = false;
                continue;
            }
            _fragments.append(frame.mid(5, total - 7));
            _fragmentIndex++;
            if (flag != FlagLast)
                continue;
            payload = _fragments;
            _fragments.clear();
            _fragmenting = false;
        } else {
            continue;
        }

        qCDebug(lcHp) << "huawei rx" << payload.toHex(' ');
        if (const auto packet = decodePayload(payload))
            handle(*packet);
    }
    return true;
}

bool HuaweiBackend::tick()
{
    if (!receive())
        return false;
    if (ready() && _queue.empty() && !_inflight && _sinceBattery.elapsed() > BatteryPollMs) {
        _sinceBattery.restart();
        read(CmdBattery, {1, 2, 3});
    }
    pump();
    if (!flush())
        return false;
    if (!ready() && _sinceStart.elapsed() > StatusTimeoutMs + specOf(_model).startDelayMs)
        return fail(QStringLiteral("no battery status from the earbuds"));
    return true;
}

void HuaweiBackend::handle(const Packet &p)
{
    switch (p.id) {
    case CmdDeviceInfo: {
        const QString model = text(p.value(10));
        const QString sku = model.contains(u'-') ? model.section(u'-', 0, 0) : (model.isEmpty() ? text(p.value(15)) : model);
        _modelId = model.contains(u'-') ? model.section(u'-', 1) : QString();
        const HuaweiModel m = modelFromInfo(sku, _modelId);
        if (m != HuaweiModel::Unknown)
            _model = m;
        qCInfo(lcHp) << "huawei device info: sku" << sku << "model id" << _modelId << "firmware" << text(p.value(7));
        break;
    }
    case CmdBattery:
    case CmdBatteryPush: {
        Battery b;
        const QByteArray overall = p.value(1);
        const QByteArray levels = p.value(2);
        const QByteArray charging = p.value(3);
        if (overall.size() == 1)
            b.overall = uint8_t(overall.at(0));
        if (levels.size() >= 3) {
            b.left = uint8_t(levels.at(0));
            b.right = uint8_t(levels.at(1));
            b.caseLevel = uint8_t(levels.at(2));
        }
        if (charging.size() >= 3) {
            b.chargingLeft = charging.at(0) != 0;
            b.chargingRight = charging.at(1) != 0;
            b.chargingCase = charging.at(2) != 0;
        }
        b.chargingAny = std::any_of(charging.cbegin(), charging.cend(), [](char c) { return c != 0; });
        if (b.overall < 0 && b.left < 0)
            break;
        if (!_battery)
            qCInfo(lcHp) << "initialized";
        _battery = b;
        _sinceBattery.restart();
        break;
    }
    case CmdAncGet: {
        const QByteArray v = p.value(1);
        if (v.size() == 2)
            _anc = Anc{uint8_t(v.at(1)), uint8_t(v.at(0))};
        break;
    }
    case CmdAncPush: {
        const QByteArray v = p.value(1);
        if (_model == HuaweiModel::FreeLacePro && v.size() == 1 && uint8_t(v.at(0)) <= AncAwareness)
            read(CmdAncGet, {1});
        break;
    }
    case CmdWearGet: {
        const QByteArray v = p.value(1);
        if (v.size() == 1)
            _wearDetection = v.at(0) != 0;
        break;
    }
    case CmdLowLatency: {
        const QByteArray v = p.value(2);
        if (v.size() == 1)
            _lowLatency = v.at(0) != 0;
        break;
    }
    case CmdEqualizerGet: {
        const QByteArray active = p.value(2);
        if (active.size() == 1)
            _equalizer = uint8_t(active.at(0));
        const QByteArray available = p.value(3);
        if (!available.isEmpty()) {
            _equalizerPresets.clear();
            for (const char c : available) {
                if (uint8_t(c) != 0xFF && !_equalizerPresets.contains(uint8_t(c)))
                    _equalizerPresets << uint8_t(c);
            }
        }
        break;
    }
    case CmdMultipointGet: {
        const QByteArray v = p.value(1);
        if (v.size() == 1)
            _multipoint = v.at(0) != 0;
        break;
    }
    default:
        break;
    }

    if (p.has(TlvStatus) && p.value(TlvStatus).size() == 4) {
        const QByteArray s = p.value(TlvStatus);
        const uint32_t status = (uint32_t(uint8_t(s.at(0))) << 24) | (uint32_t(uint8_t(s.at(1))) << 16) | (uint32_t(uint8_t(s.at(2))) << 8)
                                | uint8_t(s.at(3));
        qCDebug(lcHp) << "huawei status" << Qt::hex << p.id << Qt::dec << status;
    }

    if (_inflight && _inflight->reply == p.id) {
        auto done = std::move(_inflight->done);
        _inflight.reset();
        _sinceIdle.restart();
        if (done)
            done(true);
    }
}

Snapshot HuaweiBackend::snapshot()
{
    Snapshot s;
    if (!_battery)
        return s;
    const Battery &b = *_battery;

    const auto valid = [](int level) { return level > 0 && level <= 100; };
    if (has(FeatTws) && (valid(b.left) || valid(b.right))) {
        int buds = -1;
        const auto addBud = [&](const char *part, int level, bool charging) {
            if (!valid(level))
                return;
            s.batteries.append(QJsonObject{
                {QStringLiteral("part"), QString::fromLatin1(part)},
                {QStringLiteral("level"), level},
                {QStringLiteral("charging"), charging},
            });
            buds = buds < 0 ? level : std::min(buds, level);
        };
        addBud("left", b.left, b.chargingLeft);
        addBud("right", b.right, b.chargingRight);
        if (valid(b.caseLevel)) {
            s.batteries.append(QJsonObject{
                {QStringLiteral("part"), QStringLiteral("case")},
                {QStringLiteral("level"), b.caseLevel},
                {QStringLiteral("charging"), b.chargingCase},
            });
        }
        s.battery = buds;
    } else if (valid(b.overall)) {
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QStringLiteral("main")},
            {QStringLiteral("level"), b.overall},
            {QStringLiteral("charging"), b.chargingAny},
        });
        s.battery = b.overall;
    }

    s.modes = modes();
    if (!s.modes.isEmpty() && _anc) {
        switch (_anc->mode) {
        case AncCancelling:
            s.mode = ModeNc;
            break;
        case AncAwareness:
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

    if (has(FeatEqualizer) && !_equalizerPresets.isEmpty()) {
        QJsonArray presets;
        for (const int id : std::as_const(_equalizerPresets))
            presets.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("key"), presetKey(id)}});
        o[QStringLiteral("equalizer")] = QJsonObject{
            {QStringLiteral("preset"), _equalizer.value_or(-1)},
            {QStringLiteral("presets"), presets},
        };
    }

    QJsonArray toggles;
    const auto toggle = [&](const char *key, bool value) {
        toggles.append(QJsonObject{{QStringLiteral("key"), QString::fromLatin1(key)}, {QStringLiteral("value"), value}});
    };
    if (has(FeatVoiceBoost) && _anc && _anc->mode == AncAwareness)
        toggle("voiceBoost", _anc->level == AwarenessVoiceBoost);
    if (has(FeatWear) && _wearDetection)
        toggle("wearDetection", *_wearDetection);
    if (has(FeatLowLatency) && _lowLatency)
        toggle("lowLatency", *_lowLatency);
    if (has(FeatMultipoint) && _multipoint)
        toggle("multipoint", *_multipoint);
    if (!toggles.isEmpty())
        o[QStringLiteral("toggles")] = toggles;

    const QList<int> levels = ancLevels();
    if (has(FeatCancelling) && !levels.isEmpty() && _anc && _anc->mode == AncCancelling) {
        QJsonArray options;
        for (const int level : levels)
            options.append(ancLevelKey(level));
        o[QStringLiteral("choices")] = QJsonArray{QJsonObject{
            {QStringLiteral("key"), QStringLiteral("ancStrength")},
            {QStringLiteral("options"), options},
            {QStringLiteral("value"), levels.contains(_anc->level) ? ancLevelKey(_anc->level) : QString()},
        }};
    }

    s.settings = o;
    return s;
}

void HuaweiBackend::writeAnc(int mode, int level)
{
    request(CmdAncSet, {{1, bytes({mode, level})}});
    read(CmdAncGet, {1}, {}, SettleMs);
    if (!_anc)
        _anc = Anc{};
    _anc->mode = mode;
    if (level != LevelAuto)
        _anc->level = level;
}

QDBusError HuaweiBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!modes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("These headphones do not support mode %1").arg(mode));
    const int value = mode == ModeNc ? AncCancelling : mode == ModeAmbient ? AncAwareness : AncOff;
    writeAnc(value, _anc && _anc->mode == value ? _anc->level : LevelAuto);
    return {};
}

QDBusError HuaweiBackend::setAmbientLevel(int)
{
    return unsupported();
}

QDBusError HuaweiBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();

    const QJsonObject eq = in.value(QStringLiteral("equalizer")).toObject();
    if (eq.contains(QStringLiteral("preset"))) {
        const int preset = eq.value(QStringLiteral("preset")).toInt(-1);
        if (!has(FeatEqualizer) || !_equalizerPresets.contains(preset))
            return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Unknown equalizer preset %1").arg(preset));
        request(CmdEqualizerSet, {{1, bytes({preset})}});
        read(CmdEqualizerGet, {2}, {}, SettleMs);
        _equalizer = preset;
    }

    const QJsonObject toggles = in.value(QStringLiteral("toggles")).toObject();
    for (auto it = toggles.constBegin(); it != toggles.constEnd(); ++it) {
        if (!it.value().isBool())
            continue;
        const bool on = it.value().toBool();
        if (it.key() == QLatin1String("wearDetection") && has(FeatWear) && _wearDetection) {
            request(CmdWearSet, {{1, bytes({on})}});
            read(CmdWearGet, {1}, {}, SettleMs);
            _wearDetection = on;
        } else if (it.key() == QLatin1String("lowLatency") && has(FeatLowLatency) && _lowLatency) {
            request(CmdLowLatency, {{1, bytes({on})}});
            read(CmdLowLatency, {2}, {}, LowLatencySettleMs);
            _lowLatency = on;
        } else if (it.key() == QLatin1String("multipoint") && has(FeatMultipoint) && _multipoint) {
            request(CmdMultipointSet, {{1, bytes({on})}});
            read(CmdMultipointGet, {1}, {}, SettleMs);
            _multipoint = on;
        } else if (it.key() == QLatin1String("voiceBoost") && has(FeatVoiceBoost) && _anc && _anc->mode == AncAwareness) {
            writeAnc(AncAwareness, on ? AwarenessVoiceBoost : AwarenessNormal);
        }
    }

    const QJsonObject choices = in.value(QStringLiteral("choices")).toObject();
    if (const QJsonValue v = choices.value(QStringLiteral("ancStrength")); v.isString()) {
        if (!has(FeatCancelling) || !_anc || _anc->mode != AncCancelling)
            return QDBusError(QDBusError::Failed, QStringLiteral("Noise cancelling is not active"));
        const QList<int> levels = ancLevels();
        const auto level = std::find_if(levels.cbegin(), levels.cend(), [&](int l) { return ancLevelKey(l) == v.toString(); });
        if (level == levels.cend())
            return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Unknown noise cancelling strength %1").arg(v.toString()));
        writeAnc(AncCancelling, *level);
    }
    return {};
}
