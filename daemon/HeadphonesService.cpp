#include "HeadphonesService.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSettings>

#include <algorithm>
#include <array>

#include <mdr-bt/ConnectionLinux.h>
#include <mdr-c/Connection.h>

Q_LOGGING_CATEGORY(lcHp, "sony.headphonesd", QtInfoMsg)

using namespace std::chrono_literals;

namespace {

using InterfaceMap = QMap<QString, QVariantMap>;
using ManagedObjects = QMap<QDBusObjectPath, InterfaceMap>;

constexpr auto TickInterval = 50ms;
constexpr qint64 ReconnectIntervalMs = 5000;
constexpr qint64 ConnectTimeoutMs = 15000;
constexpr qint64 SyncIntervalMs = 60000;
constexpr int MaxEventsPerTick = 32;

const QString ModeNc = QStringLiteral("noise-cancelling");
const QString ModeAmbient = QStringLiteral("ambient");
const QString ModeOff = QStringLiteral("off");
const QString AmbientLevelKey = QStringLiteral("ambientLevel");

QByteArray framePayload(const unsigned char *frame, int size)
{
    if (size < 2 || frame[0] != 0x3E || frame[size - 1] != 0x3C)
        return {};
    QByteArray raw;
    raw.reserve(size);
    for (int i = 1; i < size - 1; i++) {
        if (frame[i] == 0x3D && i + 1 < size - 1)
            raw.append(char(frame[++i] | 0x10));
        else
            raw.append(char(frame[i]));
    }
    if (raw.size() < 7)
        return {};
    const auto b = reinterpret_cast<const uint8_t *>(raw.constData());
    const int len = (b[2] << 24) | (b[3] << 16) | (b[4] << 8) | b[5];
    if (len <= 0 || 6 + len > raw.size())
        return {};
    return raw.mid(6, len);
}

}

Q_DECLARE_METATYPE(InterfaceMap)
Q_DECLARE_METATYPE(ManagedObjects)

HeadphonesService::HeadphonesService(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<InterfaceMap>();
    qDBusRegisterMetaType<ManagedObjects>();

    _lastAmbientLevel = std::clamp(QSettings().value(AmbientLevelKey, _lastAmbientLevel).toInt(), 1, 20);

    _linux = mdrConnectionLinuxCreate();
    _conn = mdrConnectionLinuxGet(_linux);

    publishState();
    connect(&_tick, &QTimer::timeout, this, &HeadphonesService::tick);
    _tick.start(TickInterval);
}

HeadphonesService::~HeadphonesService()
{
    if (_hp)
        mdrHeadphonesDestroy(_hp);
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
        serviceHeadphones();
        break;
    }
    publishState();
}

std::optional<HeadphonesService::Device> HeadphonesService::findConnectedDevice() const
{
    const QString sonyUuid = QString::fromLatin1(MDR_SERVICE_UUID_XM5);
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
        if (!uuids.contains(sonyUuid, Qt::CaseInsensitive))
            continue;
        const QString adapterPath = dev->value(QStringLiteral("Adapter")).value<QDBusObjectPath>().path();
        const QString adapterAddress = objects.value(QDBusObjectPath(adapterPath))
                                           .value(QStringLiteral("org.bluez.Adapter1"))
                                           .value(QStringLiteral("Address"))
                                           .toString();
        return Device{dev->value(QStringLiteral("Alias")).toString(), dev->value(QStringLiteral("Address")).toString(), adapterAddress};
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
    const MDRResult r = mdrConnectionConnect(_conn, _device.address.toLatin1().constData(), MDR_SERVICE_UUID_XM5);
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
    if (mdrHeadphonesCreate(MDR_ABI_VERSION, _conn, MDR_PROTOCOL_V2, &_hp) != MDR_RESULT_OK) {
        dropConnection(QStringLiteral("libmdr ABI mismatch"));
        return;
    }
    mdrHeadphonesSetPacketCallback(_hp, &HeadphonesService::onPacket, this);
    if (mdrHeadphonesRequestInit(_hp) != MDR_RESULT_OK) {
        dropConnection(QStringLiteral("init request failed"));
        return;
    }
    qCInfo(lcHp) << "connected to" << _device.name << _device.address;
    _link = Link::Connected;
}

void HeadphonesService::serviceHeadphones()
{
    for (int i = 0; i < MaxEventsPerTick; i++) {
        MDREvent event = MDR_EVENT_NONE;
        if (mdrHeadphonesPoll(_hp, &event) != MDR_RESULT_OK) {
            dropConnection(QString::fromUtf8(mdrConnectionGetLastError(_conn)));
            return;
        }
        if (event == MDR_EVENT_NONE)
            break;
        switch (event) {
        case MDR_EVENT_INITIALIZE_COMPLETE:
            qCInfo(lcHp) << "initialized";
            _initDone = true;
            _syncWanted = true;
            break;
        case MDR_EVENT_NEED_SYNC:
            _syncWanted = true;
            break;
        default:
            break;
        }
    }

    if (!_initDone || !mdrHeadphonesIsReady(_hp))
        return;
    applyPending();
    if (mdrHeadphonesIsDirty(_hp)) {
        if (mdrHeadphonesRequestCommit(_hp) != MDR_RESULT_OK)
            dropConnection(QStringLiteral("commit failed"));
    } else if (_syncWanted || !_sinceSync.isValid() || _sinceSync.elapsed() >= SyncIntervalMs) {
        _syncWanted = false;
        _sinceSync.start();
        if (mdrHeadphonesRequestSync(_hp) != MDR_RESULT_OK)
            dropConnection(QStringLiteral("sync failed"));
    }
}

void HeadphonesService::applyPending()
{
    if (_pending.noiseMode || _pending.ambientLevel || _pending.focusOnVoice) {
        MDRNoiseControl nc{};
        if (mdrHeadphonesGetNoiseControl(_hp, &nc) == MDR_RESULT_OK) {
            if (_pending.noiseMode)
                nc.mode = *_pending.noiseMode;
            if (_pending.ambientLevel)
                nc.ambient_level = uint8_t(*_pending.ambientLevel);
            if (_pending.focusOnVoice)
                nc.focus_on_voice = *_pending.focusOnVoice ? MDR_TRUE : MDR_FALSE;
            const MDRResult r = mdrHeadphonesSetNoiseControl(_hp, &nc);
            if (r != MDR_RESULT_OK)
                qCWarning(lcHp) << "SetNoiseControl failed:" << r;
        }
        _pending.noiseMode.reset();
        _pending.ambientLevel.reset();
        _pending.focusOnVoice.reset();
    }
    if (_pending.deviceCommand) {
        const auto &[command, mac] = *_pending.deviceCommand;
        const MDRPairedDeviceAction action{command, mac.constData(), uint32_t(mac.size())};
        const MDRResult r = mdrHeadphonesSetPairedDevice(_hp, &action);
        if (r != MDR_RESULT_OK)
            qCWarning(lcHp) << "SetPairedDevice failed:" << r;
        _pending.deviceCommand.reset();
    }
}

void HeadphonesService::dropConnection(const QString &reason)
{
    qCInfo(lcHp) << "disconnected:" << reason;
    if (_hp) {
        mdrHeadphonesDestroy(_hp);
        _hp = nullptr;
    }
    mdrConnectionDisconnect(_conn);
    _link = Link::Idle;
    _initDone = false;
    _syncWanted = false;
    _sinceSync.invalidate();
    _pending = {};
    _connectionOrder.clear();
    _deviceAction = {};
}

void HeadphonesService::onPacket(void *self, MDRPacketDirection direction, const unsigned char *frame, int size)
{
    if (direction != MDR_PACKET_DIRECTION_RX)
        return;
    const QByteArray payload = framePayload(frame, size);
    if (!payload.isEmpty())
        static_cast<HeadphonesService *>(self)->handlePayload(payload);
}

void HeadphonesService::handlePayload(const QByteArray &p)
{
    const auto b = reinterpret_cast<const uint8_t *>(p.constData());
    const int n = p.size();
    if (n < 3)
        return;

    if ((b[0] == 0x37 || b[0] == 0x39) && b[1] == 0x02) {
        QHash<QString, int> order;
        int i = 3;
        for (int k = 0; k < b[2]; k++) {
            if (i + 22 > n)
                return;
            const int len = b[i + 21];
            if (i + 22 + len > n)
                return;
            order.insert(QString::fromLatin1(p.mid(i, 17)).toUpper(), b[i + 17]);
            i += 22 + len;
        }
        _connectionOrder = order;
        return;
    }

    if (b[0] == 0x3D && (b[1] == 0x00 || b[1] == 0x02) && n >= 21) {
        static const char *states[] = {"ok", "error", "in-progress", "busy"};
        const uint8_t result = b[3];
        const QString state = QString::fromLatin1((result & 0x0f) < 4 ? states[result & 0x0f] : "error");
        const QString mac = QString::fromLatin1(p.mid(4, 17)).toUpper();
        qCInfo(lcHp) << "device" << mac << (b[2] == 0 ? "disconnect" : "connect") << "->" << state;
        _deviceAction = QJsonObject{
            {QStringLiteral("mac"), mac},
            {QStringLiteral("action"), b[2] == 0 ? QStringLiteral("disconnect") : QStringLiteral("connect")},
            {QStringLiteral("state"), state},
        };
    }
}

void HeadphonesService::publishState()
{
    QVariantMap s;
    const bool up = _hp && _initDone;
    s[QStringLiteral("Connected")] = up;
    s[QStringLiteral("DeviceName")] = up ? _device.name : QString();
    s[QStringLiteral("Address")] = up ? _device.address : QString();

    int battery = -1;
    QString mode;
    int ambient = _lastAmbientLevel;
    bool voice = false;
    QJsonArray devices;
    if (up) {
        std::array<MDRBattery, 4> batteries{};
        uint32_t count = batteries.size();
        if (mdrHeadphonesGetBatteries(_hp, batteries.data(), &count) == MDR_RESULT_OK) {
            for (uint32_t i = 0; i < count; i++) {
                if (batteries[i].present && batteries[i].part == MDR_BATTERY_MAIN)
                    battery = batteries[i].level_percent;
            }
        }

        MDRNoiseControl nc{};
        if (mdrHeadphonesGetNoiseControl(_hp, &nc) == MDR_RESULT_OK) {
            const MDRNoiseMode m = _pending.noiseMode.value_or(nc.mode);
            mode = m == MDR_NOISE_MODE_CANCELLING ? ModeNc : m == MDR_NOISE_MODE_AMBIENT ? ModeAmbient : ModeOff;
            voice = _pending.focusOnVoice.value_or(nc.focus_on_voice == MDR_TRUE);
            if (m == MDR_NOISE_MODE_AMBIENT)
                ambient = std::clamp(_pending.ambientLevel.value_or(nc.ambient_level), 1, 20);
        }

        std::array<MDRPairedDevice, 16> paired{};
        count = paired.size();
        if (mdrHeadphonesGetPairedDevices(_hp, paired.data(), &count) == MDR_RESULT_OK) {
            std::stable_sort(paired.begin(), paired.begin() + count,
                             [](const MDRPairedDevice &a, const MDRPairedDevice &b) { return a.connected > b.connected; });
            for (uint32_t i = 0; i < count; i++) {
                const MDRPairedDevice &d = paired[i];
                const QString mac = QString::fromLatin1(d.macAddress).toUpper();
                devices.append(QJsonObject{
                    {QStringLiteral("mac"), mac},
                    {QStringLiteral("name"), QString::fromUtf8(d.name)},
                    {QStringLiteral("connected"), d.connected == MDR_TRUE},
                    {QStringLiteral("order"), _connectionOrder.value(mac, d.connected == MDR_TRUE ? int(i) + 1 : 0)},
                    {QStringLiteral("active"), d.playback_device == MDR_TRUE},
                    {QStringLiteral("self"), mac.compare(_device.adapterAddress, Qt::CaseInsensitive) == 0},
                });
            }
        }
    }
    if (ambient != _lastAmbientLevel) {
        _lastAmbientLevel = ambient;
        QSettings().setValue(AmbientLevelKey, ambient);
    }
    s[QStringLiteral("Battery")] = battery;
    s[QStringLiteral("Mode")] = mode;
    s[QStringLiteral("AmbientLevel")] = ambient;
    s[QStringLiteral("FocusOnVoice")] = voice;
    s[QStringLiteral("Devices")] = QString::fromUtf8(QJsonDocument(devices).toJson(QJsonDocument::Compact));
    s[QStringLiteral("DeviceAction")] = _deviceAction.isEmpty()
        ? QString()
        : QString::fromUtf8(QJsonDocument(_deviceAction).toJson(QJsonDocument::Compact));

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

bool HeadphonesService::requireConnected()
{
    if (_hp && _initDone)
        return true;
    if (calledFromDBus())
        sendErrorReply(QStringLiteral("org.rodada.SonyHeadphones.Error.NotConnected"), QStringLiteral("Headphones are not connected"));
    return false;
}

void HeadphonesService::SetMode(const QString &mode)
{
    if (!requireConnected())
        return;
    if (mode == ModeNc) {
        _pending.noiseMode = MDR_NOISE_MODE_CANCELLING;
    } else if (mode == ModeAmbient) {
        _pending.noiseMode = MDR_NOISE_MODE_AMBIENT;
        _pending.ambientLevel = _lastAmbientLevel;
    } else if (mode == ModeOff) {
        _pending.noiseMode = MDR_NOISE_MODE_OFF;
    } else {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Mode must be noise-cancelling, ambient or off"));
        return;
    }
    publishState();
}

void HeadphonesService::SetAmbientLevel(int level)
{
    if (!requireConnected())
        return;
    level = std::clamp(level, 1, 20);
    if (mode() == ModeAmbient) {
        _pending.ambientLevel = level;
    } else {
        _lastAmbientLevel = level;
        QSettings().setValue(AmbientLevelKey, level);
    }
    publishState();
}

void HeadphonesService::SetFocusOnVoice(bool enabled)
{
    if (!requireConnected())
        return;
    _pending.focusOnVoice = enabled;
    publishState();
}

void HeadphonesService::CycleMode()
{
    if (!requireConnected())
        return;
    const QString current = mode();
    SetMode(current == ModeNc ? ModeAmbient : current == ModeAmbient ? ModeOff : ModeNc);
}

void HeadphonesService::queueDeviceCommand(MDRPairedDeviceCommand command, const QString &mac)
{
    if (!requireConnected())
        return;
    const QString m = mac.toUpper();
    std::array<MDRPairedDevice, 16> paired{};
    uint32_t count = paired.size();
    mdrHeadphonesGetPairedDevices(_hp, paired.data(), &count);
    const auto it = std::find_if(paired.begin(), paired.begin() + count,
                                 [&](const MDRPairedDevice &d) { return m == QString::fromLatin1(d.macAddress).toUpper(); });
    if (it == paired.begin() + count) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Device %1 is not paired with the headphones").arg(mac));
        return;
    }
    if (command == MDR_PAIRED_DEVICE_SELECT_PLAYBACK && it->connected != MDR_TRUE) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Device %1 is not connected to the headphones").arg(mac));
        return;
    }
    if (command == MDR_PAIRED_DEVICE_DISCONNECT && m.compare(_device.adapterAddress, Qt::CaseInsensitive) == 0) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Refusing to disconnect this computer"));
        return;
    }
    _pending.deviceCommand = std::make_pair(command, QByteArray(it->macAddress));
    if (command != MDR_PAIRED_DEVICE_SELECT_PLAYBACK) {
        _deviceAction = QJsonObject{
            {QStringLiteral("mac"), m},
            {QStringLiteral("action"), command == MDR_PAIRED_DEVICE_DISCONNECT ? QStringLiteral("disconnect") : QStringLiteral("connect")},
            {QStringLiteral("state"), QStringLiteral("pending")},
        };
    }
    publishState();
}

void HeadphonesService::SwitchPlayback(const QString &mac)
{
    queueDeviceCommand(MDR_PAIRED_DEVICE_SELECT_PLAYBACK, mac);
}

void HeadphonesService::ConnectDevice(const QString &mac)
{
    queueDeviceCommand(MDR_PAIRED_DEVICE_CONNECT, mac);
}

void HeadphonesService::DisconnectDevice(const QString &mac)
{
    queueDeviceCommand(MDR_PAIRED_DEVICE_DISCONNECT, mac);
}
