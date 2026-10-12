#include "GalaxyBackend.h"

#include <algorithm>

#include "Log.h"

namespace {

constexpr uint8_t Som = 0xFD;
constexpr uint8_t Eom = 0xDD;
constexpr uint8_t LegacySom = 0xFE;
constexpr uint8_t LegacyEom = 0xEE;

constexpr uint8_t MsgStatusUpdated = 0x60;
constexpr uint8_t MsgExtendedStatusUpdated = 0x61;
constexpr uint8_t MsgSetAncWithOneEarbud = 0x6F;
constexpr uint8_t MsgNoiseControlsUpdate = 0x77;
constexpr uint8_t MsgNoiseControls = 0x78;
constexpr uint8_t MsgSetAmbientMode = 0x80;
constexpr uint8_t MsgAmbientModeUpdated = 0x81;
constexpr uint8_t MsgAmbientVolume = 0x84;
constexpr uint8_t MsgEqualizer = 0x86;
constexpr uint8_t MsgManagerInfo = 0x88;
constexpr uint8_t MsgLockTouchpad = 0x90;
constexpr uint8_t MsgSetNoiseReduction = 0x98;
constexpr uint8_t MsgNoiseReductionModeUpdate = 0x9B;
constexpr uint8_t MsgSetSeamlessConnection = 0xAF;

constexpr int NoiseOff = 0;
constexpr int NoiseCancelling = 1;
constexpr int NoiseAmbient = 2;
constexpr int NoiseAdaptive = 3;

constexpr uint8_t TouchUnlocked = 1 << 7;
constexpr uint8_t TouchHold = 1 << 0;
constexpr uint8_t TouchTripleTap = 1 << 1;
constexpr uint8_t TouchDoubleTap = 1 << 2;
constexpr uint8_t TouchSingleTap = 1 << 3;
constexpr uint8_t TouchDoubleTapCall = 1 << 4;
constexpr uint8_t TouchHoldCall = 1 << 5;

constexpr uint8_t LegacyWearRight = 1;
constexpr uint8_t LegacyWearLeft = 16;
constexpr int PlacementWearing = 1;

constexpr qint64 StatusTimeoutMs = 10000;

const char *const EqualizerPresets[] = {"off", "bass-boost", "soft", "dynamic", "clear", "treble-boost"};
constexpr int EqualizerPresetCount = int(std::size(EqualizerPresets));

struct NamedModel {
    const char *name;
    GalaxyModel model;
};

constexpr NamedModel ModelNames[] = {
    {"Buds2 Pro", GalaxyModel::Buds2Pro},
    {"Buds3 Pro", GalaxyModel::Buds3Pro},
    {"Buds4 Pro", GalaxyModel::Buds4Pro},
    {"Buds3 FE", GalaxyModel::Buds3Fe},
    {"Buds FE", GalaxyModel::BudsFe},
    {"Buds Core", GalaxyModel::BudsCore},
    {"Buds Live", GalaxyModel::BudsLive},
    {"Buds Pro", GalaxyModel::BudsPro},
    {"Buds+", GalaxyModel::BudsPlus},
    {"Buds2", GalaxyModel::Buds2},
    {"Buds3", GalaxyModel::Buds3},
    {"Buds4", GalaxyModel::Buds4},
};

uint16_t crc16(const QByteArray &data)
{
    uint16_t crc = 0;
    for (const char c : data) {
        crc ^= uint16_t(uint8_t(c)) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

uint8_t at(const QByteArray &p, int i)
{
    return i >= 0 && i < p.size() ? uint8_t(p.at(i)) : 0;
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

}

GalaxyModel GalaxyBackend::modelFromName(const QString &name, DeviceKind kind)
{
    if (kind == DeviceKind::GalaxyLegacy)
        return GalaxyModel::Buds;
    for (const auto &m : ModelNames) {
        if (name.contains(QLatin1String(m.name), Qt::CaseInsensitive))
            return m.model;
    }
    if (kind == DeviceKind::GalaxyClassic)
        return GalaxyModel::BudsPlus;
    return GalaxyModel::Unknown;
}

QString GalaxyBackend::modelName(GalaxyModel model)
{
    if (model == GalaxyModel::Buds)
        return QStringLiteral("Galaxy Buds");
    for (const auto &m : ModelNames) {
        if (m.model == model)
            return QStringLiteral("Galaxy ") + QLatin1String(m.name);
    }
    return QStringLiteral("Galaxy Buds");
}

GalaxyBackend::GalaxyBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
    , _model(modelFromName(device.name, device.kind))
{
}

bool GalaxyBackend::combinedNoiseControl() const
{
    switch (_model) {
    case GalaxyModel::Buds:
    case GalaxyModel::BudsPlus:
    case GalaxyModel::BudsLive:
        return false;
    default:
        return true;
    }
}

bool GalaxyBackend::hasAnc() const
{
    return _model != GalaxyModel::Buds && _model != GalaxyModel::BudsPlus;
}

bool GalaxyBackend::hasAmbient() const
{
    return _model != GalaxyModel::BudsLive;
}

bool GalaxyBackend::advancedTouchLock() const
{
    switch (_model) {
    case GalaxyModel::Buds:
    case GalaxyModel::BudsPlus:
    case GalaxyModel::BudsLive:
    case GalaxyModel::BudsPro:
        return false;
    default:
        return true;
    }
}

int GalaxyBackend::ambientMax() const
{
    switch (_model) {
    case GalaxyModel::Buds:
    case GalaxyModel::Buds4Pro:
        return 4;
    case GalaxyModel::BudsPro:
    case GalaxyModel::Buds2Pro:
    case GalaxyModel::Buds3Pro:
        return 3;
    case GalaxyModel::BudsLive:
        return 0;
    default:
        return 2;
    }
}

QStringList GalaxyBackend::modes() const
{
    QStringList m;
    if (hasAnc())
        m << ModeNc;
    if (hasAmbient())
        m << ModeAmbient;
    m << ModeOff;
    return m;
}

bool GalaxyBackend::start()
{
    _sinceStart.start();
    qCInfo(lcHp) << "galaxy model:" << modelName(_model) << (_model == GalaxyModel::Unknown ? "(unrecognized, using the Buds2 layout)" : "");
    return true;
}

void GalaxyBackend::send(uint8_t id, const QByteArray &payload)
{
    QByteArray body;
    body.append(char(id));
    body.append(payload);
    const uint16_t crc = crc16(body);
    const uint16_t size = uint16_t(body.size() + 2);

    if (legacyFraming()) {
        _tx.append(char(LegacySom));
        _tx.append(char(0));
        _tx.append(char(size & 0xFF));
    } else {
        _tx.append(char(Som));
        _tx.append(char(size & 0xFF));
        _tx.append(char((size >> 8) & 0x03));
    }
    _tx.append(body);
    _tx.append(char(crc & 0xFF));
    _tx.append(char(crc >> 8));
    _tx.append(char(legacyFraming() ? LegacyEom : Eom));
    qCDebug(lcHp) << "galaxy tx" << Qt::hex << id << payload.toHex(' ');
}

bool GalaxyBackend::flush()
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

bool GalaxyBackend::receive()
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

    const char som = char(legacyFraming() ? LegacySom : Som);
    const uint8_t eom = legacyFraming() ? LegacyEom : Eom;
    for (;;) {
        const qsizetype start = _rx.indexOf(som);
        if (start < 0) {
            _rx.clear();
            break;
        }
        _rx.remove(0, start);
        if (_rx.size() < 3)
            break;
        int size;
        bool fragment;
        if (legacyFraming()) {
            size = uint8_t(_rx.at(2));
            fragment = false;
        } else {
            const int header = uint8_t(_rx.at(1)) | (uint8_t(_rx.at(2)) << 8);
            size = header & 0x3FF;
            fragment = header & 0x2000;
        }
        const qsizetype total = 1 + 2 + size + 1;
        if (size < 3) {
            _rx.remove(0, 1);
            continue;
        }
        if (_rx.size() < total)
            break;
        if (uint8_t(_rx.at(total - 1)) != eom) {
            _rx.remove(0, 1);
            continue;
        }
        const QByteArray body = _rx.mid(3, size - 2);
        const uint16_t crc = uint8_t(_rx.at(3 + size - 2)) | (uint8_t(_rx.at(3 + size - 1)) << 8);
        _rx.remove(0, total);
        if (fragment || crc16(body) != crc)
            continue;
        qCDebug(lcHp) << "galaxy rx" << Qt::hex << uint8_t(body.at(0)) << body.mid(1).toHex(' ');
        handle(uint8_t(body.at(0)), body.mid(1));
    }
    return true;
}

bool GalaxyBackend::tick()
{
    if (!receive() || !flush())
        return false;
    if (!_status && _sinceStart.elapsed() > StatusTimeoutMs)
        return fail(QStringLiteral("no status from the earbuds"));
    return true;
}

void GalaxyBackend::handle(uint8_t id, const QByteArray &p)
{
    switch (id) {
    case MsgExtendedStatusUpdated:
        parseExtendedStatus(p);
        if (_status && !_managerInfoSent) {
            _managerInfoSent = true;
            send(MsgManagerInfo, QByteArray("\x01\x02\x22", 3));
            qCInfo(lcHp) << "initialized, status revision" << _status->revision;
        }
        break;
    case MsgStatusUpdated:
        parseStatus(p);
        break;
    case MsgNoiseControlsUpdate:
        if (_status && !p.isEmpty())
            _status->noiseMode = at(p, 0);
        break;
    case MsgAmbientModeUpdated:
        if (_status && !p.isEmpty())
            _status->noiseMode = at(p, 0) == 1 ? NoiseAmbient : NoiseOff;
        break;
    case MsgNoiseReductionModeUpdate:
        if (_status && !p.isEmpty())
            _status->noiseMode = at(p, 0) == 1 ? NoiseCancelling : NoiseOff;
        break;
    case MsgAmbientVolume:
        if (_status && !p.isEmpty())
            _status->ambientVolume = at(p, 0);
        break;
    default:
        break;
    }
}

void GalaxyBackend::parseExtendedStatus(const QByteArray &p)
{
    Status s = _status.value_or(Status{});
    s.revision = at(p, 0);
    s.batteryLeft = at(p, 2);
    s.batteryRight = at(p, 3);

    switch (_model) {
    case GalaxyModel::Buds: {
        if (p.size() < 13)
            return;
        s.leftPresent = s.batteryLeft > 0;
        s.rightPresent = s.batteryRight > 0;
        s.wearingLeft = at(p, 6) & LegacyWearLeft;
        s.wearingRight = at(p, 6) & LegacyWearRight;
        s.noiseMode = at(p, 7) == 1 ? NoiseAmbient : NoiseOff;
        s.ambientVolume = at(p, 9);
        s.equalizer = at(p, 10) == 1 ? (at(p, 11) % 5) + 1 : 0;
        const bool longForm = p.size() >= 15;
        s.touchLocked = longForm ? at(p, 12) == 1 : (at(p, 12) >> 4) != 0;
        if (s.revision >= 3) {
            s.seamless = at(p, longForm ? 14 : 13) == 0;
            s.seamlessKnown = true;
        }
        break;
    }
    case GalaxyModel::BudsPlus:
        if (p.size() < 13)
            return;
        s.leftPresent = (at(p, 6) >> 4) != 0;
        s.rightPresent = (at(p, 6) & 0x0F) != 0;
        s.wearingLeft = (at(p, 6) >> 4) == PlacementWearing;
        s.wearingRight = (at(p, 6) & 0x0F) == PlacementWearing;
        s.batteryCase = at(p, 7);
        s.noiseMode = at(p, 8) == 1 ? NoiseAmbient : NoiseOff;
        s.ambientVolume = at(p, 9);
        s.equalizer = at(p, 11);
        s.touchLocked = at(p, 12) == 1;
        if (s.revision >= 11 && p.size() > 21) {
            s.seamless = at(p, 21) == 0;
            s.seamlessKnown = true;
        }
        break;
    default:
        if (p.size() < 20)
            return;
        s.leftPresent = (at(p, 6) >> 4) != 0;
        s.rightPresent = (at(p, 6) & 0x0F) != 0;
        s.wearingLeft = (at(p, 6) >> 4) == PlacementWearing;
        s.wearingRight = (at(p, 6) & 0x0F) == PlacementWearing;
        s.batteryCase = at(p, 7);
        s.equalizer = at(p, 9);
        s.touchFlags = at(p, 10);
        s.touchLocked = advancedTouchLock() ? !(s.touchFlags & TouchUnlocked) : s.touchFlags == 1;
        if (_model == GalaxyModel::BudsLive)
            s.noiseMode = at(p, 12) == 1 ? NoiseCancelling : NoiseOff;
        else
            s.noiseMode = at(p, 12);
        s.seamless = at(p, 19) == 0;
        s.seamlessKnown = true;
        if (combinedNoiseControl() && p.size() > 23)
            s.ambientVolume = at(p, 23);
        if (_model == GalaxyModel::BudsPro) {
            if (s.revision >= 8 && p.size() > 32) {
                s.oneEarbud = at(p, 32) == 1;
                s.oneEarbudKnown = true;
            }
        } else if (combinedNoiseControl() && (_model != GalaxyModel::Buds2 || s.revision >= 3) && p.size() > 28) {
            s.oneEarbud = at(p, 28) == 1;
            s.oneEarbudKnown = true;
        }
        if (_model == GalaxyModel::Buds2 && s.revision >= 10 && p.size() > 36) {
            const uint8_t charging = at(p, 36);
            s.chargingLeft = charging & 0x10;
            s.chargingRight = charging & 0x04;
            s.chargingCase = charging & 0x01;
        }
        break;
    }
    _status = s;
}

void GalaxyBackend::parseStatus(const QByteArray &p)
{
    if (!_status || p.size() < 6)
        return;
    Status &s = *_status;
    s.batteryLeft = at(p, 1);
    s.batteryRight = at(p, 2);
    if (_model == GalaxyModel::Buds) {
        s.leftPresent = s.batteryLeft > 0;
        s.rightPresent = s.batteryRight > 0;
        return;
    }
    s.leftPresent = (at(p, 5) >> 4) != 0;
    s.rightPresent = (at(p, 5) & 0x0F) != 0;
    s.wearingLeft = (at(p, 5) >> 4) == PlacementWearing;
    s.wearingRight = (at(p, 5) & 0x0F) == PlacementWearing;
    if (p.size() > 6)
        s.batteryCase = at(p, 6);
    if (advancedTouchLock() && p.size() > 7) {
        const uint8_t charging = at(p, 7);
        s.chargingLeft = charging & 0x10;
        s.chargingRight = charging & 0x04;
        s.chargingCase = charging & 0x01;
    }
}

Snapshot GalaxyBackend::snapshot()
{
    Snapshot s;
    if (!_status)
        return s;
    const Status &st = *_status;

    int buds = -1;
    const auto addBud = [&](const char *part, int level, bool present, bool charging) {
        if (!present || level <= 0 || level > 100)
            return;
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QString::fromLatin1(part)},
            {QStringLiteral("level"), level},
            {QStringLiteral("charging"), charging},
        });
        buds = buds < 0 ? level : std::min(buds, level);
    };
    addBud("left", st.batteryLeft, st.leftPresent, st.chargingLeft);
    addBud("right", st.batteryRight, st.rightPresent, st.chargingRight);
    if (st.batteryCase > 0 && st.batteryCase <= 100) {
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QStringLiteral("case")},
            {QStringLiteral("level"), st.batteryCase},
            {QStringLiteral("charging"), st.chargingCase},
        });
    }
    s.battery = buds;

    s.modes = modes();
    switch (st.noiseMode) {
    case NoiseCancelling:
        s.mode = ModeNc;
        break;
    case NoiseAmbient:
        s.mode = ModeAmbient;
        break;
    case NoiseAdaptive:
        s.mode = ModeAdaptive;
        break;
    default:
        s.mode = ModeOff;
        break;
    }
    s.ambientLevel = std::clamp(st.ambientVolume, 0, std::max(ambientMax(), 0)) + 1;

    QJsonObject o;
    o[QStringLiteral("model")] = modelName(_model);
    if (hasAmbient() && ambientMax() > 0) {
        o[QStringLiteral("ambient")] = QJsonObject{
            {QStringLiteral("min"), 1},
            {QStringLiteral("max"), ambientMax() + 1},
            {QStringLiteral("focusOnVoice"), false},
        };
    }
    QJsonArray presets;
    for (int i = 0; i < EqualizerPresetCount; i++)
        presets.append(QJsonObject{{QStringLiteral("id"), i}, {QStringLiteral("key"), QString::fromLatin1(EqualizerPresets[i])}});
    o[QStringLiteral("equalizer")] = QJsonObject{
        {QStringLiteral("preset"), std::clamp(st.equalizer, 0, EqualizerPresetCount - 1)},
        {QStringLiteral("presets"), presets},
    };
    o[QStringLiteral("touchLock")] = st.touchLocked;
    o[QStringLiteral("wearing")] = QJsonObject{
        {QStringLiteral("left"), st.wearingLeft},
        {QStringLiteral("right"), st.wearingRight},
    };
    if (st.oneEarbudKnown)
        o[QStringLiteral("oneEarbudNoise")] = st.oneEarbud;
    if (st.seamlessKnown)
        o[QStringLiteral("seamless")] = st.seamless;
    s.settings = o;
    return s;
}

QDBusError GalaxyBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!modes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("These earbuds do not support mode %1").arg(mode));
    const int value = mode == ModeNc ? NoiseCancelling : mode == ModeAmbient ? NoiseAmbient : NoiseOff;

    if (combinedNoiseControl()) {
        send(MsgNoiseControls, QByteArray(1, char(value)));
    } else {
        if (hasAmbient())
            send(MsgSetAmbientMode, QByteArray(1, char(value == NoiseAmbient)));
        if (hasAnc())
            send(MsgSetNoiseReduction, QByteArray(1, char(value == NoiseCancelling)));
    }
    _status->noiseMode = value;
    return {};
}

QDBusError GalaxyBackend::setAmbientLevel(int level)
{
    if (!ready())
        return notReady();
    if (!hasAmbient() || ambientMax() == 0)
        return unsupported();
    const int volume = std::clamp(level, 1, ambientMax() + 1) - 1;
    send(MsgAmbientVolume, QByteArray(1, char(volume)));
    _status->ambientVolume = volume;
    return {};
}

QDBusError GalaxyBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();
    Status &st = *_status;

    const QJsonObject eq = in.value(QStringLiteral("equalizer")).toObject();
    if (eq.contains(QStringLiteral("preset"))) {
        const int preset = std::clamp(eq.value(QStringLiteral("preset")).toInt(), 0, EqualizerPresetCount - 1);
        if (_model == GalaxyModel::Buds) {
            QByteArray payload;
            payload.append(char(preset > 0));
            payload.append(char(preset > 0 ? preset - 1 + 5 : 5));
            send(MsgEqualizer, payload);
        } else {
            send(MsgEqualizer, QByteArray(1, char(preset)));
        }
        st.equalizer = preset;
    }

    if (const QJsonValue v = in.value(QStringLiteral("touchLock")); v.isBool()) {
        const bool locked = v.toBool();
        if (advancedTouchLock()) {
            QByteArray payload;
            payload.append(char(locked ? 0 : 1));
            payload.append(char(bool(st.touchFlags & TouchSingleTap)));
            payload.append(char(bool(st.touchFlags & TouchDoubleTap)));
            payload.append(char(bool(st.touchFlags & TouchTripleTap)));
            payload.append(char(bool(st.touchFlags & TouchHold)));
            if (st.revision >= 7) {
                payload.append(char(bool(st.touchFlags & TouchDoubleTapCall)));
                payload.append(char(bool(st.touchFlags & TouchHoldCall)));
            }
            send(MsgLockTouchpad, payload);
            st.touchFlags = locked ? (st.touchFlags & ~TouchUnlocked) : (st.touchFlags | TouchUnlocked);
        } else {
            send(MsgLockTouchpad, QByteArray(1, char(locked)));
        }
        st.touchLocked = locked;
    }

    if (const QJsonValue v = in.value(QStringLiteral("oneEarbudNoise")); v.isBool() && st.oneEarbudKnown) {
        send(MsgSetAncWithOneEarbud, QByteArray(1, char(v.toBool())));
        st.oneEarbud = v.toBool();
    }

    if (const QJsonValue v = in.value(QStringLiteral("seamless")); v.isBool() && st.seamlessKnown) {
        send(MsgSetSeamlessConnection, QByteArray(1, char(v.toBool() ? 0 : 1)));
        st.seamless = v.toBool();
    }
    return {};
}
