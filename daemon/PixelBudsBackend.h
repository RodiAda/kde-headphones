#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>

#include <array>
#include <cstdint>
#include <optional>

#include "Backend.h"

namespace maestro {

struct Frame {
    uint32_t address = 0;
    uint8_t control = 0;
    QByteArray data;
};

struct RpcPacket {
    uint32_t type = 0;
    uint32_t channel = 0;
    uint32_t service = 0;
    uint32_t method = 0;
    QByteArray payload;
    uint32_t status = 0;
    uint32_t callId = 0;

    QByteArray encode() const;
    static std::optional<RpcPacket> decode(const QByteArray &data);
};

uint32_t crc32(const QByteArray &data);
uint32_t idHash(const char *name);
uint32_t channelAddress(uint32_t channel);
QByteArray encodeFrame(const Frame &frame);
std::optional<Frame> decodeFrame(const QByteArray &escaped);
QList<QByteArray> takeFrames(QByteArray &buffer);

}

class PixelBudsBackend : public Backend
{
public:
    PixelBudsBackend(MDRConnection *connection, const DeviceInfo &device);

    static QString modelName(const QString &deviceName);

    QString vendor() const override { return QStringLiteral("google"); }
    bool start() override;
    bool tick() override;
    bool ready() const override { return _channel != 0 && _runtimeKnown && _readsDone; }
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError apply(const QJsonObject &change) override;

private:
    struct Battery {
        int level = -1;
        bool charging = false;
    };

    QStringList modes() const;
    void send(uint32_t channel, uint32_t method, const QByteArray &payload);
    void begin(uint32_t channel);
    void readNext();
    void writeSetting(const QByteArray &settingValue);
    bool flush();
    bool receive();
    void handle(const maestro::RpcPacket &packet);
    void parseRuntimeInfo(const QByteArray &p);
    void parseSettingsRsp(const QByteArray &p);
    void parseSettingValue(const QByteArray &p);

    QString _model;
    QByteArray _rx;
    QByteArray _tx;
    QElapsedTimer _sinceStart;
    QElapsedTimer _sinceRead;
    uint32_t _channel = 0;
    bool _probed = false;
    bool _runtimeKnown = false;
    bool _readsDone = false;
    QList<int> _pendingReads;
    int _reading = 0;

    Battery _left;
    Battery _right;
    Battery _case;
    std::optional<int> _anc;
    bool _loopAdaptive = false;
    std::optional<bool> _earDetection;
    std::optional<bool> _touchControls;
    std::optional<bool> _multipoint;
    std::optional<bool> _volumeEq;
    std::optional<bool> _mono;
    std::optional<bool> _speechDetection;
    std::optional<int> _balance;
    std::optional<std::array<float, 5>> _eq;
};
