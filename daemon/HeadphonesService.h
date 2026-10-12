#pragma once

#include <QDBusContext>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

#include <deque>
#include <functional>
#include <optional>

#include <mdr-c/Headphones.h>

struct MDRConnectionLinux;

class HeadphonesService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.rodada.SonyHeadphones")

    Q_PROPERTY(bool Connected READ connected)
    Q_PROPERTY(QString DeviceName READ deviceName)
    Q_PROPERTY(QString Address READ address)
    Q_PROPERTY(int Battery READ battery)
    Q_PROPERTY(QString Mode READ mode)
    Q_PROPERTY(int AmbientLevel READ ambientLevel)
    Q_PROPERTY(bool FocusOnVoice READ focusOnVoice)
    Q_PROPERTY(QString Devices READ devices)
    Q_PROPERTY(QString DeviceAction READ deviceAction)
    Q_PROPERTY(QString Settings READ settings)
    Q_PROPERTY(QString Alert READ alert)

public:
    static constexpr auto Service = "org.rodada.SonyHeadphones";
    static constexpr auto Path = "/org/rodada/SonyHeadphones";

    explicit HeadphonesService(QObject *parent = nullptr);
    ~HeadphonesService() override;

    bool connected() const { return _state.value(QStringLiteral("Connected")).toBool(); }
    QString deviceName() const { return _state.value(QStringLiteral("DeviceName")).toString(); }
    QString address() const { return _state.value(QStringLiteral("Address")).toString(); }
    int battery() const { return _state.value(QStringLiteral("Battery"), -1).toInt(); }
    QString mode() const { return _state.value(QStringLiteral("Mode")).toString(); }
    int ambientLevel() const { return _state.value(QStringLiteral("AmbientLevel"), 0).toInt(); }
    bool focusOnVoice() const { return _state.value(QStringLiteral("FocusOnVoice")).toBool(); }
    QString devices() const { return _state.value(QStringLiteral("Devices")).toString(); }
    QString deviceAction() const { return _state.value(QStringLiteral("DeviceAction")).toString(); }
    QString settings() const { return _state.value(QStringLiteral("Settings")).toString(); }
    QString alert() const { return _state.value(QStringLiteral("Alert")).toString(); }

public Q_SLOTS:
    void SetMode(const QString &mode);
    void SetAmbientLevel(int level);
    void SetFocusOnVoice(bool enabled);
    void CycleMode();
    void SwitchPlayback(const QString &mac);
    void ConnectDevice(const QString &mac);
    void DisconnectDevice(const QString &mac);
    void Apply(const QString &json);
    void Playback(const QString &action);
    void PowerOff();
    void RespondToAlert(bool accept);

private:
    enum class Link { Idle, Connecting, Connected };

    struct Device {
        QString name;
        QString address;
        QString adapterAddress;
    };
    struct Pending {
        std::optional<MDRNoiseMode> noiseMode;
        std::optional<int> ambientLevel;
        std::optional<bool> focusOnVoice;
        std::optional<std::pair<MDRPairedDeviceCommand, QByteArray>> deviceCommand;
    };

    void tick();
    void tryConnect();
    void pollConnecting();
    void serviceHeadphones();
    void applyPending();
    void dropConnection(const QString &reason);
    void publishState();
    bool requireConnected();
    void queueDeviceCommand(MDRPairedDeviceCommand command, const QString &mac);
    std::optional<Device> findConnectedDevice() const;
    bool has(MDRFeature feature) const;
    QString text(MDRText id, uint32_t index = 0) const;
    QJsonObject settingsJson() const;
    void queueOp(std::function<MDRResult()> op);
    void runOps();

    static void onPacket(void *self, MDRPacketDirection direction, const unsigned char *frame, int size);
    void handlePayload(const QByteArray &payload);

    MDRConnectionLinux *_linux = nullptr;
    MDRConnection *_conn = nullptr;
    MDRHeadphones *_hp = nullptr;
    Link _link = Link::Idle;
    Device _device;
    Pending _pending;
    QHash<QString, int> _connectionOrder;
    QJsonObject _deviceAction;
    std::deque<std::function<MDRResult()>> _ops;
    QString _alert;
    QString _settingsCache;
    bool _settingsDirty = true;
    int _lastAmbientLevel = 10;

    QTimer _tick;
    QElapsedTimer _sinceConnectAttempt;
    QElapsedTimer _sinceSync;
    bool _initDone = false;
    bool _syncWanted = false;

    QVariantMap _state;
};
