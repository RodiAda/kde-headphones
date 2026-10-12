#include "GalaxyBackend.h"

#include <algorithm>

#include "Log.h"

namespace {

constexpr uint8_t Som = 0xFD;
constexpr uint8_t Eom = 0xDD;

constexpr uint8_t MsgStatusUpdated = 0x60;
constexpr uint8_t MsgExtendedStatusUpdated = 0x61;
constexpr uint8_t MsgNoiseControlsUpdate = 0x77;
constexpr uint8_t MsgNoiseControls = 0x78;
constexpr uint8_t MsgAmbientVolume = 0x84;
constexpr uint8_t MsgEqualizer = 0x86;
constexpr uint8_t MsgManagerInfo = 0x88;
constexpr uint8_t MsgLockTouchpad = 0x90;
constexpr uint8_t MsgSetSeamlessConnection = 0xAF;

constexpr int NoiseOff = 0;
constexpr int NoiseCancelling = 1;
constexpr int NoiseAmbient = 2;

constexpr uint8_t TouchUnlocked = 1 << 7;
constexpr uint8_t TouchHold = 1 << 0;
constexpr uint8_t TouchTripleTap = 1 << 1;
constexpr uint8_t TouchDoubleTap = 1 << 2;
constexpr uint8_t TouchSingleTap = 1 << 3;
constexpr uint8_t TouchDoubleTapCall = 1 << 4;
constexpr uint8_t TouchHoldCall = 1 << 5;

constexpr qint64 StatusTimeoutMs = 10000;

const char *const EqualizerPresets[] = {"off", "bass-boost", "soft", "dynamic", "clear", "treble-boost"};

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
    return i < p.size() ? uint8_t(p.at(i)) : 0;
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

}

GalaxyBackend::GalaxyBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
{
}

bool GalaxyBackend::start()
{
    _sinceStart.start();
    return true;
}

void GalaxyBackend::send(uint8_t id, const QByteArray &payload)
{
    QByteArray body;
    body.append(char(id));
    body.append(payload);
    const uint16_t crc = crc16(body);
    const uint16_t size = uint16_t(body.size() + 2);

    _tx.append(char(Som));
    _tx.append(char(size & 0xFF));
    _tx.append(char((size >> 8) & 0x03));
    _tx.append(body);
    _tx.append(char(crc & 0xFF));
    _tx.append(char(crc >> 8));
    _tx.append(char(Eom));
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

    for (;;) {
        const qsizetype start = _rx.indexOf(char(Som));
        if (start < 0) {
            _rx.clear();
            break;
        }
        _rx.remove(0, start);
        if (_rx.size() < 3)
            break;
        const int header = uint8_t(_rx.at(1)) | (uint8_t(_rx.at(2)) << 8);
        const int size = header & 0x3FF;
        const bool fragment = header & 0x2000;
        const qsizetype total = 1 + 2 + size + 1;
        if (size < 3) {
            _rx.remove(0, 1);
            continue;
        }
        if (_rx.size() < total)
            break;
        if (uint8_t(_rx.at(total - 1)) != Eom) {
            _rx.remove(0, 1);
            continue;
        }
        const QByteArray body = _rx.mid(3, size - 2);
        const uint16_t crc = uint8_t(_rx.at(3 + size - 2)) | (uint8_t(_rx.at(3 + size - 1)) << 8);
        _rx.remove(0, total);
        if (fragment || crc16(body) != crc)
            continue;
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
        if (!_managerInfoSent) {
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
    if (p.size() < 25)
        return;
    Status s;
    s.revision = at(p, 0);
    s.batteryLeft = at(p, 2);
    s.batteryRight = at(p, 3);
    s.placementLeft = (at(p, 6) & 0xF0) >> 4;
    s.placementRight = at(p, 6) & 0x0F;
    s.batteryCase = at(p, 7);
    s.equalizer = at(p, 9);
    s.touchFlags = at(p, 10);
    s.noiseMode = at(p, 12);
    s.seamless = at(p, 19) == 0;
    s.ambientVolume = at(p, 23);
    if (s.revision >= 10 && p.size() > 36) {
        const uint8_t charging = at(p, 36);
        s.chargingLeft = charging & 0x10;
        s.chargingRight = charging & 0x04;
        s.chargingCase = charging & 0x01;
    }
    _status = s;
}

void GalaxyBackend::parseStatus(const QByteArray &p)
{
    if (!_status || p.size() < 7)
        return;
    _status->batteryLeft = at(p, 1);
    _status->batteryRight = at(p, 2);
    _status->placementLeft = (at(p, 5) & 0xF0) >> 4;
    _status->placementRight = at(p, 5) & 0x0F;
    _status->batteryCase = at(p, 6);
    if (p.size() > 7) {
        const uint8_t charging = at(p, 7);
        _status->chargingLeft = charging & 0x10;
        _status->chargingRight = charging & 0x04;
        _status->chargingCase = charging & 0x01;
    }
}

int GalaxyBackend::ambientMax() const
{
    const QString name = _device.name;
    if (name.contains(QLatin1String("Buds2"), Qt::CaseInsensitive) && !name.contains(QLatin1String("Pro"), Qt::CaseInsensitive))
        return 3;
    return 0;
}

Snapshot GalaxyBackend::snapshot()
{
    Snapshot s;
    if (!_status)
        return s;
    const Status &st = *_status;

    int buds = -1;
    const auto addBud = [&](const char *part, int level, int placement, bool charging) {
        if (placement == 0 || level <= 0 || level > 100)
            return;
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QString::fromLatin1(part)},
            {QStringLiteral("level"), level},
            {QStringLiteral("charging"), charging},
        });
        buds = buds < 0 ? level : std::min(buds, level);
    };
    addBud("left", st.batteryLeft, st.placementLeft, st.chargingLeft);
    addBud("right", st.batteryRight, st.placementRight, st.chargingRight);
    if (st.batteryCase > 0 && st.batteryCase <= 100) {
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QStringLiteral("case")},
            {QStringLiteral("level"), st.batteryCase},
            {QStringLiteral("charging"), st.chargingCase},
        });
    }
    s.battery = buds;

    s.modes = {ModeNc, ModeAmbient, ModeOff};
    s.mode = st.noiseMode == NoiseCancelling ? ModeNc : st.noiseMode == NoiseAmbient ? ModeAmbient : ModeOff;
    s.ambientLevel = st.ambientVolume + 1;

    QJsonObject o;
    o[QStringLiteral("model")] = _device.name;
    if (ambientMax() > 0) {
        o[QStringLiteral("ambient")] = QJsonObject{
            {QStringLiteral("min"), 1},
            {QStringLiteral("max"), ambientMax()},
            {QStringLiteral("focusOnVoice"), false},
        };
    }
    QJsonArray presets;
    for (int i = 0; i < int(std::size(EqualizerPresets)); i++)
        presets.append(QJsonObject{{QStringLiteral("id"), i}, {QStringLiteral("key"), QString::fromLatin1(EqualizerPresets[i])}});
    o[QStringLiteral("equalizer")] = QJsonObject{
        {QStringLiteral("preset"), st.equalizer},
        {QStringLiteral("presets"), presets},
    };
    o[QStringLiteral("touchLock")] = !(st.touchFlags & TouchUnlocked);
    o[QStringLiteral("seamless")] = st.seamless;
    s.settings = o;
    return s;
}

QDBusError GalaxyBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    const int value = mode == ModeNc ? NoiseCancelling : mode == ModeAmbient ? NoiseAmbient : mode == ModeOff ? NoiseOff : -1;
    if (value < 0)
        return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Mode must be noise-cancelling, ambient or off"));
    send(MsgNoiseControls, QByteArray(1, char(value)));
    _status->noiseMode = value;
    return {};
}

QDBusError GalaxyBackend::setAmbientLevel(int level)
{
    if (!ready())
        return notReady();
    if (ambientMax() == 0)
        return unsupported();
    const int volume = std::clamp(level, 1, ambientMax()) - 1;
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
        const int preset = std::clamp(eq.value(QStringLiteral("preset")).toInt(), 0, int(std::size(EqualizerPresets)) - 1);
        send(MsgEqualizer, QByteArray(1, char(preset)));
        st.equalizer = preset;
    }

    if (const QJsonValue v = in.value(QStringLiteral("touchLock")); v.isBool()) {
        const bool locked = v.toBool();
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
    }

    if (const QJsonValue v = in.value(QStringLiteral("seamless")); v.isBool()) {
        send(MsgSetSeamlessConnection, QByteArray(1, char(v.toBool() ? 0 : 1)));
        st.seamless = v.toBool();
    }
    return {};
}
