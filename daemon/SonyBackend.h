#pragma once

#include <QElapsedTimer>
#include <QHash>

#include <deque>
#include <functional>
#include <optional>

#include <mdr-c/Headphones.h>

#include "Backend.h"

class SonyBackend : public Backend
{
public:
    SonyBackend(MDRConnection *connection, const DeviceInfo &device);
    ~SonyBackend() override;

    QString vendor() const override { return QStringLiteral("sony"); }
    bool start() override;
    bool tick() override;
    bool ready() const override { return _hp && _initDone; }
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError setFocusOnVoice(bool enabled) override;
    QDBusError deviceCommand(DeviceCommand command, const QString &mac) override;
    QDBusError apply(const QJsonObject &change) override;
    QDBusError playback(const QString &action) override;
    QDBusError powerOff() override;
    QDBusError respondToAlert(bool accept) override;

private:
    struct Pending {
        std::optional<MDRNoiseMode> noiseMode;
        std::optional<int> ambientLevel;
        std::optional<bool> focusOnVoice;
        std::optional<std::pair<MDRPairedDeviceCommand, QByteArray>> deviceCommand;
    };

    bool has(MDRFeature feature) const;
    QString text(MDRText id, uint32_t index = 0) const;
    QStringList availableModes() const;
    QJsonObject settingsJson() const;
    void applyPending();
    void queueOp(std::function<MDRResult()> op);
    void runOps();

    static void onPacket(void *self, MDRPacketDirection direction, const unsigned char *frame, int size);
    void handlePayload(const QByteArray &payload);

    MDRHeadphones *_hp = nullptr;
    Pending _pending;
    Pending _expected;
    QElapsedTimer _sinceExpected;
    QHash<QString, int> _connectionOrder;
    QJsonObject _deviceAction;
    std::deque<std::function<MDRResult()>> _ops;
    QString _alert;
    QJsonObject _settingsCache;
    bool _settingsDirty = true;
    int _lastAmbientLevel = 10;
    QElapsedTimer _sinceSync;
    bool _initDone = false;
    bool _syncWanted = false;
};
