#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QMap>

#include <deque>
#include <optional>

#include "Backend.h"

enum class SennheiserModel {
    Momentum4,
    Momentum5,
    Unknown,
};

class SennheiserBackend : public Backend
{
public:
    SennheiserBackend(MDRConnection *connection, const DeviceInfo &device);

    static SennheiserModel modelFromId(const QString &id);
    static SennheiserModel modelFromName(const QString &name);
    static QString modelName(SennheiserModel model);
    static QByteArray encodeFrame(uint16_t vendor, uint16_t command, const QByteArray &payload);

    QString vendor() const override { return QStringLiteral("sennheiser"); }
    bool start() override;
    bool tick() override;
    bool ready() const override { return _battery.has_value(); }
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError apply(const QJsonObject &change) override;

    SennheiserModel model() const { return _model; }

private:
    struct Request {
        uint16_t command;
        QByteArray payload;
    };

    struct Equalizer {
        int bands = 0;
        int min = 0;
        int max = 0;
        QList<int> gains;
    };

    void enqueue(uint16_t command, const QByteArray &payload = {});
    void enqueueNoiseReads();
    void sendQueries();
    void pump();
    bool flush();
    bool receive();
    void handleFrame(uint16_t vendor, uint16_t command, const QByteArray &payload);
    void handleState(uint16_t getter, const QByteArray &payload, bool notification);
    void parseAncModes(const QByteArray &payload);

    QStringList modes() const;
    QString currentMode() const;
    QStringList windOptions() const;
    QDBusError applyToggle(const QString &key, bool value);
    QDBusError applySlider(const QString &key, int value);
    QDBusError applyChoice(const QString &key, const QString &value);
    QDBusError applyPreset(int id);
    void writeAncMode(uint8_t id, uint8_t value);

    SennheiserModel _model;
    QByteArray _rx;
    QByteArray _tx;
    std::deque<Request> _queue;
    std::optional<uint16_t> _pending;
    QElapsedTimer _pendingSince;
    QElapsedTimer _sinceStart;
    QElapsedTimer _sincePoll;
    qint64 _lastBatteryPoll = 0;
    bool _queriesSent = false;

    std::optional<int> _battery;
    bool _charging = false;
    QString _firmware;
    std::optional<bool> _ancEnabled;
    std::optional<bool> _transparent;
    std::optional<int> _ancLevel;
    QList<QPair<uint8_t, uint8_t>> _ancModes;
    std::optional<int> _soundMode;
    std::optional<Equalizer> _eq;
    QMap<uint16_t, bool> _switches;
    std::optional<int> _prompts;
    std::optional<int> _autoOff;
};
