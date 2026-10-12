#pragma once

#include <QByteArray>
#include <QElapsedTimer>

#include <optional>

#include "Backend.h"

enum class GalaxyModel {
    Buds,
    BudsPlus,
    BudsLive,
    BudsPro,
    Buds2,
    Buds2Pro,
    BudsFe,
    Buds3,
    Buds3Pro,
    Buds3Fe,
    BudsCore,
    Buds4,
    Buds4Pro,
    Unknown,
};

class GalaxyBackend : public Backend
{
public:
    GalaxyBackend(MDRConnection *connection, const DeviceInfo &device);

    static GalaxyModel modelFromName(const QString &name, DeviceKind kind);
    static QString modelName(GalaxyModel model);

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
        int batteryCase = -1;
        bool leftPresent = false;
        bool rightPresent = false;
        bool chargingLeft = false;
        bool chargingRight = false;
        bool chargingCase = false;
        int equalizer = 0;
        bool touchLocked = false;
        uint8_t touchFlags = 0;
        int noiseMode = 0;
        bool seamless = false;
        bool seamlessKnown = false;
        int ambientVolume = 0;
        bool wearingLeft = false;
        bool wearingRight = false;
        bool oneEarbud = false;
        bool oneEarbudKnown = false;
    };

    bool legacyFraming() const { return _model == GalaxyModel::Buds; }
    bool combinedNoiseControl() const;
    bool hasAnc() const;
    bool hasAmbient() const;
    bool advancedTouchLock() const;
    int ambientMax() const;
    QStringList modes() const;

    void send(uint8_t id, const QByteArray &payload);
    bool flush();
    bool receive();
    void handle(uint8_t id, const QByteArray &payload);
    void parseExtendedStatus(const QByteArray &p);
    void parseStatus(const QByteArray &p);

    GalaxyModel _model;
    QByteArray _rx;
    QByteArray _tx;
    std::optional<Status> _status;
    bool _managerInfoSent = false;
    QElapsedTimer _sinceStart;
};
