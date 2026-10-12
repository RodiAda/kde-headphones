#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>

#include <optional>

#include "Backend.h"

enum class SoundcoreFamily {
    LifeQ30,
    LifeQ35,
    SpaceQ45,
    LifeNote3,
    LifeNote3S,
    LifeDot2Nc,
    SpaceA40,
    LibertyAir2Pro,
    Liberty4Nc,
    P30i,
    Unknown,
};

class SoundcoreBackend : public Backend
{
public:
    SoundcoreBackend(MDRConnection *connection, const DeviceInfo &device);

    static QByteArray frame(uint16_t command, const QByteArray &body);
    static SoundcoreFamily familyFromName(const QString &name);

    QString vendor() const override { return QStringLiteral("soundcore"); }
    bool start() override;
    bool tick() override;
    bool ready() const override { return _status.has_value(); }
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError apply(const QJsonObject &change) override;

    SoundcoreFamily family() const { return _family; }
    QString modelName() const { return _modelName; }

private:
    struct Flag {
        const char *key;
        uint16_t command;
        bool inverted;
        bool value;
    };

    struct Status {
        int level = -1;
        bool charging = false;
        int left = -1;
        int right = -1;
        bool chargingLeft = false;
        bool chargingRight = false;
        int caseLevel = -1;
        bool twsConnected = true;
        int host = 0;
        QByteArray sound;
        int eqPreset = 0;
        QByteArray eq;
        uint8_t gender = 0;
        uint8_t ageRange = 0xFF;
        bool hearIdEnabled = false;
        QByteArray hearIdAdjustments;
        QByteArray hearIdTime;
        uint8_t hearIdType = 0;
        QByteArray hearIdCustom;
        QByteArray hearIdGenre;
        QList<Flag> flags;
    };

    struct Packet {
        uint16_t command;
        QByteArray frame;
    };

    void send(uint16_t command, const QByteArray &body);
    bool flush();
    bool receive();
    void handle(uint16_t command, const QByteArray &body);
    std::optional<Status> parseState(const QByteArray &body) const;
    void identify(const QByteArray &body);

    QString currentMode() const;
    QStringList modes() const;
    int soundSize() const;
    void sendSound(const QByteArray &sound);
    void setSoundField(int index, uint8_t value);
    void applyEqualizer(int presetId, const QList<int> &values);

    SoundcoreFamily _family;
    QString _modelName;
    QByteArray _rx;
    QByteArray _tx;
    QList<Packet> _queue;
    std::optional<uint16_t> _awaiting;
    QElapsedTimer _sinceSent;
    QElapsedTimer _sinceStart;
    QElapsedTimer _sinceRequest;
    std::optional<Status> _status;
};
