#include "AirPodsBackend.h"

#include <QStringList>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

#include "Log.h"

namespace {

constexpr uint16_t AapPsm = 0x1001;

constexpr uint16_t PacketConnectResponse = 0x0001;
constexpr uint16_t PacketMessage = 0x0004;
constexpr uint16_t ServiceControl = 0x0004;

constexpr uint16_t MsgBattery = 0x0004;
constexpr uint16_t MsgEarDetection = 0x0006;
constexpr uint16_t MsgBudRole = 0x0008;
constexpr uint16_t MsgControl = 0x0009;
constexpr uint16_t MsgNotificationFilter = 0x000F;
constexpr uint16_t MsgInformation = 0x001D;
constexpr uint16_t MsgFeatureFlags = 0x004D;

constexpr uint8_t CtlEarDetection = 0x0A;
constexpr uint8_t CtlListeningMode = 0x0D;
constexpr uint8_t CtlPersonalizedVolume = 0x26;
constexpr uint8_t CtlConversationAwareness = 0x28;
constexpr uint8_t CtlAdaptiveStrength = 0x2E;
constexpr uint8_t CtlAllowOff = 0x34;

constexpr uint8_t Enabled = 0x01;
constexpr uint8_t Disabled = 0x02;

constexpr uint8_t ListeningOff = 0x01;
constexpr uint8_t ListeningNc = 0x02;
constexpr uint8_t ListeningTransparency = 0x03;
constexpr uint8_t ListeningAdaptive = 0x04;

constexpr uint8_t ComponentHeadset = 0x01;
constexpr uint8_t ComponentRight = 0x02;
constexpr uint8_t ComponentLeft = 0x04;
constexpr uint8_t ComponentCase = 0x08;

constexpr uint8_t BatteryCharging = 0x01;
constexpr uint8_t BatteryDisconnected = 0x04;
constexpr uint8_t BatteryOptimizedCharging = 0x05;

constexpr uint8_t RoleLeftPrimary = 0x01;
constexpr uint8_t RoleRightPrimary = 0x02;

constexpr uint32_t FeatAnc = 1 << 0;
constexpr uint32_t FeatAdaptive = 1 << 1;
constexpr uint32_t FeatConversation = 1 << 2;
constexpr uint32_t FeatPersonalizedVolume = 1 << 3;
constexpr uint32_t FeatEarDetection = 1 << 4;

constexpr qint64 ConnectTimeoutMs = 15000;
constexpr qint64 AcknowledgeWaitMs = 1000;
constexpr qint64 RetryAfterMs = 4000;
constexpr qint64 StatusTimeoutMs = 15000;

constexpr int AdaptiveStrengthMax = 100;

struct ModelSpec {
    AirPodsModel model;
    const char *name;
    uint32_t features;
};

constexpr uint32_t H2Features = FeatAnc | FeatAdaptive | FeatConversation | FeatPersonalizedVolume | FeatEarDetection;

constexpr ModelSpec ModelSpecs[] = {
    {AirPodsModel::AirPods1, "AirPods", FeatEarDetection},
    {AirPodsModel::AirPods2, "AirPods 2", FeatEarDetection},
    {AirPodsModel::AirPods3, "AirPods 3", FeatEarDetection},
    {AirPodsModel::AirPods4, "AirPods 4", FeatEarDetection | FeatPersonalizedVolume},
    {AirPodsModel::AirPods4Anc, "AirPods 4 (ANC)", H2Features},
    {AirPodsModel::AirPodsPro, "AirPods Pro", FeatAnc | FeatEarDetection},
    {AirPodsModel::AirPodsPro2, "AirPods Pro 2", H2Features},
    {AirPodsModel::AirPodsPro3, "AirPods Pro 3", H2Features},
    {AirPodsModel::AirPodsMax, "AirPods Max", FeatAnc | FeatEarDetection},
};

struct ModelNumber {
    const char *number;
    AirPodsModel model;
};

constexpr ModelNumber ModelNumbers[] = {
    {"A1523", AirPodsModel::AirPods1},
    {"A1722", AirPodsModel::AirPods1},
    {"A2031", AirPodsModel::AirPods2},
    {"A2032", AirPodsModel::AirPods2},
    {"A2564", AirPodsModel::AirPods3},
    {"A2565", AirPodsModel::AirPods3},
    {"A3050", AirPodsModel::AirPods4},
    {"A3053", AirPodsModel::AirPods4},
    {"A3054", AirPodsModel::AirPods4},
    {"A3055", AirPodsModel::AirPods4Anc},
    {"A3056", AirPodsModel::AirPods4Anc},
    {"A3057", AirPodsModel::AirPods4Anc},
    {"A2083", AirPodsModel::AirPodsPro},
    {"A2084", AirPodsModel::AirPodsPro},
    {"A2698", AirPodsModel::AirPodsPro2},
    {"A2699", AirPodsModel::AirPodsPro2},
    {"A2931", AirPodsModel::AirPodsPro2},
    {"A3047", AirPodsModel::AirPodsPro2},
    {"A3048", AirPodsModel::AirPodsPro2},
    {"A3049", AirPodsModel::AirPodsPro2},
    {"A3063", AirPodsModel::AirPodsPro3},
    {"A3064", AirPodsModel::AirPodsPro3},
    {"A3065", AirPodsModel::AirPodsPro3},
    {"A2096", AirPodsModel::AirPodsMax},
    {"A3184", AirPodsModel::AirPodsMax},
};

struct NamedModel {
    const char *name;
    AirPodsModel model;
};

constexpr NamedModel ModelNames[] = {
    {"AirPods Max", AirPodsModel::AirPodsMax},
    {"AirPods Pro 3", AirPodsModel::AirPodsPro3},
    {"AirPods Pro 2", AirPodsModel::AirPodsPro2},
};

struct Toggle {
    const char *key;
    uint8_t id;
    uint32_t feature;
};

constexpr Toggle Toggles[] = {
    {"conversationAwareness", CtlConversationAwareness, FeatConversation},
    {"personalizedVolume", CtlPersonalizedVolume, FeatPersonalizedVolume},
    {"earDetection", CtlEarDetection, FeatEarDetection},
    {"offListeningMode", CtlAllowOff, FeatAnc},
};

const QString SliderAdaptiveAudio = QStringLiteral("adaptiveAudio");

uint8_t at(const QByteArray &p, int i)
{
    return i >= 0 && i < p.size() ? uint8_t(p.at(i)) : 0;
}

uint16_t le16(const QByteArray &p, int i)
{
    return uint16_t(at(p, i) | (at(p, i + 1) << 8));
}

QByteArray messageHeader(uint16_t opcode)
{
    QByteArray h;
    h.append(char(PacketMessage & 0xFF));
    h.append(char(PacketMessage >> 8));
    h.append(char(ServiceControl & 0xFF));
    h.append(char(ServiceControl >> 8));
    h.append(char(opcode & 0xFF));
    h.append(char(opcode >> 8));
    return h;
}

std::optional<bdaddr_t> parseAddress(const QString &text)
{
    const QStringList parts = text.split(QLatin1Char(':'));
    if (parts.size() != 6)
        return std::nullopt;
    bdaddr_t addr{};
    for (int i = 0; i < 6; i++) {
        bool ok = false;
        const uint value = parts.at(i).toUInt(&ok, 16);
        if (!ok || parts.at(i).size() != 2)
            return std::nullopt;
        addr.b[5 - i] = uint8_t(value);
    }
    return addr;
}

QString errnoString(int error)
{
    return QString::fromLocal8Bit(std::strerror(error));
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

}

AirPodsModel AirPodsBackend::modelFromNumber(const QString &modelNumber)
{
    const QString number = modelNumber.trimmed();
    for (const auto &m : ModelNumbers) {
        if (number.compare(QLatin1String(m.number), Qt::CaseInsensitive) == 0)
            return m.model;
    }
    return AirPodsModel::Unknown;
}

AirPodsModel AirPodsBackend::modelFromName(const QString &name)
{
    for (const auto &m : ModelNames) {
        if (name.contains(QLatin1String(m.name), Qt::CaseInsensitive))
            return m.model;
    }
    return AirPodsModel::Unknown;
}

QString AirPodsBackend::modelName(AirPodsModel model)
{
    for (const auto &s : ModelSpecs) {
        if (s.model == model)
            return QString::fromLatin1(s.name);
    }
    return QStringLiteral("AirPods");
}

std::optional<AirPodsInfo> AirPodsBackend::parseInfo(const QByteArray &packet)
{
    if (!packet.startsWith(messageHeader(MsgInformation)))
        return std::nullopt;
    const QByteArray payload = packet.mid(6);
    if (payload.size() < 6)
        return std::nullopt;
    const QList<QByteArray> fields = payload.mid(5).split('\0');
    const auto field = [&](int i) { return i < fields.size() ? QString::fromUtf8(fields.at(i)) : QString(); };
    AirPodsInfo info{field(0), field(1), field(2), field(3), field(4)};
    if (info.modelNumber.isEmpty())
        return std::nullopt;
    return info;
}

QByteArray AirPodsBackend::handshakePacket()
{
    QByteArray p(16, '\0');
    p[2] = char(ServiceControl & 0xFF);
    p[4] = 0x01;
    p[6] = 0x02;
    return p;
}

QByteArray AirPodsBackend::featuresPacket()
{
    QByteArray p = messageHeader(MsgFeatureFlags);
    p.append(char(0xD7));
    p.append(QByteArray(7, '\0'));
    return p;
}

QByteArray AirPodsBackend::notificationsPacket()
{
    QByteArray p = messageHeader(MsgNotificationFilter);
    p.append(QByteArray(4, char(0xFF)));
    return p;
}

QByteArray AirPodsBackend::controlPacket(uint8_t id, uint8_t value)
{
    QByteArray p = messageHeader(MsgControl);
    p.append(char(id));
    p.append(char(value));
    p.append(QByteArray(3, '\0'));
    return p;
}

AirPodsBackend::AirPodsBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
    , _model(modelFromName(device.name))
{
}

AirPodsBackend::~AirPodsBackend()
{
    closeSocket();
}

void AirPodsBackend::closeSocket()
{
    if (_fd >= 0)
        ::close(_fd);
    _fd = -1;
    _connected = false;
}

bool AirPodsBackend::supports(uint32_t feature) const
{
    if (_model != AirPodsModel::Unknown) {
        for (const auto &s : ModelSpecs) {
            if (s.model == _model)
                return s.features & feature;
        }
        return false;
    }
    if (feature == FeatAnc)
        return _controls.contains(CtlListeningMode);
    if (feature == FeatAdaptive)
        return _controls.contains(CtlAdaptiveStrength) || control(CtlListeningMode) == ListeningAdaptive;
    return true;
}

QStringList AirPodsBackend::modes() const
{
    QStringList m;
    if (!supports(FeatAnc))
        return m;
    m << ModeNc << ModeAmbient;
    if (supports(FeatAdaptive))
        m << ModeAdaptive;
    if (control(CtlAllowOff) != Disabled)
        m << ModeOff;
    return m;
}

std::optional<uint8_t> AirPodsBackend::control(uint8_t id) const
{
    const auto it = _controls.constFind(id);
    if (it == _controls.cend())
        return std::nullopt;
    return *it;
}

bool AirPodsBackend::controlEnabled(uint8_t id) const
{
    return control(id) == Enabled;
}

bool AirPodsBackend::offersToggle(uint8_t id) const
{
    const auto value = control(id);
    if (value != Enabled && value != Disabled)
        return false;
    for (const auto &t : Toggles) {
        if (t.id == id)
            return supports(t.feature);
    }
    return false;
}

QList<QByteArray> AirPodsBackend::takeOutgoing()
{
    return std::exchange(_tx, {});
}

void AirPodsBackend::send(const QByteArray &packet)
{
    _tx.append(packet);
    qCDebug(lcHp) << "airpods tx" << packet.toHex(' ');
}

bool AirPodsBackend::start()
{
    _sinceStart.start();
    const auto remote = parseAddress(_device.address);
    if (!remote)
        return fail(QStringLiteral("invalid Bluetooth address %1").arg(_device.address));

    _fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_L2CAP);
    if (_fd < 0)
        return fail(QStringLiteral("cannot create L2CAP socket: %1").arg(errnoString(errno)));

    if (const auto local = parseAddress(_device.adapterAddress)) {
        sockaddr_l2 bindAddr{};
        bindAddr.l2_family = AF_BLUETOOTH;
        bindAddr.l2_bdaddr = *local;
        if (::bind(_fd, reinterpret_cast<sockaddr *>(&bindAddr), sizeof(bindAddr)) < 0)
            qCInfo(lcHp) << "airpods: cannot bind to adapter" << _device.adapterAddress << errnoString(errno);
    }

    sockaddr_l2 addr{};
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = htobs(AapPsm);
    addr.l2_bdaddr = *remote;
    if (::connect(_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
        _connected = true;
        _sinceConnect.start();
        send(handshakePacket());
    } else if (errno != EINPROGRESS) {
        const int error = errno;
        closeSocket();
        return fail(QStringLiteral("cannot connect to AAP: %1").arg(errnoString(error)));
    }
    qCInfo(lcHp) << "airpods model:" << modelName(_model) << (_model == AirPodsModel::Unknown ? "(waiting for device information)" : "");
    return true;
}

bool AirPodsBackend::connectStep()
{
    if (_connected)
        return true;
    pollfd pfd{_fd, POLLOUT, 0};
    const int r = ::poll(&pfd, 1, 0);
    if (r < 0 && errno != EINTR)
        return fail(QStringLiteral("poll failed: %1").arg(errnoString(errno)));
    if (r <= 0) {
        if (_sinceStart.elapsed() > ConnectTimeoutMs)
            return fail(QStringLiteral("AAP connect timed out"));
        return true;
    }
    int error = 0;
    socklen_t len = sizeof(error);
    if (::getsockopt(_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
        error = errno;
    if (error != 0)
        return fail(QStringLiteral("cannot connect to AAP: %1").arg(errnoString(error)));
    _connected = true;
    _sinceConnect.start();
    send(handshakePacket());
    qCInfo(lcHp) << "airpods: AAP channel open";
    return true;
}

bool AirPodsBackend::receive()
{
    std::array<char, 4096> buffer;
    for (;;) {
        const ssize_t n = ::recv(_fd, buffer.data(), buffer.size(), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            if (errno == EINTR)
                continue;
            return fail(QStringLiteral("AAP connection lost: %1").arg(errnoString(errno)));
        }
        if (n == 0)
            return fail(QStringLiteral("AAP connection closed by the AirPods"));
        handlePacket(QByteArray(buffer.data(), int(n)));
    }
}

bool AirPodsBackend::flush()
{
    while (!_tx.isEmpty()) {
        const QByteArray &packet = _tx.constFirst();
        const ssize_t n = ::send(_fd, packet.constData(), size_t(packet.size()), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            if (errno == EINTR)
                continue;
            return fail(QStringLiteral("AAP send failed: %1").arg(errnoString(errno)));
        }
        _tx.removeFirst();
    }
    return true;
}

bool AirPodsBackend::tick()
{
    if (_fd < 0)
        return fail(QStringLiteral("AAP socket is not open"));
    if (!connectStep())
        return false;
    if (!_connected)
        return true;
    if (!receive())
        return false;

    if (!_registered && (_acknowledged || _sinceConnect.elapsed() > AcknowledgeWaitMs)) {
        send(featuresPacket());
        send(notificationsPacket());
        _registered = true;
    }
    if (_registered && !_batteryKnown && !_retried && _sinceConnect.elapsed() > RetryAfterMs) {
        send(handshakePacket());
        send(featuresPacket());
        send(notificationsPacket());
        _retried = true;
    }
    if (!flush())
        return false;
    if (!_batteryKnown && _sinceConnect.elapsed() > StatusTimeoutMs)
        return fail(QStringLiteral("no status from the AirPods"));
    return true;
}

void AirPodsBackend::handlePacket(const QByteArray &packet)
{
    if (packet.size() < 4)
        return;
    qCDebug(lcHp) << "airpods rx" << packet.toHex(' ');
    const uint16_t type = le16(packet, 0);
    if (type == PacketConnectResponse) {
        _acknowledged = true;
        return;
    }
    if (type != PacketMessage || le16(packet, 2) != ServiceControl || packet.size() < 6)
        return;
    const QByteArray payload = packet.mid(6);
    switch (le16(packet, 4)) {
    case MsgBattery:
        handleBattery(payload);
        break;
    case MsgEarDetection:
        handleEarDetection(payload);
        break;
    case MsgBudRole:
        if (at(payload, 0) == RoleLeftPrimary)
            _primary = Part::Left;
        else if (at(payload, 0) == RoleRightPrimary)
            _primary = Part::Right;
        break;
    case MsgControl:
        handleControl(payload);
        break;
    case MsgInformation:
        handleInfo(packet);
        break;
    default:
        break;
    }
}

void AirPodsBackend::handleBattery(const QByteArray &p)
{
    const int count = at(p, 0);
    if (count == 0 || p.size() != 1 + 5 * count)
        return;
    std::optional<Part> firstPod;
    for (int i = 0; i < count; i++) {
        const int o = 1 + 5 * i;
        Part part;
        switch (at(p, o)) {
        case ComponentHeadset:
            part = Part::Main;
            break;
        case ComponentLeft:
            part = Part::Left;
            break;
        case ComponentRight:
            part = Part::Right;
            break;
        case ComponentCase:
            part = Part::Case;
            break;
        default:
            continue;
        }
        const uint8_t level = at(p, o + 2);
        const uint8_t status = at(p, o + 3);
        Battery &b = _batteries[int(part)];
        b.present = status != BatteryDisconnected && level <= 100;
        if (b.present) {
            b.level = level;
            b.charging = status == BatteryCharging || status == BatteryOptimizedCharging;
            if (!firstPod && (part == Part::Left || part == Part::Right))
                firstPod = part;
        }
    }
    if (firstPod)
        _primary = firstPod;
    _batteryKnown = std::any_of(std::begin(_batteries), std::end(_batteries), [](const Battery &b) { return b.present; });
}

void AirPodsBackend::handleEarDetection(const QByteArray &p)
{
    if (p.size() < 2)
        return;
    const auto placement = [](uint8_t v) {
        switch (v) {
        case 0x00:
            return Placement::InEar;
        case 0x01:
            return Placement::OutOfEar;
        case 0x02:
            return Placement::InCase;
        default:
            return Placement::Unknown;
        }
    };
    _placementPrimary = placement(at(p, 0));
    _placementSecondary = placement(at(p, 1));
}

void AirPodsBackend::handleControl(const QByteArray &p)
{
    if (p.size() < 2)
        return;
    _controls[at(p, 0)] = at(p, 1);
}

void AirPodsBackend::handleInfo(const QByteArray &packet)
{
    const auto info = parseInfo(packet);
    if (!info)
        return;
    _modelNumber = info->modelNumber;
    _firmware = info->firmware;
    const AirPodsModel model = modelFromNumber(info->modelNumber);
    if (model != AirPodsModel::Unknown)
        _model = model;
    qCInfo(lcHp) << "airpods:" << info->name << info->modelNumber << "firmware" << info->firmware << "->" << modelName(_model);
}

Snapshot AirPodsBackend::snapshot()
{
    Snapshot s;
    if (!_batteryKnown)
        return s;

    const auto addBattery = [&](const char *name, Part part) {
        const Battery &b = _batteries[int(part)];
        if (!b.present)
            return;
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QString::fromLatin1(name)},
            {QStringLiteral("level"), b.level},
            {QStringLiteral("charging"), b.charging},
        });
    };
    addBattery("main", Part::Main);
    addBattery("left", Part::Left);
    addBattery("right", Part::Right);
    addBattery("case", Part::Case);

    const Battery &main = _batteries[int(Part::Main)];
    if (main.present) {
        s.battery = main.level;
    } else {
        for (const Part part : {Part::Left, Part::Right}) {
            const Battery &b = _batteries[int(part)];
            if (b.present)
                s.battery = s.battery < 0 ? b.level : std::min(s.battery, b.level);
        }
    }

    s.modes = modes();
    if (!s.modes.isEmpty()) {
        switch (control(CtlListeningMode).value_or(0)) {
        case ListeningOff:
            s.mode = ModeOff;
            break;
        case ListeningNc:
            s.mode = ModeNc;
            break;
        case ListeningTransparency:
            s.mode = ModeAmbient;
            break;
        case ListeningAdaptive:
            s.mode = ModeAdaptive;
            break;
        default:
            break;
        }
    }

    QJsonObject o;
    o[QStringLiteral("model")] = _model == AirPodsModel::Unknown && !_modelNumber.isEmpty()
        ? QStringLiteral("AirPods (%1)").arg(_modelNumber)
        : modelName(_model);
    o[QStringLiteral("untested")] = true;
    if (!_firmware.isEmpty())
        o[QStringLiteral("firmware")] = _firmware;

    QJsonArray toggles;
    for (const auto &t : Toggles) {
        if (offersToggle(t.id))
            toggles.append(QJsonObject{{QStringLiteral("key"), QString::fromLatin1(t.key)}, {QStringLiteral("value"), controlEnabled(t.id)}});
    }
    if (!toggles.isEmpty())
        o[QStringLiteral("toggles")] = toggles;

    if (supports(FeatAdaptive) && control(CtlAdaptiveStrength)) {
        o[QStringLiteral("sliders")] = QJsonArray{QJsonObject{
            {QStringLiteral("key"), SliderAdaptiveAudio},
            {QStringLiteral("value"), std::min(int(*control(CtlAdaptiveStrength)), AdaptiveStrengthMax)},
            {QStringLiteral("min"), 0},
            {QStringLiteral("max"), AdaptiveStrengthMax},
        }};
    }

    if (_primary && !_batteries[int(Part::Main)].present && _placementPrimary != Placement::Unknown) {
        const bool leftPrimary = *_primary == Part::Left;
        o[QStringLiteral("wearing")] = QJsonObject{
            {QStringLiteral("left"), (leftPrimary ? _placementPrimary : _placementSecondary) == Placement::InEar},
            {QStringLiteral("right"), (leftPrimary ? _placementSecondary : _placementPrimary) == Placement::InEar},
        };
    }
    s.settings = o;
    return s;
}

QDBusError AirPodsBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!modes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("These AirPods do not support mode %1").arg(mode));
    const uint8_t value = mode == ModeNc ? ListeningNc
        : mode == ModeAmbient            ? ListeningTransparency
        : mode == ModeAdaptive           ? ListeningAdaptive
                                         : ListeningOff;
    send(controlPacket(CtlListeningMode, value));
    _controls[CtlListeningMode] = value;
    return {};
}

QDBusError AirPodsBackend::setAmbientLevel(int)
{
    return unsupported();
}

QDBusError AirPodsBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();

    const QJsonObject toggles = in.value(QStringLiteral("toggles")).toObject();
    for (const auto &t : Toggles) {
        const QJsonValue v = toggles.value(QLatin1String(t.key));
        if (!v.isBool() || !offersToggle(t.id))
            continue;
        const uint8_t value = v.toBool() ? Enabled : Disabled;
        send(controlPacket(t.id, value));
        _controls[t.id] = value;
    }

    const QJsonObject sliders = in.value(QStringLiteral("sliders")).toObject();
    if (const QJsonValue v = sliders.value(SliderAdaptiveAudio); v.isDouble() && supports(FeatAdaptive) && control(CtlAdaptiveStrength)) {
        const uint8_t value = uint8_t(std::clamp(v.toInt(), 0, AdaptiveStrengthMax));
        send(controlPacket(CtlAdaptiveStrength, value));
        _controls[CtlAdaptiveStrength] = value;
    }
    return {};
}
