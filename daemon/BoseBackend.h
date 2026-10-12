#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QVector>

#include <deque>
#include <optional>

#include "Backend.h"

enum class BoseNoise {
    None,
    Anr,
    Cnc,
    AudioModes,
};

struct BoseModel {
    uint16_t productId;
    const char *name;
    bool anc;
    bool earbuds;
    int cncWrites;
};

class BoseBackend : public Backend
{
public:
    BoseBackend(MDRConnection *connection, const DeviceInfo &device);

    static const BoseModel *modelForProductId(uint16_t productId);
    static const BoseModel *modelForName(const QString &name);
    static QByteArray packet(uint8_t block, uint8_t function, uint8_t op, const QByteArray &payload = {});

    QString vendor() const override { return QStringLiteral("bose"); }
    bool start() override;
    bool tick() override;
    bool ready() const override;
    Snapshot snapshot() override;

    QDBusError setMode(const QString &mode) override;
    QDBusError setAmbientLevel(int level) override;
    QDBusError apply(const QJsonObject &change) override;

private:
    struct Request {
        uint8_t block;
        uint8_t function;
        uint8_t op;
        QByteArray payload;
    };

    struct Battery {
        int id;
        int level;
    };

    struct AudioMode {
        int index;
        int prompt;
        QString name;
    };

    struct AudioSettings {
        int cnc = 0;
        int autoCnc = 0;
        int spatial = 0;
        bool wind = false;
        bool anc = true;
    };

    struct EqBand {
        int min;
        int max;
        int value;
    };

    BoseNoise noise() const;
    bool earbuds() const;
    QString modelName() const;
    QStringList modes() const;
    QString currentMode() const;
    int quietIndex() const;
    int awareIndex() const;
    bool cncWritable() const;
    QList<int> autoOffOptions() const;

    void enqueue(uint8_t block, uint8_t function, uint8_t op, const QByteArray &payload = {});
    void pump();
    bool flush();
    bool receive();
    void handle(uint8_t block, uint8_t function, uint8_t op, const QByteArray &payload);
    void handleSettings(uint8_t function, const QByteArray &payload);
    void handleAudioModes(uint8_t function, uint8_t op, const QByteArray &payload);
    void parseBattery(const QByteArray &payload);
    void probeAudioModes();
    void refresh();

    void writeCnc(int raw);
    void writeAudioSettings(const AudioSettings &settings);
    void startMode(int index);
    QDBusError applyToggle(const QString &key, bool value);
    QDBusError applySlider(const QString &key, int value);
    QDBusError applyChoice(const QString &key, const QString &value);

    const BoseModel *_model = nullptr;
    QByteArray _rx;
    QByteArray _tx;
    std::deque<Request> _queue;
    std::optional<Request> _inFlight;
    QElapsedTimer _inFlightTimer;
    QElapsedTimer _sinceStart;
    QElapsedTimer _sinceRefresh;
    bool _settled = false;

    QVector<Battery> _batteries;
    std::optional<int> _anr;
    std::optional<int> _cncRaw;
    int _cncSteps = 11;
    int _lastCncRaw = 0;
    std::optional<AudioSettings> _audioSettings;
    std::optional<int> _currentMode;
    QVector<AudioMode> _audioModes;
    QVector<std::optional<EqBand>> _eq = QVector<std::optional<EqBand>>(3);
    std::optional<uint8_t> _voicePrompts;
    std::optional<uint8_t> _multipoint;
    std::optional<int> _autoOff;
    std::optional<int> _sidetone;
    std::optional<bool> _autoPause;
    std::optional<bool> _autoAnswer;
    int _lastAnr = 1;
};
