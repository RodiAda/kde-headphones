#pragma once

#include <QDBusError>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <mdr-c/Connection.h>

enum class DeviceKind {
    SonyV2,
    SonyV1,
    Galaxy,
    GalaxyClassic,
    GalaxyLegacy,
    AirPods,
    PixelBuds,
    Bose,
    Soundcore,
    Huawei,
    Nothing,
    Jbl,
    Jabra,
    Sennheiser,
    Beats,
    Xiaomi,
};

struct DeviceInfo {
    QString name;
    QString address;
    QString adapterAddress;
    DeviceKind kind = DeviceKind::SonyV2;
    QString serviceUuid;
    bool ownTransport = false;
};

enum class DeviceCommand { Connect, Disconnect, SelectPlayback };

struct Snapshot {
    int battery = -1;
    QJsonArray batteries;
    QStringList modes;
    QString mode;
    int ambientLevel = 0;
    bool focusOnVoice = false;
    QJsonArray devices;
    QJsonObject deviceAction;
    QJsonObject settings;
    QString alert;
};

class Backend
{
public:
    Backend(MDRConnection *connection, const DeviceInfo &device)
        : _conn(connection)
        , _device(device)
    {
    }
    virtual ~Backend() = default;

    virtual QString vendor() const = 0;
    virtual bool start() = 0;
    virtual bool tick() = 0;
    virtual bool ready() const = 0;
    virtual Snapshot snapshot() = 0;
    QString errorString() const { return _error; }

    virtual QDBusError setMode(const QString &mode) = 0;
    virtual QDBusError setAmbientLevel(int level) = 0;
    virtual QDBusError setFocusOnVoice(bool) { return unsupported(); }
    virtual QDBusError deviceCommand(DeviceCommand, const QString &) { return unsupported(); }
    virtual QDBusError apply(const QJsonObject &change) = 0;
    virtual QDBusError playback(const QString &) { return unsupported(); }
    virtual QDBusError powerOff() { return unsupported(); }
    virtual QDBusError respondToAlert(bool) { return unsupported(); }

protected:
    static QDBusError unsupported()
    {
        return QDBusError(QDBusError::NotSupported, QStringLiteral("Not supported by these headphones"));
    }
    bool fail(const QString &error)
    {
        _error = error;
        return false;
    }

    MDRConnection *_conn;
    DeviceInfo _device;
    QString _error;
};

inline const QString ModeNc = QStringLiteral("noise-cancelling");
inline const QString ModeAmbient = QStringLiteral("ambient");
inline const QString ModeOff = QStringLiteral("off");
inline const QString ModeAdaptive = QStringLiteral("adaptive");
