#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>

#include <array>
#include <optional>

#include "Backend.h"

enum class JabraModel {
    Evolve2_55,
    Evolve2_65,
    Evolve2_85,
    Evolve65,
    Unknown,
};

struct JabraModelSpec;

class JabraBackend : public Backend
{
public:
    JabraBackend(MDRConnection *connection, const DeviceInfo &device);

    static JabraModel modelFromName(const QString &name);
    static QByteArray encodeFrame(uint8_t sequence, uint8_t type, uint16_t feature, const QByteArray &data);

    QString vendor() const override { return QStringLiteral("jabra"); }
    bool start() override;
    bool tick() override;
    bool ready() const override { return _batteryAnswered; }
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError apply(const QJsonObject &change) override;

    JabraModel model() const { return _model; }

private:
    struct BatteryPart {
        QString part;
        int level = -1;
        bool charging = false;
    };

    const JabraModelSpec &spec() const;
    QString modelName() const;
    QStringList modes() const;

    void query(uint16_t feature, const QByteArray &data = {});
    void command(uint16_t feature, const QByteArray &data);
    void sendQueries();
    bool flush();
    bool receive();
    void handle(uint8_t sequence, uint8_t type, uint16_t feature, const QByteArray &payload);
    void handleEvent(uint16_t feature, const QByteArray &payload);
    void parseBattery(const QByteArray &p);
    void parseCurve(const QByteArray &p);
    void writeEqualizer(int preset);

    JabraModel _model;
    QByteArray _rx;
    QByteArray _tx;
    uint8_t _sequence = 1;
    QHash<uint8_t, uint16_t> _pending;
    QElapsedTimer _sinceStart;
    QElapsedTimer _sinceKeepalive;
    QElapsedTimer _sinceBattery;
    bool _anyData = false;
    bool _batteryAnswered = false;

    QString _product;
    QString _firmware;
    int _battery = -1;
    bool _charging = false;
    QList<BatteryPart> _parts;
    std::optional<uint8_t> _noise;
    std::optional<bool> _eqEnabled;
    std::optional<std::array<int16_t, 5>> _curve;
    std::optional<bool> _muteReminder;
    std::optional<bool> _autoReject;
    std::optional<bool> _buttonSounds;
    std::optional<bool> _sidetone;
    int _sidetoneLevel = 3;
    std::optional<uint8_t> _guidance;
    std::optional<int> _sleepMinutes;
    std::optional<uint8_t> _callEq;
};
