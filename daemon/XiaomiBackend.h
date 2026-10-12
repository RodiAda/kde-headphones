#pragma once

#include <QByteArray>
#include <QElapsedTimer>

#include <array>
#include <optional>

#include "Backend.h"

enum class XiaomiModel {
    RedmiBuds3Pro,
    RedmiBuds4Active,
    RedmiBuds5Pro,
    RedmiBuds6Pro,
    RedmiBuds6Active,
    RedmiBuds6Lite,
    RedmiBuds6,
    RedmiBuds8Active,
    Unknown,
};

struct XiaomiModelSpec;

class XiaomiBackend : public Backend
{
public:
    XiaomiBackend(MDRConnection *connection, const DeviceInfo &device);

    static XiaomiModel modelFromName(const QString &name);
    static QString modelName(XiaomiModel model);
    static QByteArray encodeFrame(uint8_t type, uint8_t opcode, uint8_t sequence, const QByteArray &payload);
    static std::array<uint8_t, 16> challengeResponse(const std::array<uint8_t, 16> &challenge);

    QString vendor() const override { return QStringLiteral("xiaomi"); }
    bool start() override;
    bool tick() override;
    bool ready() const override { return _battery.has_value(); }
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError apply(const QJsonObject &change) override;

    XiaomiModel model() const { return _model; }
    bool authenticated() const { return _authenticated; }

private:
    struct Battery {
        int left = -1;
        int right = -1;
        int caseLevel = -1;
        bool chargingLeft = false;
        bool chargingRight = false;
        bool chargingCase = false;
    };

    const XiaomiModelSpec &spec() const;
    QStringList modes() const;
    QString currentMode() const;

    void request(uint8_t opcode, const QByteArray &payload);
    void respond(uint8_t opcode, uint8_t sequence, const QByteArray &payload = {});
    void setConfig(uint8_t key, const QByteArray &value);
    void setAnc(uint8_t mode);
    void sendQueries();
    bool flush();
    bool receive();
    void handle(uint8_t type, uint8_t opcode, uint8_t sequence, const QByteArray &payload);
    void handleAuth(uint8_t type, uint8_t opcode, uint8_t sequence, const QByteArray &payload);
    void parseDeviceInfo(const QByteArray &p);
    void parseRunInfo(const QByteArray &p);
    void parseStatus(const QByteArray &p);
    void parseConfig(const QByteArray &p, bool notify);
    void parseBattery(const QByteArray &p, int offset);

    XiaomiModel _model;
    QByteArray _rx;
    QByteArray _tx;
    uint8_t _sequence = 0;
    QElapsedTimer _sinceStart;
    QElapsedTimer _sincePoll;
    bool _authenticated = false;

    std::optional<Battery> _battery;
    std::optional<uint8_t> _anc;
    std::optional<uint8_t> _ancStrength;
    uint8_t _manualStrength;
    std::optional<uint8_t> _ambientStrength;
    std::optional<bool> _adaptiveAnc;
    std::optional<int> _equalizer;
    std::optional<bool> _earDetection;
    std::optional<bool> _autoAnswer;
    std::optional<bool> _multipoint;
    std::optional<bool> _adaptiveSound;
    QString _firmware;
};
