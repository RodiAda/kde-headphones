#include "XiaomiBackend.h"

#include <QRandomGenerator>

#include <algorithm>
#include <span>

#include "Log.h"

namespace {

constexpr uint8_t Header[] = {0xFE, 0xDC, 0xBA};
constexpr uint8_t Trailer = 0xEF;
constexpr int PrefixSize = 7;
constexpr int MaxLength = 1024;

constexpr uint8_t TypePhoneRequest = 0xC4;
constexpr uint8_t TypeResponse = 0x04;
constexpr uint8_t RequestBit = 0x40;

constexpr uint8_t OpDeviceInfo = 0x02;
constexpr uint8_t OpAnc = 0x08;
constexpr uint8_t OpRunInfo = 0x09;
constexpr uint8_t OpStatus = 0x0E;
constexpr uint8_t OpAuthChallenge = 0x50;
constexpr uint8_t OpAuthConfirm = 0x51;
constexpr uint8_t OpSetConfig = 0xF2;
constexpr uint8_t OpGetConfig = 0xF3;
constexpr uint8_t OpNotifyConfig = 0xF4;

constexpr uint8_t InfoFirmware = 0x01;
constexpr uint8_t InfoBattery = 0x07;
constexpr uint8_t RunAnc = 0x09;
constexpr uint8_t RunEarDetection = 0x0A;
constexpr uint8_t StatusBattery = 0x00;
constexpr uint8_t StatusAnc = 0x04;
constexpr uint8_t AncTlvMode = 0x04;
constexpr uint8_t AncTlvEarDetection = 0x06;

constexpr uint8_t CfgAutoAnswer = 0x03;
constexpr uint8_t CfgMultipoint = 0x04;
constexpr uint8_t CfgEqPreset = 0x07;
constexpr uint8_t CfgEffectStrength = 0x0B;
constexpr uint8_t CfgAdaptiveAnc = 0x25;
constexpr uint8_t CfgAdaptiveSound = 0x29;

constexpr uint8_t AncOff = 0x00;
constexpr uint8_t AncOn = 0x01;
constexpr uint8_t AncTransparency = 0x02;

constexpr uint8_t TargetAnc = 0x01;
constexpr uint8_t TargetTransparency = 0x02;

constexpr uint8_t StrengthBalanced = 0x00;
constexpr uint8_t StrengthLight = 0x01;
constexpr uint8_t StrengthDeep = 0x02;
constexpr uint8_t StrengthAdaptive = 0x03;

constexpr uint8_t BatteryAbsent = 0xFF;
constexpr uint8_t BatteryLevelMask = 0x7F;
constexpr uint8_t BatteryChargingBit = 0x80;

constexpr qint64 StatusTimeoutMs = 10000;
constexpr qint64 InfoPollMs = 60000;

struct Preset {
    int id;
    const char *key;
};

constexpr Preset Buds3ProPresets[] = {
    {0x00, "standard"},
    {0x05, "bass-boost"},
    {0x06, "treble-boost"},
    {0x01, "vocal"},
};

constexpr Preset Buds5ProPresets[] = {
    {0x00, "standard"},
    {0x05, "bass-boost"},
    {0x06, "treble-boost"},
    {0x01, "vocal"},
    {0x0A, "custom"},
};

constexpr Preset Buds6ActivePresets[] = {
    {0x00, "standard"},
    {0x05, "bass-boost"},
    {0x06, "treble-boost"},
    {0x01, "vocal"},
    {0x07, "volume-boost"},
};

constexpr Preset Buds8ActivePresets[] = {
    {0x15, "balanced"},
    {0x05, "bass-boost"},
    {0x06, "treble-boost"},
    {0x01, "vocal"},
    {0x07, "volume-boost"},
    {0x0A, "custom"},
};

struct Level {
    uint8_t code;
    const char *key;
};

constexpr Level AncLevels[] = {
    {StrengthLight, "low"},
    {StrengthBalanced, "medium"},
    {StrengthDeep, "high"},
};

constexpr Level AmbientLevels[] = {
    {0x00, "regular"},
    {0x01, "enhance-voice"},
    {0x02, "enhance-ambient"},
};

enum class Adaptive { None, Strength, Config };

uint8_t at(const QByteArray &p, int i)
{
    return i >= 0 && i < p.size() ? uint8_t(p.at(i)) : 0;
}

QDBusError notReady()
{
    return QDBusError(QDBusError::Failed, QStringLiteral("Headphones are not ready"));
}

QJsonObject control(const QString &key, const QJsonValue &value)
{
    return QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("value"), value}};
}

QJsonObject choice(const QString &key, const QString &value, const QStringList &options)
{
    QJsonObject o = control(key, value);
    o[QStringLiteral("options")] = QJsonArray::fromStringList(options);
    return o;
}

uint32_t powMod(uint32_t base, uint32_t exponent, uint32_t modulus)
{
    uint32_t result = 1;
    base %= modulus;
    while (exponent) {
        if (exponent & 1)
            result = result * base % modulus;
        base = base * base % modulus;
        exponent >>= 1;
    }
    return result;
}

struct SaferTables {
    std::array<uint8_t, 256> exp{};
    std::array<uint8_t, 256> log{};
    std::array<std::array<uint8_t, 16>, 16> bias{};
};

const SaferTables &saferTables()
{
    static const SaferTables tables = [] {
        SaferTables t;
        for (int i = 0; i < 256; i++) {
            const uint32_t v = powMod(45, uint32_t(i), 257);
            t.exp[i] = uint8_t(v == 256 ? 0 : v);
            t.log[t.exp[i]] = uint8_t(i);
        }
        for (int k = 0; k < 16; k++) {
            for (int j = 0; j < 16; j++) {
                const uint32_t v = powMod(45, powMod(45, uint32_t(17 * (k + 2) + j + 1), 257), 257);
                t.bias[k][j] = uint8_t(v == 256 ? 0 : v);
            }
        }
        return t;
    }();
    return tables;
}

bool xorLane(int i)
{
    return (0x9999 >> i) & 1;
}

void linearLayer(std::array<uint8_t, 16> &v)
{
    static constexpr int Shuffle[16] = {8, 11, 12, 15, 2, 1, 6, 5, 10, 9, 14, 13, 0, 7, 4, 3};
    for (int pass = 0; pass < 4; pass++) {
        for (int i = 0; i < 16; i += 2) {
            const uint8_t a = v[i];
            const uint8_t b = v[i + 1];
            v[i] = uint8_t(2 * a + b);
            v[i + 1] = uint8_t(a + b);
        }
        if (pass == 3)
            break;
        const std::array<uint8_t, 16> copy = v;
        for (int i = 0; i < 16; i++)
            v[i] = copy[Shuffle[i]];
    }
}

}

struct XiaomiModelSpec {
    XiaomiModel model;
    const char *match;
    const char *name;
    bool anc;
    bool ancStrength;
    int ambientLevels;
    Adaptive adaptive;
    std::span<const Preset> presets;
    bool earDetection;
    bool autoAnswer;
    bool multipoint;
    bool adaptiveSound;
};

namespace {

using Spec = XiaomiModelSpec;

const Spec Specs[] = {
    {XiaomiModel::RedmiBuds3Pro, "Redmi Buds 3 Pro", "Redmi Buds 3 Pro", true, true, 2, Adaptive::Strength, Buds3ProPresets, true, true, true, false},
    {XiaomiModel::RedmiBuds4Active, "Redmi Buds 4 Active", "Redmi Buds 4 Active", false, false, 0, Adaptive::None, {}, false, false, false, false},
    {XiaomiModel::RedmiBuds5Pro, "Redmi Buds 5 Pro", "Redmi Buds 5 Pro", true, true, 3, Adaptive::Config, Buds5ProPresets, true, true, true, true},
    {XiaomiModel::RedmiBuds6Pro, "Redmi Buds 6 Pro", "Redmi Buds 6 Pro", true, true, 3, Adaptive::Config, Buds5ProPresets, true, true, true, true},
    {XiaomiModel::RedmiBuds6Active, "Redmi Buds 6 Active", "Redmi Buds 6 Active", false, false, 0, Adaptive::None, Buds6ActivePresets, false, false, false, false},
    {XiaomiModel::RedmiBuds6Lite, "Redmi Buds 6 Lite", "Redmi Buds 6 Lite", true, false, 0, Adaptive::None, {}, false, true, false, false},
    {XiaomiModel::RedmiBuds6, "Redmi Buds 6", "Redmi Buds 6", true, true, 3, Adaptive::Config, Buds5ProPresets, true, true, true, true},
    {XiaomiModel::RedmiBuds8Active, "Redmi Buds 8 Active", "Redmi Buds 8 Active", false, false, 0, Adaptive::None, Buds8ActivePresets, false, false, true, true},
    {XiaomiModel::Unknown, "", "Xiaomi", false, false, 0, Adaptive::None, {}, false, false, false, false},
};

}

XiaomiModel XiaomiBackend::modelFromName(const QString &name)
{
    for (const Spec &s : Specs) {
        if (s.model != XiaomiModel::Unknown && name.contains(QLatin1String(s.match), Qt::CaseInsensitive))
            return s.model;
    }
    return XiaomiModel::Unknown;
}

QString XiaomiBackend::modelName(XiaomiModel model)
{
    for (const Spec &s : Specs) {
        if (s.model == model)
            return QString::fromLatin1(s.name);
    }
    return QStringLiteral("Xiaomi");
}

QByteArray XiaomiBackend::encodeFrame(uint8_t type, uint8_t opcode, uint8_t sequence, const QByteArray &payload)
{
    const bool response = !(type & RequestBit);
    const int length = int(payload.size()) + (response ? 2 : 1);
    QByteArray frame(reinterpret_cast<const char *>(Header), sizeof(Header));
    frame.append(char(type));
    frame.append(char(opcode));
    frame.append(char((length >> 8) & 0xFF));
    frame.append(char(length & 0xFF));
    if (response)
        frame.append(char(0));
    frame.append(char(sequence));
    frame.append(payload);
    frame.append(char(Trailer));
    return frame;
}

std::array<uint8_t, 16> XiaomiBackend::challengeResponse(const std::array<uint8_t, 16> &challenge)
{
    static constexpr std::array<uint8_t, 16> Input = {0x11, 0x22, 0x33, 0x33, 0x22, 0x11, 0x11, 0x22, 0x33, 0x33, 0x22, 0x11, 0x11, 0x22, 0x33, 0x33};
    const SaferTables &t = saferTables();

    std::array<std::array<uint8_t, 16>, 17> keys{};
    keys[0] = challenge;
    keys[0][15] ^= 6;
    std::array<uint8_t, 17> reg{};
    for (int i = 0; i < 16; i++) {
        reg[i] = keys[0][i];
        reg[16] ^= keys[0][i];
    }
    for (int k = 1; k < 17; k++) {
        for (uint8_t &b : reg)
            b = uint8_t((b << 3) | (b >> 5));
        for (int i = 0; i < 16; i++)
            keys[k][i] = uint8_t(reg[(k + i) % 17] + t.bias[k - 1][i]);
    }

    std::array<uint8_t, 16> v = Input;
    for (int round = 0; round < 8; round++) {
        if (round == 2) {
            for (int i = 0; i < 16; i++)
                v[i] = xorLane(i) ? uint8_t(v[i] ^ Input[i]) : uint8_t(v[i] + Input[i]);
        }
        const auto &k1 = keys[2 * round];
        const auto &k2 = keys[2 * round + 1];
        for (int i = 0; i < 16; i++) {
            if (xorLane(i))
                v[i] = uint8_t(t.exp[uint8_t(v[i] ^ k1[i])] + k2[i]);
            else
                v[i] = uint8_t(t.log[uint8_t(v[i] + k1[i])] ^ k2[i]);
        }
        linearLayer(v);
    }
    for (int i = 0; i < 16; i++)
        v[i] = xorLane(i) ? uint8_t(v[i] ^ keys[16][i]) : uint8_t(v[i] + keys[16][i]);
    return v;
}

XiaomiBackend::XiaomiBackend(MDRConnection *connection, const DeviceInfo &device)
    : Backend(connection, device)
    , _model(modelFromName(device.name))
    , _manualStrength(StrengthBalanced)
{
}

const XiaomiModelSpec &XiaomiBackend::spec() const
{
    for (const Spec &s : Specs) {
        if (s.model == _model)
            return s;
    }
    return Specs[std::size(Specs) - 1];
}

QStringList XiaomiBackend::modes() const
{
    QStringList m;
    if (!_anc)
        return m;
    m << ModeNc;
    const Adaptive adaptive = spec().adaptive;
    if (adaptive == Adaptive::Strength || (adaptive == Adaptive::Config && _adaptiveAnc))
        m << ModeAdaptive;
    m << ModeAmbient << ModeOff;
    return m;
}

QString XiaomiBackend::currentMode() const
{
    if (!_anc)
        return {};
    switch (*_anc) {
    case AncOn: {
        const Adaptive adaptive = spec().adaptive;
        if (adaptive == Adaptive::Strength && _ancStrength == StrengthAdaptive)
            return ModeAdaptive;
        if (adaptive == Adaptive::Config && _adaptiveAnc == true)
            return ModeAdaptive;
        return ModeNc;
    }
    case AncTransparency:
        return ModeAmbient;
    default:
        return ModeOff;
    }
}

bool XiaomiBackend::start()
{
    _sinceStart.start();
    _sincePoll.start();
    qCInfo(lcHp) << "xiaomi model:" << modelName(_model) << (_model == XiaomiModel::Unknown ? "(unrecognized, using a minimal layout)" : "");
    QByteArray payload(1, char(0x01));
    for (int i = 0; i < 16; i++)
        payload.append(char(QRandomGenerator::system()->bounded(256)));
    request(OpAuthChallenge, payload);
    return flush();
}

void XiaomiBackend::request(uint8_t opcode, const QByteArray &payload)
{
    _tx.append(encodeFrame(TypePhoneRequest, opcode, _sequence, payload));
    qCDebug(lcHp) << "xiaomi tx" << Qt::hex << opcode << payload.toHex(' ');
    _sequence++;
}

void XiaomiBackend::respond(uint8_t opcode, uint8_t sequence, const QByteArray &payload)
{
    _tx.append(encodeFrame(TypeResponse, opcode, sequence, payload));
}

void XiaomiBackend::setConfig(uint8_t key, const QByteArray &value)
{
    QByteArray payload;
    payload.append(char(value.size() + 2));
    payload.append(char(0));
    payload.append(char(key));
    payload.append(value);
    request(OpSetConfig, payload);
}

void XiaomiBackend::setAnc(uint8_t mode)
{
    QByteArray payload;
    payload.append(char(0x02));
    payload.append(char(AncTlvMode));
    payload.append(char(mode));
    request(OpAnc, payload);
    _anc = mode;
}

void XiaomiBackend::sendQueries()
{
    const QByteArray all("\xFF\xFF\xFF\xFF", 4);
    request(OpDeviceInfo, all);
    request(OpRunInfo, all);
    const Spec &s = spec();
    QList<uint8_t> configs;
    if (s.ancStrength || s.ambientLevels)
        configs << CfgEffectStrength;
    if (s.adaptive == Adaptive::Config)
        configs << CfgAdaptiveAnc;
    if (!s.presets.empty())
        configs << CfgEqPreset;
    if (s.autoAnswer)
        configs << CfgAutoAnswer;
    if (s.multipoint)
        configs << CfgMultipoint;
    if (s.adaptiveSound)
        configs << CfgAdaptiveSound;
    for (const uint8_t key : configs) {
        QByteArray payload(1, char(0));
        payload.append(char(key));
        request(OpGetConfig, payload);
    }
}

bool XiaomiBackend::flush()
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

bool XiaomiBackend::receive()
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

    const QByteArray header(reinterpret_cast<const char *>(Header), sizeof(Header));
    for (;;) {
        const qsizetype start = _rx.indexOf(header);
        if (start < 0) {
            _rx = _rx.right(sizeof(Header) - 1);
            break;
        }
        _rx.remove(0, start);
        if (_rx.size() < PrefixSize)
            break;
        const uint8_t type = at(_rx, 3);
        const uint8_t opcode = at(_rx, 4);
        const int length = (at(_rx, 5) << 8) | at(_rx, 6);
        const bool response = !(type & RequestBit);
        const int overhead = response ? 2 : 1;
        if (length < overhead || length > MaxLength) {
            _rx.remove(0, 1);
            continue;
        }
        const qsizetype total = PrefixSize + length + 1;
        if (_rx.size() < total)
            break;
        if (at(_rx, int(total) - 1) != Trailer) {
            _rx.remove(0, 1);
            continue;
        }
        const uint8_t sequence = at(_rx, PrefixSize + overhead - 1);
        const QByteArray payload = _rx.mid(PrefixSize + overhead, length - overhead);
        _rx.remove(0, total);
        qCDebug(lcHp) << "xiaomi rx" << Qt::hex << type << opcode << payload.toHex(' ');
        handle(type, opcode, sequence, payload);
    }
    return true;
}

bool XiaomiBackend::tick()
{
    if (!receive())
        return false;
    if (ready() && _sincePoll.elapsed() > InfoPollMs) {
        _sincePoll.restart();
        request(OpDeviceInfo, QByteArray("\xFF\xFF\xFF\xFF", 4));
    }
    if (!flush())
        return false;
    if (!ready() && _sinceStart.elapsed() > StatusTimeoutMs)
        return fail(_authenticated ? QStringLiteral("no status from the earbuds") : QStringLiteral("earbuds did not authenticate"));
    return true;
}

void XiaomiBackend::handleAuth(uint8_t type, uint8_t opcode, uint8_t sequence, const QByteArray &payload)
{
    const bool response = !(type & RequestBit);
    if (opcode == OpAuthChallenge) {
        if (response) {
            request(OpAuthConfirm, QByteArray("\x01\x00", 2));
            return;
        }
        if (payload.size() < 17)
            return;
        std::array<uint8_t, 16> challenge{};
        for (int i = 0; i < 16; i++)
            challenge[i] = at(payload, i + 1);
        const std::array<uint8_t, 16> answer = challengeResponse(challenge);
        QByteArray reply(1, char(0x01));
        reply.append(reinterpret_cast<const char *>(answer.data()), int(answer.size()));
        respond(OpAuthChallenge, sequence, reply);
        return;
    }
    if (response)
        return;
    respond(OpAuthConfirm, sequence, QByteArray(1, char(0x01)));
    if (!_authenticated) {
        _authenticated = true;
        sendQueries();
    }
}

void XiaomiBackend::handle(uint8_t type, uint8_t opcode, uint8_t sequence, const QByteArray &payload)
{
    const bool response = !(type & RequestBit);
    switch (opcode) {
    case OpAuthChallenge:
    case OpAuthConfirm:
        handleAuth(type, opcode, sequence, payload);
        break;
    case OpDeviceInfo:
        if (response)
            parseDeviceInfo(payload);
        break;
    case OpRunInfo:
        if (response)
            parseRunInfo(payload);
        break;
    case OpStatus:
        if (!response) {
            parseStatus(payload);
            respond(OpStatus, sequence);
        }
        break;
    case OpGetConfig:
        if (response)
            parseConfig(payload, false);
        break;
    case OpNotifyConfig:
        if (!response) {
            parseConfig(payload, true);
            respond(OpNotifyConfig, sequence);
        }
        break;
    default:
        break;
    }
}

void XiaomiBackend::parseBattery(const QByteArray &p, int offset)
{
    if (p.size() < offset + 3)
        return;
    Battery b = _battery.value_or(Battery{});
    const auto read = [&](int index, int &level, bool &charging) {
        const uint8_t v = at(p, offset + index);
        if (v == BatteryAbsent || (v & BatteryLevelMask) > 100) {
            level = -1;
            charging = false;
            return;
        }
        level = v & BatteryLevelMask;
        charging = v & BatteryChargingBit;
    };
    read(0, b.left, b.chargingLeft);
    read(1, b.right, b.chargingRight);
    const int oldCase = b.caseLevel;
    const bool oldCaseCharging = b.chargingCase;
    read(2, b.caseLevel, b.chargingCase);
    if (b.caseLevel < 0 && oldCase >= 0 && (b.left >= 0 || b.right >= 0)) {
        b.caseLevel = oldCase;
        b.chargingCase = oldCaseCharging;
    }
    _battery = b;
}

void XiaomiBackend::parseDeviceInfo(const QByteArray &p)
{
    for (int i = 0; i + 1 < p.size();) {
        const int length = at(p, i);
        if (length < 1)
            break;
        const uint8_t index = at(p, i + 1);
        if (index == InfoBattery && length >= 4) {
            parseBattery(p, i + 2);
        } else if (index == InfoFirmware && length >= 5) {
            const uint8_t hi = at(p, i + 2);
            const uint8_t lo = at(p, i + 3);
            _firmware = QStringLiteral("%1.%2.%3.%4").arg(hi >> 4).arg(hi & 0xF).arg(lo >> 4).arg(lo & 0xF);
        }
        i += length + 1;
    }
}

void XiaomiBackend::parseRunInfo(const QByteArray &p)
{
    for (int i = 0; i + 2 < p.size();) {
        const int length = at(p, i);
        if (length < 1)
            break;
        const uint8_t index = at(p, i + 1);
        const uint8_t value = at(p, i + 2);
        if (index == RunAnc && length >= 2 && value <= AncTransparency && (spec().anc || _model == XiaomiModel::Unknown))
            _anc = value;
        else if (index == RunEarDetection && length >= 2 && (spec().earDetection || _model == XiaomiModel::Unknown))
            _earDetection = value == 0;
        i += length + 1;
    }
}

void XiaomiBackend::parseStatus(const QByteArray &p)
{
    for (int i = 0; i + 1 < p.size();) {
        const int length = at(p, i);
        if (length < 1)
            break;
        const uint8_t index = at(p, i + 1);
        if (index == StatusBattery && length >= 4)
            parseBattery(p, i + 2);
        else if (index == StatusAnc && length >= 2 && _anc && at(p, i + 2) <= AncTransparency)
            _anc = at(p, i + 2);
        i += length + 1;
    }
}

void XiaomiBackend::parseConfig(const QByteArray &p, bool notify)
{
    const Spec &s = spec();
    for (int i = 0; i + 2 < p.size();) {
        const int length = at(p, i);
        if (length < 2)
            break;
        const uint16_t key = uint16_t((at(p, i + 1) << 8) | at(p, i + 2));
        const QByteArray data = p.mid(i + 3, length - 2);
        i += length + 1;
        if (data.isEmpty())
            continue;
        const uint8_t v = at(data, 0);
        switch (key) {
        case CfgEffectStrength: {
            if (data.size() < 2)
                break;
            const uint8_t level = at(data, 1);
            if (notify && _anc && v <= AncTransparency)
                _anc = v;
            if (v == TargetAnc && s.ancStrength && level <= StrengthAdaptive) {
                if (level != StrengthAdaptive || s.adaptive == Adaptive::Strength)
                    _ancStrength = level;
                if (level != StrengthAdaptive)
                    _manualStrength = level;
            } else if (v == TargetTransparency && level < s.ambientLevels) {
                _ambientStrength = level;
            }
            break;
        }
        case CfgAdaptiveAnc:
            if (s.adaptive == Adaptive::Config)
                _adaptiveAnc = v == 1;
            break;
        case CfgEqPreset:
            if (!s.presets.empty())
                _equalizer = v;
            break;
        case CfgAutoAnswer:
            if (s.autoAnswer)
                _autoAnswer = v == 1;
            break;
        case CfgMultipoint:
            if (s.multipoint)
                _multipoint = v == 1;
            break;
        case CfgAdaptiveSound:
            if (s.adaptiveSound)
                _adaptiveSound = v == 1;
            break;
        default:
            break;
        }
    }
}

Snapshot XiaomiBackend::snapshot()
{
    Snapshot s;
    if (!_battery)
        return s;
    const Battery &b = *_battery;
    const Spec &sp = spec();

    const auto add = [&](const char *part, int level, bool charging) {
        if (level < 0)
            return;
        s.batteries.append(QJsonObject{
            {QStringLiteral("part"), QString::fromLatin1(part)},
            {QStringLiteral("level"), level},
            {QStringLiteral("charging"), charging},
        });
    };
    add("left", b.left, b.chargingLeft);
    add("right", b.right, b.chargingRight);
    add("case", b.caseLevel, b.chargingCase);
    if (b.left >= 0 && b.right >= 0)
        s.battery = std::min(b.left, b.right);
    else
        s.battery = std::max(b.left, b.right);

    s.modes = modes();
    s.mode = currentMode();

    QJsonObject o;
    o[QStringLiteral("model")] = _model == XiaomiModel::Unknown && !_device.name.trimmed().isEmpty() ? _device.name.trimmed() : modelName(_model);
    o[QStringLiteral("untested")] = true;
    if (!_firmware.isEmpty())
        o[QStringLiteral("firmware")] = _firmware;

    if (_equalizer) {
        QJsonArray presets;
        int current = -1;
        for (const Preset &preset : sp.presets) {
            presets.append(QJsonObject{{QStringLiteral("id"), preset.id}, {QStringLiteral("key"), QString::fromLatin1(preset.key)}});
            if (preset.id == *_equalizer)
                current = preset.id;
        }
        o[QStringLiteral("equalizer")] = QJsonObject{
            {QStringLiteral("preset"), current},
            {QStringLiteral("presets"), presets},
        };
    }

    QJsonArray toggles;
    if (_earDetection)
        toggles.append(control(QStringLiteral("earDetection"), *_earDetection));
    if (_autoAnswer)
        toggles.append(control(QStringLiteral("autoAnswer"), *_autoAnswer));
    if (_multipoint)
        toggles.append(control(QStringLiteral("multipoint"), *_multipoint));
    if (_adaptiveSound)
        toggles.append(control(QStringLiteral("adaptiveSound"), *_adaptiveSound));
    if (!toggles.isEmpty())
        o[QStringLiteral("toggles")] = toggles;

    QJsonArray choices;
    if (_ancStrength) {
        QStringList options;
        QString value;
        for (const Level &l : AncLevels) {
            options << QString::fromLatin1(l.key);
            if (l.code == _manualStrength)
                value = QString::fromLatin1(l.key);
        }
        choices.append(choice(QStringLiteral("ancStrength"), value, options));
    }
    if (_ambientStrength) {
        QStringList options;
        for (int i = 0; i < sp.ambientLevels; i++)
            options << QString::fromLatin1(AmbientLevels[i].key);
        choices.append(choice(QStringLiteral("ambientStrength"), QString::fromLatin1(AmbientLevels[*_ambientStrength].key), options));
    }
    if (!choices.isEmpty())
        o[QStringLiteral("choices")] = choices;

    s.settings = o;
    return s;
}

QDBusError XiaomiBackend::setMode(const QString &mode)
{
    if (!ready())
        return notReady();
    if (!modes().contains(mode))
        return QDBusError(QDBusError::NotSupported, QStringLiteral("These earbuds do not support mode %1").arg(mode));
    const Spec &sp = spec();
    if (mode == ModeOff) {
        setAnc(AncOff);
    } else if (mode == ModeAmbient) {
        setAnc(AncTransparency);
    } else {
        const bool adaptive = mode == ModeAdaptive;
        setAnc(AncOn);
        if (sp.adaptive == Adaptive::Config && _adaptiveAnc && *_adaptiveAnc != adaptive) {
            setConfig(CfgAdaptiveAnc, QByteArray(1, char(adaptive ? 1 : 0)));
            _adaptiveAnc = adaptive;
        } else if (sp.adaptive == Adaptive::Strength) {
            const uint8_t level = adaptive ? StrengthAdaptive : _manualStrength;
            if (_ancStrength != level) {
                QByteArray value;
                value.append(char(TargetAnc));
                value.append(char(level));
                setConfig(CfgEffectStrength, value);
                _ancStrength = level;
            }
        }
    }
    if (sp.ancStrength || sp.ambientLevels)
        request(OpGetConfig, QByteArray("\x00\x0B", 2));
    return {};
}

QDBusError XiaomiBackend::setAmbientLevel(int)
{
    return unsupported();
}

QDBusError XiaomiBackend::apply(const QJsonObject &in)
{
    if (!ready())
        return notReady();
    const Spec &sp = spec();

    const QJsonObject eq = in.value(QStringLiteral("equalizer")).toObject();
    if (const QJsonValue v = eq.value(QStringLiteral("preset")); v.isDouble() && _equalizer) {
        const int id = v.toInt();
        const bool known = std::any_of(sp.presets.begin(), sp.presets.end(), [&](const Preset &p) { return p.id == id; });
        if (!known)
            return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Unknown equalizer preset %1").arg(id));
        setConfig(CfgEqPreset, QByteArray(1, char(id)));
        _equalizer = id;
    }

    const QJsonObject toggles = in.value(QStringLiteral("toggles")).toObject();
    if (const QJsonValue v = toggles.value(QStringLiteral("earDetection")); v.isBool() && _earDetection) {
        QByteArray payload;
        payload.append(char(0x02));
        payload.append(char(AncTlvEarDetection));
        payload.append(char(v.toBool() ? 0 : 1));
        request(OpAnc, payload);
        _earDetection = v.toBool();
    }
    const struct {
        const char *key;
        uint8_t config;
        std::optional<bool> &state;
    } flags[] = {
        {"autoAnswer", CfgAutoAnswer, _autoAnswer},
        {"multipoint", CfgMultipoint, _multipoint},
        {"adaptiveSound", CfgAdaptiveSound, _adaptiveSound},
    };
    for (const auto &f : flags) {
        if (const QJsonValue v = toggles.value(QLatin1String(f.key)); v.isBool() && f.state) {
            setConfig(f.config, QByteArray(1, char(v.toBool() ? 1 : 0)));
            f.state = v.toBool();
        }
    }

    const QJsonObject choices = in.value(QStringLiteral("choices")).toObject();
    if (const QJsonValue v = choices.value(QStringLiteral("ancStrength")); v.isString() && _ancStrength) {
        const QString key = v.toString();
        const auto it = std::find_if(std::begin(AncLevels), std::end(AncLevels), [&](const Level &l) { return key == QLatin1String(l.key); });
        if (it == std::end(AncLevels))
            return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Unknown noise cancelling strength %1").arg(key));
        if (sp.adaptive == Adaptive::Config && _adaptiveAnc == true) {
            setConfig(CfgAdaptiveAnc, QByteArray(1, char(0)));
            _adaptiveAnc = false;
        }
        QByteArray value;
        value.append(char(TargetAnc));
        value.append(char(it->code));
        setConfig(CfgEffectStrength, value);
        _ancStrength = it->code;
        _manualStrength = it->code;
    }
    if (const QJsonValue v = choices.value(QStringLiteral("ambientStrength")); v.isString() && _ambientStrength) {
        const QString key = v.toString();
        int index = -1;
        for (int i = 0; i < sp.ambientLevels; i++) {
            if (key == QLatin1String(AmbientLevels[i].key))
                index = i;
        }
        if (index < 0)
            return QDBusError(QDBusError::InvalidArgs, QStringLiteral("Unknown ambient sound strength %1").arg(key));
        QByteArray value;
        value.append(char(TargetTransparency));
        value.append(char(AmbientLevels[index].code));
        setConfig(CfgEffectStrength, value);
        _ambientStrength = uint8_t(index);
    }
    return {};
}
