#pragma once

#include <QByteArray>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QVariantMap>

#include <memory>
#include <optional>

#include "Backend.h"

inline const QString JblServiceUuid = QStringLiteral("65786365-6c70-6f69-6e74-2e636f6d0000");

struct JblFrame {
    uint8_t command = 0;
    QByteArray payload;
};

class JblGatt : public QObject
{
    Q_OBJECT
public:
    explicit JblGatt(const DeviceInfo &device);
    ~JblGatt() override;

    void poll();
    void flush();
    void write(const QByteArray &data);
    QList<QByteArray> takeIncoming();
    bool ready() const { return _stage == Stage::Ready; }
    bool failed() const { return !_error.isEmpty(); }
    QString error() const { return _error; }

private Q_SLOTS:
    void onCharacteristicChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);
    void onDeviceChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);
    void onInterfacesRemoved(const QDBusObjectPath &path, const QStringList &interfaces);

private:
    enum class Stage { Lookup, Acquiring, Ready };
    enum class ConnectedVia { None, LeBearer, Device };

    void lookup();
    void evaluate(const QDBusMessage &reply);
    void startDiscovery();
    void stopDiscovery();
    void requestConnect(const QString &devicePath, bool leBearer);
    void acquire(const QVariantMap &notifyProps, const QVariantMap &writeProps);
    void fallbackNotify();
    void checkReady();
    void watchLink();
    void receive();
    void setError(const QString &error);

    DeviceInfo _device;
    Stage _stage = Stage::Lookup;
    QString _error;
    bool _lookupInFlight = false;
    QElapsedTimer _sinceLookup;
    QString _adapterPath;
    QString _devicePath;
    QString _servicePath;
    QString _notifyPath;
    QString _writePath;
    QString _writeType;
    bool _discovering = false;
    bool _discoveryRequested = false;
    bool _connectInFlight = false;
    QElapsedTimer _sinceConnect;
    ConnectedVia _connectedVia = ConnectedVia::None;
    QString _connectedPath;
    int _notifyFd = -1;
    int _writeFd = -1;
    bool _notifyReady = false;
    bool _writeReady = false;
    bool _startedNotify = false;
    bool _writeInFlight = false;
    bool _watching = false;
    QList<QByteArray> _incoming;
    QList<QByteArray> _outgoing;
};

class JblBackend : public Backend
{
public:
    enum class AncScheme { Slots, Status };
    enum class Noise { Off, Anc, Ambient, TalkThru };

    JblBackend(MDRConnection *connection, const DeviceInfo &device);
    ~JblBackend() override;

    static QByteArray frame(uint8_t command, const QByteArray &payload = {});
    static QList<JblFrame> splitFrames(QByteArray &buffer);
    static QByteArray nameRequest();
    static QByteArray batteryRequest();
    static QByteArray batteryRequestLevels();
    static QByteArray batteryRequestLegacy();
    static QByteArray statusRequest(uint8_t field);
    static QByteArray noiseRequest();
    static QByteArray ancLevelRequest();
    static QByteArray spatialRequest();
    static QByteArray noiseSlotsCommand(bool anc, bool ambient, bool talkThru);
    static QByteArray ancSwitchCommand(bool on);
    static QByteArray ancLevelCommand(int level);
    static QByteArray autoOffCommand(bool on, int minutes);
    static QByteArray autoPlayCommand(bool on);
    static QByteArray eqPresetCommand(int preset);
    static QByteArray spatialCommand(bool on, int mode);
    static AncScheme schemeFromName(const QString &name);
    static bool earbudsFromName(const QString &name);

    QString vendor() const override { return QStringLiteral("jbl"); }
    bool start() override;
    bool tick() override;
    bool ready() const override { return _batteryKnown; }
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError apply(const QJsonObject &change) override;

    void requestState();
    void handleData(const QByteArray &data);
    void handleFrame(const JblFrame &frame);
    QList<QByteArray> takeOutgoing();
    void flushRefreshNow() { flushRefresh(); }
    AncScheme scheme() const { return _scheme; }
    bool earbuds() const { return _earbuds; }
    std::optional<Noise> noise() const { return _noise; }

private:
    struct Battery {
        int level = -1;
        bool charging = false;
    };

    void send(const QByteArray &packet);
    void sendNoise(Noise noise);
    void scheduleRefresh(const QList<QByteArray> &requests);
    void flushRefresh();
    void refresh();
    QList<QByteArray> noiseRequests() const;
    void updateNoise();
    void handleBattery(const QByteArray &p);
    void handleNoise(const QByteArray &p);
    void handleStatus(const QByteArray &p);
    void handleAck(const QByteArray &p);
    QStringList modes() const;
    QString wearKey() const;
    QString modelName() const;
    bool equalizerSupported() const;

    AncScheme _scheme;
    bool _earbuds;
    QString _reportedName;

    Battery _main;
    Battery _left;
    Battery _right;
    Battery _case;
    bool _batteryKnown = false;

    std::optional<bool> _ancOn;
    std::optional<uint8_t> _passMode;
    std::optional<Noise> _noise;
    Noise _passThrough = Noise::Ambient;
    std::optional<int> _ancLevel;
    std::optional<bool> _autoOff;
    int _autoOffMinutes = 30;
    std::optional<bool> _autoPlay;
    std::optional<int> _eqPreset;
    std::optional<bool> _spatial;
    int _spatialMode = 1;

    std::unique_ptr<JblGatt> _gatt;
    bool _linkUp = false;
    int _batteryRetries = 0;
    QByteArray _rx;
    QElapsedTimer _sinceRx;
    QList<QByteArray> _tx;
    QElapsedTimer _sinceStart;
    QElapsedTimer _sinceLink;
    QElapsedTimer _refreshTimer;
    QList<QByteArray> _pendingRefresh;
};
