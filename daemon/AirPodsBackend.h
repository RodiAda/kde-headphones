#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>

#include <optional>

#include "Backend.h"

enum class AirPodsModel {
    AirPods1,
    AirPods2,
    AirPods3,
    AirPods4,
    AirPods4Anc,
    AirPodsPro,
    AirPodsPro2,
    AirPodsPro3,
    AirPodsMax,
    PowerbeatsPro,
    PowerbeatsPro2,
    Powerbeats3,
    Powerbeats4,
    PowerbeatsFit,
    BeatsFitPro,
    BeatsStudioBuds,
    BeatsStudioBudsPlus,
    BeatsSoloBuds,
    BeatsFlex,
    BeatsX,
    BeatsSolo3,
    BeatsSolo4,
    BeatsSoloPro,
    BeatsStudio3,
    BeatsStudioPro,
    Beats360,
    Unknown,
};

struct AirPodsInfo {
    QString name;
    QString modelNumber;
    QString manufacturer;
    QString serialNumber;
    QString firmware;
};

class AirPodsBackend : public Backend
{
public:
    AirPodsBackend(MDRConnection *connection, const DeviceInfo &device);
    ~AirPodsBackend() override;

    static AirPodsModel modelFromNumber(const QString &modelNumber);
    static AirPodsModel modelFromName(const QString &name);
    static QString modelName(AirPodsModel model);
    static bool isBeats(AirPodsModel model);
    static std::optional<AirPodsInfo> parseInfo(const QByteArray &packet);
    static QByteArray handshakePacket();
    static QByteArray featuresPacket();
    static QByteArray notificationsPacket();
    static QByteArray controlPacket(uint8_t id, uint8_t value);

    QString vendor() const override;
    bool start() override;
    bool tick() override;
    bool ready() const override { return _batteryKnown; }
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError apply(const QJsonObject &change) override;

    void handlePacket(const QByteArray &packet);
    AirPodsModel model() const { return _model; }
    QList<QByteArray> takeOutgoing();

private:
    enum class Part { Main, Left, Right, Case, Count };

    struct Battery {
        int level = -1;
        bool charging = false;
        bool present = false;
    };

    enum class Placement { Unknown, InEar, OutOfEar, InCase };

    bool beats() const;
    QString displayName() const;
    bool supports(uint32_t feature) const;
    QStringList modes() const;
    std::optional<uint8_t> control(uint8_t id) const;
    bool controlEnabled(uint8_t id) const;
    bool offersToggle(uint8_t id) const;

    void send(const QByteArray &packet);
    bool connectStep();
    bool receive();
    bool flush();
    void closeSocket();

    void handleBattery(const QByteArray &payload);
    void handleEarDetection(const QByteArray &payload);
    void handleControl(const QByteArray &payload);
    void handleInfo(const QByteArray &packet);

    AirPodsModel _model;
    QString _modelNumber;
    QString _firmware;
    Battery _batteries[int(Part::Count)];
    bool _batteryKnown = false;
    std::optional<Part> _primary;
    Placement _placementPrimary = Placement::Unknown;
    Placement _placementSecondary = Placement::Unknown;
    QHash<uint8_t, uint8_t> _controls;

    int _fd = -1;
    bool _connected = false;
    bool _acknowledged = false;
    bool _registered = false;
    bool _retried = false;
    QList<QByteArray> _tx;
    QElapsedTimer _sinceStart;
    QElapsedTimer _sinceConnect;
};
