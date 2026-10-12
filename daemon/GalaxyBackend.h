#pragma once

#include <QByteArray>
#include <QElapsedTimer>

#include <optional>

#include "Backend.h"

class GalaxyBackend : public Backend
{
public:
    GalaxyBackend(MDRConnection *connection, const DeviceInfo &device);

    QString vendor() const override { return QStringLiteral("samsung"); }
    bool start() override;
    bool tick() override;
    bool ready() const override { return _status.has_value(); }
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError apply(const QJsonObject &change) override;

private:
    struct Status {
        int revision = 0;
        int batteryLeft = 0;
        int batteryRight = 0;
        int batteryCase = 0;
        int placementLeft = 0;
        int placementRight = 0;
        bool chargingLeft = false;
        bool chargingRight = false;
        bool chargingCase = false;
        int equalizer = 0;
        uint8_t touchFlags = 0;
        int noiseMode = 0;
        bool seamless = false;
        int ambientVolume = 0;
    };

    void send(uint8_t id, const QByteArray &payload);
    bool flush();
    bool receive();
    void handle(uint8_t id, const QByteArray &payload);
    void parseExtendedStatus(const QByteArray &p);
    void parseStatus(const QByteArray &p);
    int ambientMax() const;

    QByteArray _rx;
    QByteArray _tx;
    std::optional<Status> _status;
    bool _managerInfoSent = false;
    QElapsedTimer _sinceStart;
};
