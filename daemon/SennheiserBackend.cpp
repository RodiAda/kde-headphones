#include "SennheiserBackend.h"

#include <algorithm>
#include <cmath>

#include "Log.h"

namespace {

constexpr uint8_t Sof = 0xFF;
constexpr uint8_t TxVersion = 0x03;
constexpr uint8_t FlagChecksum = 0x01;
constexpr uint8_t FlagLongLength = 0x02;
constexpr int MaxPayload = 4096;

constexpr uint16_t VendorSennheiser = 0x0495;
constexpr uint16_t TypeMask = 0x0180;
constexpr int TypeNotification = 1;
constexpr int TypeResponse = 2;
constexpr int TypeError = 3;

constexpr uint16_t RegisterNotification = 0x0007;
constexpr uint16_t SetTimer = 0x0600;
constexpr uint16_t GetTimer = 0x0601;
constexpr uint16_t GetCharging = 0x0602;
constexpr uint16_t GetBattery = 0x0603;
constexpr uint16_t SetPromptMode = 0x0801;
constexpr uint16_t GetPromptMode = 0x0802;
constexpr uint16_t SetSoundMode = 0x0803;
constexpr uint16_t GetSoundMode = 0x0804;
constexpr uint16_t GetEqConfig = 0x1000;
constexpr uint16_t SetEqBand = 0x1001;
constexpr uint16_t GetEqBand = 0x1002;
constexpr uint16_t GetEqGains = 0x1003;
constexpr uint16_t GetFirmware = 0x1201;
constexpr uint16_t GetModelId = 0x1206;
constexpr uint16_t SetTransparent = 0x1804;
constexpr uint16_t GetTransparent = 0x1805;
constexpr uint16_t SetAncMode = 0x1A00;
constexpr uint16_t GetAncModes = 0x1A01;
constexpr uint16_t SetAncLevel = 0x1A02;
constexpr uint16_t GetAncLevel = 0x1A03;
constexpr uint16_t SetAncEnabled = 0x1A04;
constexpr uint16_t GetAncEnabled = 0x1A05;

constexpr uint8_t FeatureBattery = 3;
constexpr uint8_t FeatureEqualizer = 8;
constexpr uint8_t FeatureMmi = 11;
constexpr uint8_t FeatureTransparent = 12;
constexpr uint8_t FeatureAnc = 13;

constexpr uint8_t AncAntiWind = 1;
constexpr uint8_t AncAdaptive = 3;

constexpr uint16_t TimerAutoPowerOff = 0;
constexpr int AutoOffSeconds[] = {0, 900, 1800, 3600};

constexpr int SoundModePersonalized = 3;

constexpr qint64 RequestTimeoutMs = 2000;
constexpr qint64 StatusTimeoutMs = 10000;
constexpr qint64 NoisePollMs = 5000;
constexpr qint64 BatteryPollMs = 60000;

struct Switch {
    const char *key;
    uint16_t getter;
    uint16_t setter;
    bool inverted;
    bool spatial;
};

constexpr Switch Switches[] = {
    {"wearDetection", 0x0401, 0x0400, false, false},
    {"touchControls", 0x1607, 0x1606, true, false},
    {"bassBoost", 0x1009, 0x1008, false, false},
    {"smartPause", 0x080D, 0x080C, false, false},
    {"autoAnswer", 0x080B, 0x080A, false, false},
    {"comfortCall", 0x0815, 0x0814, false, false},
    {"batteryProtection", 0x060B, 0x060A, false, false},
    {"spatialAudio", 0x3605, 0x3604, false, true},
    {"headTracking", 0x3603, 0x3602, false, true},
};

struct Preset {
    int id;
    const char *key;
};

constexpr Preset Presets[] = {
    {0, "off"},
    {1, "custom"},
    {2, "podcast"},
};

const char *const FiveBandKeys[] = {"eqLowBass", "eqBass", "eqMid", "eqTreble", "eqUpperTreble"};
const char *const PromptKeys[] = {"off", "tones", "voice"};

uint8_t at(const QByteArray &p, int i)
{
    return i >= 0 && i < p.size() ? uint8_t(p.at(i)) : 0;
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

QDBusError invalid(const QString &message)
{
    return QDBusError(QDBusError::InvalidArgs, message);
}

QJsonObject control(const QString &key, const QJsonValue &value)
{
    return QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("value"), value}};
}

const Switch *findSwitch(const QString &key)
{
    for (const Switch &s : Switches) {
        if (key == QLatin1String(s.key))
            return &s;
    }
    return nullptr;
}

const Switch *switchByGetter(uint16_t getter)
{
    for (const Switch &s : Switches) {
        if (s.getter == getter)
            return &s;
    }
    return nullptr;
}

QString autoOffName(int seconds)
{
    return seconds == 0 ? QStringLiteral("never") : QStringLiteral("%1-min").arg(seconds / 60);
}

QString bandKey(int band, int count)
{
    if (count == int(std::size(FiveBandKeys)))
        return QString::fromLatin1(FiveBandKeys[band]);
    return QStringLiteral("eqBand%1").arg(band + 1);
}

int ceilDb(int tenths)
{
    return int(std::ceil(tenths / 10.0));
}

int floorDb(int tenths)
{
    return int(std::floor(tenths / 10.0));
}

}

SennheiserModel SennheiserBackend::modelFromId(const QString &id)
{
    const QString trimmed = id.trimmed();
    const auto matches = [&](QLatin1String prefix) { return trimmed == prefix || trimmed.startsWith(prefix + QLatin1Char(' ')); };
    if (matches(QLatin1String("M4AEBT")))
        return SennheiserModel::Momentum4;
    if (matches(QLatin1String("M5AEBT")))
        return SennheiserModel::Momentum5;
    return SennheiserModel::Unknown;
}

SennheiserModel SennheiserBackend::modelFromName(const QString &name)
{
    const QString simplified = name.simplified();
    if (simplified.contains(QLatin1String("MOMENTUM 4"), Qt::CaseInsensitive))
        return SennheiserModel::Momentum4;
    if (simplified.contains(QLatin1String("MOMENTUM 5"), Qt::CaseInsensitive))
        return SennheiserModel::Momentum5;
    return SennheiserModel::Unknown;
}

QString SennheiserBackend::modelName(SennheiserModel model)
{
    switch (model) {
    case SennheiserModel::Momentum4:
        return QStringLiteral("Sennheiser MOMENTUM 4");
    case SennheiserModel::Momentum5:
        return QStringLiteral("Sennheiser MOMENTUM 5");
    default:
        return QStringLiteral("Sennheiser");
    }
}

QByteArray SennheiserBackend::encodeFrame(uint16_t vendor, uint16_t command, const QByteArray &payload)
{
    QByteArray frame;
    frame.append(char(Sof));
    frame.append(char(TxVersion));
    frame.append(char(0));
    frame.append(char(payload.size()));
    frame.append(char(vendor >> 8));
    frame.append(char(vendor & 0xFF));
    frame.append(char(command >> 8));
    frame.append(char(command & 0xFF));
    frame.append(payload);
    return frame;
}

SennheiserBackend::SennheiserBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
    , _model(modelFromName(device.name))
{
}

bool SennheiserBackend::start()
{
    _sinceStart.start();
    _sincePoll.start();
    qCInfo(lcHp) << "sennheiser model from name:" << modelName(_model);
    enqueue(GetModelId);
    enqueue(GetFirmware);
    pump();
    return true;
}

void SennheiserBackend::enqueue(uint16_t command, const QByteArray &payload)
{
    _queue.push_back({command, payload.left(255)});
}

void SennheiserBackend::enqueueNoiseReads()
{
    enqueue(GetAncEnabled);
    enqueue(GetTransparent);
    enqueue(GetAncModes);
    enqueue(GetAncLevel);
}

void SennheiserBackend::sendQueries()
{
    _queriesSent = true;
    for (const uint8_t feature : {FeatureBattery, FeatureAnc, FeatureTransparent, FeatureEqualizer, FeatureMmi})
        enqueue(RegisterNotification, QByteArray(1, char(feature)));
    enqueue(GetBattery);
    enqueue(GetCharging);
    enqueueNoiseReads();
    enqueue(GetSoundMode);
    enqueue(GetEqConfig);
    enqueue(GetEqGains, QByteArray(1, char(0)));
    enqueue(GetPromptMode);
    enqueue(GetTimer, QByteArray(1, char(TimerAutoPowerOff)));
    for (const Switch &s : Switches) {
        if (s.spatial && _model == SennheiserModel::Momentum4)
            continue;
        enqueue(s.getter);
    }
}

void SennheiserBackend::pump()
{
    if (_pending && _pendingSince.elapsed() > RequestTimeoutMs) {
        qCDebug(lcHp) << "sennheiser no answer to" << Qt::hex << *_pending;
        _pending.reset();
    }
    if (_pending)
        return;
    if (_queue.empty()) {
        if (!_queriesSent)
            sendQueries();
        if (_queue.empty())
            return;
    }
    const Request r = _queue.front();
    _queue.pop_front();
    _tx.append(encodeFrame(VendorSennheiser, r.command, r.payload));
    _pending = r.command;
    _pendingSince.restart();
    qCDebug(lcHp) << "sennheiser tx" << Qt::hex << r.command << r.payload.toHex(' ');
}

bool SennheiserBackend::flush()
{
    while (!_tx.isEmpty()) {
        int sent = 0;
        const MDRResult r = mdrConnectionSend(_conn, _tx.constData(), int(_tx.size()), &sent);
        if (r == MDR_RESULT_INPROGRESS)
            return true;
        if (r != MDR_RESULT_OK)
            return fail(QString::fromUtf8(mdrConnectionGetLastError(_conn)));
        _tx.remove(0, sent);
    }
    return true;
}

bool SennheiserBackend::receive()
{
    char buffer[1024];
    for (;;) {
        int received = 0;
        const MDRResult r = mdrConnectionRecv(_conn, buffer, sizeof(buffer), &received);
        if (r == MDR_RESULT_INPROGRESS)
            break;
        if (r != MDR_RESULT_OK)
            return fail(QString::fromUtf8(mdrConnectionGetLastError(_conn)));
        _rx.append(buffer, received);
    }

    for (;;) {
        const qsizetype start = _rx.indexOf(char(Sof));
        if (start < 0) {
            _rx.clear();
            break;
        }
        _rx.remove(0, start);
        if (_rx.size() < 4)
            break;
        const uint8_t version = at(_rx, 1);
        const uint8_t flags = at(_rx, 2);
        if ((version != 1 && version != 3 && version != 4) || (flags & ~(FlagChecksum | FlagLongLength))) {
            _rx.remove(0, 1);
            continue;
        }
        int header = 4;
        int length = at(_rx, 3);
        if (flags & FlagLongLength) {
            if (_rx.size() < 5)
                break;
            header = 5;
            length = (at(_rx, 3) << 8) | at(_rx, 4);
            if (length > MaxPayload) {
                _rx.remove(0, 1);
                continue;
            }
        }
        const bool checksum = flags & FlagChecksum;
        const qsizetype total = header + 4 + length + (checksum ? 1 : 0);
        if (_rx.size() < total)
            break;
        if (checksum) {
            uint8_t x = 0;
            for (qsizetype i = 0; i < total - 1; i++)
                x ^= uint8_t(_rx.at(i));
            if (x != at(_rx, int(total) - 1)) {
                _rx.remove(0, 1);
                continue;
            }
        }
        const uint16_t vendor = uint16_t((at(_rx, header) << 8) | at(_rx, header + 1));
        const uint16_t command = uint16_t((at(_rx, header + 2) << 8) | at(_rx, header + 3));
        const QByteArray payload = _rx.mid(header + 4, length);
        _rx.remove(0, total);
        qCDebug(lcHp) << "sennheiser rx" << Qt::hex << vendor << command << payload.toHex(' ');
        handleFrame(vendor, command, payload);
    }
    return true;
}

bool SennheiserBackend::tick()
{
    if (!receive())
        return false;
    if (ready() && _queue.empty() && !_pending && _sincePoll.elapsed() > NoisePollMs) {
        _sincePoll.restart();
        if (_ancEnabled)
            enqueueNoiseReads();
        if (_sinceStart.elapsed() - _lastBatteryPoll > BatteryPollMs) {
            _lastBatteryPoll = _sinceStart.elapsed();
            enqueue(GetBattery);
            enqueue(GetCharging);
        }
    }
    pump();
    if (!flush())
        return false;
    if (!ready() && _sinceStart.elapsed() > StatusTimeoutMs)
        return fail(QStringLiteral("no status from the headphones"));
    return true;
}

void SennheiserBackend::handleFrame(uint16_t vendor, uint16_t command, const QByteArray &payload)
{
    if (vendor != VendorSennheiser)
        return;
    const int type = (command >> 7) & 0x03;
    const uint16_t base = command & ~TypeMask;
    if (type == TypeResponse || type == TypeError) {
        if (_pending && *_pending == base)
            _pending.reset();
        if (type == TypeError) {
            qCDebug(lcHp) << "sennheiser error for" << Qt::hex << base << payload.toHex(' ');
            return;
        }
        handleState(base, payload, false);
    } else if (type == TypeNotification) {
        handleState(base, payload, true);
    }
    pump();
}

void SennheiserBackend::parseAncModes(const QByteArray &p)
{
    if (p.isEmpty() || p.size() % 2)
        return;
    QList<QPair<uint8_t, uint8_t>> modes;
    for (int i = 0; i < p.size(); i += 2)
        modes.append({at(p, i), at(p, i + 1)});
    _ancModes = modes;
}

void SennheiserBackend::handleState(uint16_t getter, const QByteArray &p, bool notification)
{
    switch (getter) {
    case GetModelId: {
        const qsizetype end = p.indexOf('\0');
        const SennheiserModel m = modelFromId(QString::fromUtf8(end >= 0 ? p.left(end) : p));
        if (m != SennheiserModel::Unknown)
            _model = m;
        qCInfo(lcHp) << "sennheiser model id:" << QString::fromUtf8(end >= 0 ? p.left(end) : p) << "->" << modelName(_model);
        break;
    }
    case GetFirmware:
        if (p.size() == 6)
            _firmware = QStringLiteral("%1.%2.%3").arg((at(p, 0) << 8) | at(p, 1)).arg((at(p, 2) << 8) | at(p, 3)).arg((at(p, 4) << 8) | at(p, 5));
        break;
    case GetBattery:
        if (!p.isEmpty() && at(p, 0) <= 100)
            _battery = at(p, 0);
        break;
    case GetCharging:
        if (!p.isEmpty())
            _charging = at(p, 0) == 1;
        break;
    case GetTimer:
        if (p.size() == 3 && at(p, 0) == TimerAutoPowerOff)
            _autoOff = (at(p, 1) << 8) | at(p, 2);
        break;
    case GetPromptMode:
        if (p.size() == 1 && at(p, 0) <= 2)
            _prompts = at(p, 0);
        break;
    case GetSoundMode:
        if (p.size() == 2 && at(p, 0) == 0 && at(p, 1) <= SoundModePersonalized)
            _soundMode = at(p, 1);
        break;
    case GetEqConfig: {
        const int bands = at(p, 0);
        const int min = int8_t(at(p, 1));
        const int max = int8_t(at(p, 2));
        if (p.size() < 3 || bands < 1 || bands > 16 || min >= max)
            break;
        Equalizer eq{bands, min, max, {}};
        if (_eq && _eq->bands == bands)
            eq.gains = _eq->gains;
        _eq = eq;
        break;
    }
    case GetEqBand:
    case GetEqGains:
        if (getter == GetEqBand && !notification)
            break;
        if (_eq && p.size() == _eq->bands) {
            QList<int> gains;
            for (int i = 0; i < p.size(); i++)
                gains.append(int8_t(at(p, i)));
            _eq->gains = gains;
        }
        break;
    case GetTransparent:
        if (p.size() == 1 && at(p, 0) <= 1)
            _transparent = at(p, 0) == 1;
        break;
    case GetAncModes:
        parseAncModes(p);
        break;
    case GetAncLevel:
        if (p.size() == 1 && at(p, 0) <= 100)
            _ancLevel = at(p, 0);
        break;
    case GetAncEnabled:
        if (p.size() == 1 && at(p, 0) <= 1)
            _ancEnabled = at(p, 0) == 1;
        break;
    default:
        if (const Switch *s = switchByGetter(getter); s && p.size() == 1 && at(p, 0) <= 1)
            _switches[getter] = at(p, 0) == 1;
        break;
    }
}

QStringList SennheiserBackend::modes() const
{
    QStringList m;
    if (!_ancEnabled)
        return m;
    m << ModeNc;
    if (std::any_of(_ancModes.begin(), _ancModes.end(), [](const auto &pair) { return pair.first == AncAdaptive; }))
        m << ModeAdaptive;
    if (_transparent)
        m << ModeAmbient;
    m << ModeOff;
    return m;
}

QString SennheiserBackend::currentMode() const
{
    if (!_ancEnabled)
        return {};
    if (!*_ancEnabled)
        return ModeOff;
    if (_transparent.value_or(false))
        return ModeAmbient;
    for (const auto &pair : _ancModes) {
        if (pair.first == AncAdaptive && pair.second == 1)
            return ModeAdaptive;
    }
    return ModeNc;
}

QStringList SennheiserBackend::windOptions() const
{
    if (_model == SennheiserModel::Momentum4)
        return {QStringLiteral("off"), QStringLiteral("high"), QStringLiteral("auto")};
    return {QStringLiteral("off"), QStringLiteral("on")};
}

Snapshot SennheiserBackend::snapshot()
{
    Snapshot s;
    if (!_battery)
        return s;
    s.battery = *_battery;
    s.batteries.append(QJsonObject{
        {QStringLiteral("part"), QStringLiteral("main")},
        {QStringLiteral("level"), *_battery},
        {QStringLiteral("charging"), _charging},
    });
    s.modes = modes();
    s.mode = currentMode();

    QJsonObject o;
    o[QStringLiteral("model")] = modelName(_model);
    o[QStringLiteral("untested")] = true;
    if (!_firmware.isEmpty())
        o[QStringLiteral("firmware")] = _firmware;

    if (_soundMode) {
        QJsonArray presets;
        int current = -1;
        for (const Preset &preset : Presets) {
            presets.append(QJsonObject{{QStringLiteral("id"), preset.id}, {QStringLiteral("key"), QString::fromLatin1(preset.key)}});
            if (preset.id == *_soundMode)
                current = preset.id;
        }
        o[QStringLiteral("equalizer")] = QJsonObject{
            {QStringLiteral("preset"), current},
            {QStringLiteral("presets"), presets},
        };
    }

    QJsonArray toggles;
    for (const Switch &sw : Switches) {
        const auto it = _switches.constFind(sw.getter);
        if (it != _switches.cend())
            toggles.append(control(QString::fromLatin1(sw.key), *it != sw.inverted));
    }
    if (!toggles.isEmpty())
        o[QStringLiteral("toggles")] = toggles;

    QJsonArray sliders;
    const auto slider = [&](const QString &key, int value, int min, int max) {
        sliders.append(QJsonObject{
            {QStringLiteral("key"), key},
            {QStringLiteral("value"), value},
            {QStringLiteral("min"), min},
            {QStringLiteral("max"), max},
        });
    };
    if (_ancLevel && currentMode() == ModeNc)
        slider(QStringLiteral("ancStrength"), 100 - *_ancLevel, 0, 100);
    if (_eq && _eq->gains.size() == _eq->bands) {
        const int min = ceilDb(_eq->min);
        const int max = floorDb(_eq->max);
        for (int i = 0; i < _eq->bands; i++)
            slider(bandKey(i, _eq->bands), std::clamp(int(std::lround(_eq->gains.at(i) / 10.0)), min, max), min, max);
    }
    if (!sliders.isEmpty())
        o[QStringLiteral("sliders")] = sliders;

    QJsonArray choices;
    const auto choice = [&](const QString &key, const QString &value, const QStringList &options) {
        QJsonObject c = control(key, value);
        c[QStringLiteral("options")] = QJsonArray::fromStringList(options);
        choices.append(c);
    };
    for (const auto &pair : _ancModes) {
        const QStringList options = windOptions();
        if (pair.first == AncAntiWind && pair.second < options.size())
            choice(QStringLiteral("windNoiseReduction"), options.at(pair.second), options);
    }
    if (_prompts) {
        QStringList options;
        for (const char *k : PromptKeys)
            options << QString::fromLatin1(k);
        choice(QStringLiteral("voicePrompts"), options.at(*_prompts), options);
    }
    if (_autoOff) {
        QStringList options;
        for (const int seconds : AutoOffSeconds)
            options << autoOffName(seconds);
        const QString current = autoOffName(*_autoOff);
        if (!options.contains(current))
            options << current;
        choice(QStringLiteral("autoPowerOff"), current, options);
    }
    if (!choices.isEmpty())
        o[QStringLiteral("choices")] = choices;

    s.settings = o;
    return s;
}

void SennheiserBackend::writeAncMode(uint8_t id, uint8_t value)
{
    QByteArray payload;
    payload.append(char(id));
    payload.append(char(value));
    enqueue(SetAncMode, payload);
    for (auto &pair : _ancModes) {
        if (pair.first == id)
            pair.second = value;
    }
    if (_transparent.value_or(false) && _ancEnabled.value_or(false))
        enqueue(SetTransparent, QByteArray(1, char(1)));
}

QDBusError SennheiserBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!modes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("These headphones do not support mode %1").arg(mode));
    const bool anc = _ancEnabled.value_or(false);
    const bool transparent = _transparent.value_or(false);
    if (mode == ModeOff) {
        if (anc)
            enqueue(SetAncEnabled, QByteArray(1, char(0)));
        _ancEnabled = false;
    } else if (mode == ModeAmbient) {
        if (!anc) {
            enqueue(SetAncEnabled, QByteArray(1, char(1)));
            enqueue(SetTransparent, QByteArray(1, char(1)));
        } else if (!transparent) {
            enqueue(SetTransparent, QByteArray(1, char(1)));
        }
        _ancEnabled = true;
        _transparent = true;
    } else {
        if (anc && transparent)
            enqueue(SetTransparent, QByteArray(1, char(0)));
        if (!anc)
            enqueue(SetAncEnabled, QByteArray(1, char(1)));
        _ancEnabled = true;
        _transparent = false;
        const bool adaptive = mode == ModeAdaptive;
        const auto it = std::find_if(_ancModes.begin(), _ancModes.end(), [](const auto &pair) { return pair.first == AncAdaptive; });
        if (it != _ancModes.end() && (it->second == 1) != adaptive)
            writeAncMode(AncAdaptive, adaptive ? 1 : 0);
    }
    enqueueNoiseReads();
    pump();
    return {};
}

QDBusError SennheiserBackend::setAmbientLevel(int)
{
    return unsupported();
}

QDBusError SennheiserBackend::applyToggle(const QString &key, bool value)
{
    const Switch *sw = findSwitch(key);
    if (!sw || !_switches.contains(sw->getter))
        return unsupported();
    if (sw->spatial && value && _soundMode == SoundModePersonalized)
        return QDBusError(QDBusError::NotSupported, QStringLiteral("Turn off sound personalization first"));
    if (key == QLatin1String("headTracking") && value && !_switches.value(findSwitch(QStringLiteral("spatialAudio"))->getter, false))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("Turn on spatial audio first"));
    const bool raw = value != sw->inverted;
    enqueue(sw->setter, QByteArray(1, char(raw ? 1 : 0)));
    enqueue(sw->getter);
    _switches[sw->getter] = raw;
    return {};
}

QDBusError SennheiserBackend::applySlider(const QString &key, int value)
{
    if (key == QLatin1String("ancStrength")) {
        if (!_ancLevel || currentMode() != ModeNc)
            return unsupported();
        const int level = 100 - std::clamp(value, 0, 100);
        enqueue(SetAncLevel, QByteArray(1, char(level)));
        enqueue(GetAncLevel);
        _ancLevel = level;
        return {};
    }
    if (!_eq || _eq->gains.size() != _eq->bands)
        return unsupported();
    for (int i = 0; i < _eq->bands; i++) {
        if (key != bandKey(i, _eq->bands))
            continue;
        const int db = std::clamp(value, ceilDb(_eq->min), floorDb(_eq->max));
        const int raw = std::clamp(db * 10, _eq->min, _eq->max);
        QByteArray payload;
        payload.append(char(i));
        payload.append(char(int8_t(raw)));
        enqueue(SetEqBand, payload);
        enqueue(GetEqGains, QByteArray(1, char(0)));
        _eq->gains[i] = raw;
        return {};
    }
    return unsupported();
}

QDBusError SennheiserBackend::applyChoice(const QString &key, const QString &value)
{
    if (key == QLatin1String("windNoiseReduction")) {
        const auto it = std::find_if(_ancModes.begin(), _ancModes.end(), [](const auto &pair) { return pair.first == AncAntiWind; });
        const qsizetype index = windOptions().indexOf(value);
        if (it == _ancModes.end())
            return unsupported();
        if (index < 0)
            return invalid(QStringLiteral("Unknown wind noise reduction %1").arg(value));
        writeAncMode(AncAntiWind, uint8_t(index));
        enqueueNoiseReads();
        return {};
    }
    if (key == QLatin1String("voicePrompts")) {
        if (!_prompts)
            return unsupported();
        const auto it = std::find_if(std::begin(PromptKeys), std::end(PromptKeys), [&](const char *k) { return value == QLatin1String(k); });
        if (it == std::end(PromptKeys))
            return invalid(QStringLiteral("Unknown voice prompt mode %1").arg(value));
        const int mode = int(it - std::begin(PromptKeys));
        enqueue(SetPromptMode, QByteArray(1, char(mode)));
        enqueue(GetPromptMode);
        _prompts = mode;
        return {};
    }
    if (key == QLatin1String("autoPowerOff")) {
        if (!_autoOff)
            return unsupported();
        const auto it = std::find_if(std::begin(AutoOffSeconds), std::end(AutoOffSeconds), [&](int s) { return autoOffName(s) == value; });
        if (it == std::end(AutoOffSeconds))
            return invalid(QStringLiteral("Unknown automatic power off %1").arg(value));
        QByteArray payload;
        payload.append(char(TimerAutoPowerOff));
        payload.append(char(*it >> 8));
        payload.append(char(*it & 0xFF));
        enqueue(SetTimer, payload);
        enqueue(GetTimer, QByteArray(1, char(TimerAutoPowerOff)));
        _autoOff = *it;
        return {};
    }
    return unsupported();
}

QDBusError SennheiserBackend::applyPreset(int id)
{
    if (!_soundMode)
        return unsupported();
    const bool known = std::any_of(std::begin(Presets), std::end(Presets), [&](const Preset &p) { return p.id == id; });
    if (!known)
        return invalid(QStringLiteral("Unknown equalizer preset %1").arg(id));
    QByteArray payload;
    payload.append(char(0));
    payload.append(char(id));
    enqueue(SetSoundMode, payload);
    enqueue(GetSoundMode);
    _soundMode = id;
    return {};
}

QDBusError SennheiserBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();
    QDBusError result;
    const auto keep = [&](const QDBusError &e) {
        if (e.isValid() && !result.isValid())
            result = e;
    };

    const QJsonObject eq = in.value(QStringLiteral("equalizer")).toObject();
    if (const QJsonValue v = eq.value(QStringLiteral("preset")); v.isDouble())
        keep(applyPreset(v.toInt()));
    const QJsonObject toggles = in.value(QStringLiteral("toggles")).toObject();
    for (auto it = toggles.begin(); it != toggles.end(); ++it) {
        if (it.value().isBool())
            keep(applyToggle(it.key(), it.value().toBool()));
    }
    const QJsonObject sliders = in.value(QStringLiteral("sliders")).toObject();
    for (auto it = sliders.begin(); it != sliders.end(); ++it) {
        if (it.value().isDouble())
            keep(applySlider(it.key(), it.value().toInt()));
    }
    const QJsonObject choices = in.value(QStringLiteral("choices")).toObject();
    for (auto it = choices.begin(); it != choices.end(); ++it) {
        if (it.value().isString())
            keep(applyChoice(it.key(), it.value().toString()));
    }
    pump();
    return result;
}
