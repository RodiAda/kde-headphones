#include "SoundcoreBackend.h"

#include <algorithm>
#include <cmath>

#include "Log.h"

namespace {

const QByteArray OutboundPrefix = QByteArray::fromHex("08ee000000");
const QByteArray InboundPrefix = QByteArray::fromHex("09ff000001");

constexpr int HeaderSize = 9;
constexpr int MaxPacketSize = 4096;

constexpr uint16_t CmdState = 0x0101;
constexpr uint16_t CmdTwsStatus = 0x0102;
constexpr uint16_t CmdBatteryLevel = 0x0103;
constexpr uint16_t CmdBatteryCharging = 0x0104;
constexpr uint16_t CmdGamingModeUpdate = 0x0111;
constexpr uint16_t CmdSoundModesUpdate = 0x0601;
constexpr uint16_t CmdSetSoundModes = 0x0681;
constexpr uint16_t CmdSetEqualizer = 0x0281;
constexpr uint16_t CmdSetEqualizerDrc = 0x0283;
constexpr uint16_t CmdSetEqualizerHearId = 0x0386;
constexpr uint16_t CmdSetEqualizerHearIdSupported = 0x0387;

constexpr uint16_t CmdAutoPlayPause = 0x0181;
constexpr uint16_t CmdTouchTone = 0x0183;
constexpr uint16_t CmdGamingMode = 0x0187;
constexpr uint16_t CmdVoicePromptQ45 = 0x01AE;
constexpr uint16_t CmdSurroundSound = 0x0286;
constexpr uint16_t CmdTouchLock = 0x0486;
constexpr uint16_t CmdDualConnections = 0x0B84;
constexpr uint16_t CmdGamingModeLiberty = 0x1085;

constexpr qint64 StateTimeoutMs = 10000;
constexpr qint64 StateRetryMs = 2000;
constexpr qint64 AckTimeoutMs = 1500;

constexpr int CustomPreset = 0xFEFE;
constexpr int VisibleBands = 8;
constexpr int HearIdChannels = 2;
constexpr int MinVolume = -120;
constexpr int MaxVolume = 134;

constexpr uint8_t AmbientNoiseCancelling = 0;
constexpr uint8_t AmbientTransparency = 1;
constexpr uint8_t AmbientNormal = 2;
constexpr uint8_t NcManual = 0;
constexpr uint8_t NcAdaptive = 1;

enum class BatteryKind { Single, SingleLevel, Dual };
enum class SoundKind { None, Classic, SpaceQ45, SpaceA40, Liberty4Nc, P30i };
enum class EqKind { Plain, Drc, HearId, HearIdForced, SpaceQ45, Liberty4Nc };
enum class PresetSet { Common, Type2, SpaceQ45 };

struct FamilyInfo {
    SoundcoreFamily family;
    BatteryKind battery;
    int batteryMax;
    int caseMax;
    SoundKind sound;
    bool transparencyChoice;
    EqKind eq;
    int channels;
    int bands;
    PresetSet presets;
    int serialOffset;
};

constexpr FamilyInfo Families[] = {
    {SoundcoreFamily::LifeQ30, BatteryKind::Single, 5, 0, SoundKind::Classic, false, EqKind::Plain, 1, 8, PresetSet::Common, 44},
    {SoundcoreFamily::LifeQ35, BatteryKind::Single, 5, 0, SoundKind::Classic, false, EqKind::Plain, 1, 8, PresetSet::Common, 44},
    {SoundcoreFamily::SpaceQ45, BatteryKind::SingleLevel, 5, 0, SoundKind::SpaceQ45, false, EqKind::SpaceQ45, 1, 10, PresetSet::SpaceQ45, 7},
    {SoundcoreFamily::LifeNote3, BatteryKind::Dual, 5, 5, SoundKind::Classic, true, EqKind::Plain, 2, 10, PresetSet::Common, 16},
    {SoundcoreFamily::LifeNote3S, BatteryKind::Dual, 5, 5, SoundKind::None, false, EqKind::Plain, 2, 10, PresetSet::Common, 16},
    {SoundcoreFamily::LifeDot2Nc, BatteryKind::Dual, 5, 0, SoundKind::Classic, true, EqKind::Drc, 2, 8, PresetSet::Common, -1},
    {SoundcoreFamily::SpaceA40, BatteryKind::Dual, 5, 10, SoundKind::SpaceA40, false, EqKind::HearIdForced, 2, 10, PresetSet::Type2, 16},
    {SoundcoreFamily::LibertyAir2Pro, BatteryKind::Dual, 5, 0, SoundKind::Classic, true, EqKind::HearId, 2, 8, PresetSet::Common, -1},
    {SoundcoreFamily::Liberty4Nc, BatteryKind::Dual, 5, 5, SoundKind::Liberty4Nc, false, EqKind::Liberty4Nc, 2, 10, PresetSet::Type2, 16},
    {SoundcoreFamily::P30i, BatteryKind::Dual, 10, 0, SoundKind::P30i, false, EqKind::Drc, 1, 10, PresetSet::Type2, 16},
};

const FamilyInfo &info(SoundcoreFamily family)
{
    for (const auto &f : Families) {
        if (f.family == family)
            return f;
    }
    return Families[0];
}

struct ModelEntry {
    const char *code;
    const char *name;
    const char *nameHint;
    SoundcoreFamily family;
};

constexpr ModelEntry Models[] = {
    {"3027", "Soundcore Life Q35", "Life Q35", SoundcoreFamily::LifeQ35},
    {"3030", "Soundcore Life Tune Pro", "Life Tune Pro", SoundcoreFamily::LifeQ35},
    {"3028", "Soundcore Life Q30", "Life Q30", SoundcoreFamily::LifeQ30},
    {"3029", "Soundcore Life Tune", "Life Tune", SoundcoreFamily::LifeQ30},
    {"3040", "Soundcore Space Q45", "Space Q45", SoundcoreFamily::SpaceQ45},
    {"3945", "Soundcore Life Note 3S", "Life Note 3S", SoundcoreFamily::LifeNote3S},
    {"3933", "Soundcore Life Note 3", "Life Note 3", SoundcoreFamily::LifeNote3},
    {"3939", "Soundcore Life P3", "Life P3", SoundcoreFamily::LifeNote3},
    {"3931", "Soundcore Life Dot 2 NC", "Life Dot 2 NC", SoundcoreFamily::LifeDot2Nc},
    {"3935", "Soundcore Life A2 NC", "Life A2 NC", SoundcoreFamily::LifeDot2Nc},
    {"3936", "Soundcore Space A40", "Space A40", SoundcoreFamily::SpaceA40},
    {"3951", "Soundcore Liberty Air 2 Pro", "Liberty Air 2 Pro", SoundcoreFamily::LibertyAir2Pro},
    {"3947", "Soundcore Liberty 4 NC", "Liberty 4 NC", SoundcoreFamily::Liberty4Nc},
    {"3959", "Soundcore P30i", "P30i", SoundcoreFamily::P30i},
    {"3959", "Soundcore R50i NC", "R50i NC", SoundcoreFamily::P30i},
    {"3028", "Soundcore Life Q30", "Q30", SoundcoreFamily::LifeQ30},
};

struct Preset {
    int id;
    const char *key;
    int values[VisibleBands];
};

constexpr Preset Presets[] = {
    {0, "soundcore-signature", {0, 0, 0, 0, 0, 0, 0, 0}},
    {1, "acoustic", {40, 10, 20, 20, 40, 40, 40, 20}},
    {2, "bass-booster", {40, 30, 10, 0, 0, 0, 0, 0}},
    {3, "bass-reducer", {-40, -30, -10, 0, 0, 0, 0, 0}},
    {4, "classical", {30, 30, -20, -20, 0, 20, 30, 40}},
    {5, "podcast", {-30, 20, 40, 40, 30, 20, 0, -20}},
    {6, "dance", {20, -30, -10, 10, 20, 20, 10, -30}},
    {7, "deep", {20, 10, 30, 30, 20, -20, -40, -50}},
    {8, "electronic", {30, 20, -20, 20, 10, 20, 30, 30}},
    {9, "flat", {-20, -20, -10, 0, 0, 0, -20, -20}},
    {10, "hip-hop", {20, 30, -10, -10, 20, -10, 20, 30}},
    {11, "jazz", {20, 20, -20, -20, 0, 20, 30, 40}},
    {12, "latin", {0, 0, -20, -20, -20, 0, 30, 50}},
    {13, "lounge", {-10, 20, 40, 30, 0, -20, 20, 10}},
    {14, "piano", {0, 30, 30, 20, 40, 50, 30, 40}},
    {15, "pop", {-10, 10, 30, 30, 10, -10, -20, -30}},
    {16, "rnb", {60, 20, -20, -20, 20, 30, 30, 40}},
    {17, "rock", {30, 20, -10, -10, 10, 30, 30, 30}},
    {18, "small-speakers", {40, 30, 10, 0, -20, -30, -40, -40}},
    {19, "spoken-word", {-30, -20, 10, 20, 20, 10, 0, -30}},
    {20, "treble-booster", {-20, -20, -20, -10, 10, 20, 20, 40}},
    {21, "treble-reducer", {0, 0, 0, -20, -30, -40, -40, -60}},
};

constexpr int RockPresetId = 17;
constexpr int RockType2[VisibleBands] = {30, 20, -10, -10, 10, 30, 40, 50};
constexpr int BassBoosterPresetId = 2;

bool hasPreset(PresetSet set, int id)
{
    return !(set == PresetSet::SpaceQ45 && id == BassBoosterPresetId);
}

std::optional<QList<int>> presetValues(PresetSet set, int id)
{
    for (const Preset &p : Presets) {
        if (p.id != id || !hasPreset(set, id))
            continue;
        const int *values = (id == RockPresetId && set != PresetSet::Common) ? RockType2 : p.values;
        QList<int> out(values, values + VisibleBands);
        out << 0 << MinVolume;
        return out;
    }
    return std::nullopt;
}

class Reader
{
public:
    explicit Reader(const QByteArray &data)
        : _data(data)
    {
    }

    bool ok() const { return _ok; }
    int pos() const { return _pos; }

    uint8_t u8()
    {
        if (_pos >= _data.size()) {
            _ok = false;
            return 0;
        }
        return uint8_t(_data.at(_pos++));
    }

    bool flag() { return u8() == 1; }

    int u16()
    {
        const int lo = u8();
        return lo | (u8() << 8);
    }

    QByteArray take(int n)
    {
        if (_pos + n > _data.size()) {
            _ok = false;
            _pos = int(_data.size());
            return {};
        }
        const QByteArray out = _data.mid(_pos, n);
        _pos += n;
        return out;
    }

    void skip(int n) { take(n); }

private:
    const QByteArray &_data;
    int _pos = 0;
    bool _ok = true;
};

uint8_t checksum(const QByteArray &data)
{
    uint8_t sum = 0;
    for (const char c : data)
        sum = uint8_t(sum + uint8_t(c));
    return sum;
}

uint8_t volumeByte(int value)
{
    return uint8_t(std::clamp(value, MinVolume, MaxVolume) - MinVolume);
}

QByteArray volumeBytes(const QList<int> &values, int bands)
{
    QByteArray out;
    for (int i = 0; i < bands; i++)
        out.append(char(volumeByte(values.value(i))));
    return out;
}

QByteArray normalizedChannels(const QByteArray &raw, int channels, int bands)
{
    QByteArray out;
    for (int c = 0; c < channels; c++) {
        const QByteArray channel = raw.mid(c * bands, bands);
        if (channel.size() < bands || uint8_t(channel.at(0)) == 0xFF) {
            out.append(QByteArray(bands, char(0xFF)));
            continue;
        }
        for (const char b : channel)
            out.append(char(std::min<uint8_t>(uint8_t(b), 0xFE)));
    }
    return out;
}

QByteArray dynamicRangeCompression(const QList<int> &values, int bands)
{
    constexpr double Smaller = 0.85;
    constexpr double Larger = 0.95;
    static constexpr double Coefficients[8][8] = {
        {1.26, -0.71 * Smaller, 0.177, -0.0494, 0.0345, -0.0197, 0.0075, -0.00217},
        {-0.71 * Smaller, 1.73 * Larger, 0.0, 0.204, -0.068, 0.045, -0.0235, 0.0075},
        {0.177, -0.81 * Smaller, 1.73 * Larger, -0.81 * Smaller, 0.208, -0.07, 0.045, -0.0197},
        {-0.0494, 0.204, 0.0, 1.73 * Larger, -0.82 * Smaller, 0.208, -0.068, 0.0345},
        {0.0345, -0.068, 0.208, -0.82 * Smaller, 1.73 * Larger, 0.0, 0.204, -0.0494},
        {-0.0197, 0.045, -0.07, 0.208, -0.81 * Smaller, 1.73 * Larger, -0.81 * Smaller, 0.177},
        {0.0075, -0.0235, 0.045, -0.068, 0.204, 0.0, 1.83 * Larger, -0.71 * Smaller},
        {-0.00217, 0.0075, -0.0197, 0.0345, -0.0494, 0.177, -0.71 * Smaller, 1.5},
    };
    double db[8];
    for (int i = 0; i < 8; i++)
        db[i] = std::clamp(values.value(i), MinVolume, MaxVolume) / 10.0;
    const double lows = db[2] * 0.81 * Smaller;
    const double highs = db[5] * 0.81 * Smaller;

    QByteArray out;
    for (int band = 0; band < bands; band++) {
        int value = band == 9 ? MinVolume : 0;
        if (band < 8) {
            double acc = 0.0;
            for (int i = 0; i < 8; i++) {
                if ((band == 1 || band == 3) && i == 2)
                    acc -= lows;
                else if ((band == 4 || band == 6) && i == 5)
                    acc -= highs;
                else
                    acc += db[i] * Coefficients[band][i];
            }
            value = int(std::round(acc / 10.0 * 10.0));
        }
        out.append(char(volumeByte(value)));
    }
    return out;
}

int firmwareNumber(const QByteArray &text)
{
    if (text.size() != 5 || text.at(2) != '.')
        return -1;
    bool okMajor = false;
    bool okMinor = false;
    const int major = text.left(2).toInt(&okMajor);
    const int minor = text.mid(3, 2).toInt(&okMinor);
    return okMajor && okMinor ? major * 100 + minor : -1;
}

int percent(int level, int max)
{
    return max > 0 ? std::clamp(int(std::lround(level * 100.0 / max)), 0, 100) : -1;
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

QDBusError wrongMode()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Not available in the current sound mode"));
}

const char *const ClassicNoiseCancellingModes[] = {"transport", "outdoor", "indoor"};
const char *const FullOrVocal[] = {"full", "vocal"};
const char *const TalkOrManual[] = {"talk", "manual"};
const char *const Strengths[] = {"low", "medium", "high"};

QJsonArray optionList(const char *const *options, int count)
{
    QJsonArray out;
    for (int i = 0; i < count; i++)
        out.append(QString::fromLatin1(options[i]));
    return out;
}

int optionIndex(const char *const *options, int count, const QString &value)
{
    for (int i = 0; i < count; i++) {
        if (value == QLatin1String(options[i]))
            return i;
    }
    return -1;
}

QJsonObject choice(const char *key, const char *const *options, int count, int index)
{
    return QJsonObject{
        {QStringLiteral("key"), QString::fromLatin1(key)},
        {QStringLiteral("options"), optionList(options, count)},
        {QStringLiteral("value"), index >= 0 && index < count ? QString::fromLatin1(options[index]) : QString()},
    };
}

QJsonObject slider(const char *key, int min, int max, int value)
{
    return QJsonObject{
        {QStringLiteral("key"), QString::fromLatin1(key)},
        {QStringLiteral("min"), min},
        {QStringLiteral("max"), max},
        {QStringLiteral("value"), std::clamp(value, min, max)},
    };
}

}

SoundcoreFamily SoundcoreBackend::familyFromName(const QString &name)
{
    for (const auto &m : Models) {
        if (name.contains(QLatin1String(m.nameHint), Qt::CaseInsensitive))
            return m.family;
    }
    return SoundcoreFamily::Unknown;
}

QByteArray SoundcoreBackend::frame(uint16_t command, const QByteArray &body)
{
    QByteArray out = OutboundPrefix;
    const int length = int(OutboundPrefix.size()) + 4 + int(body.size()) + 1;
    out.append(char(command >> 8));
    out.append(char(command & 0xFF));
    out.append(char(length & 0xFF));
    out.append(char((length >> 8) & 0xFF));
    out.append(body);
    out.append(char(checksum(out)));
    return out;
}

SoundcoreBackend::SoundcoreBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
    , _family(SoundcoreFamily::Unknown)
{
    for (const auto &m : Models) {
        if (device.name.contains(QLatin1String(m.nameHint), Qt::CaseInsensitive)) {
            _family = m.family;
            _modelName = QString::fromLatin1(m.name);
            break;
        }
    }
}

bool SoundcoreBackend::start()
{
    _sinceStart.start();
    _sinceRequest.start();
    qCInfo(lcHp) << "soundcore model:" << (_modelName.isEmpty() ? QStringLiteral("unrecognized name, waiting for the serial number") : _modelName);
    send(CmdState, {});
    return true;
}

void SoundcoreBackend::send(uint16_t command, const QByteArray &body)
{
    _queue.append({command, frame(command, body)});
    qCDebug(lcHp) << "soundcore queue" << Qt::hex << command << body.toHex(' ');
}

bool SoundcoreBackend::flush()
{
    if (_awaiting && _sinceSent.elapsed() > AckTimeoutMs) {
        qCDebug(lcHp) << "soundcore no ack for" << Qt::hex << *_awaiting;
        _awaiting.reset();
    }
    if (!_awaiting && _tx.isEmpty() && !_queue.isEmpty()) {
        const Packet p = _queue.takeFirst();
        _tx = p.frame;
        _awaiting = p.command;
        _sinceSent.start();
    }
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

bool SoundcoreBackend::receive()
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
        const qsizetype start = _rx.indexOf(InboundPrefix);
        if (start < 0) {
            _rx = _rx.right(InboundPrefix.size() - 1);
            break;
        }
        _rx.remove(0, start);
        if (_rx.size() < HeaderSize)
            break;
        const int length = uint8_t(_rx.at(7)) | (uint8_t(_rx.at(8)) << 8);
        if (length < HeaderSize + 1 || length > MaxPacketSize) {
            _rx.remove(0, 1);
            continue;
        }
        if (_rx.size() < length)
            break;
        if (checksum(_rx.left(length - 1)) != uint8_t(_rx.at(length - 1))) {
            _rx.remove(0, 1);
            continue;
        }
        const uint16_t command = uint16_t((uint8_t(_rx.at(5)) << 8) | uint8_t(_rx.at(6)));
        const QByteArray body = _rx.mid(HeaderSize, length - HeaderSize - 1);
        _rx.remove(0, length);
        qCDebug(lcHp) << "soundcore rx" << Qt::hex << command << body.toHex(' ');
        handle(command, body);
    }
    return true;
}

bool SoundcoreBackend::tick()
{
    if (!receive())
        return false;
    if (!_status) {
        if (_sinceStart.elapsed() > StateTimeoutMs)
            return fail(_error.isEmpty() ? QStringLiteral("no state from the headphones") : _error);
        if (_sinceRequest.elapsed() > StateRetryMs && _queue.isEmpty() && !_awaiting) {
            _sinceRequest.start();
            send(CmdState, {});
        }
    }
    return flush();
}

void SoundcoreBackend::identify(const QByteArray &body)
{
    for (const auto &m : Models) {
        const int offset = info(m.family).serialOffset;
        if (offset >= 0 && body.mid(offset, 4) == QByteArray(m.code)) {
            _family = m.family;
            _modelName = QString::fromLatin1(m.name);
            qCInfo(lcHp) << "soundcore model from serial number:" << _modelName;
            return;
        }
    }
}

void SoundcoreBackend::handle(uint16_t command, const QByteArray &body)
{
    if (_awaiting && *_awaiting == command)
        _awaiting.reset();

    switch (command) {
    case CmdState: {
        if (_family == SoundcoreFamily::Unknown)
            identify(body);
        if (_family == SoundcoreFamily::Unknown) {
            _error = QStringLiteral("unsupported Soundcore model");
            return;
        }
        if (auto parsed = parseState(body)) {
            if (!_status)
                qCInfo(lcHp) << "soundcore initialized:" << _modelName;
            _status = std::move(parsed);
        } else {
            qCWarning(lcHp) << "soundcore state packet does not match" << _modelName << "layout, size" << body.size();
        }
        break;
    }
    case CmdTwsStatus:
        if (_status && body.size() >= 2) {
            _status->host = uint8_t(body.at(0));
            _status->twsConnected = body.at(1) == 1;
        }
        break;
    case CmdBatteryLevel:
        if (!_status || body.isEmpty())
            break;
        if (info(_family).battery == BatteryKind::Dual) {
            if (body.size() >= 2) {
                _status->left = uint8_t(body.at(0));
                _status->right = uint8_t(body.at(1));
            }
        } else {
            _status->level = uint8_t(body.at(0));
        }
        break;
    case CmdBatteryCharging:
        if (!_status || body.isEmpty())
            break;
        if (info(_family).battery == BatteryKind::Dual) {
            if (body.size() >= 2) {
                _status->chargingLeft = body.at(0) == 1;
                _status->chargingRight = body.at(1) == 1;
            }
        } else {
            _status->charging = body.at(0) == 1;
        }
        break;
    case CmdSoundModesUpdate:
        if (_status && body.size() == soundSize())
            _status->sound = body;
        break;
    case CmdGamingModeUpdate:
        if (_status && !body.isEmpty()) {
            for (Flag &f : _status->flags) {
                if (f.command == CmdGamingMode)
                    f.value = body.at(0) == 1;
            }
        }
        break;
    default:
        break;
    }
}

std::optional<SoundcoreBackend::Status> SoundcoreBackend::parseState(const QByteArray &body) const
{
    const FamilyInfo &f = info(_family);
    Reader r(body);
    Status s;

    const auto tws = [&] {
        s.host = r.u8();
        s.twsConnected = r.flag();
    };
    const auto dualBattery = [&] {
        s.left = r.u8();
        s.right = r.u8();
        s.chargingLeft = r.flag();
        s.chargingRight = r.flag();
    };
    const auto equalizer = [&] {
        s.eqPreset = r.u16();
        s.eq = r.take(f.channels * f.bands);
    };
    const auto sound = [&] { s.sound = r.take(soundSize()); };
    const auto addFlag = [&](const char *key, uint16_t command, bool inverted = false) {
        s.flags.append({key, command, inverted, r.flag() != inverted});
    };
    const auto hearIdAdjustments = [&](bool customFirst) {
        const int size = HearIdChannels * f.bands;
        s.hearIdEnabled = r.flag();
        s.hearIdAdjustments = r.take(size);
        s.hearIdTime = r.take(4);
        s.hearIdType = r.u8();
        if (customFirst)
            s.hearIdCustom = r.take(size);
    };

    switch (_family) {
    case SoundcoreFamily::LifeQ30:
    case SoundcoreFamily::LifeQ35:
        s.level = r.u8();
        s.charging = r.flag();
        equalizer();
        s.gender = r.u8();
        s.ageRange = r.u8();
        r.skip(21);
        sound();
        r.skip(5 + 16);
        if (_family == SoundcoreFamily::LifeQ35)
            addFlag("wearDetection", CmdAutoPlayPause);
        break;
    case SoundcoreFamily::SpaceQ45:
        s.level = r.u8();
        r.skip(1 + 5 + 16);
        equalizer();
        r.skip(10 + 2 + 1 + 1);
        sound();
        r.skip(6 + 1);
        addFlag("multipoint", CmdDualConnections);
        r.skip(2 + 2 + 3);
        addFlag("voicePrompts", CmdVoicePromptQ45);
        r.skip(1 + 5);
        hearIdAdjustments(true);
        r.skip(10);
        s.hearIdGenre = r.take(2);
        break;
    case SoundcoreFamily::LifeNote3:
        tws();
        dualBattery();
        r.skip(10 + 16);
        equalizer();
        s.ageRange = r.u8();
        r.skip(48 + 12 + 1);
        sound();
        r.skip(2);
        if (r.pos() + 4 <= body.size()) {
            addFlag("touchTone", CmdTouchTone);
            addFlag("wearDetection", CmdAutoPlayPause);
            addFlag("lowLatency", CmdGamingMode);
            s.caseLevel = r.u8();
        }
        break;
    case SoundcoreFamily::LifeNote3S:
        tws();
        dualBattery();
        r.skip(10 + 16);
        equalizer();
        r.skip(12);
        addFlag("touchTone", CmdTouchTone);
        addFlag("wearDetection", CmdAutoPlayPause);
        addFlag("lowLatency", CmdGamingMode);
        s.caseLevel = r.u8();
        break;
    case SoundcoreFamily::LifeDot2Nc:
        tws();
        dualBattery();
        equalizer();
        r.skip(12);
        sound();
        r.skip(1);
        addFlag("touchTone", CmdTouchTone);
        break;
    case SoundcoreFamily::SpaceA40:
        tws();
        dualBattery();
        r.skip(10 + 16);
        equalizer();
        s.ageRange = r.u8();
        hearIdAdjustments(true);
        s.hearIdGenre = r.take(2);
        r.skip(1 + 12 + 1);
        sound();
        addFlag("touchTone", CmdTouchTone);
        s.caseLevel = r.u8();
        r.skip(1 + 1);
        addFlag("multipoint", CmdDualConnections);
        r.skip(2);
        addFlag("lowLatency", CmdGamingMode);
        break;
    case SoundcoreFamily::LibertyAir2Pro:
        tws();
        dualBattery();
        equalizer();
        s.gender = r.u8();
        s.ageRange = r.u8();
        hearIdAdjustments(false);
        s.hearIdGenre = r.take(1) + QByteArray(1, '\0');
        s.hearIdCustom = r.take(HearIdChannels * f.bands);
        r.skip(12);
        sound();
        r.skip(1);
        addFlag("wearDetection", CmdAutoPlayPause);
        addFlag("touchTone", CmdTouchTone);
        break;
    case SoundcoreFamily::Liberty4Nc:
        tws();
        dualBattery();
        r.skip(10 + 16 + 5);
        equalizer();
        r.skip(1);
        hearIdAdjustments(true);
        s.hearIdGenre = r.take(1) + QByteArray(1, '\0');
        r.skip(1 + 1 + 16 + 1);
        sound();
        r.skip(6);
        s.caseLevel = r.u8();
        r.skip(1 + 1 + 1);
        addFlag("lowLatency", CmdGamingModeLiberty);
        addFlag("touchTone", CmdTouchTone);
        r.skip(3);
        addFlag("surroundSound", CmdSurroundSound);
        r.skip(3);
        addFlag("earDetection", CmdAutoPlayPause);
        r.skip(1 + 2);
        addFlag("touchControls", CmdTouchLock, true);
        break;
    case SoundcoreFamily::P30i: {
        tws();
        dualBattery();
        const QByteArray firmware = r.take(10);
        r.skip(16);
        equalizer();
        r.skip(10 + 1 + 8 + 1);
        sound();
        r.skip(1);
        addFlag("touchTone", CmdTouchTone);
        addFlag("multipoint", CmdDualConnections);
        addFlag("surroundSound", CmdSurroundSound);
        r.skip(2 + 1);
        int oldest = -1;
        for (const QByteArray &side : {firmware.left(5), firmware.mid(5, 5)}) {
            const int version = firmwareNumber(side);
            if (version > 0)
                oldest = oldest < 0 ? version : std::min(oldest, version);
        }
        if (oldest >= 160)
            addFlag("lowLatency", CmdGamingMode);
        else
            r.skip(1);
        break;
    }
    case SoundcoreFamily::Unknown:
        return std::nullopt;
    }

    if (!r.ok())
        return std::nullopt;
    return s;
}

int SoundcoreBackend::soundSize() const
{
    switch (info(_family).sound) {
    case SoundKind::Classic:
        return 4;
    case SoundKind::SpaceQ45:
    case SoundKind::SpaceA40:
        return 6;
    case SoundKind::Liberty4Nc:
    case SoundKind::P30i:
        return 7;
    case SoundKind::None:
        break;
    }
    return 0;
}

QStringList SoundcoreBackend::modes() const
{
    switch (info(_family).sound) {
    case SoundKind::None:
        return {};
    case SoundKind::Classic:
        return {ModeNc, ModeAmbient, ModeOff};
    default:
        return {ModeNc, ModeAdaptive, ModeAmbient, ModeOff};
    }
}

QString SoundcoreBackend::currentMode() const
{
    if (!_status || _status->sound.size() < 4)
        return {};
    const QByteArray &s = _status->sound;
    switch (uint8_t(s.at(0))) {
    case AmbientNoiseCancelling:
        if (info(_family).sound != SoundKind::Classic && uint8_t(s.at(3)) == NcAdaptive)
            return ModeAdaptive;
        return ModeNc;
    case AmbientTransparency:
        return ModeAmbient;
    default:
        return ModeOff;
    }
}

void SoundcoreBackend::sendSound(const QByteArray &sound)
{
    send(CmdSetSoundModes, sound);
    _status->sound = sound;
}

void SoundcoreBackend::setSoundField(int index, uint8_t value)
{
    QByteArray next = _status->sound;
    next[index] = char(value);
    if (index == 0 && info(_family).sound == SoundKind::P30i)
        next[2] = char(value);
    sendSound(next);
}

Snapshot SoundcoreBackend::snapshot()
{
    Snapshot s;
    if (!_status)
        return s;
    const Status &st = *_status;
    const FamilyInfo &f = info(_family);

    if (f.battery == BatteryKind::Dual) {
        int buds = -1;
        const auto addBud = [&](const char *part, int level, bool charging, int side) {
            if (level < 0 || (!st.twsConnected && st.host != side))
                return;
            const int value = percent(level, f.batteryMax);
            s.batteries.append(QJsonObject{
                {QStringLiteral("part"), QString::fromLatin1(part)},
                {QStringLiteral("level"), value},
                {QStringLiteral("charging"), charging},
            });
            buds = buds < 0 ? value : std::min(buds, value);
        };
        addBud("left", st.left, st.chargingLeft, 0);
        addBud("right", st.right, st.chargingRight, 1);
        if (f.caseMax > 0 && st.caseLevel >= 0 && st.caseLevel != 0xFF) {
            s.batteries.append(QJsonObject{
                {QStringLiteral("part"), QStringLiteral("case")},
                {QStringLiteral("level"), percent(st.caseLevel, f.caseMax)},
                {QStringLiteral("charging"), false},
            });
        }
        s.battery = buds;
    } else if (st.level >= 0) {
        s.battery = percent(st.level, f.batteryMax);
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QStringLiteral("main")},
            {QStringLiteral("level"), s.battery},
            {QStringLiteral("charging"), st.charging},
        });
    }

    s.modes = modes();
    s.mode = currentMode();

    QJsonObject o;
    o[QStringLiteral("model")] = _modelName;
    o[QStringLiteral("untested")] = true;

    QJsonArray presets;
    for (const Preset &p : Presets) {
        if (hasPreset(f.presets, p.id))
            presets.append(QJsonObject{{QStringLiteral("id"), p.id}, {QStringLiteral("key"), QString::fromLatin1(p.key)}});
    }
    if (st.eqPreset == CustomPreset)
        presets.append(QJsonObject{{QStringLiteral("id"), CustomPreset}, {QStringLiteral("key"), QStringLiteral("custom")}});
    QJsonArray bands;
    for (int c = 0; c < f.channels; c++) {
        const QByteArray channel = st.eq.mid(c * f.bands, f.bands);
        if (channel.size() < VisibleBands || uint8_t(channel.at(0)) == 0xFF)
            continue;
        for (int i = 0; i < VisibleBands; i++)
            bands.append(int(std::lround((int(uint8_t(channel.at(i))) + MinVolume) / 10.0)));
        break;
    }
    o[QStringLiteral("equalizer")] = QJsonObject{
        {QStringLiteral("preset"), st.eqPreset},
        {QStringLiteral("presets"), presets},
        {QStringLiteral("bands"), bands},
    };

    QJsonArray toggles;
    for (const Flag &flag : st.flags)
        toggles.append(QJsonObject{{QStringLiteral("key"), QString::fromLatin1(flag.key)}, {QStringLiteral("value"), flag.value}});
    if (!toggles.isEmpty())
        o[QStringLiteral("toggles")] = toggles;

    QJsonArray sliders;
    QJsonArray choices;
    const QByteArray &sound = st.sound;
    if (sound.size() == soundSize() && sound.size() >= 4) {
        const uint8_t ambient = uint8_t(sound.at(0));
        const uint8_t manual = uint8_t(sound.at(1)) >> 4;
        const bool manualNc = ambient == AmbientNoiseCancelling && uint8_t(sound.at(3)) == NcManual;
        switch (f.sound) {
        case SoundKind::Classic:
            if (ambient == AmbientNoiseCancelling)
                choices.append(choice("noiseCancellingMode", ClassicNoiseCancellingModes, 3, uint8_t(sound.at(1))));
            if (ambient == AmbientTransparency && f.transparencyChoice)
                choices.append(choice("transparencyMode", FullOrVocal, 2, uint8_t(sound.at(2))));
            break;
        case SoundKind::SpaceQ45:
            if (manualNc)
                sliders.append(slider("ancStrength", 1, 5, manual));
            if (ambient == AmbientTransparency) {
                choices.append(choice("transparencyMode", TalkOrManual, 2, uint8_t(sound.at(2))));
                if (uint8_t(sound.at(2)) == 1)
                    sliders.append(slider("ambientStrength", 1, 5, uint8_t(sound.at(5))));
            }
            break;
        case SoundKind::SpaceA40:
            if (manualNc)
                choices.append(choice("ancStrength", Strengths, 3, manual - 1));
            if (ambient == AmbientTransparency)
                choices.append(choice("transparencyMode", FullOrVocal, 2, uint8_t(sound.at(2))));
            break;
        case SoundKind::Liberty4Nc:
            if (manualNc)
                sliders.append(slider("ancStrength", 1, 5, manual));
            if (ambient == AmbientTransparency)
                choices.append(choice("transparencyMode", FullOrVocal, 2, uint8_t(sound.at(2))));
            break;
        case SoundKind::P30i:
            if (manualNc)
                sliders.append(slider("ancStrength", 1, 5, manual));
            break;
        case SoundKind::None:
            break;
        }
    }
    if (!sliders.isEmpty())
        o[QStringLiteral("sliders")] = sliders;
    if (!choices.isEmpty())
        o[QStringLiteral("choices")] = choices;

    s.settings = o;
    return s;
}

QDBusError SoundcoreBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!modes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("These headphones do not support mode %1").arg(mode));
    if (_status->sound.size() != soundSize())
        return notReady();

    const SoundKind kind = info(_family).sound;
    const uint8_t ambient = mode == ModeAmbient ? AmbientTransparency : mode == ModeOff ? AmbientNormal : AmbientNoiseCancelling;
    QByteArray target = _status->sound;
    target[0] = char(ambient);
    if (kind == SoundKind::P30i)
        target[2] = char(ambient);
    if (kind != SoundKind::Classic) {
        if (mode == ModeAdaptive)
            target[3] = char(NcAdaptive);
        else if (mode == ModeNc && uint8_t(target.at(3)) == NcAdaptive)
            target[3] = char(NcManual);
    }
    if (target == _status->sound)
        return {};

    const bool migrate = kind == SoundKind::SpaceQ45 || kind == SoundKind::Liberty4Nc || kind == SoundKind::P30i;
    if (migrate && uint8_t(_status->sound.at(0)) != AmbientNoiseCancelling && target.at(3) != _status->sound.at(3)) {
        QByteArray step = _status->sound;
        step[0] = char(AmbientNoiseCancelling);
        if (kind == SoundKind::P30i)
            step[2] = char(AmbientNoiseCancelling);
        sendSound(step);
    }
    sendSound(target);
    return {};
}

QDBusError SoundcoreBackend::setAmbientLevel(int)
{
    return unsupported();
}

void SoundcoreBackend::applyEqualizer(int presetId, const QList<int> &values)
{
    const FamilyInfo &f = info(_family);
    Status &st = *_status;
    const QByteArray channel = volumeBytes(values, f.bands);
    const QByteArray channels = channel.repeated(f.channels);
    const QByteArray drc = dynamicRangeCompression(values, f.bands);
    QByteArray preset;
    preset.append(char(presetId & 0xFF));
    preset.append(char(presetId >> 8));
    const int hearIdSize = HearIdChannels * f.bands;
    const QByteArray genre = st.hearIdGenre.size() == 2 ? st.hearIdGenre : QByteArray(2, '\0');
    const QByteArray time = st.hearIdTime.size() == 4 ? st.hearIdTime : QByteArray(4, '\0');

    switch (f.eq) {
    case EqKind::Plain:
        send(CmdSetEqualizer, preset + channels);
        break;
    case EqKind::Drc:
        send(CmdSetEqualizerDrc, preset + channels + drc.repeated(f.channels));
        break;
    case EqKind::HearId:
    case EqKind::HearIdForced: {
        const bool supported = f.eq == EqKind::HearIdForced || st.ageRange != 0xFF;
        const QByteArray blank(hearIdSize, char(0xFF));
        QByteArray body = preset;
        if (supported)
            body += genre;
        body += channels;
        body.append(char(supported ? st.gender : 0xFF));
        body.append(char(supported ? st.ageRange : 0xFF));
        body.append('\0');
        body += supported ? normalizedChannels(st.hearIdAdjustments, HearIdChannels, f.bands) : blank;
        body += supported ? time : QByteArray(4, '\0');
        body.append(char(st.hearIdType));
        body += supported ? normalizedChannels(st.hearIdCustom, HearIdChannels, f.bands) : blank;
        body += drc.repeated(f.channels);
        send(supported ? CmdSetEqualizerHearIdSupported : CmdSetEqualizerHearId, body);
        break;
    }
    case EqKind::SpaceQ45: {
        const QByteArray custom = normalizedChannels(st.hearIdCustom, 1, f.bands);
        QByteArray body = preset + genre + channel + channel;
        body.append('\0');
        body.append('\1');
        body.append('\0');
        body += normalizedChannels(st.hearIdAdjustments, HearIdChannels, f.bands);
        body += time;
        body.append(char(st.hearIdType));
        body += custom + custom;
        body += drc + drc;
        body.append('\0');
        send(CmdSetEqualizerHearIdSupported, body);
        st.hearIdEnabled = false;
        break;
    }
    case EqKind::Liberty4Nc: {
        QByteArray limited = drc;
        if (limited.size() >= 10)
            limited[9] = '\0';
        QByteArray body = preset + genre + channels;
        body.append('\0');
        body.append('\0');
        body.append('\0');
        body += normalizedChannels(st.hearIdAdjustments, HearIdChannels, f.bands);
        body += time;
        body.append(char(st.hearIdType));
        body += normalizedChannels(st.hearIdCustom, HearIdChannels, f.bands);
        body += limited.repeated(f.channels);
        body.append('\0');
        send(CmdSetEqualizerHearIdSupported, body);
        st.hearIdEnabled = false;
        break;
    }
    }
    st.eqPreset = presetId;
    st.eq = channels;
}

QDBusError SoundcoreBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();
    Status &st = *_status;
    const FamilyInfo &f = info(_family);

    const QJsonObject eq = in.value(QStringLiteral("equalizer")).toObject();
    if (eq.contains(QStringLiteral("preset")) && !eq.contains(QStringLiteral("bands"))) {
        const int id = eq.value(QStringLiteral("preset")).toInt(-1);
        if (const auto values = presetValues(f.presets, id)) {
            applyEqualizer(id, *values);
        } else if (id == CustomPreset) {
            QList<int> values;
            const QJsonArray current = snapshot().settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("bands")).toArray();
            for (int i = 0; i < VisibleBands; i++)
                values << current.at(i).toInt() * 10;
            values << 0 << 0;
            applyEqualizer(CustomPreset, values);
        } else {
            return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Unknown equalizer preset %1").arg(id));
        }
    }
    if (const QJsonValue v = eq.value(QStringLiteral("bands")); v.isArray()) {
        const QJsonArray bands = v.toArray();
        QList<int> values;
        for (int i = 0; i < VisibleBands; i++)
            values << std::clamp(bands.at(i).toInt(), -12, 13) * 10;
        values << 0 << 0;
        applyEqualizer(CustomPreset, values);
    }

    const QJsonObject toggles = in.value(QStringLiteral("toggles")).toObject();
    for (auto it = toggles.begin(); it != toggles.end(); ++it) {
        for (Flag &flag : st.flags) {
            if (it.key() != QLatin1String(flag.key) || !it.value().isBool())
                continue;
            flag.value = it.value().toBool();
            send(flag.command, QByteArray(1, char(flag.value != flag.inverted)));
        }
    }

    const QByteArray &sound = st.sound;
    const bool haveSound = sound.size() == soundSize() && sound.size() >= 4;
    const uint8_t ambient = haveSound ? uint8_t(sound.at(0)) : 0xFF;
    const bool manualNc = haveSound && ambient == AmbientNoiseCancelling && uint8_t(sound.at(3)) == NcManual;
    const auto setManual = [&](int level) {
        const uint8_t byte = uint8_t((level << 4) | (uint8_t(sound.at(1)) & 0x0F));
        setSoundField(1, byte);
    };

    const QJsonObject sliders = in.value(QStringLiteral("sliders")).toObject();
    if (const QJsonValue v = sliders.value(QStringLiteral("ancStrength")); v.isDouble()) {
        if (!manualNc || f.sound == SoundKind::Classic || f.sound == SoundKind::SpaceA40)
            return wrongMode();
        setManual(std::clamp(v.toInt(), 1, 5));
    }
    if (const QJsonValue v = sliders.value(QStringLiteral("ambientStrength")); v.isDouble()) {
        if (f.sound != SoundKind::SpaceQ45 || ambient != AmbientTransparency || uint8_t(sound.at(2)) != 1)
            return wrongMode();
        setSoundField(5, uint8_t(std::clamp(v.toInt(), 1, 5)));
    }

    const QJsonObject choices = in.value(QStringLiteral("choices")).toObject();
    if (const QJsonValue v = choices.value(QStringLiteral("noiseCancellingMode")); v.isString()) {
        const int index = optionIndex(ClassicNoiseCancellingModes, 3, v.toString());
        if (f.sound != SoundKind::Classic || ambient != AmbientNoiseCancelling || index < 0)
            return wrongMode();
        setSoundField(1, uint8_t(index));
    }
    if (const QJsonValue v = choices.value(QStringLiteral("transparencyMode")); v.isString()) {
        const bool talk = f.sound == SoundKind::SpaceQ45;
        const int index = talk ? optionIndex(TalkOrManual, 2, v.toString()) : optionIndex(FullOrVocal, 2, v.toString());
        const bool available = f.sound == SoundKind::SpaceQ45 || f.sound == SoundKind::SpaceA40 || f.sound == SoundKind::Liberty4Nc
            || (f.sound == SoundKind::Classic && f.transparencyChoice);
        if (!available || ambient != AmbientTransparency || index < 0)
            return wrongMode();
        setSoundField(2, uint8_t(index));
    }
    if (const QJsonValue v = choices.value(QStringLiteral("ancStrength")); v.isString()) {
        const int index = optionIndex(Strengths, 3, v.toString());
        if (f.sound != SoundKind::SpaceA40 || !manualNc || index < 0)
            return wrongMode();
        setManual(index + 1);
    }
    return {};
}
