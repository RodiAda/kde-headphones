#include "HeadphonesService.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QJsonDocument>

#include <mdr-bt/ConnectionLinux.h>
#include <mdr-c/Headphones.h>

#include "GalaxyBackend.h"
#include "Log.h"
#include "SonyBackend.h"


using namespace std::chrono_literals;

namespace {

using InterfaceMap = QMap<QString, QVariantMap>;
using ManagedObjects = QMap<QDBusObjectPath, InterfaceMap>;

constexpr auto TickInterval = 50ms;
constexpr qint64 ReconnectIntervalMs = 5000;
constexpr qint64 ConnectTimeoutMs = 15000;

constexpr auto GalaxyServiceUuid = "2E73A4AD-332D-41FC-90E2-16BEF06523F2";

struct KnownService {
    const char *uuid;
    DeviceKind kind;
};

constexpr KnownService KnownServices[] = {
    {MDR_SERVICE_UUID_XM5, DeviceKind::SonyV2},
    {MDR_SERVICE_UUID_LEGACY, DeviceKind::SonyV1},
    {GalaxyServiceUuid, DeviceKind::Galaxy},
};

const char *serviceUuid(DeviceKind kind)
{
    for (const auto &s : KnownServices) {
        if (s.kind == kind)
            return s.uuid;
    }
    return MDR_SERVICE_UUID_XM5;
}

QString compact(const QJsonArray &a)
{
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

QString compact(const QJsonObject &o)
{
    return o.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

}

Q_DECLARE_METATYPE(InterfaceMap)
Q_DECLARE_METATYPE(ManagedObjects)

HeadphonesService::HeadphonesService(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<InterfaceMap>();
    qDBusRegisterMetaType<ManagedObjects>();

    _linux = mdrConnectionLinuxCreate();
    _conn = mdrConnectionLinuxGet(_linux);

    publishState();
    connect(&_tick, &QTimer::timeout, this, &HeadphonesService::tick);
    _tick.start(TickInterval);
}

HeadphonesService::~HeadphonesService()
{
    _backend.reset();
    if (_link != Link::Idle)
        mdrConnectionDisconnect(_conn);
    mdrConnectionLinuxDestroy(_linux);
}

void HeadphonesService::tick()
{
    switch (_link) {
    case Link::Idle:
        tryConnect();
        break;
    case Link::Connecting:
        pollConnecting();
        break;
    case Link::Connected:
        if (!_backend->tick())
            dropConnection(_backend->errorString());
        break;
    }
    publishState();
}

std::optional<DeviceInfo> HeadphonesService::findConnectedDevice() const
{
    auto call = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), QStringLiteral("/"),
                                               QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                                               QStringLiteral("GetManagedObjects"));
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 2000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return std::nullopt;

    ManagedObjects objects;
    reply.arguments().constFirst().value<QDBusArgument>() >> objects;
    for (const InterfaceMap &ifaces : std::as_const(objects)) {
        const auto dev = ifaces.constFind(QStringLiteral("org.bluez.Device1"));
        if (dev == ifaces.cend() || !dev->value(QStringLiteral("Connected")).toBool())
            continue;
        const QStringList uuids = dev->value(QStringLiteral("UUIDs")).toStringList();
        const auto known = std::find_if(std::begin(KnownServices), std::end(KnownServices), [&](const KnownService &s) {
            return uuids.contains(QString::fromLatin1(s.uuid), Qt::CaseInsensitive);
        });
        if (known == std::end(KnownServices))
            continue;
        const QString adapterPath = dev->value(QStringLiteral("Adapter")).value<QDBusObjectPath>().path();
        const QString adapterAddress = objects.value(QDBusObjectPath(adapterPath))
                                           .value(QStringLiteral("org.bluez.Adapter1"))
                                           .value(QStringLiteral("Address"))
                                           .toString();
        return DeviceInfo{dev->value(QStringLiteral("Alias")).toString(), dev->value(QStringLiteral("Address")).toString(),
                          adapterAddress, known->kind};
    }
    return std::nullopt;
}

void HeadphonesService::tryConnect()
{
    if (_sinceConnectAttempt.isValid() && _sinceConnectAttempt.elapsed() < ReconnectIntervalMs)
        return;
    _sinceConnectAttempt.start();

    const auto device = findConnectedDevice();
    if (!device)
        return;
    _device = *device;
    qCInfo(lcHp) << "connecting to" << _device.name << _device.address;
    const MDRResult r = mdrConnectionConnect(_conn, _device.address.toLatin1().constData(), serviceUuid(_device.kind));
    if (r != MDR_RESULT_OK && r != MDR_RESULT_INPROGRESS) {
        qCInfo(lcHp) << "connect failed:" << mdrConnectionGetLastError(_conn);
        mdrConnectionDisconnect(_conn);
        return;
    }
    _link = Link::Connecting;
}

void HeadphonesService::pollConnecting()
{
    const MDRResult r = mdrConnectionPoll(_conn, 0);
    if (r == MDR_RESULT_INPROGRESS || r == MDR_RESULT_ERROR_TIMEOUT) {
        if (_sinceConnectAttempt.elapsed() > ConnectTimeoutMs)
            dropConnection(QStringLiteral("connect timed out"));
        return;
    }
    if (r != MDR_RESULT_OK) {
        dropConnection(QString::fromUtf8(mdrConnectionGetLastError(_conn)));
        return;
    }
    if (_device.kind == DeviceKind::Galaxy)
        _backend = std::make_unique<GalaxyBackend>(_conn, _device);
    else
        _backend = std::make_unique<SonyBackend>(_conn, _device);
    if (!_backend->start()) {
        dropConnection(_backend->errorString());
        return;
    }
    qCInfo(lcHp) << "connected to" << _device.name << _device.address << "via" << _backend->vendor()
                 << (_device.kind == DeviceKind::SonyV1 ? "(MDR V1)" : "");
    _link = Link::Connected;
}

void HeadphonesService::dropConnection(const QString &reason)
{
    qCInfo(lcHp) << "disconnected:" << reason;
    _backend.reset();
    mdrConnectionDisconnect(_conn);
    _link = Link::Idle;
}

void HeadphonesService::publishState()
{
    const bool up = _backend && _backend->ready();
    const Snapshot snap = up ? _backend->snapshot() : Snapshot{};

    QJsonObject settings = snap.settings;
    if (up)
        settings[QStringLiteral("vendor")] = _backend->vendor();

    QVariantMap s;
    s[QStringLiteral("Connected")] = up;
    s[QStringLiteral("Vendor")] = up ? _backend->vendor() : QString();
    s[QStringLiteral("DeviceName")] = up ? _device.name : QString();
    s[QStringLiteral("Address")] = up ? _device.address : QString();
    s[QStringLiteral("Battery")] = snap.battery;
    s[QStringLiteral("Batteries")] = compact(snap.batteries);
    s[QStringLiteral("Modes")] = compact(QJsonArray::fromStringList(snap.modes));
    s[QStringLiteral("Mode")] = snap.mode;
    s[QStringLiteral("AmbientLevel")] = snap.ambientLevel;
    s[QStringLiteral("FocusOnVoice")] = snap.focusOnVoice;
    s[QStringLiteral("Devices")] = compact(snap.devices);
    s[QStringLiteral("DeviceAction")] = compact(snap.deviceAction);
    s[QStringLiteral("Settings")] = up ? compact(settings) : QString();
    s[QStringLiteral("Alert")] = snap.alert;

    QVariantMap changed;
    for (auto it = s.cbegin(); it != s.cend(); ++it) {
        if (_state.value(it.key()) != it.value())
            changed.insert(it.key(), it.value());
    }
    _state = s;
    if (changed.isEmpty())
        return;

    auto signal = QDBusMessage::createSignal(QString::fromLatin1(Path), QStringLiteral("org.freedesktop.DBus.Properties"),
                                             QStringLiteral("PropertiesChanged"));
    signal << QString::fromLatin1(Service) << changed << QStringList();
    QDBusConnection::sessionBus().send(signal);
}

Backend *HeadphonesService::requireReady()
{
    if (_backend && _backend->ready())
        return _backend.get();
    if (calledFromDBus())
        sendErrorReply(QStringLiteral("org.rodada.Headphones.Error.NotConnected"), QStringLiteral("Headphones are not connected"));
    return nullptr;
}

void HeadphonesService::reply(const QDBusError &error)
{
    if (error.isValid() && calledFromDBus())
        sendErrorReply(error.name(), error.message());
    publishState();
}

void HeadphonesService::SetMode(const QString &mode)
{
    if (Backend *b = requireReady())
        reply(b->setMode(mode));
}

void HeadphonesService::SetAmbientLevel(int level)
{
    if (Backend *b = requireReady())
        reply(b->setAmbientLevel(level));
}

void HeadphonesService::SetFocusOnVoice(bool enabled)
{
    if (Backend *b = requireReady())
        reply(b->setFocusOnVoice(enabled));
}

void HeadphonesService::CycleMode()
{
    Backend *b = requireReady();
    if (!b)
        return;
    const QStringList available = b->snapshot().modes;
    if (available.isEmpty())
        return;
    const qsizetype i = available.indexOf(mode());
    reply(b->setMode(available.at((i + 1) % available.size())));
}

void HeadphonesService::SwitchPlayback(const QString &mac)
{
    if (Backend *b = requireReady())
        reply(b->deviceCommand(DeviceCommand::SelectPlayback, mac));
}

void HeadphonesService::ConnectDevice(const QString &mac)
{
    if (Backend *b = requireReady())
        reply(b->deviceCommand(DeviceCommand::Connect, mac));
}

void HeadphonesService::DisconnectDevice(const QString &mac)
{
    if (Backend *b = requireReady())
        reply(b->deviceCommand(DeviceCommand::Disconnect, mac));
}

void HeadphonesService::Apply(const QString &json)
{
    Backend *b = requireReady();
    if (!b)
        return;
    QJsonParseError err{};
    const QJsonObject in = QJsonDocument::fromJson(json.toUtf8(), &err).object();
    if (err.error != QJsonParseError::NoError) {
        reply(QDBusError(QDBusError::InvalidArgs, err.errorString()));
        return;
    }
    reply(b->apply(in));
}

void HeadphonesService::Playback(const QString &action)
{
    if (Backend *b = requireReady())
        reply(b->playback(action));
}

void HeadphonesService::PowerOff()
{
    if (Backend *b = requireReady())
        reply(b->powerOff());
}

void HeadphonesService::RespondToAlert(bool accept)
{
    if (Backend *b = requireReady())
        reply(b->respondToAlert(accept));
}
