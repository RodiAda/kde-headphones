#include "PixelBudsBackend.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string_view>

#include "Log.h"

namespace {

constexpr uint8_t Flag = 0x7E;
constexpr uint8_t Escape = 0x7D;
constexpr uint8_t EscapeMask = 0x20;
constexpr uint8_t ControlUnnumbered = 0x03;
constexpr qsizetype MaxBuffer = 64 * 1024;

constexpr uint32_t PacketRequest = 0;
constexpr uint32_t PacketResponse = 1;
constexpr uint32_t PacketServerError = 5;
constexpr uint32_t PacketServerStream = 7;

constexpr uint32_t PeerCase = 2;
constexpr uint32_t PeerLeftBtCore = 3;
constexpr uint32_t PeerRightBtCore = 4;
constexpr uint32_t PeerLeftSensorHub = 5;
constexpr uint32_t PeerRightSensorHub = 6;
constexpr uint32_t PeerMaestroA = 10;
constexpr uint32_t PeerMaestroB = 13;

constexpr uint32_t FirstChannel = 18;
constexpr uint32_t ChannelRemotes[] = {PeerCase, PeerLeftBtCore, PeerLeftSensorHub, PeerRightBtCore, PeerRightSensorHub};
constexpr uint32_t ProbeChannels[] = {18, 19, 21, 23, 24, 26};

constexpr uint32_t hash65599(std::string_view id)
{
    uint32_t hash = uint32_t(id.size());
    uint32_t coef = 65599;
    for (const char c : id) {
        hash += coef * uint32_t(uint8_t(c));
        coef *= 65599;
    }
    return hash;
}

constexpr uint32_t ServiceMaestro = hash65599("maestro_pw.Maestro");
constexpr uint32_t MethodGetSoftwareInfo = hash65599("GetSoftwareInfo");
constexpr uint32_t MethodSubscribeRuntimeInfo = hash65599("SubscribeRuntimeInfo");
constexpr uint32_t MethodWriteSetting = hash65599("WriteSetting");
constexpr uint32_t MethodReadSetting = hash65599("ReadSetting");
constexpr uint32_t MethodSubscribeToSettingsChanges = hash65599("SubscribeToSettingsChanges");

constexpr int SettingOhd = 2;
constexpr int SettingGesture = 4;
constexpr int SettingMultipoint = 11;
constexpr int SettingAncLoop = 12;
constexpr int SettingAnc = 13;
constexpr int SettingVolumeEq = 15;
constexpr int SettingUserEq = 16;
constexpr int SettingAsymmetry = 17;
constexpr int SettingMono = 19;
constexpr int SettingSpeechDetection = 22;

constexpr int AncOff = 1;
constexpr int AncActive = 2;
constexpr int AncAware = 3;
constexpr int AncAdaptive = 4;

constexpr int BatteryCharging = 2;

constexpr float EqMin = -6.0f;
constexpr float EqMax = 6.0f;
const char *const EqKeys[] = {"eqLowBass", "eqBass", "eqMid", "eqTreble", "eqUpperTreble"};

constexpr qint64 ProbeAfterMs = 1500;
constexpr qint64 ReadTimeoutMs = 1500;
constexpr qint64 StatusTimeoutMs = 10000;

constexpr int WireVarint = 0;
constexpr int WireFixed64 = 1;
constexpr int WireBytes = 2;
constexpr int WireFixed32 = 5;

void putVarint(QByteArray &out, uint64_t v)
{
    while (v >= 0x80) {
        out.append(char((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.append(char(v));
}

void putTag(QByteArray &out, uint32_t field, int wire)
{
    putVarint(out, (uint64_t(field) << 3) | uint64_t(wire));
}

void putVarintField(QByteArray &out, uint32_t field, uint64_t v)
{
    putTag(out, field, WireVarint);
    putVarint(out, v);
}

void putBytesField(QByteArray &out, uint32_t field, const QByteArray &bytes)
{
    putTag(out, field, WireBytes);
    putVarint(out, uint64_t(bytes.size()));
    out.append(bytes);
}

void putFixed32Field(QByteArray &out, uint32_t field, uint32_t v)
{
    putTag(out, field, WireFixed32);
    for (int i = 0; i < 4; i++)
        out.append(char((v >> (8 * i)) & 0xFF));
}

struct Field {
    uint32_t number = 0;
    int wire = 0;
    uint64_t value = 0;
    QByteArray bytes;
};

class Reader
{
public:
    explicit Reader(const QByteArray &data)
        : _d(data)
    {
    }

    bool next(Field &f)
    {
        if (_pos >= _d.size())
            return false;
        uint64_t key = 0;
        if (!varint(key))
            return false;
        f.number = uint32_t(key >> 3);
        f.wire = int(key & 7);
        f.value = 0;
        f.bytes.clear();
        switch (f.wire) {
        case WireVarint:
            return varint(f.value);
        case WireFixed64:
            return fixed(8, f.value);
        case WireFixed32:
            return fixed(4, f.value);
        case WireBytes: {
            uint64_t size = 0;
            if (!varint(size) || size > uint64_t(_d.size() - _pos))
                return fail();
            f.bytes = _d.mid(_pos, qsizetype(size));
            _pos += qsizetype(size);
            return true;
        }
        default:
            return fail();
        }
    }

private:
    bool fail()
    {
        _pos = _d.size();
        return false;
    }

    bool varint(uint64_t &v)
    {
        v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (_pos >= _d.size())
                return fail();
            const uint8_t b = uint8_t(_d.at(_pos++));
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80))
                return true;
        }
        return fail();
    }

    bool fixed(int size, uint64_t &v)
    {
        if (_d.size() - _pos < size)
            return fail();
        v = 0;
        for (int i = 0; i < size; i++)
            v |= uint64_t(uint8_t(_d.at(_pos + i))) << (8 * i);
        _pos += size;
        return true;
    }

    const QByteArray &_d;
    qsizetype _pos = 0;
};

void putHdlcByte(QByteArray &out, uint8_t b)
{
    if (b == Flag || b == Escape) {
        out.append(char(Escape));
        out.append(char(b ^ EscapeMask));
    } else {
        out.append(char(b));
    }
}

int balanceFromRaw(int raw)
{
    const int value = raw >> 1;
    return (raw & 1) ? value + 1 : -value;
}

int balanceToRaw(int balance)
{
    return balance > 0 ? ((balance - 1) << 1) | 1 : (-balance) << 1;
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

}

namespace maestro {

uint32_t crc32(const QByteArray &data)
{
    uint32_t crc = 0xFFFFFFFF;
    for (const char c : data) {
        crc ^= uint8_t(c);
        for (int i = 0; i < 8; i++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
    }
    return ~crc;
}

uint32_t idHash(const char *name)
{
    return hash65599(name);
}

uint32_t channelAddress(uint32_t channel)
{
    if (channel < FirstChannel || channel >= FirstChannel + 2 * std::size(ChannelRemotes))
        return 0;
    const uint32_t index = channel - FirstChannel;
    const uint32_t local = index < std::size(ChannelRemotes) ? PeerMaestroA : PeerMaestroB;
    const uint32_t remote = ChannelRemotes[index % std::size(ChannelRemotes)];
    return (local << 6) | (remote << 10);
}

QByteArray encodeFrame(const Frame &frame)
{
    QByteArray body;
    uint32_t address = frame.address;
    while (address >> 7) {
        body.append(char((address & 0x7F) << 1));
        address >>= 7;
    }
    body.append(char(((address & 0x7F) << 1) | 1));
    body.append(char(frame.control));
    body.append(frame.data);
    const uint32_t crc = crc32(body);
    for (int i = 0; i < 4; i++)
        body.append(char((crc >> (8 * i)) & 0xFF));

    QByteArray out;
    out.append(char(Flag));
    for (const char c : std::as_const(body))
        putHdlcByte(out, uint8_t(c));
    out.append(char(Flag));
    return out;
}

std::optional<Frame> decodeFrame(const QByteArray &escaped)
{
    QByteArray buf;
    buf.reserve(escaped.size());
    for (qsizetype i = 0; i < escaped.size(); i++) {
        const uint8_t b = uint8_t(escaped.at(i));
        if (b == Escape) {
            if (++i >= escaped.size())
                return std::nullopt;
            buf.append(char(uint8_t(escaped.at(i)) ^ EscapeMask));
        } else {
            buf.append(char(b));
        }
    }
    if (buf.size() < 6)
        return std::nullopt;
    const qsizetype end = buf.size() - 4;
    uint32_t expected = 0;
    for (int i = 0; i < 4; i++)
        expected |= uint32_t(uint8_t(buf.at(end + i))) << (8 * i);
    if (crc32(buf.left(end)) != expected)
        return std::nullopt;

    Frame f;
    qsizetype n = 0;
    for (;; n++) {
        if (n >= end || n >= 5)
            return std::nullopt;
        const uint8_t b = uint8_t(buf.at(n));
        f.address |= uint32_t(b >> 1) << (7 * n);
        if (b & 1)
            break;
    }
    n++;
    if (n >= end)
        return std::nullopt;
    f.control = uint8_t(buf.at(n));
    f.data = buf.mid(n + 1, end - n - 1);
    return f;
}

QList<QByteArray> takeFrames(QByteArray &buffer)
{
    QList<QByteArray> frames;
    for (;;) {
        const qsizetype start = buffer.indexOf(char(Flag));
        if (start < 0) {
            buffer.clear();
            break;
        }
        const qsizetype end = buffer.indexOf(char(Flag), start + 1);
        if (end < 0) {
            buffer.remove(0, start);
            if (buffer.size() > MaxBuffer)
                buffer.clear();
            break;
        }
        if (end > start + 1)
            frames.append(buffer.mid(start + 1, end - start - 1));
        buffer.remove(0, end);
    }
    return frames;
}

QByteArray RpcPacket::encode() const
{
    QByteArray out;
    if (type)
        putVarintField(out, 1, type);
    if (channel)
        putVarintField(out, 2, channel);
    if (service)
        putFixed32Field(out, 3, service);
    if (method)
        putFixed32Field(out, 4, method);
    if (!payload.isEmpty())
        putBytesField(out, 5, payload);
    if (status)
        putVarintField(out, 6, status);
    if (callId)
        putVarintField(out, 7, callId);
    return out;
}

std::optional<RpcPacket> RpcPacket::decode(const QByteArray &data)
{
    RpcPacket p;
    Reader r(data);
    Field f;
    qsizetype seen = 0;
    while (r.next(f)) {
        seen++;
        switch (f.number) {
        case 1:
            p.type = uint32_t(f.value);
            break;
        case 2:
            p.channel = uint32_t(f.value);
            break;
        case 3:
            p.service = uint32_t(f.value);
            break;
        case 4:
            p.method = uint32_t(f.value);
            break;
        case 5:
            p.payload = f.bytes;
            break;
        case 6:
            p.status = uint32_t(f.value);
            break;
        case 7:
            p.callId = uint32_t(f.value);
            break;
        default:
            break;
        }
    }
    if (!seen && !data.isEmpty())
        return std::nullopt;
    return p;
}

}

QString PixelBudsBackend::modelName(const QString &deviceName)
{
    if (deviceName.contains(QLatin1String("Pro 2"), Qt::CaseInsensitive))
        return QStringLiteral("Pixel Buds Pro 2");
    return QStringLiteral("Pixel Buds Pro");
}

PixelBudsBackend::PixelBudsBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
    , _model(modelName(device.name))
{
}

QStringList PixelBudsBackend::modes() const
{
    QStringList m{ModeNc, ModeAmbient};
    if (_loopAdaptive || _anc == AncAdaptive || _model.endsWith(QLatin1String("Pro 2")))
        m << ModeAdaptive;
    m << ModeOff;
    return m;
}

bool PixelBudsBackend::start()
{
    _sinceStart.start();
    qCInfo(lcHp) << "pixel buds model:" << _model;
    return true;
}

void PixelBudsBackend::send(uint32_t channel, uint32_t method, const QByteArray &payload)
{
    maestro::RpcPacket p;
    p.type = PacketRequest;
    p.channel = channel;
    p.service = ServiceMaestro;
    p.method = method;
    p.payload = payload;
    _tx.append(maestro::encodeFrame({maestro::channelAddress(channel), ControlUnnumbered, p.encode()}));
    qCDebug(lcHp) << "pixel tx" << channel << Qt::hex << method << payload.toHex(' ');
}

void PixelBudsBackend::begin(uint32_t channel)
{
    _channel = channel;
    qCInfo(lcHp) << "maestro channel" << channel;
    send(_channel, MethodSubscribeRuntimeInfo, {});
    send(_channel, MethodSubscribeToSettingsChanges, {});
    _pendingReads = {SettingAnc, SettingAncLoop, SettingOhd, SettingGesture, SettingMultipoint, SettingSpeechDetection,
                     SettingVolumeEq, SettingUserEq, SettingAsymmetry, SettingMono};
    readNext();
}

void PixelBudsBackend::readNext()
{
    _reading = 0;
    if (_pendingReads.isEmpty()) {
        if (!_readsDone)
            qCInfo(lcHp) << "settings read";
        _readsDone = true;
        return;
    }
    _reading = _pendingReads.takeFirst();
    QByteArray msg;
    putVarintField(msg, 4, uint64_t(_reading));
    send(_channel, MethodReadSetting, msg);
    _sinceRead.start();
}

void PixelBudsBackend::writeSetting(const QByteArray &settingValue)
{
    QByteArray msg;
    putBytesField(msg, 4, settingValue);
    send(_channel, MethodWriteSetting, msg);
}

bool PixelBudsBackend::flush()
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

bool PixelBudsBackend::receive()
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

    for (const QByteArray &raw : maestro::takeFrames(_rx)) {
        const auto frame = maestro::decodeFrame(raw);
        if (!frame || frame->control != ControlUnnumbered)
            continue;
        const auto packet = maestro::RpcPacket::decode(frame->data);
        if (!packet)
            continue;
        qCDebug(lcHp) << "pixel rx type" << packet->type << "channel" << packet->channel << Qt::hex << "method"
                      << packet->method << "status" << packet->status << packet->payload.toHex(' ');
        handle(*packet);
    }
    return true;
}

bool PixelBudsBackend::tick()
{
    if (!receive())
        return false;
    if (_channel == 0 && !_probed && _sinceStart.elapsed() > ProbeAfterMs) {
        _probed = true;
        qCInfo(lcHp) << "no greeting from the earbuds, probing maestro channels";
        for (const uint32_t channel : ProbeChannels)
            send(channel, MethodGetSoftwareInfo, {});
    }
    if (_reading && _sinceRead.elapsed() > ReadTimeoutMs) {
        qCInfo(lcHp) << "no answer for setting" << _reading;
        readNext();
    }
    if (!flush())
        return false;
    if (!ready() && _sinceStart.elapsed() > StatusTimeoutMs)
        return fail(QStringLiteral("no status from the earbuds"));
    return true;
}

void PixelBudsBackend::handle(const maestro::RpcPacket &p)
{
    if (_channel == 0) {
        if (p.type == PacketServerError || p.status != 0 || maestro::channelAddress(p.channel) == 0)
            return;
        begin(p.channel);
    }
    if (p.channel != _channel || p.service != ServiceMaestro)
        return;

    if (p.method == MethodSubscribeRuntimeInfo && p.type == PacketServerStream) {
        parseRuntimeInfo(p.payload);
    } else if (p.method == MethodSubscribeToSettingsChanges && p.type == PacketServerStream) {
        parseSettingsRsp(p.payload);
    } else if (p.method == MethodReadSetting && (p.type == PacketResponse || p.type == PacketServerError)) {
        if (p.type == PacketResponse && p.status == 0)
            parseSettingsRsp(p.payload);
        else
            qCInfo(lcHp) << "setting" << _reading << "not readable, status" << p.status;
        if (_reading)
            readNext();
    } else if (p.method == MethodWriteSetting && (p.type == PacketServerError || p.status != 0)) {
        qCWarning(lcHp) << "setting write rejected, status" << p.status;
    }
}

void PixelBudsBackend::parseRuntimeInfo(const QByteArray &p)
{
    _runtimeKnown = true;
    Reader r(p);
    Field f;
    while (r.next(f)) {
        if (f.number != 6 || f.wire != WireBytes)
            continue;
        _left = _right = _case = Battery{};
        Reader parts(f.bytes);
        Field part;
        while (parts.next(part)) {
            if (part.wire != WireBytes)
                continue;
            Battery b;
            Reader fields(part.bytes);
            Field v;
            while (fields.next(v)) {
                if (v.number == 1 && v.wire == WireVarint)
                    b.level = int(int32_t(uint32_t(v.value)));
                else if (v.number == 2 && v.wire == WireVarint)
                    b.charging = v.value == BatteryCharging;
            }
            if (part.number == 1)
                _case = b;
            else if (part.number == 2)
                _left = b;
            else if (part.number == 3)
                _right = b;
        }
    }
}

void PixelBudsBackend::parseSettingsRsp(const QByteArray &p)
{
    Reader r(p);
    Field f;
    while (r.next(f)) {
        if (f.number == 4 && f.wire == WireBytes)
            parseSettingValue(f.bytes);
    }
}

void PixelBudsBackend::parseSettingValue(const QByteArray &p)
{
    Reader r(p);
    Field f;
    while (r.next(f)) {
        if (f.wire == WireVarint) {
            const bool flag = f.value != 0;
            switch (f.number) {
            case SettingOhd:
                _earDetection = flag;
                break;
            case SettingGesture:
                _touchControls = flag;
                break;
            case SettingMultipoint:
                _multipoint = flag;
                break;
            case SettingAnc:
                _anc = int(f.value);
                break;
            case SettingVolumeEq:
                _volumeEq = flag;
                break;
            case SettingAsymmetry:
                _balance = std::clamp(balanceFromRaw(int(int32_t(uint32_t(f.value)))), -100, 100);
                break;
            case SettingMono:
                _mono = flag;
                break;
            case SettingSpeechDetection:
                _speechDetection = flag;
                break;
            default:
                break;
            }
        } else if (f.wire == WireBytes && f.number == SettingAncLoop) {
            bool adaptive = false;
            Reader loop(f.bytes);
            Field v;
            while (loop.next(v)) {
                if (v.number == 4 && v.wire == WireVarint)
                    adaptive = v.value != 0;
            }
            _loopAdaptive = adaptive;
        } else if (f.wire == WireBytes && f.number == SettingUserEq) {
            std::array<float, 5> bands{};
            Reader eq(f.bytes);
            Field v;
            while (eq.next(v)) {
                if (v.wire == WireFixed32 && v.number >= 1 && v.number <= 5) {
                    const float value = std::bit_cast<float>(uint32_t(v.value));
                    bands[v.number - 1] = std::isfinite(value) ? std::clamp(value, EqMin, EqMax) : 0.0f;
                }
            }
            _eq = bands;
        }
    }
}

Snapshot PixelBudsBackend::snapshot()
{
    Snapshot s;
    if (!ready())
        return s;

    int buds = -1;
    const auto addBattery = [&](const char *part, const Battery &b, bool bud) {
        if (b.level <= 0 || b.level > 100)
            return;
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QString::fromLatin1(part)},
            {QStringLiteral("level"), b.level},
            {QStringLiteral("charging"), b.charging},
        });
        if (bud)
            buds = buds < 0 ? b.level : std::min(buds, b.level);
    };
    addBattery("left", _left, true);
    addBattery("right", _right, true);
    addBattery("case", _case, false);
    s.battery = buds;

    s.modes = modes();
    switch (_anc.value_or(AncOff)) {
    case AncActive:
        s.mode = ModeNc;
        break;
    case AncAware:
        s.mode = ModeAmbient;
        break;
    case AncAdaptive:
        s.mode = ModeAdaptive;
        break;
    default:
        s.mode = ModeOff;
        break;
    }

    QJsonObject o;
    o[QStringLiteral("model")] = _model;
    o[QStringLiteral("untested")] = true;

    QJsonArray toggles;
    const auto addToggle = [&](const char *key, const std::optional<bool> &value) {
        if (value)
            toggles.append(QJsonObject{{QStringLiteral("key"), QString::fromLatin1(key)}, {QStringLiteral("value"), *value}});
    };
    addToggle("earDetection", _earDetection);
    addToggle("touchControls", _touchControls);
    addToggle("multipoint", _multipoint);
    addToggle("conversationAwareness", _speechDetection);
    addToggle("volumeEq", _volumeEq);
    addToggle("monoAudio", _mono);
    if (!toggles.isEmpty())
        o[QStringLiteral("toggles")] = toggles;

    QJsonArray sliders;
    const auto addSlider = [&](const char *key, int value, int min, int max) {
        sliders.append(QJsonObject{
            {QStringLiteral("key"), QString::fromLatin1(key)},
            {QStringLiteral("value"), value},
            {QStringLiteral("min"), min},
            {QStringLiteral("max"), max},
        });
    };
    if (_eq) {
        for (size_t i = 0; i < _eq->size(); i++)
            addSlider(EqKeys[i], int(std::lround((*_eq)[i])), int(EqMin), int(EqMax));
    }
    if (_balance)
        addSlider("balance", *_balance, -100, 100);
    if (!sliders.isEmpty())
        o[QStringLiteral("sliders")] = sliders;

    s.settings = o;
    return s;
}

QDBusError PixelBudsBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!modes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("These earbuds do not support mode %1").arg(mode));
    const int value = mode == ModeNc ? AncActive : mode == ModeAmbient ? AncAware : mode == ModeAdaptive ? AncAdaptive : AncOff;
    QByteArray setting;
    putVarintField(setting, SettingAnc, uint64_t(value));
    writeSetting(setting);
    _anc = value;
    return {};
}

QDBusError PixelBudsBackend::setAmbientLevel(int)
{
    return unsupported();
}

QDBusError PixelBudsBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();

    struct ToggleSetting {
        const char *key;
        int id;
        std::optional<bool> *value;
    };
    const ToggleSetting toggleSettings[] = {
        {"earDetection", SettingOhd, &_earDetection},
        {"touchControls", SettingGesture, &_touchControls},
        {"multipoint", SettingMultipoint, &_multipoint},
        {"conversationAwareness", SettingSpeechDetection, &_speechDetection},
        {"volumeEq", SettingVolumeEq, &_volumeEq},
        {"monoAudio", SettingMono, &_mono},
    };
    const QJsonObject toggles = in.value(QStringLiteral("toggles")).toObject();
    for (const ToggleSetting &t : toggleSettings) {
        const QJsonValue v = toggles.value(QLatin1String(t.key));
        if (!v.isBool() || !t.value->has_value())
            continue;
        QByteArray setting;
        putVarintField(setting, uint32_t(t.id), v.toBool() ? 1 : 0);
        writeSetting(setting);
        *t.value = v.toBool();
    }

    const QJsonObject sliders = in.value(QStringLiteral("sliders")).toObject();
    if (const QJsonValue v = sliders.value(QStringLiteral("balance")); v.isDouble() && _balance) {
        const int balance = std::clamp(v.toInt(), -100, 100);
        QByteArray setting;
        putVarintField(setting, SettingAsymmetry, uint64_t(balanceToRaw(balance)));
        writeSetting(setting);
        _balance = balance;
    }
    if (_eq) {
        std::array<float, 5> bands = *_eq;
        bool changed = false;
        for (size_t i = 0; i < bands.size(); i++) {
            const QJsonValue v = sliders.value(QLatin1String(EqKeys[i]));
            if (!v.isDouble())
                continue;
            bands[i] = std::clamp(float(v.toDouble()), EqMin, EqMax);
            changed = true;
        }
        if (changed) {
            QByteArray eq;
            for (size_t i = 0; i < bands.size(); i++) {
                if (bands[i] != 0.0f)
                    putFixed32Field(eq, uint32_t(i + 1), std::bit_cast<uint32_t>(bands[i]));
            }
            QByteArray setting;
            putBytesField(setting, SettingUserEq, eq);
            writeSetting(setting);
            _eq = bands;
        }
    }
    return {};
}
