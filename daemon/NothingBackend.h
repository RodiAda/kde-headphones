#pragma once

#include <QByteArray>
#include <QElapsedTimer>

#include <optional>

#include "Backend.h"

enum class NothingModel {
    Ear1,
    Ear2,
    EarStick,
    EarA,
    Ear,
    EarOpen,
    Ear3,
    Ear3A,
    Headphone1,
    Headphone1Pro,
    HeadphoneA,
    CmfBuds,
    CmfBudsPro,
    CmfBudsPro2,
    CmfBuds2,
    CmfBuds2Plus,
    CmfBuds2A,
    CmfNeckbandPro,
    CmfHeadphonePro,
    Unknown,
};

struct NothingModelSpec;

class NothingBackend : public Backend
{
public:
    NothingBackend(MDRConnection *connection, const DeviceInfo &device);

    static NothingModel modelFromName(const QString &name);
    static QString modelName(NothingModel model);
    static QByteArray encodeFrame(uint16_t command, uint8_t operation, const QByteArray &payload);
    static uint16_t crc16(const QByteArray &data);

    QString vendor() const override { return QStringLiteral("nothing"); }
    bool start() override;
    bool tick() override;
    bool ready() const override { return _battery.has_value(); }
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError apply(const QJsonObject &change) override;

    NothingModel model() const { return _model; }

private:
    struct Battery {
        int left = -1;
        int right = -1;
        int caseLevel = -1;
        int main = -1;
        bool chargingLeft = false;
        bool chargingRight = false;
        bool chargingCase = false;
        bool chargingMain = false;
    };

    const NothingModelSpec &spec() const;
    QStringList modes() const;
    QStringList strengths() const;
    uint8_t strengthCode() const;

    void send(uint16_t command, const QByteArray &payload = {});
    void sendQueries();
    bool flush();
    bool receive();
    void handle(uint16_t command, const QByteArray &payload);
    void parseBattery(const QByteArray &p);
    void parseNoiseControl(const QByteArray &p);
    void parseDeviceInfo(const QByteArray &p);

    NothingModel _model;
    QByteArray _rx;
    QByteArray _tx;
    uint8_t _operation = 0;
    QElapsedTimer _sinceStart;
    QElapsedTimer _sincePoll;
    qint64 _lastBatteryPoll = 0;
    bool _queriesSent = false;

    std::optional<Battery> _battery;
    std::optional<uint8_t> _noise;
    uint8_t _strength = 0;
    std::optional<int> _equalizer;
    std::optional<bool> _inEar;
    std::optional<bool> _lowLatency;
    std::optional<bool> _bassBoost;
    uint8_t _bassLevel = 0;
    QString _firmware;
};
