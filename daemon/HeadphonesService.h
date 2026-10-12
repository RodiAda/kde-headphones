#pragma once

#include <QDBusContext>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

#include <memory>
#include <optional>

#include "Backend.h"

struct MDRConnectionLinux;

class HeadphonesService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.rodiada.Headphones")

    Q_PROPERTY(bool Connected READ connected)
    Q_PROPERTY(QString Vendor READ vendor)
    Q_PROPERTY(QString DeviceName READ deviceName)
    Q_PROPERTY(QString Address READ address)
    Q_PROPERTY(int Battery READ battery)
    Q_PROPERTY(QString Batteries READ batteries)
    Q_PROPERTY(QString Modes READ modes)
    Q_PROPERTY(QString Mode READ mode)
    Q_PROPERTY(int AmbientLevel READ ambientLevel)
    Q_PROPERTY(bool FocusOnVoice READ focusOnVoice)
    Q_PROPERTY(QString Devices READ devices)
    Q_PROPERTY(QString DeviceAction READ deviceAction)
    Q_PROPERTY(QString Settings READ settings)
    Q_PROPERTY(QString Alert READ alert)

public:
    static constexpr auto Service = "io.github.rodiada.Headphones";
    static constexpr auto Path = "/io/github/rodiada/Headphones";

    explicit HeadphonesService(QObject *parent = nullptr);
    ~HeadphonesService() override;

    bool connected() const { return value("Connected").toBool(); }
    QString vendor() const { return value("Vendor").toString(); }
    QString deviceName() const { return value("DeviceName").toString(); }
    QString address() const { return value("Address").toString(); }
    int battery() const { return value("Battery", -1).toInt(); }
    QString batteries() const { return value("Batteries").toString(); }
    QString modes() const { return value("Modes").toString(); }
    QString mode() const { return value("Mode").toString(); }
    int ambientLevel() const { return value("AmbientLevel", 0).toInt(); }
    bool focusOnVoice() const { return value("FocusOnVoice").toBool(); }
    QString devices() const { return value("Devices").toString(); }
    QString deviceAction() const { return value("DeviceAction").toString(); }
    QString settings() const { return value("Settings").toString(); }
    QString alert() const { return value("Alert").toString(); }

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

    QVariant value(const char *key, const QVariant &fallback = {}) const
    {
        return _state.value(QString::fromLatin1(key), fallback);
    }

    void tick();
    void tryConnect();
    void pollConnecting();
    void dropConnection(const QString &reason);
    void publishState();
    Backend *requireReady();
    void reply(const QDBusError &error);
    std::optional<DeviceInfo> findConnectedDevice() const;

    MDRConnectionLinux *_linux = nullptr;
    MDRConnection *_conn = nullptr;
    std::unique_ptr<Backend> _backend;
    Link _link = Link::Idle;
    DeviceInfo _device;

    QTimer _tick;
    QElapsedTimer _sinceConnectAttempt;

    QVariantMap _state;
};
