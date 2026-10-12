#include "JblBackend.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Log.h"

namespace {

constexpr uint8_t Header = 0xAA;

constexpr uint8_t CmdAck = 0x00;
constexpr uint8_t CmdNameGet = 0x11;
constexpr uint8_t CmdNameRet = 0x12;
constexpr uint8_t CmdStatusGet = 0x21;
constexpr uint8_t CmdStatusRet = 0x22;
constexpr uint8_t CmdBattery = 0x25;
constexpr uint8_t CmdAncSwitch = 0x31;
constexpr uint8_t CmdAutoOff = 0x33;
constexpr uint8_t CmdAutoPlay = 0x35;
constexpr uint8_t CmdEqPreset = 0x40;
constexpr uint8_t CmdNoise = 0x91;
constexpr uint8_t CmdSpatial = 0x9D;
constexpr uint8_t CmdEqCurve = 0xA2;

constexpr uint8_t NoiseSet = 0x10;
constexpr uint8_t NoiseGet = 0x11;
constexpr uint8_t NoiseRet = 0x12;
constexpr uint8_t AncTuningSet = 0x20;
constexpr uint8_t AncTuningGet = 0x21;
constexpr uint8_t AncTuningRet = 0x22;

constexpr uint8_t SlotAnc = 0x01;
constexpr uint8_t SlotAmbient = 0x02;
constexpr uint8_t SlotTalkThru = 0x03;
constexpr uint8_t TuningManualLevel = 0x04;

constexpr uint8_t FieldAnc = 0x31;
constexpr uint8_t FieldPassThrough = 0x32;
constexpr uint8_t FieldAutoOff = 0x33;
constexpr uint8_t FieldEqPreset = 0x34;
constexpr uint8_t FieldAutoPlay = 0x38;
constexpr uint8_t FieldInEar = 0x41;

constexpr uint8_t PassOff = 0x00;
constexpr uint8_t PassTalkThru = 0x01;
constexpr uint8_t PassAmbient = 0x02;

constexpr uint8_t SpatialStatus = 0x02;

constexpr int AncLevelMin = 1;
constexpr int AncLevelMax = 7;
constexpr int BatteryAbsent = 100;

constexpr qint64 SetupTimeoutMs = 15000;
constexpr qint64 StatusTimeoutMs = 15000;
constexpr qint64 BatteryRetryMs = 3000;
constexpr qint64 RefreshDelayMs = 600;
constexpr qint64 LookupIntervalMs = 1000;
constexpr qint64 ConnectRetryMs = 4000;
constexpr qint64 StaleRxMs = 1000;
constexpr int DbusTimeoutMs = 20000;
constexpr int MaxRxBuffer = 2048;

const QString BluezService = QStringLiteral("org.bluez");
const QString AdapterIface = QStringLiteral("org.bluez.Adapter1");
const QString DeviceIface = QStringLiteral("org.bluez.Device1");
const QString LeBearerIface = QStringLiteral("org.bluez.Bearer.LE1");
const QString ServiceIface = QStringLiteral("org.bluez.GattService1");
const QString CharIface = QStringLiteral("org.bluez.GattCharacteristic1");
const QString PropertiesIface = QStringLiteral("org.freedesktop.DBus.Properties");
const QString ObjectManagerIface = QStringLiteral("org.freedesktop.DBus.ObjectManager");

using InterfaceProps = QMap<QString, QVariantMap>;
using BluezObjects = QMap<QDBusObjectPath, InterfaceProps>;

struct Preset {
    int id;
    const char *key;
};

constexpr Preset LegacyPresets[] = {
    {0x00, "off"},
    {0x01, "jazz"},
    {0x02, "vocal"},
    {0x03, "bass"},
    {0x05, "rock"},
    {0x06, "piano"},
    {0x07, "club"},
    {0x08, "studio"},
};

constexpr int AutoOffChoices[] = {30, 60, 120};

const char *const EarbudMarkers[] = {"PRO", "BEAM", "FLEX", "BUDS", "WAVE", "FREE", "TWS", "VIBE", "PEAK", "REFLECT", "ENDURANCE"};

uint8_t at(const QByteArray &data, int index)
{
    return index >= 0 && index < data.size() ? uint8_t(data[index]) : 0;
}

QJsonObject control(const QString &key, const QJsonValue &value)
{
    return QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("value"), value}};
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("The headphones have not reported their state yet"));
}

QString errnoString(int error)
{
    return QString::fromLocal8Bit(std::strerror(error));
}

QString autoOffKey(int minutes)
{
    return QStringLiteral("%1-minutes").arg(minutes);
}

int takeFd(const QDBusUnixFileDescriptor &descriptor)
{
    if (!descriptor.isValid())
        return -1;
    const int fd = ::fcntl(descriptor.fileDescriptor(), F_DUPFD_CLOEXEC, 0);
    if (fd >= 0)
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

bool sameName(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    return a.compare(b, Qt::CaseInsensitive) == 0 || a.startsWith(b, Qt::CaseInsensitive) || b.startsWith(a, Qt::CaseInsensitive);
}

}

JblGatt::JblGatt(const DeviceInfo &device)
    : _device(device)
{
}

JblGatt::~JblGatt()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (_notifyFd >= 0)
        ::close(_notifyFd);
    if (_writeFd >= 0)
        ::close(_writeFd);
    if (_startedNotify)
        bus.asyncCall(QDBusMessage::createMethodCall(BluezService, _notifyPath, CharIface, QStringLiteral("StopNotify")));
    if ((_discovering || _discoveryRequested) && !_adapterPath.isEmpty())
        bus.asyncCall(QDBusMessage::createMethodCall(BluezService, _adapterPath, AdapterIface, QStringLiteral("StopDiscovery")));
    if (_connectedVia == ConnectedVia::LeBearer)
        bus.asyncCall(QDBusMessage::createMethodCall(BluezService, _connectedPath, LeBearerIface, QStringLiteral("Disconnect")));
    else if (_connectedVia == ConnectedVia::Device)
        bus.asyncCall(QDBusMessage::createMethodCall(BluezService, _connectedPath, DeviceIface, QStringLiteral("Disconnect")));
}

void JblGatt::setError(const QString &error)
{
    if (_error.isEmpty()) {
        _error = error;
        qCInfo(lcHp) << "jbl:" << error;
    }
}

void JblGatt::poll()
{
    if (failed())
        return;
    if (_stage == Stage::Lookup && !_lookupInFlight && (!_sinceLookup.isValid() || _sinceLookup.elapsed() >= LookupIntervalMs))
        lookup();
    if (_stage == Stage::Ready) {
        receive();
        flush();
    }
}

void JblGatt::lookup()
{
    _lookupInFlight = true;
    _sinceLookup.start();
    const auto call = QDBusConnection::systemBus().asyncCall(
        QDBusMessage::createMethodCall(BluezService, QStringLiteral("/"), ObjectManagerIface, QStringLiteral("GetManagedObjects")), DbusTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        _lookupInFlight = false;
        if (w->isError()) {
            setError(QStringLiteral("cannot list BlueZ objects: %1").arg(w->error().message()));
            return;
        }
        if (_stage == Stage::Lookup)
            evaluate(w->reply());
    });
}

void JblGatt::evaluate(const QDBusMessage &reply)
{
    if (reply.arguments().isEmpty())
        return;
    BluezObjects objects;
    reply.arguments().constFirst().value<QDBusArgument>() >> objects;

    QString classicAdapter;
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        const auto adapter = it->constFind(AdapterIface);
        if (adapter != it->cend() && adapter->value(QStringLiteral("Address")).toString().compare(_device.adapterAddress, Qt::CaseInsensitive) == 0)
            _adapterPath = it.key().path();
        const auto dev = it->constFind(DeviceIface);
        if (dev != it->cend() && dev->value(QStringLiteral("Address")).toString().compare(_device.address, Qt::CaseInsensitive) == 0)
            classicAdapter = dev->value(QStringLiteral("Adapter")).value<QDBusObjectPath>().path();
    }
    if (_adapterPath.isEmpty())
        _adapterPath = classicAdapter;

    const auto rank = [&](const QString &path) -> int {
        const auto obj = objects.constFind(QDBusObjectPath(path));
        if (obj == objects.cend())
            return -1;
        const auto dev = obj->constFind(DeviceIface);
        if (dev == obj->cend())
            return -1;
        if (!_adapterPath.isEmpty() && dev->value(QStringLiteral("Adapter")).value<QDBusObjectPath>().path() != _adapterPath)
            return -1;
        if (dev->value(QStringLiteral("Address")).toString().compare(_device.address, Qt::CaseInsensitive) == 0)
            return 0;
        if (sameName(dev->value(QStringLiteral("Name")).toString(), _device.name) || sameName(dev->value(QStringLiteral("Alias")).toString(), _device.name))
            return 1;
        return -1;
    };

    QString bestService;
    int bestRank = 2;
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        const auto service = it->constFind(ServiceIface);
        if (service == it->cend() || service->value(QStringLiteral("UUID")).toString().compare(JblServiceUuid, Qt::CaseInsensitive) != 0)
            continue;
        const QString devicePath = service->value(QStringLiteral("Device")).value<QDBusObjectPath>().path();
        const int r = rank(devicePath);
        if (r >= 0 && r < bestRank) {
            bestRank = r;
            bestService = it.key().path();
            _devicePath = devicePath;
        }
    }

    if (!bestService.isEmpty()) {
        const InterfaceProps &owner = objects.value(QDBusObjectPath(_devicePath));
        const auto le = owner.constFind(LeBearerIface);
        const bool linked = le != owner.cend() ? le->value(QStringLiteral("Connected")).toBool()
                                               : owner.value(DeviceIface).value(QStringLiteral("Connected")).toBool();
        if (!linked) {
            requestConnect(_devicePath, le != owner.cend());
            return;
        }
        if (!owner.value(DeviceIface).value(QStringLiteral("ServicesResolved")).toBool())
            return;
        QString notifyPath, writePath;
        QVariantMap notifyProps, writeProps;
        int notifyScore = -1, writeScore = -1;
        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
            const auto chr = it->constFind(CharIface);
            if (chr == it->cend() || chr->value(QStringLiteral("Service")).value<QDBusObjectPath>().path() != bestService)
                continue;
            const QStringList flags = chr->value(QStringLiteral("Flags")).toStringList();
            const bool canNotify = flags.contains(QStringLiteral("notify"));
            const bool canWrite = flags.contains(QStringLiteral("write")) || flags.contains(QStringLiteral("write-without-response"));
            if (canNotify && (canWrite ? 0 : 1) > notifyScore) {
                notifyScore = canWrite ? 0 : 1;
                notifyPath = it.key().path();
                notifyProps = *chr;
            }
            if (canWrite && (canNotify ? 0 : 1) > writeScore) {
                writeScore = canNotify ? 0 : 1;
                writePath = it.key().path();
                writeProps = *chr;
            }
        }
        if (notifyPath.isEmpty() || writePath.isEmpty())
            return;
        _servicePath = bestService;
        _notifyPath = notifyPath;
        _writePath = writePath;
        qCInfo(lcHp) << "jbl: control service at" << _servicePath << "notify" << _notifyPath << "write" << _writePath;
        acquire(notifyProps, writeProps);
        return;
    }

    QString candidate;
    int candidateRank = 2;
    bool candidateHasUuid = false;
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        const auto dev = it->constFind(DeviceIface);
        if (dev == it->cend())
            continue;
        const int r = rank(it.key().path());
        if (r < 0)
            continue;
        const bool hasUuid = dev->value(QStringLiteral("UUIDs")).toStringList().contains(JblServiceUuid, Qt::CaseInsensitive);
        if (candidate.isEmpty() || (hasUuid && !candidateHasUuid) || (hasUuid == candidateHasUuid && r < candidateRank)) {
            candidate = it.key().path();
            candidateRank = r;
            candidateHasUuid = hasUuid;
        }
    }

    startDiscovery();
    if (candidate.isEmpty())
        return;
    const InterfaceProps &props = objects.value(QDBusObjectPath(candidate));
    const auto le = props.constFind(LeBearerIface);
    if (le != props.cend()) {
        if (!le->value(QStringLiteral("Connected")).toBool())
            requestConnect(candidate, true);
        return;
    }
    const QVariantMap dev = props.value(DeviceIface);
    if (!dev.value(QStringLiteral("Connected")).toBool())
        requestConnect(candidate, false);
}

void JblGatt::startDiscovery()
{
    if (_discoveryRequested || _adapterPath.isEmpty())
        return;
    _discoveryRequested = true;
    QDBusConnection bus = QDBusConnection::systemBus();
    QDBusMessage filter = QDBusMessage::createMethodCall(BluezService, _adapterPath, AdapterIface, QStringLiteral("SetDiscoveryFilter"));
    filter << QVariantMap{{QStringLiteral("Transport"), QStringLiteral("le")}};
    bus.asyncCall(filter, DbusTimeoutMs);
    const auto call = bus.asyncCall(QDBusMessage::createMethodCall(BluezService, _adapterPath, AdapterIface, QStringLiteral("StartDiscovery")), DbusTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError()) {
            qCInfo(lcHp) << "jbl: cannot start LE discovery:" << w->error().message();
            return;
        }
        _discovering = true;
        qCInfo(lcHp) << "jbl: scanning for the LE control channel";
        if (_stage == Stage::Ready)
            stopDiscovery();
    });
}

void JblGatt::stopDiscovery()
{
    if (!_discovering)
        return;
    _discovering = false;
    QDBusConnection::systemBus().asyncCall(QDBusMessage::createMethodCall(BluezService, _adapterPath, AdapterIface, QStringLiteral("StopDiscovery")));
}

void JblGatt::requestConnect(const QString &devicePath, bool leBearer)
{
    if (_connectInFlight || (_sinceConnect.isValid() && _sinceConnect.elapsed() < ConnectRetryMs))
        return;
    _connectInFlight = true;
    _sinceConnect.start();
    qCInfo(lcHp) << "jbl: connecting LE link of" << devicePath;
    const auto call = QDBusConnection::systemBus().asyncCall(
        QDBusMessage::createMethodCall(BluezService, devicePath, leBearer ? LeBearerIface : DeviceIface, QStringLiteral("Connect")), DbusTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, devicePath, leBearer](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        _connectInFlight = false;
        _sinceConnect.start();
        if (w->isError()) {
            qCInfo(lcHp) << "jbl: LE connect failed:" << w->error().message();
            return;
        }
        _connectedVia = leBearer ? ConnectedVia::LeBearer : ConnectedVia::Device;
        _connectedPath = devicePath;
    });
}

void JblGatt::acquire(const QVariantMap &notifyProps, const QVariantMap &writeProps)
{
    _stage = Stage::Acquiring;
    QDBusConnection bus = QDBusConnection::systemBus();

    const QStringList writeFlags = writeProps.value(QStringLiteral("Flags")).toStringList();
    _writeType = writeFlags.contains(QStringLiteral("write-without-response")) ? QStringLiteral("command") : QStringLiteral("request");

    if (notifyProps.value(QStringLiteral("NotifyAcquired")).toBool() || notifyProps.value(QStringLiteral("Notifying")).toBool()) {
        fallbackNotify();
    } else {
        QDBusMessage m = QDBusMessage::createMethodCall(BluezService, _notifyPath, CharIface, QStringLiteral("AcquireNotify"));
        m << QVariantMap{};
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(m, DbusTimeoutMs), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            QDBusPendingReply<QDBusUnixFileDescriptor, quint16> reply = *w;
            if (!reply.isError())
                _notifyFd = takeFd(reply.argumentAt<0>());
            if (_notifyFd < 0) {
                qCInfo(lcHp) << "jbl: AcquireNotify unavailable, falling back to StartNotify:" << reply.error().message();
                fallbackNotify();
                return;
            }
            _notifyReady = true;
            checkReady();
        });
    }

    if (_writeType == QStringLiteral("command")) {
        QDBusMessage m = QDBusMessage::createMethodCall(BluezService, _writePath, CharIface, QStringLiteral("AcquireWrite"));
        m << QVariantMap{};
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(m, DbusTimeoutMs), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            QDBusPendingReply<QDBusUnixFileDescriptor, quint16> reply = *w;
            if (!reply.isError())
                _writeFd = takeFd(reply.argumentAt<0>());
            if (_writeFd < 0)
                qCInfo(lcHp) << "jbl: AcquireWrite unavailable, using WriteValue:" << reply.error().message();
            _writeReady = true;
            checkReady();
        });
    } else {
        _writeReady = true;
    }
}

void JblGatt::fallbackNotify()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    bus.connect(BluezService, _notifyPath, PropertiesIface, QStringLiteral("PropertiesChanged"), this,
                SLOT(onCharacteristicChanged(QString, QVariantMap, QStringList)));
    auto *watcher = new QDBusPendingCallWatcher(
        bus.asyncCall(QDBusMessage::createMethodCall(BluezService, _notifyPath, CharIface, QStringLiteral("StartNotify")), DbusTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError()) {
            setError(QStringLiteral("cannot subscribe to JBL notifications: %1").arg(w->error().message()));
            return;
        }
        _startedNotify = true;
        _notifyReady = true;
        checkReady();
    });
}

void JblGatt::checkReady()
{
    if (_stage != Stage::Acquiring || !_notifyReady || !_writeReady)
        return;
    _stage = Stage::Ready;
    stopDiscovery();
    watchLink();
    qCInfo(lcHp) << "jbl: LE control channel open" << (_notifyFd >= 0 ? "(acquired notify)" : "(StartNotify)")
                 << (_writeFd >= 0 ? "(acquired write)" : "(WriteValue)");
}

void JblGatt::watchLink()
{
    if (_watching)
        return;
    _watching = true;
    QDBusConnection bus = QDBusConnection::systemBus();
    bus.connect(BluezService, _devicePath, PropertiesIface, QStringLiteral("PropertiesChanged"), this,
                SLOT(onDeviceChanged(QString, QVariantMap, QStringList)));
    bus.connect(BluezService, QStringLiteral("/"), ObjectManagerIface, QStringLiteral("InterfacesRemoved"), this,
                SLOT(onInterfacesRemoved(QDBusObjectPath, QStringList)));
}

void JblGatt::onCharacteristicChanged(const QString &interface, const QVariantMap &changed, const QStringList &)
{
    if (interface != CharIface)
        return;
    const auto value = changed.constFind(QStringLiteral("Value"));
    if (value != changed.cend())
        _incoming.append(value->toByteArray());
}

void JblGatt::onDeviceChanged(const QString &interface, const QVariantMap &changed, const QStringList &)
{
    if (interface != DeviceIface && interface != LeBearerIface)
        return;
    const auto connected = changed.constFind(QStringLiteral("Connected"));
    if (connected != changed.cend() && !connected->toBool())
        setError(QStringLiteral("Bluetooth LE link to the JBL headphones lost"));
}

void JblGatt::onInterfacesRemoved(const QDBusObjectPath &path, const QStringList &)
{
    const QString p = path.path();
    if (p == _notifyPath || p == _writePath || p == _servicePath || p == _devicePath)
        setError(QStringLiteral("JBL control service disappeared"));
}

void JblGatt::receive()
{
    if (_notifyFd < 0)
        return;
    std::array<char, 1024> buffer;
    for (;;) {
        const ssize_t n = ::recv(_notifyFd, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            setError(QStringLiteral("JBL notification channel failed: %1").arg(errnoString(errno)));
            return;
        }
        if (n == 0) {
            setError(QStringLiteral("JBL notification channel closed"));
            return;
        }
        _incoming.append(QByteArray(buffer.data(), int(n)));
    }
}

void JblGatt::write(const QByteArray &data)
{
    _outgoing.append(data);
}

void JblGatt::flush()
{
    if (_stage != Stage::Ready || failed())
        return;
    if (_writeFd >= 0) {
        while (!_outgoing.isEmpty()) {
            const QByteArray &packet = _outgoing.constFirst();
            const ssize_t n = ::send(_writeFd, packet.constData(), size_t(packet.size()), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                if (errno == EINTR)
                    continue;
                setError(QStringLiteral("JBL write channel failed: %1").arg(errnoString(errno)));
                return;
            }
            _outgoing.removeFirst();
        }
        return;
    }
    if (_writeInFlight || _outgoing.isEmpty())
        return;
    QDBusMessage m = QDBusMessage::createMethodCall(BluezService, _writePath, CharIface, QStringLiteral("WriteValue"));
    m << _outgoing.takeFirst() << QVariantMap{{QStringLiteral("type"), _writeType}};
    _writeInFlight = true;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m, DbusTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        _writeInFlight = false;
        if (w->isError())
            qCInfo(lcHp) << "jbl: write failed:" << w->error().message();
        flush();
    });
}

QList<QByteArray> JblGatt::takeIncoming()
{
    return std::exchange(_incoming, {});
}

JblBackend::JblBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
    , _scheme(schemeFromName(device.name))
    , _earbuds(earbudsFromName(device.name))
{
}

JblBackend::~JblBackend() = default;

QByteArray JblBackend::frame(uint8_t command, const QByteArray &payload)
{
    QByteArray f;
    f.append(char(Header));
    f.append(char(command));
    f.append(char(payload.size()));
    f.append(payload);
    return f;
}

QList<JblFrame> JblBackend::splitFrames(QByteArray &buffer)
{
    QList<JblFrame> frames;
    for (;;) {
        const qsizetype start = buffer.indexOf(char(Header));
        if (start < 0) {
            buffer.clear();
            break;
        }
        if (start > 0)
            buffer.remove(0, start);
        if (buffer.size() < 3)
            break;
        if (at(buffer, 1) == CmdEqCurve) {
            buffer.clear();
            break;
        }
        const int length = at(buffer, 2);
        if (buffer.size() < 3 + length)
            break;
        frames.append(JblFrame{at(buffer, 1), buffer.mid(3, length)});
        buffer.remove(0, 3 + length);
    }
    return frames;
}

QByteArray JblBackend::nameRequest()
{
    return frame(CmdNameGet);
}

QByteArray JblBackend::batteryRequest()
{
    return frame(CmdBattery);
}

QByteArray JblBackend::batteryRequestLevels()
{
    return frame(CmdBattery, QByteArray(1, char(0x01)));
}

QByteArray JblBackend::batteryRequestLegacy()
{
    return frame(CmdBattery, QByteArray(1, char(0x00)));
}

QByteArray JblBackend::statusRequest(uint8_t field)
{
    return frame(CmdStatusGet, QByteArray(1, char(field)));
}

QByteArray JblBackend::noiseRequest()
{
    return frame(CmdNoise, QByteArray(1, char(NoiseGet)));
}

QByteArray JblBackend::ancLevelRequest()
{
    return frame(CmdNoise, QByteArray(1, char(AncTuningGet)));
}

QByteArray JblBackend::spatialRequest()
{
    return frame(CmdSpatial, QByteArray(1, char(0x01)));
}

QByteArray JblBackend::noiseSlotsCommand(bool anc, bool ambient, bool talkThru)
{
    const char payload[] = {char(NoiseSet), char(SlotAnc), char(anc), char(SlotAmbient), char(ambient), char(SlotTalkThru), char(talkThru)};
    return frame(CmdNoise, QByteArray(payload, sizeof(payload)));
}

QByteArray JblBackend::ancSwitchCommand(bool on)
{
    return frame(CmdAncSwitch, QByteArray(1, char(on ? 1 : 0)));
}

QByteArray JblBackend::ancLevelCommand(int level)
{
    const char payload[] = {char(AncTuningSet), char(TuningManualLevel), char(std::clamp(level, AncLevelMin, AncLevelMax))};
    return frame(CmdNoise, QByteArray(payload, sizeof(payload)));
}

QByteArray JblBackend::autoOffCommand(bool on, int minutes)
{
    const char payload[] = {char(on ? 1 : 0), char(std::clamp(minutes, 1, 255)), 0};
    return frame(CmdAutoOff, QByteArray(payload, sizeof(payload)));
}

QByteArray JblBackend::autoPlayCommand(bool on)
{
    return frame(CmdAutoPlay, QByteArray(1, char(on ? 1 : 0)));
}

QByteArray JblBackend::eqPresetCommand(int preset)
{
    return frame(CmdEqPreset, QByteArray(1, char(preset)));
}

QByteArray JblBackend::spatialCommand(bool on, int mode)
{
    const char payload[] = {0, char(on ? 1 : 0), char(mode)};
    return frame(CmdSpatial, QByteArray(payload, sizeof(payload)));
}

JblBackend::AncScheme JblBackend::schemeFromName(const QString &name)
{
    return name.contains(QLatin1String("LIVE PRO 2"), Qt::CaseInsensitive) ? AncScheme::Status : AncScheme::Slots;
}

bool JblBackend::earbudsFromName(const QString &name)
{
    return std::any_of(std::begin(EarbudMarkers), std::end(EarbudMarkers),
                       [&](const char *marker) { return name.contains(QLatin1String(marker), Qt::CaseInsensitive); });
}

bool JblBackend::start()
{
    _sinceStart.start();
    _gatt = std::make_unique<JblGatt>(_device);
    qCInfo(lcHp) << "jbl:" << _device.name << (_earbuds ? "earbuds" : "headphones")
                 << (_scheme == AncScheme::Status ? "(status-field noise control)" : "(slot noise control)");
    return true;
}

bool JblBackend::tick()
{
    if (!_gatt)
        return fail(QStringLiteral("JBL backend was not started"));
    _gatt->poll();
    if (_gatt->failed())
        return fail(_gatt->error());
    if (!_gatt->ready()) {
        if (_sinceStart.elapsed() > SetupTimeoutMs)
            return fail(QStringLiteral("JBL control service not reachable over Bluetooth LE"));
        return true;
    }
    if (!_linkUp) {
        _linkUp = true;
        _sinceLink.start();
        requestState();
    }
    for (const QByteArray &data : _gatt->takeIncoming())
        handleData(data);
    if (!_batteryKnown && _batteryRetries < 2 && _sinceLink.elapsed() > BatteryRetryMs * (_batteryRetries + 1)) {
        send(_batteryRetries == 0 ? batteryRequestLevels() : batteryRequestLegacy());
        _batteryRetries++;
    }
    if (_refreshTimer.isValid() && _refreshTimer.elapsed() > RefreshDelayMs) {
        _refreshTimer.invalidate();
        flushRefresh();
    }
    for (const QByteArray &packet : takeOutgoing())
        _gatt->write(packet);
    _gatt->flush();
    if (_gatt->failed())
        return fail(_gatt->error());
    if (!_batteryKnown && _sinceLink.elapsed() > StatusTimeoutMs)
        return fail(QStringLiteral("no battery status from the JBL headphones"));
    return true;
}

void JblBackend::send(const QByteArray &packet)
{
    _tx.append(packet);
    qCDebug(lcHp) << "jbl tx" << packet.toHex(' ');
}

QList<QByteArray> JblBackend::takeOutgoing()
{
    return std::exchange(_tx, {});
}

void JblBackend::requestState()
{
    send(nameRequest());
    send(batteryRequest());
    refresh();
    send(ancLevelRequest());
    send(statusRequest(FieldAutoOff));
    send(statusRequest(FieldAutoPlay));
    send(spatialRequest());
    if (equalizerSupported())
        send(statusRequest(FieldEqPreset));
    if (_earbuds)
        send(statusRequest(FieldInEar));
}

void JblBackend::refresh()
{
    for (const QByteArray &request : noiseRequests())
        send(request);
}

QList<QByteArray> JblBackend::noiseRequests() const
{
    QList<QByteArray> requests{statusRequest(FieldAnc), statusRequest(FieldPassThrough)};
    if (_scheme == AncScheme::Slots)
        requests.append(noiseRequest());
    return requests;
}

void JblBackend::scheduleRefresh(const QList<QByteArray> &requests)
{
    for (const QByteArray &request : requests) {
        if (!_pendingRefresh.contains(request))
            _pendingRefresh.append(request);
    }
    _refreshTimer.start();
}

void JblBackend::flushRefresh()
{
    for (const QByteArray &request : std::exchange(_pendingRefresh, {}))
        send(request);
}

void JblBackend::handleData(const QByteArray &data)
{
    qCDebug(lcHp) << "jbl rx" << data.toHex(' ');
    if (!_rx.isEmpty() && _sinceRx.isValid() && _sinceRx.elapsed() > StaleRxMs)
        _rx.clear();
    _rx.append(data);
    _sinceRx.start();
    for (const JblFrame &f : splitFrames(_rx))
        handleFrame(f);
    if (_rx.size() > MaxRxBuffer)
        _rx.clear();
}

void JblBackend::handleFrame(const JblFrame &f)
{
    switch (f.command) {
    case CmdBattery:
        handleBattery(f.payload);
        break;
    case CmdNoise:
        handleNoise(f.payload);
        break;
    case CmdStatusRet:
        handleStatus(f.payload);
        break;
    case CmdAck:
        handleAck(f.payload);
        break;
    case CmdNameRet: {
        const qsizetype end = f.payload.indexOf('\0');
        const QString name = QString::fromUtf8(end < 0 ? f.payload : f.payload.left(end)).trimmed();
        if (!name.isEmpty())
            _reportedName = name;
        break;
    }
    case CmdSpatial:
        if (at(f.payload, 0) == SpatialStatus && f.payload.size() >= 3) {
            _spatial = at(f.payload, 1) != 0;
            if (at(f.payload, 2) != 0)
                _spatialMode = at(f.payload, 2);
        }
        break;
    default:
        break;
    }
}

void JblBackend::handleBattery(const QByteArray &p)
{
    if (at(p, 0) != 0x01 || p.size() < 5)
        return;
    const auto decode = [](uint8_t value, uint8_t flag) -> Battery {
        Battery b;
        const int level = value & 0x7F;
        if (value == 0xFF || level > BatteryAbsent)
            return b;
        b.level = level;
        b.charging = (value & 0x80) != 0 || flag == 0x01;
        return b;
    };
    const Battery first = decode(at(p, 3), at(p, 1));
    const Battery second = decode(at(p, 4), at(p, 2));
    const Battery box = p.size() >= 7 ? decode(at(p, 6), at(p, 5)) : Battery{};
    if (!_earbuds && box.level >= 0)
        _earbuds = true;
    if (_earbuds) {
        _left = first;
        _right = second;
        _case = box;
        _main = Battery{};
        _batteryKnown = _left.level >= 0 || _right.level >= 0 || _case.level >= 0;
    } else {
        _main = second.level >= 0 ? second : first;
        _batteryKnown = _main.level >= 0;
    }
}

void JblBackend::handleNoise(const QByteArray &p)
{
    const uint8_t sub = at(p, 0);
    if (sub == NoiseRet || sub == NoiseSet) {
        if (_scheme != AncScheme::Slots)
            return;
        std::optional<bool> anc, ambient, talkThru;
        for (int i = 1; i + 1 < p.size(); i += 2) {
            const bool on = at(p, i + 1) != 0;
            switch (at(p, i)) {
            case SlotAnc:
                anc = on;
                break;
            case SlotAmbient:
                ambient = on;
                break;
            case SlotTalkThru:
                talkThru = on;
                break;
            default:
                break;
            }
        }
        if (!anc && !ambient && !talkThru)
            return;
        _ancOn = anc.value_or(false);
        _passMode = talkThru.value_or(false) ? PassTalkThru : ambient.value_or(false) ? PassAmbient : PassOff;
        updateNoise();
        return;
    }
    if (sub == AncTuningRet) {
        const int pairs = (int(p.size()) - 1) / 2;
        for (int i = 0; i < pairs; i++) {
            const int o = 1 + 2 * i;
            if (at(p, o) == TuningManualLevel) {
                const int level = at(p, o + 1);
                if (level >= AncLevelMin && level <= AncLevelMax)
                    _ancLevel = level;
            }
        }
    }
}

void JblBackend::handleStatus(const QByteArray &p)
{
    if (p.size() < 2)
        return;
    switch (at(p, 0)) {
    case FieldAnc:
        _ancOn = at(p, 1) != 0;
        updateNoise();
        break;
    case FieldPassThrough:
        if (at(p, 1) <= PassAmbient) {
            _passMode = at(p, 1);
            updateNoise();
        }
        break;
    case FieldAutoOff:
        if (p.size() >= 3) {
            _autoOff = at(p, 1) != 0;
            if (at(p, 2) != 0)
                _autoOffMinutes = at(p, 2);
        }
        break;
    case FieldEqPreset:
        _eqPreset = at(p, 1);
        break;
    case FieldAutoPlay:
        _autoPlay = at(p, 1) != 0;
        break;
    case FieldInEar:
        if (p.size() >= 3)
            qCDebug(lcHp) << "jbl in-ear" << (at(p, 1) == 1) << (at(p, 2) == 1);
        break;
    default:
        break;
    }
}

void JblBackend::handleAck(const QByteArray &p)
{
    if (p.size() < 2 || at(p, 1) == 0)
        return;
    qCInfo(lcHp) << "jbl: command" << Qt::hex << at(p, 0) << "refused with status" << at(p, 1);
    switch (at(p, 0)) {
    case CmdAncSwitch:
    case CmdNoise:
        refresh();
        break;
    case CmdAutoOff:
        send(statusRequest(FieldAutoOff));
        break;
    case CmdAutoPlay:
        send(statusRequest(FieldAutoPlay));
        break;
    case CmdSpatial:
        send(spatialRequest());
        break;
    default:
        break;
    }
}

void JblBackend::updateNoise()
{
    if (_passMode == PassTalkThru) {
        _noise = Noise::TalkThru;
        _passThrough = Noise::TalkThru;
    } else if (_passMode == PassAmbient) {
        _noise = Noise::Ambient;
        _passThrough = Noise::Ambient;
    } else if (_ancOn) {
        _noise = *_ancOn ? Noise::Anc : Noise::Off;
    }
}

QStringList JblBackend::modes() const
{
    if (!_noise)
        return {};
    return {ModeNc, ModeAmbient, ModeOff};
}

QString JblBackend::wearKey() const
{
    return _earbuds ? QStringLiteral("earDetection") : QStringLiteral("wearDetection");
}

QString JblBackend::modelName() const
{
    return _reportedName.isEmpty() ? _device.name : _reportedName;
}

bool JblBackend::equalizerSupported() const
{
    return _scheme == AncScheme::Status;
}

Snapshot JblBackend::snapshot()
{
    Snapshot s;
    if (!_batteryKnown)
        return s;

    const auto add = [&](const char *part, const Battery &b) {
        if (b.level < 0)
            return;
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QString::fromLatin1(part)},
            {QStringLiteral("level"), b.level},
            {QStringLiteral("charging"), b.charging},
        });
    };
    if (_earbuds) {
        add("left", _left);
        add("right", _right);
        add("case", _case);
        if (_left.level >= 0 && _right.level >= 0)
            s.battery = std::min(_left.level, _right.level);
        else
            s.battery = std::max(_left.level, _right.level);
    } else {
        add("main", _main);
        s.battery = _main.level;
    }

    s.modes = modes();
    if (_noise) {
        switch (*_noise) {
        case Noise::Anc:
            s.mode = ModeNc;
            break;
        case Noise::Ambient:
        case Noise::TalkThru:
            s.mode = ModeAmbient;
            break;
        case Noise::Off:
            s.mode = ModeOff;
            break;
        }
    }

    QJsonObject o;
    o[QStringLiteral("model")] = modelName();
    o[QStringLiteral("untested")] = true;

    if (equalizerSupported() && _eqPreset) {
        QJsonArray presets;
        int current = -1;
        for (const Preset &preset : LegacyPresets) {
            presets.append(QJsonObject{{QStringLiteral("id"), preset.id}, {QStringLiteral("key"), QString::fromLatin1(preset.key)}});
            if (preset.id == *_eqPreset)
                current = preset.id;
        }
        o[QStringLiteral("equalizer")] = QJsonObject{
            {QStringLiteral("preset"), current},
            {QStringLiteral("presets"), presets},
        };
    }

    QJsonArray toggles;
    if (_autoPlay)
        toggles.append(control(wearKey(), *_autoPlay));
    if (_spatial)
        toggles.append(control(QStringLiteral("spatialAudio"), *_spatial));
    if (!toggles.isEmpty())
        o[QStringLiteral("toggles")] = toggles;

    if (_ancLevel) {
        QJsonObject slider = control(QStringLiteral("ancStrength"), *_ancLevel);
        slider[QStringLiteral("min")] = AncLevelMin;
        slider[QStringLiteral("max")] = AncLevelMax;
        o[QStringLiteral("sliders")] = QJsonArray{slider};
    }

    QJsonArray choices;
    if (_noise) {
        QJsonObject choice = control(QStringLiteral("ambientType"),
                                     _passThrough == Noise::TalkThru ? QStringLiteral("talkThru") : QStringLiteral("ambientAware"));
        choice[QStringLiteral("options")] = QJsonArray{QStringLiteral("ambientAware"), QStringLiteral("talkThru")};
        choices.append(choice);
    }
    if (_autoOff) {
        QStringList options{QStringLiteral("off")};
        for (const int minutes : AutoOffChoices)
            options.append(autoOffKey(minutes));
        const QString current = *_autoOff ? autoOffKey(_autoOffMinutes) : QStringLiteral("off");
        if (!options.contains(current))
            options.append(current);
        QJsonObject choice = control(QStringLiteral("autoPowerOff"), current);
        choice[QStringLiteral("options")] = QJsonArray::fromStringList(options);
        choices.append(choice);
    }
    if (!choices.isEmpty())
        o[QStringLiteral("choices")] = choices;

    s.settings = o;
    return s;
}

void JblBackend::sendNoise(Noise noise)
{
    if (_scheme == AncScheme::Status) {
        switch (noise) {
        case Noise::Anc:
            send(ancSwitchCommand(true));
            break;
        case Noise::Off:
            send(ancSwitchCommand(false));
            break;
        case Noise::Ambient:
            send(noiseSlotsCommand(false, true, false));
            break;
        case Noise::TalkThru:
            send(noiseSlotsCommand(false, false, true));
            break;
        }
    } else {
        send(noiseSlotsCommand(noise == Noise::Anc, noise == Noise::Ambient, noise == Noise::TalkThru));
    }
    _noise = noise;
    _ancOn = noise == Noise::Anc;
    _passMode = noise == Noise::TalkThru ? PassTalkThru : noise == Noise::Ambient ? PassAmbient : PassOff;
    scheduleRefresh(noiseRequests());
}

QDBusError JblBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!modes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("These headphones do not support mode %1").arg(mode));
    if (mode == ModeNc)
        sendNoise(Noise::Anc);
    else if (mode == ModeAmbient)
        sendNoise(_passThrough);
    else
        sendNoise(Noise::Off);
    return {};
}

QDBusError JblBackend::setAmbientLevel(int)
{
    return unsupported();
}

QDBusError JblBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();

    const QJsonObject eq = in.value(QStringLiteral("equalizer")).toObject();
    if (const QJsonValue v = eq.value(QStringLiteral("preset")); v.isDouble() && equalizerSupported() && _eqPreset) {
        const int id = v.toInt();
        const bool known = std::any_of(std::begin(LegacyPresets), std::end(LegacyPresets), [&](const Preset &p) { return p.id == id; });
        if (!known)
            return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Unknown equalizer preset %1").arg(id));
        send(eqPresetCommand(id));
        scheduleRefresh({statusRequest(FieldEqPreset)});
        _eqPreset = id;
    }

    const QJsonObject toggles = in.value(QStringLiteral("toggles")).toObject();
    if (const QJsonValue v = toggles.value(wearKey()); v.isBool() && _autoPlay) {
        send(autoPlayCommand(v.toBool()));
        scheduleRefresh({statusRequest(FieldAutoPlay)});
        _autoPlay = v.toBool();
    }
    if (const QJsonValue v = toggles.value(QStringLiteral("spatialAudio")); v.isBool() && _spatial) {
        send(spatialCommand(v.toBool(), _spatialMode));
        _spatial = v.toBool();
    }

    const QJsonObject sliders = in.value(QStringLiteral("sliders")).toObject();
    if (const QJsonValue v = sliders.value(QStringLiteral("ancStrength")); v.isDouble() && _ancLevel) {
        const int level = v.toInt();
        if (level < AncLevelMin || level > AncLevelMax)
            return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Noise cancelling strength must be %1 to %2").arg(AncLevelMin).arg(AncLevelMax));
        send(ancLevelCommand(level));
        scheduleRefresh({ancLevelRequest()});
        _ancLevel = level;
    }

    const QJsonObject choices = in.value(QStringLiteral("choices")).toObject();
    if (const QJsonValue v = choices.value(QStringLiteral("ambientType")); v.isString() && _noise) {
        Noise wanted;
        if (v.toString() == QLatin1String("ambientAware"))
            wanted = Noise::Ambient;
        else if (v.toString() == QLatin1String("talkThru"))
            wanted = Noise::TalkThru;
        else
            return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Unknown ambient sound type %1").arg(v.toString()));
        _passThrough = wanted;
        if (_noise == Noise::Ambient || _noise == Noise::TalkThru)
            sendNoise(wanted);
    }
    if (const QJsonValue v = choices.value(QStringLiteral("autoPowerOff")); v.isString() && _autoOff) {
        const QString key = v.toString();
        if (key == QLatin1String("off")) {
            send(autoOffCommand(false, _autoOffMinutes));
            _autoOff = false;
        } else {
            bool ok = false;
            const int minutes = key.endsWith(QLatin1String("-minutes")) ? key.chopped(8).toInt(&ok) : 0;
            if (!ok || minutes < 1 || minutes > 255)
                return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Unknown automatic power off setting %1").arg(key));
            send(autoOffCommand(true, minutes));
            _autoOff = true;
            _autoOffMinutes = minutes;
        }
        scheduleRefresh({statusRequest(FieldAutoOff)});
    }
    return {};
}
