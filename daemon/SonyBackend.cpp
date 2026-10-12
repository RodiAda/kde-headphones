#include "SonyBackend.h"

#include <QJsonDocument>
#include <QSettings>

#include <algorithm>
#include <array>

#include "Log.h"

namespace {

constexpr qint64 SyncIntervalMs = 60000;
constexpr int MaxEventsPerTick = 32;
const QString AmbientLevelKey = QStringLiteral("sony/ambientLevel");

template<class T>
struct Names {
    T value;
    const char *name;
};

constexpr Names<MDRSpeechSensitivity> SpeechSensitivities[] = {
    {MDR_SPEECH_SENSITIVITY_AUTO, "auto"}, {MDR_SPEECH_SENSITIVITY_LOW, "low"}, {MDR_SPEECH_SENSITIVITY_HIGH, "high"}};
constexpr Names<MDRSpeakTimeout> SpeakTimeouts[] = {
    {MDR_SPEAK_TIMEOUT_SHORT, "short"}, {MDR_SPEAK_TIMEOUT_MEDIUM, "medium"},
    {MDR_SPEAK_TIMEOUT_LONG, "long"}, {MDR_SPEAK_TIMEOUT_MANUAL, "manual"}};
constexpr Names<MDRNoiseButtonMode> NoiseButtonModes[] = {
    {MDR_NOISE_BUTTON_NOISE_AMBIENT_OFF, "nc-ambient-off"}, {MDR_NOISE_BUTTON_NOISE_AMBIENT, "nc-ambient"},
    {MDR_NOISE_BUTTON_NOISE_OFF, "nc-off"}, {MDR_NOISE_BUTTON_AMBIENT_OFF, "ambient-off"}};
constexpr Names<MDRAudioPriority> AudioPriorities[] = {
    {MDR_AUDIO_PRIORITY_QUALITY, "quality"}, {MDR_AUDIO_PRIORITY_STABILITY, "stability"}};
constexpr Names<MDRPlaybackStatus> PlaybackStatuses[] = {
    {MDR_PLAYBACK_STOPPED, "stopped"}, {MDR_PLAYBACK_PLAYING, "playing"}, {MDR_PLAYBACK_PAUSED, "paused"}};
constexpr Names<MDRPlaybackAction> PlaybackActions[] = {
    {MDR_PLAYBACK_PLAY, "play"}, {MDR_PLAYBACK_PAUSE, "pause"}, {MDR_PLAYBACK_NEXT, "next"}, {MDR_PLAYBACK_PREVIOUS, "previous"}};
constexpr Names<MDRBatteryPart> BatteryParts[] = {
    {MDR_BATTERY_MAIN, "main"}, {MDR_BATTERY_LEFT, "left"}, {MDR_BATTERY_RIGHT, "right"}, {MDR_BATTERY_CASE, "case"}};
constexpr Names<MDRAudioCodec> Codecs[] = {
    {MDR_AUDIO_CODEC_SBC, "SBC"}, {MDR_AUDIO_CODEC_AAC, "AAC"}, {MDR_AUDIO_CODEC_LDAC, "LDAC"},
    {MDR_AUDIO_CODEC_APTX, "aptX"}, {MDR_AUDIO_CODEC_APTX_HD, "aptX HD"}, {MDR_AUDIO_CODEC_LC3, "LC3"}};
constexpr Names<MDREqualizerPreset> EqualizerPresets[] = {
    {MDR_EQ_OFF, "off"}, {MDR_EQ_ROCK, "rock"}, {MDR_EQ_POP, "pop"}, {MDR_EQ_JAZZ, "jazz"},
    {MDR_EQ_DANCE, "dance"}, {MDR_EQ_EDM, "edm"}, {MDR_EQ_R_AND_B_HIP_HOP, "rnb-hiphop"},
    {MDR_EQ_ACOUSTIC, "acoustic"}, {MDR_EQ_BRIGHT, "bright"}, {MDR_EQ_EXCITED, "excited"},
    {MDR_EQ_MELLOW, "mellow"}, {MDR_EQ_RELAXED, "relaxed"}, {MDR_EQ_VOCAL, "vocal"},
    {MDR_EQ_TREBLE, "treble-boost"}, {MDR_EQ_BASS, "bass-boost"}, {MDR_EQ_SPEECH, "speech"},
    {MDR_EQ_CUSTOM, "custom"}, {MDR_EQ_USER_1, "user-1"}, {MDR_EQ_USER_2, "user-2"},
    {MDR_EQ_USER_3, "user-3"}, {MDR_EQ_USER_4, "user-4"}, {MDR_EQ_USER_5, "user-5"}};

template<class T, size_t N>
QString nameOf(const Names<T> (&table)[N], T value)
{
    for (const auto &e : table) {
        if (e.value == value)
            return QString::fromLatin1(e.name);
    }
    return {};
}

template<class T, size_t N>
std::optional<T> valueOf(const Names<T> (&table)[N], const QString &name)
{
    for (const auto &e : table) {
        if (name == QLatin1String(e.name))
            return e.value;
    }
    return std::nullopt;
}

MDRBoolean mdrBool(bool b)
{
    return b ? MDR_TRUE : MDR_FALSE;
}

QByteArray framePayload(const unsigned char *frame, int size)
{
    if (size < 2 || frame[0] != 0x3E || frame[size - 1] != 0x3C)
        return {};
    QByteArray raw;
    raw.reserve(size);
    for (int i = 1; i < size - 1; i++) {
        if (frame[i] == 0x3D && i + 1 < size - 1)
            raw.append(char(frame[++i] | 0x10));
        else
            raw.append(char(frame[i]));
    }
    if (raw.size() < 7)
        return {};
    const auto b = reinterpret_cast<const uint8_t *>(raw.constData());
    const int len = (b[2] << 24) | (b[3] << 16) | (b[4] << 8) | b[5];
    if (len <= 0 || 6 + len > raw.size())
        return {};
    return raw.mid(6, len);
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

}

SonyBackend::SonyBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
{
    _lastAmbientLevel = std::clamp(QSettings().value(AmbientLevelKey, _lastAmbientLevel).toInt(), 1, 20);
}

SonyBackend::~SonyBackend()
{
    if (_hp)
        mdrHeadphonesDestroy(_hp);
}

bool SonyBackend::start()
{
    const MDRProtocolVersion protocol = _device.kind == DeviceKind::SonyV1 ? MDR_PROTOCOL_V1 : MDR_PROTOCOL_V2;
    if (mdrHeadphonesCreate(MDR_ABI_VERSION, _conn, protocol, &_hp) != MDR_RESULT_OK)
        return fail(QStringLiteral("libmdr ABI mismatch"));
    mdrHeadphonesSetPacketCallback(_hp, &SonyBackend::onPacket, this);
    if (mdrHeadphonesRequestInit(_hp) != MDR_RESULT_OK)
        return fail(QStringLiteral("init request failed"));
    return true;
}

bool SonyBackend::tick()
{
    for (int i = 0; i < MaxEventsPerTick; i++) {
        MDREvent event = MDR_EVENT_NONE;
        if (mdrHeadphonesPoll(_hp, &event) != MDR_RESULT_OK)
            return fail(QString::fromUtf8(mdrConnectionGetLastError(_conn)));
        if (event == MDR_EVENT_NONE)
            break;
        _settingsDirty = true;
        switch (event) {
        case MDR_EVENT_INITIALIZE_COMPLETE: {
            QStringList available;
            for (MDRFeature f = MDR_FEATURE_IDENTITY; f <= MDR_FEATURE_MAX_VALUE; f++) {
                if (has(f))
                    available << QString::number(f);
            }
            qCInfo(lcHp) << "initialized, features:" << available.join(QLatin1Char(' '));
            _initDone = true;
            _syncWanted = true;
            break;
        }
        case MDR_EVENT_NEED_SYNC:
            _syncWanted = true;
            break;
        case MDR_EVENT_ALERT:
            _alert = text(MDR_TEXT_LAST_ALERT);
            qCInfo(lcHp) << "alert:" << _alert;
            break;
        default:
            break;
        }
    }

    if (!_initDone || !mdrHeadphonesIsReady(_hp))
        return true;
    applyPending();
    runOps();
    if (!mdrHeadphonesIsReady(_hp))
        return true;
    if (mdrHeadphonesIsDirty(_hp)) {
        if (mdrHeadphonesRequestCommit(_hp) != MDR_RESULT_OK)
            return fail(QStringLiteral("commit failed"));
    } else if (_syncWanted || !_sinceSync.isValid() || _sinceSync.elapsed() >= SyncIntervalMs) {
        _syncWanted = false;
        _sinceSync.start();
        if (mdrHeadphonesRequestSync(_hp) != MDR_RESULT_OK)
            return fail(QStringLiteral("sync failed"));
    }
    return true;
}

void SonyBackend::applyPending()
{
    if (_pending.noiseMode || _pending.ambientLevel || _pending.focusOnVoice) {
        MDRNoiseControl nc{};
        if (mdrHeadphonesGetNoiseControl(_hp, &nc) == MDR_RESULT_OK) {
            if (_pending.noiseMode)
                nc.mode = *_pending.noiseMode;
            if (_pending.ambientLevel)
                nc.ambient_level = uint8_t(*_pending.ambientLevel);
            if (_pending.focusOnVoice)
                nc.focus_on_voice = mdrBool(*_pending.focusOnVoice);
            const MDRResult r = mdrHeadphonesSetNoiseControl(_hp, &nc);
            if (r != MDR_RESULT_OK)
                qCWarning(lcHp) << "SetNoiseControl failed:" << r;
        }
        _pending.noiseMode.reset();
        _pending.ambientLevel.reset();
        _pending.focusOnVoice.reset();
    }
    if (_pending.deviceCommand) {
        const auto &[command, mac] = *_pending.deviceCommand;
        const MDRPairedDeviceAction action{command, mac.constData(), uint32_t(mac.size())};
        const MDRResult r = mdrHeadphonesSetPairedDevice(_hp, &action);
        if (r != MDR_RESULT_OK)
            qCWarning(lcHp) << "SetPairedDevice failed:" << r;
        _pending.deviceCommand.reset();
    }
}

void SonyBackend::queueOp(std::function<MDRResult()> op)
{
    _ops.push_back(std::move(op));
    _settingsDirty = true;
}

void SonyBackend::runOps()
{
    while (!_ops.empty() && mdrHeadphonesIsReady(_hp)) {
        const MDRResult r = _ops.front()();
        if (r == MDR_RESULT_INPROGRESS)
            return;
        if (r != MDR_RESULT_OK)
            qCWarning(lcHp) << "operation failed:" << r;
        _ops.pop_front();
        _settingsDirty = true;
    }
}

void SonyBackend::onPacket(void *self, MDRPacketDirection direction, const unsigned char *frame, int size)
{
    auto *backend = static_cast<SonyBackend *>(self);
    if (direction != MDR_PACKET_DIRECTION_RX || backend->_device.kind != DeviceKind::SonyV2)
        return;
    const QByteArray payload = framePayload(frame, size);
    if (!payload.isEmpty())
        backend->handlePayload(payload);
}

void SonyBackend::handlePayload(const QByteArray &p)
{
    const auto b = reinterpret_cast<const uint8_t *>(p.constData());
    const int n = p.size();
    if (n < 3)
        return;

    if ((b[0] == 0x37 || b[0] == 0x39) && b[1] == 0x02) {
        QHash<QString, int> order;
        int i = 3;
        for (int k = 0; k < b[2]; k++) {
            if (i + 22 > n)
                return;
            const int len = b[i + 21];
            if (i + 22 + len > n)
                return;
            order.insert(QString::fromLatin1(p.mid(i, 17)).toUpper(), b[i + 17]);
            i += 22 + len;
        }
        _connectionOrder = order;
        return;
    }

    if (b[0] == 0x3D && (b[1] == 0x00 || b[1] == 0x02) && n >= 21) {
        static const char *states[] = {"ok", "error", "in-progress", "busy"};
        const uint8_t result = b[3];
        const QString state = QString::fromLatin1((result & 0x0f) < 4 ? states[result & 0x0f] : "error");
        const QString mac = QString::fromLatin1(p.mid(4, 17)).toUpper();
        qCInfo(lcHp) << "device" << mac << (b[2] == 0 ? "disconnect" : "connect") << "->" << state;
        _deviceAction = QJsonObject{
            {QStringLiteral("mac"), mac},
            {QStringLiteral("action"), b[2] == 0 ? QStringLiteral("disconnect") : QStringLiteral("connect")},
            {QStringLiteral("state"), state},
        };
    }
}

bool SonyBackend::has(MDRFeature feature) const
{
    MDRFeatureAvailability a = MDR_AVAILABILITY_UNKNOWN;
    return _hp && mdrHeadphonesGetFeature(_hp, feature, &a) == MDR_RESULT_OK && a == MDR_AVAILABILITY_AVAILABLE;
}

QString SonyBackend::text(MDRText id, uint32_t index) const
{
    char buffer[512] = {};
    uint32_t size = sizeof(buffer);
    if (!_hp || mdrHeadphonesGetText(_hp, id, index, buffer, &size) != MDR_RESULT_OK)
        return {};
    return QString::fromUtf8(buffer, qstrnlen(buffer, sizeof(buffer)));
}

QStringList SonyBackend::availableModes() const
{
    QStringList modes;
    if (has(MDR_FEATURE_NOISE_CANCELLING))
        modes << ModeNc;
    if (has(MDR_FEATURE_AMBIENT_SOUND))
        modes << ModeAmbient;
    if (!modes.isEmpty())
        modes << ModeOff;
    return modes;
}

Snapshot SonyBackend::snapshot()
{
    Snapshot s;
    s.ambientLevel = _lastAmbientLevel;
    if (!ready())
        return s;

    std::array<MDRBattery, 4> batteries{};
    uint32_t count = batteries.size();
    if (mdrHeadphonesGetBatteries(_hp, batteries.data(), &count) == MDR_RESULT_OK) {
        int buds = -1;
        for (uint32_t i = 0; i < count; i++) {
            const MDRBattery &b = batteries[i];
            if (b.present != MDR_TRUE)
                continue;
            const QString part = nameOf(BatteryParts, b.part);
            if (part.isEmpty())
                continue;
            s.batteries.append(QJsonObject{
                {QStringLiteral("part"), part},
                {QStringLiteral("level"), int(b.level_percent)},
                {QStringLiteral("charging"), b.charging == MDR_CHARGING_YES},
            });
            if (b.part == MDR_BATTERY_MAIN)
                s.battery = b.level_percent;
            else if (b.part == MDR_BATTERY_LEFT || b.part == MDR_BATTERY_RIGHT)
                buds = buds < 0 ? b.level_percent : std::min<int>(buds, b.level_percent);
        }
        if (s.battery < 0)
            s.battery = buds;
    }
    s.modes = availableModes();

    MDRNoiseControl nc{};
    if (mdrHeadphonesGetNoiseControl(_hp, &nc) == MDR_RESULT_OK) {
        const MDRNoiseMode m = _pending.noiseMode.value_or(nc.mode);
        s.mode = m == MDR_NOISE_MODE_CANCELLING ? ModeNc : m == MDR_NOISE_MODE_AMBIENT ? ModeAmbient : ModeOff;
        s.focusOnVoice = _pending.focusOnVoice.value_or(nc.focus_on_voice == MDR_TRUE);
        if (m == MDR_NOISE_MODE_AMBIENT)
            s.ambientLevel = std::clamp(_pending.ambientLevel.value_or(nc.ambient_level), 1, 20);
    }
    if (s.ambientLevel != _lastAmbientLevel) {
        _lastAmbientLevel = s.ambientLevel;
        QSettings().setValue(AmbientLevelKey, s.ambientLevel);
    }

    std::array<MDRPairedDevice, 16> paired{};
    count = paired.size();
    if (mdrHeadphonesGetPairedDevices(_hp, paired.data(), &count) == MDR_RESULT_OK) {
        std::stable_sort(paired.begin(), paired.begin() + count,
                         [](const MDRPairedDevice &a, const MDRPairedDevice &b) { return a.connected > b.connected; });
        for (uint32_t i = 0; i < count; i++) {
            const MDRPairedDevice &d = paired[i];
            const QString mac = QString::fromLatin1(d.macAddress).toUpper();
            s.devices.append(QJsonObject{
                {QStringLiteral("mac"), mac},
                {QStringLiteral("name"), QString::fromUtf8(d.name)},
                {QStringLiteral("connected"), d.connected == MDR_TRUE},
                {QStringLiteral("order"), _connectionOrder.value(mac, d.connected == MDR_TRUE ? int(i) + 1 : 0)},
                {QStringLiteral("active"), d.playback_device == MDR_TRUE},
                {QStringLiteral("self"), mac.compare(_device.adapterAddress, Qt::CaseInsensitive) == 0},
            });
        }
    }

    if (_settingsDirty) {
        _settingsDirty = false;
        _settingsCache = settingsJson();
    }
    s.settings = _settingsCache;
    s.deviceAction = _deviceAction;
    s.alert = _alert;
    return s;
}

QJsonObject SonyBackend::settingsJson() const
{
    QJsonObject o;
    o[QStringLiteral("model")] = text(MDR_TEXT_MODEL_NAME);
    o[QStringLiteral("firmware")] = text(MDR_TEXT_FIRMWARE_VERSION);
    MDRModel model{};
    if (mdrHeadphonesGetModel(_hp, &model) == MDR_RESULT_OK)
        o[QStringLiteral("codec")] = nameOf(Codecs, model.audio_codec);

    if (has(MDR_FEATURE_AMBIENT_SOUND)) {
        o[QStringLiteral("ambient")] = QJsonObject{
            {QStringLiteral("min"), 1},
            {QStringLiteral("max"), 20},
            {QStringLiteral("focusOnVoice"), true},
        };
    }

    if (has(MDR_FEATURE_PLAYBACK_METADATA) || has(MDR_FEATURE_PLAYBACK_CONTROL) || has(MDR_FEATURE_PLAYBACK_VOLUME)) {
        MDRPlayback pb{};
        mdrHeadphonesGetPlayback(_hp, &pb);
        o[QStringLiteral("playback")] = QJsonObject{
            {QStringLiteral("status"), nameOf(PlaybackStatuses, pb.status)},
            {QStringLiteral("title"), text(MDR_TEXT_TRACK_TITLE)},
            {QStringLiteral("artist"), text(MDR_TEXT_TRACK_ARTIST)},
            {QStringLiteral("album"), text(MDR_TEXT_TRACK_ALBUM)},
            {QStringLiteral("volume"), has(MDR_FEATURE_PLAYBACK_VOLUME) ? QJsonValue(int(pb.volume)) : QJsonValue()},
            {QStringLiteral("control"), has(MDR_FEATURE_PLAYBACK_CONTROL)},
        };
    }

    if (has(MDR_FEATURE_SPEAK_TO_CHAT)) {
        MDRSpeakToChat stc{};
        if (mdrHeadphonesGetSpeakToChat(_hp, &stc) == MDR_RESULT_OK) {
            o[QStringLiteral("speakToChat")] = QJsonObject{
                {QStringLiteral("enabled"), stc.enabled == MDR_TRUE},
                {QStringLiteral("sensitivity"), nameOf(SpeechSensitivities, stc.sensitivity)},
                {QStringLiteral("timeout"), nameOf(SpeakTimeouts, stc.timeout)},
            };
        }
    }

    MDREqualizer eq{};
    const bool eqOk = (has(MDR_FEATURE_EQUALIZER) || has(MDR_FEATURE_DSEE)) && mdrHeadphonesGetEqualizer(_hp, &eq) == MDR_RESULT_OK;
    if (eqOk && has(MDR_FEATURE_EQUALIZER)) {
        std::array<int8_t, 16> bands{};
        uint32_t bandCount = bands.size();
        QJsonArray bandArray;
        if (mdrHeadphonesGetEqualizerBands(_hp, bands.data(), &bandCount) == MDR_RESULT_OK) {
            for (uint32_t i = 0; i < bandCount; i++)
                bandArray.append(bands[i]);
        }
        std::array<MDREqualizerPreset, 32> presets{};
        uint32_t presetCount = presets.size();
        QJsonArray presetArray;
        if (mdrHeadphonesGetEqualizerPresets(_hp, presets.data(), &presetCount) == MDR_RESULT_OK) {
            for (uint32_t i = 0; i < presetCount; i++) {
                QString key = nameOf(EqualizerPresets, presets[i]);
                if (key.isEmpty())
                    key = text(MDR_TEXT_EQUALIZER_PRESET_NAME, i);
                presetArray.append(QJsonObject{{QStringLiteral("id"), int(presets[i])}, {QStringLiteral("key"), key}});
            }
        }
        o[QStringLiteral("equalizer")] = QJsonObject{
            {QStringLiteral("preset"), int(eq.preset)},
            {QStringLiteral("presets"), presetArray},
            {QStringLiteral("bands"), bandArray},
            {QStringLiteral("clearBass"), eq.clear_bass},
            {QStringLiteral("customPreset"), int(MDR_EQ_CUSTOM)},
        };
    }
    if (eqOk && has(MDR_FEATURE_DSEE) && eq.dsee_available == MDR_TRUE)
        o[QStringLiteral("dsee")] = eq.dsee_enabled == MDR_TRUE;

    if (has(MDR_FEATURE_NOISE_CONTROL_BUTTON)) {
        MDRNoiseControl nc{};
        if (mdrHeadphonesGetNoiseControl(_hp, &nc) == MDR_RESULT_OK)
            o[QStringLiteral("noiseButton")] = nameOf(NoiseButtonModes, nc.button_mode);
    }

    if (has(MDR_FEATURE_AUTO_POWER_OFF) || has(MDR_FEATURE_WEARING_DETECTION) || has(MDR_FEATURE_AUTO_PAUSE)) {
        MDRPower power{};
        if (mdrHeadphonesGetPower(_hp, &power) == MDR_RESULT_OK) {
            QJsonObject p;
            if (has(MDR_FEATURE_AUTO_POWER_OFF))
                p[QStringLiteral("autoPowerOff")] = int(power.auto_power_off_minutes);
            if (has(MDR_FEATURE_WEARING_DETECTION))
                p[QStringLiteral("offWhenRemoved")] = power.wearing_power == MDR_WEARING_POWER_WHEN_REMOVED;
            if (has(MDR_FEATURE_AUTO_PAUSE))
                p[QStringLiteral("autoPause")] = power.auto_pause == MDR_TRUE;
            o[QStringLiteral("power")] = p;
        }
    }
    o[QStringLiteral("canPowerOff")] = has(MDR_FEATURE_SHUTDOWN);

    if (has(MDR_FEATURE_VOICE_GUIDANCE)) {
        MDRVoiceGuidance vg{};
        if (mdrHeadphonesGetVoiceGuidance(_hp, &vg) == MDR_RESULT_OK) {
            QJsonObject v{{QStringLiteral("enabled"), vg.enabled == MDR_TRUE}};
            if (has(MDR_FEATURE_VOICE_GUIDANCE_VOLUME))
                v[QStringLiteral("volume")] = vg.volume;
            o[QStringLiteral("voiceGuidance")] = v;
        }
    }

    if (has(MDR_FEATURE_CONNECTION_MODE)) {
        MDRConnectionMode cm{};
        if (mdrHeadphonesGetConnectionMode(_hp, &cm) == MDR_RESULT_OK)
            o[QStringLiteral("connectionMode")] = nameOf(AudioPriorities, cm.audio_priority);
    }

    if (has(MDR_FEATURE_SOURCE_SWITCH_CONTROL)) {
        MDRBoolean enabled = MDR_FALSE;
        if (mdrHeadphonesGetSourceSwitchControl(_hp, &enabled) == MDR_RESULT_OK)
            o[QStringLiteral("autoSwitch")] = enabled == MDR_TRUE;
    }

    if (has(MDR_FEATURE_PAIRING_MODE)) {
        MDRPairing pairing{};
        if (mdrHeadphonesGetPairing(_hp, &pairing) == MDR_RESULT_OK)
            o[QStringLiteral("pairing")] = pairing.enabled == MDR_TRUE;
    }

    if (has(MDR_FEATURE_SAFE_LISTENING)) {
        MDRSafeListening sl{};
        if (mdrHeadphonesGetSafeListening(_hp, &sl) == MDR_RESULT_OK)
            o[QStringLiteral("soundPressure")] = int(sl.sound_pressure);
    }

    if (has(MDR_FEATURE_GENERAL_SETTINGS)) {
        std::array<MDRGeneralSettingInfo, 16> infos{};
        uint32_t count = infos.size();
        QJsonArray general;
        if (mdrHeadphonesGetGeneralSettingInfo(_hp, infos.data(), &count) == MDR_RESULT_OK) {
            for (uint32_t i = 0; i < count; i++) {
                if (infos[i].type != MDR_GENERAL_SETTING_BOOLEAN)
                    continue;
                MDRGeneralSetting setting{};
                if (mdrHeadphonesGetGeneralSetting(_hp, infos[i].index, &setting) != MDR_RESULT_OK)
                    continue;
                general.append(QJsonObject{
                    {QStringLiteral("index"), int(infos[i].index)},
                    {QStringLiteral("subject"), text(MDR_TEXT_GENERAL_SETTING_SUBJECT, infos[i].index)},
                    {QStringLiteral("summary"), text(MDR_TEXT_GENERAL_SETTING_SUMMARY, infos[i].index)},
                    {QStringLiteral("value"), setting.boolean_value == MDR_TRUE},
                    {QStringLiteral("writable"), infos[i].writable == MDR_TRUE},
                });
            }
        }
        o[QStringLiteral("general")] = general;
    }
    return o;
}

QDBusError SonyBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!availableModes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("These headphones do not support mode %1").arg(mode));
    if (mode == ModeNc) {
        _pending.noiseMode = MDR_NOISE_MODE_CANCELLING;
    } else if (mode == ModeAmbient) {
        _pending.noiseMode = MDR_NOISE_MODE_AMBIENT;
        _pending.ambientLevel = _lastAmbientLevel;
    } else {
        _pending.noiseMode = MDR_NOISE_MODE_OFF;
    }
    return {};
}

QDBusError SonyBackend::setAmbientLevel(int level)
{
    if (!ready())
        return notReady();
    level = std::clamp(level, 1, 20);
    MDRNoiseControl nc{};
    mdrHeadphonesGetNoiseControl(_hp, &nc);
    if (_pending.noiseMode.value_or(nc.mode) == MDR_NOISE_MODE_AMBIENT) {
        _pending.ambientLevel = level;
    } else {
        _lastAmbientLevel = level;
        QSettings().setValue(AmbientLevelKey, level);
    }
    return {};
}

QDBusError SonyBackend::setFocusOnVoice(bool enabled)
{
    if (!ready())
        return notReady();
    _pending.focusOnVoice = enabled;
    return {};
}

QDBusError SonyBackend::deviceCommand(DeviceCommand command, const QString &mac)
{
    if (!ready())
        return notReady();
    if (!has(MDR_FEATURE_PAIRED_DEVICE_MANAGEMENT))
        return unsupported();
    const QString m = mac.toUpper();
    std::array<MDRPairedDevice, 16> paired{};
    uint32_t count = paired.size();
    mdrHeadphonesGetPairedDevices(_hp, paired.data(), &count);
    const auto it = std::find_if(paired.begin(), paired.begin() + count,
                                 [&](const MDRPairedDevice &d) { return m == QString::fromLatin1(d.macAddress).toUpper(); });
    if (it == paired.begin() + count)
        return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Device %1 is not paired with the headphones").arg(mac));
    if (command == DeviceCommand::SelectPlayback && it->connected != MDR_TRUE)
        return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Device %1 is not connected to the headphones").arg(mac));
    if (command == DeviceCommand::Disconnect && m.compare(_device.adapterAddress, Qt::CaseInsensitive) == 0)
        return QDBusError(QDBusError::AccessDenied, QStringLiteral("Refusing to disconnect this computer"));

    const MDRPairedDeviceCommand mdrCommand = command == DeviceCommand::Connect ? MDR_PAIRED_DEVICE_CONNECT
        : command == DeviceCommand::Disconnect                                  ? MDR_PAIRED_DEVICE_DISCONNECT
                                                                                : MDR_PAIRED_DEVICE_SELECT_PLAYBACK;
    _pending.deviceCommand = std::make_pair(mdrCommand, QByteArray(it->macAddress));
    if (command != DeviceCommand::SelectPlayback) {
        _deviceAction = QJsonObject{
            {QStringLiteral("mac"), m},
            {QStringLiteral("action"), command == DeviceCommand::Disconnect ? QStringLiteral("disconnect") : QStringLiteral("connect")},
            {QStringLiteral("state"), QStringLiteral("pending")},
        };
    }
    return {};
}

QDBusError SonyBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();
    MDRHeadphones *hp = _hp;

    if (in.contains(QStringLiteral("speakToChat"))) {
        const QJsonObject v = in.value(QStringLiteral("speakToChat")).toObject();
        queueOp([hp, v] {
            MDRSpeakToChat stc{};
            if (MDRResult r = mdrHeadphonesGetSpeakToChat(hp, &stc); r != MDR_RESULT_OK)
                return r;
            if (v.contains(QStringLiteral("enabled")))
                stc.enabled = mdrBool(v.value(QStringLiteral("enabled")).toBool());
            if (auto s = valueOf(SpeechSensitivities, v.value(QStringLiteral("sensitivity")).toString()))
                stc.sensitivity = *s;
            if (auto t = valueOf(SpeakTimeouts, v.value(QStringLiteral("timeout")).toString()))
                stc.timeout = *t;
            return mdrHeadphonesSetSpeakToChat(hp, &stc);
        });
    }

    if (in.contains(QStringLiteral("equalizer")) || in.contains(QStringLiteral("dsee"))) {
        const QJsonObject v = in.value(QStringLiteral("equalizer")).toObject();
        const QJsonValue dsee = in.value(QStringLiteral("dsee"));
        if (v.contains(QStringLiteral("preset")) || v.contains(QStringLiteral("clearBass")) || dsee.isBool()) {
            queueOp([hp, v, dsee] {
                MDREqualizer eq{};
                if (MDRResult r = mdrHeadphonesGetEqualizer(hp, &eq); r != MDR_RESULT_OK)
                    return r;
                if (v.contains(QStringLiteral("preset")))
                    eq.preset = MDREqualizerPreset(v.value(QStringLiteral("preset")).toInt());
                if (v.contains(QStringLiteral("clearBass")))
                    eq.clear_bass = int8_t(std::clamp(v.value(QStringLiteral("clearBass")).toInt(), -10, 10));
                if (dsee.isBool())
                    eq.dsee_enabled = mdrBool(dsee.toBool());
                return mdrHeadphonesSetEqualizer(hp, &eq);
            });
        }
        if (v.contains(QStringLiteral("bands"))) {
            std::vector<int8_t> bands;
            for (const QJsonValue &b : v.value(QStringLiteral("bands")).toArray())
                bands.push_back(int8_t(std::clamp(b.toInt(), -10, 10)));
            queueOp([hp, bands] { return mdrHeadphonesSetEqualizerBands(hp, bands.data(), uint32_t(bands.size())); });
        }
    }

    if (auto mode = valueOf(NoiseButtonModes, in.value(QStringLiteral("noiseButton")).toString())) {
        queueOp([hp, m = *mode] {
            MDRNoiseControl nc{};
            if (MDRResult r = mdrHeadphonesGetNoiseControl(hp, &nc); r != MDR_RESULT_OK)
                return r;
            nc.button_mode = m;
            return mdrHeadphonesSetNoiseControl(hp, &nc);
        });
    }

    if (in.contains(QStringLiteral("power"))) {
        const QJsonObject v = in.value(QStringLiteral("power")).toObject();
        queueOp([hp, v] {
            MDRPower power{};
            if (MDRResult r = mdrHeadphonesGetPower(hp, &power); r != MDR_RESULT_OK)
                return r;
            if (v.contains(QStringLiteral("autoPowerOff")))
                power.auto_power_off_minutes = uint32_t(v.value(QStringLiteral("autoPowerOff")).toInt());
            if (v.contains(QStringLiteral("offWhenRemoved")))
                power.wearing_power = v.value(QStringLiteral("offWhenRemoved")).toBool() ? MDR_WEARING_POWER_WHEN_REMOVED : MDR_WEARING_POWER_DISABLED;
            if (v.contains(QStringLiteral("autoPause")))
                power.auto_pause = mdrBool(v.value(QStringLiteral("autoPause")).toBool());
            power.shutdown_requested = MDR_FALSE;
            return mdrHeadphonesSetPower(hp, &power);
        });
    }

    if (in.contains(QStringLiteral("voiceGuidance"))) {
        const QJsonObject v = in.value(QStringLiteral("voiceGuidance")).toObject();
        queueOp([hp, v] {
            MDRVoiceGuidance vg{};
            if (MDRResult r = mdrHeadphonesGetVoiceGuidance(hp, &vg); r != MDR_RESULT_OK)
                return r;
            if (v.contains(QStringLiteral("enabled")))
                vg.enabled = mdrBool(v.value(QStringLiteral("enabled")).toBool());
            if (v.contains(QStringLiteral("volume")))
                vg.volume = int8_t(v.value(QStringLiteral("volume")).toInt());
            return mdrHeadphonesSetVoiceGuidance(hp, &vg);
        });
    }

    if (auto priority = valueOf(AudioPriorities, in.value(QStringLiteral("connectionMode")).toString())) {
        queueOp([hp, p = *priority] {
            const MDRConnectionMode cm{p};
            return mdrHeadphonesSetConnectionMode(hp, &cm);
        });
    }

    if (const QJsonValue v = in.value(QStringLiteral("autoSwitch")); v.isBool())
        queueOp([hp, on = v.toBool()] { return mdrHeadphonesSetSourceSwitchControl(hp, mdrBool(on)); });

    if (const QJsonValue v = in.value(QStringLiteral("pairing")); v.isBool()) {
        queueOp([hp, on = v.toBool()] {
            const MDRPairing pairing{mdrBool(on)};
            return mdrHeadphonesSetPairing(hp, &pairing);
        });
    }

    if (const QJsonValue v = in.value(QStringLiteral("volume")); v.isDouble()) {
        queueOp([hp, volume = v.toInt()] {
            MDRPlayback pb{};
            if (MDRResult r = mdrHeadphonesGetPlayback(hp, &pb); r != MDR_RESULT_OK)
                return r;
            pb.volume = uint8_t(std::clamp(volume, 0, 30));
            return mdrHeadphonesSetPlayback(hp, &pb);
        });
    }

    const QJsonObject general = in.value(QStringLiteral("general")).toObject();
    for (auto it = general.constBegin(); it != general.constEnd(); ++it) {
        bool ok = false;
        const uint32_t index = it.key().toUInt(&ok);
        if (!ok || !it.value().isBool())
            continue;
        queueOp([hp, index, on = it.value().toBool()] {
            const MDRGeneralSetting setting{index, mdrBool(on)};
            return mdrHeadphonesSetGeneralSetting(hp, &setting);
        });
    }
    return {};
}

QDBusError SonyBackend::playback(const QString &action)
{
    if (!ready())
        return notReady();
    const auto a = valueOf(PlaybackActions, action);
    if (!a)
        return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Action must be play, pause, next or previous"));
    queueOp([hp = _hp, a = *a] {
        const MDRPlaybackCommand command{a};
        return mdrHeadphonesPlayback(hp, &command);
    });
    return {};
}

QDBusError SonyBackend::powerOff()
{
    if (!ready())
        return notReady();
    queueOp([hp = _hp] {
        MDRPower power{};
        if (MDRResult r = mdrHeadphonesGetPower(hp, &power); r != MDR_RESULT_OK)
            return r;
        power.shutdown_requested = MDR_TRUE;
        return mdrHeadphonesSetPower(hp, &power);
    });
    return {};
}

QDBusError SonyBackend::respondToAlert(bool accept)
{
    if (!ready())
        return notReady();
    _alert.clear();
    queueOp([hp = _hp, accept] {
        return mdrHeadphonesRequestRespondToAlert(hp, accept ? MDR_ALERT_ACTION_POSITIVE : MDR_ALERT_ACTION_NEGATIVE);
    });
    return {};
}
