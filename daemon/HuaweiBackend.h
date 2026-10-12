#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QMap>

#include <deque>
#include <functional>
#include <optional>

#include "Backend.h"

enum class HuaweiModel {
    FreeBuds3,
    FreeBuds4,
    FreeBuds4i,
    FreeBuds5,
    FreeBuds5i,
    FreeBuds6,
    FreeBuds6i,
    FreeBudsPro,
    FreeBudsPro2,
    FreeBudsPro3,
    FreeBudsPro4,
    FreeBudsPro5,
    FreeBudsSe,
    FreeBudsSe2,
    FreeBudsSe4,
    FreeBudsStudio,
    FreeClip,
    FreeClip2,
    FreeLacePro,
    FreeLacePro2,
    HonorEarbuds2,
    Unknown,
};

class HuaweiBackend : public Backend
{
public:
    struct Packet {
        uint16_t id = 0;
        QMap<int, QByteArray> tlv;

        bool has(int type) const { return tlv.contains(type); }
        QByteArray value(int type) const { return tlv.value(type); }
    };

    HuaweiBackend(MDRConnection *connection, const DeviceInfo &device);

    static HuaweiModel modelFromName(const QString &name);
    static HuaweiModel modelFromInfo(const QString &sku, const QString &modelId);
    static QString modelName(HuaweiModel model);
    static uint16_t crc16(const QByteArray &data);
    static QByteArray encodeFrame(const QByteArray &payload);
    static QByteArray encodePacket(uint16_t id, const QList<std::pair<int, QByteArray>> &tlv);
    static std::optional<Packet> decodePayload(const QByteArray &payload);

    QString vendor() const override { return QStringLiteral("huawei"); }
    bool start() override;
    bool tick() override;
    bool ready() const override { return _battery.has_value(); }
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError apply(const QJsonObject &change) override;

    HuaweiModel model() const { return _model; }

private:
    struct Battery {
        int overall = -1;
        int left = -1;
        int right = -1;
        int caseLevel = -1;
        bool chargingLeft = false;
        bool chargingRight = false;
        bool chargingCase = false;
        bool chargingAny = false;
    };

    struct Anc {
        int mode = 0;
        int level = 0;
    };

    struct Request {
        QByteArray frame;
        uint16_t reply = 0;
        qint64 delayMs = 0;
        std::function<void(bool)> done;
    };

    bool has(uint32_t feature) const;
    QStringList modes() const;
    QList<int> ancLevels() const;
    QString ancLevelKey(int level) const;

    void request(uint16_t id, const QList<std::pair<int, QByteArray>> &tlv, std::function<void(bool)> done = {}, qint64 delayMs = 0);
    void read(uint16_t id, const QList<int> &types, std::function<void(bool)> done = {}, qint64 delayMs = 0);
    void queueFeatureReads();
    void pump();
    bool flush();
    bool receive();
    void handle(const Packet &p);
    void writeAnc(int mode, int level);

    HuaweiModel _model;
    QString _modelId;
    QByteArray _rx;
    QByteArray _tx;
    QByteArray _fragments;
    bool _fragmenting = false;
    int _fragmentIndex = 0;
    std::deque<Request> _queue;
    std::optional<Request> _inflight;
    QElapsedTimer _inflightTimer;
    QElapsedTimer _sinceIdle;
    QElapsedTimer _sinceStart;
    QElapsedTimer _sinceBattery;

    std::optional<Battery> _battery;
    std::optional<Anc> _anc;
    std::optional<bool> _wearDetection;
    std::optional<bool> _lowLatency;
    std::optional<bool> _multipoint;
    std::optional<int> _equalizer;
    QList<int> _equalizerPresets;
};
