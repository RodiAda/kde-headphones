#include "JabraBackend.h"

#include <algorithm>

#include "Log.h"

namespace {

constexpr uint8_t HostAddress = 0x04;
constexpr uint8_t DeviceAddress = 0x09;
constexpr int HeaderSize = 6;
constexpr int MaxFrame = 0x3F;
constexpr int ErrorFrameMax = 11;

constexpr uint8_t TypeQuery = 0x40;
constexpr uint8_t TypeCommand = 0x80;
constexpr uint8_t TypeReply = 0xC0;
constexpr uint8_t TypeMask = 0xC0;
constexpr uint8_t LengthMask = 0x3F;
constexpr uint8_t ReplyError = 0xCB;
constexpr uint8_t NakClass = 0xFE;

constexpr uint16_t FeatureProductName = 0x0200;
constexpr uint16_t FeatureFirmware = 0x0203;
constexpr uint16_t FeatureNotify = 0x0D4C;
constexpr uint16_t FeatureBattery = 0x1202;
constexpr uint16_t FeatureCallEqualizer = 0x1315;
constexpr uint16_t FeatureMuteReminder = 0x131E;
constexpr uint16_t FeatureGuidance = 0x133A;
constexpr uint16_t FeatureAutoReject = 0x133C;
constexpr uint16_t FeatureButtonSounds = 0x133F;
constexpr uint16_t FeatureSidetone = 0x137C;
constexpr uint16_t FeatureEqCurve = 0x137D;
constexpr uint16_t FeatureEqMode = 0x137E;
constexpr uint16_t FeatureNoiseMode = 0x1387;
constexpr uint16_t FeatureSleep = 0x1390;
constexpr uint16_t FeatureAmbience = 0x13BE;
constexpr uint16_t FeatureHeartbeat = 0xFC00;

constexpr uint8_t AmbienceParamMode = 0x01;
constexpr uint8_t AmbienceOff = 0x01;
constexpr uint8_t AmbienceHearThrough = 0x02;
constexpr uint8_t AmbienceAnc = 0x04;
constexpr uint8_t NoiseModeOff = 0x00;
constexpr uint8_t NoiseModeAnc = 0x01;
constexpr uint8_t NoiseModeHearThrough = 0x03;

constexpr uint8_t NotifyNoiseChanged = 0x09;
constexpr uint32_t NotifyMasks[] = {0x00000000, 0x00000010, 0x00000210};

constexpr uint8_t BatteryCharging = 0x01;
constexpr uint8_t BatteryMulti = 0x80;
constexpr uint8_t BatteryUnitRight = 4;
constexpr uint8_t BatteryUnitLeft = 5;
constexpr uint8_t BatteryUnitCase = 21;

constexpr uint8_t MuteReminderOn = 0x14;
constexpr uint8_t ButtonSoundsOn = 0xFF;
constexpr uint8_t SidetoneOn = 0x00;
constexpr uint8_t SidetoneOff = 0x01;
constexpr int SidetoneMax = 5;
constexpr int SleepStepMinutes = 5;
constexpr int SleepOptions[] = {15, 30, 60, 120, 240, 360, 480, 0};

constexpr uint8_t CurveMarker = 0xB4;
constexpr int CurveBands = 5;
constexpr int CurveStride = 6;

constexpr qint64 StatusTimeoutMs = 10000;
constexpr qint64 KeepaliveMs = 15000;
constexpr qint64 BatteryPollMs = 60000;

const char *const GuidanceKeys[] = {"tones", "voice", "off"};
const char *const CallEqKeys[] = {"neutral", "bass", "treble"};

enum class NoiseFamily { None, Ambience, Mode };

struct EqPreset {
    int id;
    const char *key;
    std::array<int16_t, CurveBands> gains;
};

const EqPreset EqPresets[] = {
    {0, "off", {0, 0, 0, 0, 0}},
    {1, "speech", {int16_t(0xFEF2), 0, 0, 0x003D, int16_t(0xFF88)}},
    {2, "bass-boost", {0x010E, int16_t(0xFF8A), 0, 0, 0}},
    {3, "treble-boost", {int16_t(0xFFA6), int16_t(0xFF8A), 0, 0x0076, 0x00F0}},
    {4, "smooth", {0, 0x0076, 0, int16_t(0xFF8A), 0}},
    {5, "energize", {0x00B4, 0, int16_t(0xFF8A), int16_t(0xFFC3), 0x00B4}},
};

const QByteArray CurveHead = QByteArray::fromHex("0500000000b4");
const QByteArray CurveBandTails[CurveBands] = {
    QByteArray::fromHex("06c202ee"),
    QByteArray::fromHex("06760bb8"),
    QByteArray::fromHex("06292ee0"),
    QByteArray::fromHex("063d5910"),
    QByteArray::fromHex("0828"),
};

uint8_t at(const QByteArray &p, int i)
{
    return i >= 0 && i < p.size() ? uint8_t(p.at(i)) : 0;
}

QString lengthPrefixed(const QByteArray &p)
{
    const int length = at(p, 0);
    if (length == 0 || length >= p.size())
        return {};
    return QString::fromLatin1(p.mid(1, length)).trimmed();
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

QDBusError invalid(const QString &what)
{
    return QDBusError(QDBusError::InvalidArgs, what);
}

QJsonObject toggle(const QString &key, bool value)
{
    return QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("value"), value}};
}

QJsonObject slider(const QString &key, int value, int min, int max)
{
    return QJsonObject{
        {QStringLiteral("key"), key},
        {QStringLiteral("value"), value},
        {QStringLiteral("min"), min},
        {QStringLiteral("max"), max},
    };
}

QJsonObject choice(const QString &key, const QString &value, const QStringList &options)
{
    return QJsonObject{
        {QStringLiteral("key"), key},
        {QStringLiteral("value"), value},
        {QStringLiteral("options"), QJsonArray::fromStringList(options)},
    };
}

template<std::size_t N>
QStringList keys(const char *const (&names)[N])
{
    QStringList out;
    for (const char *name : names)
        out.append(QLatin1String(name));
    return out;
}

template<std::size_t N>
int indexOf(const char *const (&names)[N], const QString &value)
{
    for (std::size_t i = 0; i < N; i++) {
        if (value == QLatin1String(names[i]))
            return int(i);
    }
    return -1;
}

QString sleepName(int minutes)
{
    return minutes == 0 ? QStringLiteral("never") : QStringLiteral("%1-min").arg(minutes);
}

}

struct JabraModelSpec {
    JabraModel model;
    const char *match;
    const char *name;
    NoiseFamily noise;
    bool swappedCallEq;
};

namespace {

const JabraModelSpec Specs[] = {
    {JabraModel::Evolve2_55, "Evolve2 55", "Jabra Evolve2 55", NoiseFamily::Ambience, false},
    {JabraModel::Evolve2_65, "Evolve2 65", "Jabra Evolve2 65", NoiseFamily::None, false},
    {JabraModel::Evolve2_85, "Evolve2 85", "Jabra Evolve2 85", NoiseFamily::Mode, false},
    {JabraModel::Evolve65, "Evolve 65", "Jabra Evolve 65", NoiseFamily::None, true},
    {JabraModel::Unknown, "", "Jabra", NoiseFamily::Ambience, false},
};

}

JabraModel JabraBackend::modelFromName(const QString &name)
{
    for (const JabraModelSpec &s : Specs) {
        if (s.model != JabraModel::Unknown && name.contains(QLatin1String(s.match), Qt::CaseInsensitive))
            return s.model;
    }
    return JabraModel::Unknown;
}

QByteArray JabraBackend::encodeFrame(uint8_t sequence, uint8_t type, uint16_t feature, const QByteArray &data)
{
    QByteArray frame;
    frame.append(char(HostAddress));
    frame.append(char(DeviceAddress));
    frame.append(char(sequence));
    frame.append(char(type | uint8_t(HeaderSize + data.size())));
    frame.append(char(feature >> 8));
    frame.append(char(feature & 0xFF));
    frame.append(data);
    return frame;
}

JabraBackend::JabraBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
    , _model(modelFromName(device.name))
{
}

const JabraModelSpec &JabraBackend::spec() const
{
    for (const JabraModelSpec &s : Specs) {
        if (s.model == _model)
            return s;
    }
    return Specs[std::size(Specs) - 1];
}

QString JabraBackend::modelName() const
{
    if (_model != JabraModel::Unknown)
        return QString::fromLatin1(spec().name);
    if (!_product.isEmpty())
        return _product.startsWith(QLatin1String("Jabra"), Qt::CaseInsensitive) ? _product : QStringLiteral("Jabra ") + _product;
    return QStringLiteral("Jabra");
}

QStringList JabraBackend::modes() const
{
    if (!_noise || spec().noise == NoiseFamily::None)
        return {};
    return {ModeNc, ModeAmbient, ModeOff};
}

bool JabraBackend::start()
{
    _sinceStart.start();
    _sinceKeepalive.start();
    _sinceBattery.start();
    qCInfo(lcHp) << "jabra model:" << modelName() << (_model == JabraModel::Unknown ? "(unrecognized, probing features)" : "");
    sendQueries();
    return true;
}

void JabraBackend::query(uint16_t feature, const QByteArray &data)
{
    _pending.insert(_sequence, feature);
    _tx.append(encodeFrame(_sequence, TypeQuery, feature, data));
    qCDebug(lcHp) << "jabra query" << Qt::hex << feature << data.toHex(' ');
    _sequence++;
}

void JabraBackend::command(uint16_t feature, const QByteArray &data)
{
    if (HeaderSize + data.size() > MaxFrame)
        return;
    _pending.insert(_sequence, feature);
    _tx.append(encodeFrame(_sequence, TypeCommand, feature, data));
    qCDebug(lcHp) << "jabra command" << Qt::hex << feature << data.toHex(' ');
    _sequence++;
}

void JabraBackend::sendQueries()
{
    query(FeatureProductName);
    query(FeatureFirmware);
    query(FeatureBattery);
    if (spec().noise == NoiseFamily::Ambience)
        query(FeatureAmbience, QByteArray(1, char(AmbienceParamMode)));
    else if (spec().noise == NoiseFamily::Mode)
        query(FeatureNoiseMode);
    query(FeatureEqMode);
    query(FeatureEqCurve, QByteArray(1, char(0)));
    query(FeatureMuteReminder);
    query(FeatureAutoReject);
    query(FeatureButtonSounds);
    query(FeatureSidetone);
    query(FeatureCallEqualizer);
    query(FeatureSleep);
    query(FeatureGuidance);
    for (const uint32_t mask : NotifyMasks) {
        QByteArray data;
        data.append(char(mask >> 24));
        data.append(char((mask >> 16) & 0xFF));
        data.append(char((mask >> 8) & 0xFF));
        data.append(char(mask & 0xFF));
        command(FeatureNotify, data);
    }
}

bool JabraBackend::flush()
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

bool JabraBackend::receive()
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

    static const QByteArray Marker("\x09\x04", 2);
    for (;;) {
        const qsizetype start = _rx.indexOf(Marker);
        if (start < 0) {
            _rx = _rx.endsWith(char(DeviceAddress)) ? _rx.right(1) : QByteArray();
            break;
        }
        _rx.remove(0, start);
        if (_rx.size() < 4)
            break;
        const uint8_t type = at(_rx, 3);
        int length = type & LengthMask;
        if (length < HeaderSize) {
            _rx.remove(0, 2);
            continue;
        }
        if (type == ReplyError) {
            if (_rx.size() < HeaderSize)
                break;
            if (_rx.size() == HeaderSize || _rx.mid(HeaderSize, 2) == Marker)
                length = HeaderSize;
            else if (_rx.size() < ErrorFrameMax)
                break;
            else
                length = ErrorFrameMax;
        }
        if (_rx.size() < length)
            break;
        const QByteArray frame = _rx.left(length);
        _rx.remove(0, length);
        _anyData = true;
        qCDebug(lcHp) << "jabra rx" << frame.toHex(' ');
        handle(at(frame, 2), type, uint16_t((at(frame, 4) << 8) | at(frame, 5)), frame.mid(HeaderSize));
    }
    return true;
}

bool JabraBackend::tick()
{
    if (!receive())
        return false;
    if (ready() && _sinceKeepalive.elapsed() > KeepaliveMs) {
        _sinceKeepalive.restart();
        query(FeatureHeartbeat);
        if (_noise && spec().noise == NoiseFamily::Ambience)
            query(FeatureAmbience, QByteArray(1, char(AmbienceParamMode)));
        else if (_noise && spec().noise == NoiseFamily::Mode)
            query(FeatureNoiseMode);
    }
    if (ready() && _battery >= 0 && _sinceBattery.elapsed() > BatteryPollMs) {
        _sinceBattery.restart();
        query(FeatureBattery);
    }
    if (!flush())
        return false;
    if (!_anyData && _sinceStart.elapsed() > StatusTimeoutMs)
        return fail(QStringLiteral("no answer from the headset"));
    if (!ready() && _sinceStart.elapsed() > StatusTimeoutMs)
        return fail(QStringLiteral("no battery status from the headset"));
    return true;
}

void JabraBackend::handle(uint8_t sequence, uint8_t type, uint16_t feature, const QByteArray &p)
{
    if ((type & TypeMask) == 0) {
        handleEvent(feature, p);
        return;
    }
    if ((type & TypeMask) != TypeReply)
        return;
    const uint16_t asked = _pending.take(sequence);
    if (type == ReplyError || (feature >> 8) == NakClass) {
        const uint16_t failed = asked ? asked : feature;
        qCDebug(lcHp) << "jabra feature not supported" << Qt::hex << failed;
        if (failed == FeatureBattery)
            _batteryAnswered = true;
        return;
    }
    switch (feature) {
    case FeatureProductName: {
        _product = lengthPrefixed(p);
        if (_model == JabraModel::Unknown && !_product.isEmpty()) {
            const JabraModel detected = modelFromName(_product);
            if (detected != JabraModel::Unknown) {
                const NoiseFamily before = spec().noise;
                _model = detected;
                qCInfo(lcHp) << "jabra model from product name:" << modelName();
                if (spec().noise != before) {
                    _noise.reset();
                    if (spec().noise == NoiseFamily::Mode)
                        query(FeatureNoiseMode);
                }
            }
        }
        break;
    }
    case FeatureFirmware:
        _firmware = lengthPrefixed(p);
        break;
    case FeatureBattery:
        parseBattery(p);
        _batteryAnswered = true;
        break;
    case FeatureAmbience:
        if (spec().noise == NoiseFamily::Ambience) {
            const uint8_t raw = p.size() == 1 ? at(p, 0) : p.size() >= 6 ? at(p, 3) : 0;
            if (raw == AmbienceOff || raw == AmbienceHearThrough || raw == AmbienceAnc)
                _noise = raw;
        }
        break;
    case FeatureNoiseMode:
        if (spec().noise == NoiseFamily::Mode && !p.isEmpty()) {
            const uint8_t raw = at(p, 0);
            if (raw == NoiseModeOff || raw == NoiseModeAnc || raw == NoiseModeHearThrough)
                _noise = raw;
        }
        break;
    case FeatureEqMode:
        if (!p.isEmpty())
            _eqEnabled = at(p, 0) != 0;
        break;
    case FeatureEqCurve:
        parseCurve(p);
        break;
    case FeatureMuteReminder:
        if (!p.isEmpty())
            _muteReminder = at(p, 0) != 0;
        break;
    case FeatureAutoReject:
        if (!p.isEmpty())
            _autoReject = at(p, 0) == 1;
        break;
    case FeatureButtonSounds:
        if (!p.isEmpty())
            _buttonSounds = at(p, 0) != 0;
        break;
    case FeatureSidetone:
        if (!p.isEmpty()) {
            _sidetone = at(p, 0) == SidetoneOn;
            if (p.size() >= 2)
                _sidetoneLevel = std::clamp(int(int8_t(at(p, 1))) / 3 + 3, 0, SidetoneMax);
        }
        break;
    case FeatureCallEqualizer:
        if (!p.isEmpty() && at(p, 0) < std::size(CallEqKeys))
            _callEq = at(p, 0);
        break;
    case FeatureSleep:
        if (!p.isEmpty())
            _sleepMinutes = at(p, 0) * SleepStepMinutes;
        break;
    case FeatureGuidance:
        if (!p.isEmpty() && at(p, 0) < std::size(GuidanceKeys))
            _guidance = at(p, 0);
        break;
    default:
        break;
    }
}

void JabraBackend::handleEvent(uint16_t feature, const QByteArray &p)
{
    if (feature == FeatureBattery) {
        parseBattery(p);
        return;
    }
    if (feature != FeatureNotify || at(p, 0) != NotifyNoiseChanged)
        return;
    if (spec().noise == NoiseFamily::Ambience) {
        const uint8_t raw = at(p, 2);
        if (p.size() >= 3 && (raw == AmbienceOff || raw == AmbienceHearThrough || raw == AmbienceAnc))
            _noise = raw;
        query(FeatureAmbience, QByteArray(1, char(AmbienceParamMode)));
    } else if (spec().noise == NoiseFamily::Mode) {
        query(FeatureNoiseMode);
    }
}

void JabraBackend::parseBattery(const QByteArray &p)
{
    if (p.size() < 2 || at(p, 1) > 100)
        return;
    const uint8_t flags = at(p, 0);
    _battery = at(p, 1);
    _charging = flags & BatteryCharging;
    _parts.clear();
    if (!(flags & BatteryMulti) || p.size() < 5)
        return;
    const int count = at(p, 4);
    if (p.size() < 5 + count * 3)
        return;
    for (int i = 0; i < count; i++) {
        const int offset = 5 + i * 3;
        const int level = at(p, offset);
        if (level > 100)
            continue;
        const uint8_t unit = at(p, offset + 2);
        const char *part = unit == BatteryUnitLeft ? "left" : unit == BatteryUnitRight ? "right" : unit == BatteryUnitCase ? "case" : nullptr;
        if (part)
            _parts.append({QString::fromLatin1(part), level, false});
    }
}

void JabraBackend::parseCurve(const QByteArray &p)
{
    const qsizetype marker = p.indexOf(char(CurveMarker));
    if (marker < 0 || marker + 1 + (CurveBands - 1) * CurveStride + 1 >= p.size())
        return;
    std::array<int16_t, CurveBands> gains{};
    for (int band = 0; band < CurveBands; band++) {
        const int index = int(marker) + 1 + band * CurveStride;
        gains[band] = int16_t((at(p, index) << 8) | at(p, index + 1));
    }
    _curve = gains;
}

Snapshot JabraBackend::snapshot()
{
    Snapshot s;
    if (!ready())
        return s;

    const auto add = [&](const QString &part, int level, bool charging) {
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), part},
            {QStringLiteral("level"), level},
            {QStringLiteral("charging"), charging},
        });
    };
    int left = -1;
    int right = -1;
    for (const BatteryPart &b : _parts) {
        add(b.part, b.level, b.charging);
        if (b.part == QLatin1String("left"))
            left = b.level;
        else if (b.part == QLatin1String("right"))
            right = b.level;
    }
    if (left >= 0 || right >= 0) {
        s.battery = left < 0 ? right : right < 0 ? left : std::min(left, right);
    } else if (_battery >= 0) {
        s.batteries = QJsonArray();
        add(QStringLiteral("main"), _battery, _charging);
        s.battery = _battery;
    }

    s.modes = modes();
    if (_noise && !s.modes.isEmpty()) {
        const uint8_t raw = *_noise;
        if (spec().noise == NoiseFamily::Ambience)
            s.mode = raw == AmbienceAnc ? ModeNc : raw == AmbienceHearThrough ? ModeAmbient : ModeOff;
        else
            s.mode = raw == NoiseModeAnc ? ModeNc : raw == NoiseModeHearThrough ? ModeAmbient : ModeOff;
    }

    QJsonObject o;
    o[QStringLiteral("model")] = modelName();
    o[QStringLiteral("untested")] = true;
    if (!_firmware.isEmpty())
        o[QStringLiteral("firmware")] = _firmware;

    if (_curve) {
        QJsonArray presets;
        int current = -1;
        for (const EqPreset &preset : EqPresets) {
            presets.append(QJsonObject{{QStringLiteral("id"), preset.id}, {QStringLiteral("key"), QString::fromLatin1(preset.key)}});
            if (preset.gains == *_curve)
                current = preset.id;
        }
        if (_eqEnabled == false)
            current = 0;
        o[QStringLiteral("equalizer")] = QJsonObject{
            {QStringLiteral("preset"), current},
            {QStringLiteral("presets"), presets},
        };
    }

    QJsonArray toggles;
    if (_muteReminder)
        toggles.append(toggle(QStringLiteral("muteReminder"), *_muteReminder));
    if (_autoReject)
        toggles.append(toggle(QStringLiteral("autoRejectCalls"), *_autoReject));
    if (_buttonSounds)
        toggles.append(toggle(QStringLiteral("buttonSounds"), *_buttonSounds));
    if (_sidetone)
        toggles.append(toggle(QStringLiteral("sidetone"), *_sidetone));
    if (!toggles.isEmpty())
        o[QStringLiteral("toggles")] = toggles;

    if (_sidetone)
        o[QStringLiteral("sliders")] = QJsonArray{slider(QStringLiteral("sidetoneLevel"), _sidetoneLevel, 0, SidetoneMax)};

    QJsonArray choices;
    if (_guidance)
        choices.append(choice(QStringLiteral("voicePrompts"), QLatin1String(GuidanceKeys[*_guidance]), keys(GuidanceKeys)));
    if (_sleepMinutes) {
        QStringList options;
        for (const int m : SleepOptions)
            options.append(sleepName(m));
        if (!options.contains(sleepName(*_sleepMinutes)))
            options.append(sleepName(*_sleepMinutes));
        choices.append(choice(QStringLiteral("autoPowerOff"), sleepName(*_sleepMinutes), options));
    }
    if (_callEq) {
        uint8_t index = *_callEq;
        if (spec().swappedCallEq && index != 0)
            index = 3 - index;
        choices.append(choice(QStringLiteral("callEqualizer"), QLatin1String(CallEqKeys[index]), keys(CallEqKeys)));
    }
    if (!choices.isEmpty())
        o[QStringLiteral("choices")] = choices;

    s.settings = o;
    return s;
}

QDBusError JabraBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!modes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("This headset does not support mode %1").arg(mode));
    if (spec().noise == NoiseFamily::Ambience) {
        const uint8_t raw = mode == ModeNc ? AmbienceAnc : mode == ModeAmbient ? AmbienceHearThrough : AmbienceOff;
        QByteArray data;
        data.append(char(AmbienceParamMode));
        data.append(char(raw));
        command(FeatureAmbience, data);
        _noise = raw;
    } else {
        const uint8_t raw = mode == ModeNc ? NoiseModeAnc : mode == ModeAmbient ? NoiseModeHearThrough : NoiseModeOff;
        command(FeatureNoiseMode, QByteArray(1, char(raw)));
        _noise = raw;
    }
    return {};
}

QDBusError JabraBackend::setAmbientLevel(int)
{
    return unsupported();
}

void JabraBackend::writeEqualizer(int preset)
{
    const EqPreset &p = EqPresets[preset];
    QByteArray curve = CurveHead;
    for (int band = 0; band < CurveBands; band++) {
        curve.append(char(uint16_t(p.gains[band]) >> 8));
        curve.append(char(uint16_t(p.gains[band]) & 0xFF));
        curve.append(CurveBandTails[band]);
    }
    command(FeatureEqMode, QByteArray(1, char(1)));
    command(FeatureEqCurve, curve);
    if (preset == 0)
        command(FeatureEqMode, QByteArray(1, char(0)));
    _curve = p.gains;
    _eqEnabled = preset != 0;
}

QDBusError JabraBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();

    const QJsonObject eq = in.value(QStringLiteral("equalizer")).toObject();
    if (const QJsonValue v = eq.value(QStringLiteral("preset")); v.isDouble() && _curve) {
        const int id = v.toInt();
        const auto it = std::find_if(std::begin(EqPresets), std::end(EqPresets), [&](const EqPreset &p) { return p.id == id; });
        if (it == std::end(EqPresets))
            return invalid(QStringLiteral("Unknown equalizer preset %1").arg(id));
        writeEqualizer(int(it - std::begin(EqPresets)));
    }

    const QJsonObject toggles = in.value(QStringLiteral("toggles")).toObject();
    if (const QJsonValue v = toggles.value(QStringLiteral("muteReminder")); v.isBool() && _muteReminder) {
        command(FeatureMuteReminder, QByteArray(1, char(v.toBool() ? MuteReminderOn : 0)));
        _muteReminder = v.toBool();
    }
    if (const QJsonValue v = toggles.value(QStringLiteral("autoRejectCalls")); v.isBool() && _autoReject) {
        command(FeatureAutoReject, QByteArray(1, char(v.toBool() ? 1 : 0)));
        _autoReject = v.toBool();
    }
    if (const QJsonValue v = toggles.value(QStringLiteral("buttonSounds")); v.isBool() && _buttonSounds) {
        command(FeatureButtonSounds, QByteArray(1, char(v.toBool() ? ButtonSoundsOn : 0)));
        _buttonSounds = v.toBool();
    }

    const QJsonObject sliders = in.value(QStringLiteral("sliders")).toObject();
    const QJsonValue sidetoneOn = toggles.value(QStringLiteral("sidetone"));
    const QJsonValue sidetoneLevel = sliders.value(QStringLiteral("sidetoneLevel"));
    if ((sidetoneOn.isBool() || sidetoneLevel.isDouble()) && _sidetone) {
        if (sidetoneOn.isBool())
            _sidetone = sidetoneOn.toBool();
        if (sidetoneLevel.isDouble())
            _sidetoneLevel = std::clamp(sidetoneLevel.toInt(), 0, SidetoneMax);
        QByteArray data;
        data.append(char(*_sidetone ? SidetoneOn : SidetoneOff));
        data.append(char(int8_t((_sidetoneLevel - 3) * 3)));
        command(FeatureSidetone, data);
    }

    const QJsonObject choices = in.value(QStringLiteral("choices")).toObject();
    if (const QJsonValue v = choices.value(QStringLiteral("voicePrompts")); v.isString() && _guidance) {
        const int index = indexOf(GuidanceKeys, v.toString());
        if (index < 0)
            return invalid(QStringLiteral("Unknown voice prompts option %1").arg(v.toString()));
        command(FeatureGuidance, QByteArray(1, char(index)));
        _guidance = uint8_t(index);
    }
    if (const QJsonValue v = choices.value(QStringLiteral("autoPowerOff")); v.isString() && _sleepMinutes) {
        const auto it = std::find_if(std::begin(SleepOptions), std::end(SleepOptions), [&](int m) { return sleepName(m) == v.toString(); });
        if (it == std::end(SleepOptions))
            return invalid(QStringLiteral("Unknown power off timeout %1").arg(v.toString()));
        command(FeatureSleep, QByteArray(1, char(*it / SleepStepMinutes)));
        _sleepMinutes = *it;
    }
    if (const QJsonValue v = choices.value(QStringLiteral("callEqualizer")); v.isString() && _callEq) {
        int index = indexOf(CallEqKeys, v.toString());
        if (index < 0)
            return invalid(QStringLiteral("Unknown call equalizer option %1").arg(v.toString()));
        if (spec().swappedCallEq && index != 0)
            index = 3 - index;
        command(FeatureCallEqualizer, QByteArray(1, char(index)));
        _callEq = uint8_t(index);
    }
    return {};
}
