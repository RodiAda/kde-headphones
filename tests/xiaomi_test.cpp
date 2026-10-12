#include <QCoreApplication>

#include <cstdio>
#include <cstring>

#include "Registry.h"
#include "XiaomiBackend.h"

namespace {

struct FakeLink {
    QByteArray incoming;
    QByteArray outgoing;
};

MDRResult fakeRecv(void *user, char *dst, int size, int *received)
{
    auto *link = static_cast<FakeLink *>(user);
    if (link->incoming.isEmpty())
        return MDR_RESULT_INPROGRESS;
    const int n = std::min<int>(size, int(link->incoming.size()));
    std::memcpy(dst, link->incoming.constData(), n);
    link->incoming.remove(0, n);
    *received = n;
    return MDR_RESULT_OK;
}

MDRResult fakeSend(void *user, const char *src, int size, int *sent)
{
    static_cast<FakeLink *>(user)->outgoing.append(src, size);
    *sent = size;
    return MDR_RESULT_OK;
}

const char *fakeError(void *)
{
    return "fake";
}

int failures = 0;

void check(bool ok, const QString &what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        failures++;
}

constexpr uint8_t PhoneRequest = 0xC4;
constexpr uint8_t Response = 0x04;
constexpr uint8_t EarbudsRequest = 0xC0;
constexpr uint8_t EarbudsNotify = 0xC7;

QByteArray frame(uint8_t type, uint8_t opcode, uint8_t sequence, const QByteArray &payload)
{
    const bool response = !(type & 0x40);
    const int length = int(payload.size()) + (response ? 2 : 1);
    QByteArray f = QByteArray::fromHex("fedcba");
    f.append(char(type));
    f.append(char(opcode));
    f.append(char(length >> 8));
    f.append(char(length & 0xFF));
    if (response)
        f.append(char(0));
    f.append(char(sequence));
    f.append(payload);
    f.append(char(0xEF));
    return f;
}

struct Decoded {
    uint8_t type;
    uint8_t opcode;
    uint8_t sequence;
    QByteArray payload;
};

QList<Decoded> decode(const QByteArray &bytes)
{
    QList<Decoded> out;
    qsizetype i = 0;
    while (i + 8 <= bytes.size()) {
        if (bytes.mid(i, 3) != QByteArray::fromHex("fedcba"))
            return {};
        const uint8_t type = uint8_t(bytes.at(i + 3));
        const int length = (uint8_t(bytes.at(i + 5)) << 8) | uint8_t(bytes.at(i + 6));
        const int overhead = (type & 0x40) ? 1 : 2;
        if (uint8_t(bytes.at(i + 7 + length)) != 0xEF)
            return {};
        out.append({type, uint8_t(bytes.at(i + 4)), uint8_t(bytes.at(i + 6 + overhead)), bytes.mid(i + 7 + overhead, length - overhead)});
        i += 8 + length;
    }
    return out;
}

bool sent(const QByteArray &bytes, uint8_t type, uint8_t opcode, const QByteArray &payload)
{
    const QList<Decoded> frames = decode(bytes);
    return std::any_of(frames.begin(), frames.end(), [&](const Decoded &d) { return d.type == type && d.opcode == opcode && d.payload == payload; });
}

bool sentWithSequence(const QByteArray &bytes, uint8_t type, uint8_t opcode, uint8_t sequence, const QByteArray &payload)
{
    const QList<Decoded> frames = decode(bytes);
    return std::any_of(frames.begin(), frames.end(), [&](const Decoded &d) {
        return d.type == type && d.opcode == opcode && d.sequence == sequence && d.payload == payload;
    });
}

int level(const QJsonArray &batteries, const char *part)
{
    for (const QJsonValue &b : batteries) {
        if (b.toObject().value(QStringLiteral("part")).toString() == QLatin1String(part))
            return b.toObject().value(QStringLiteral("level")).toInt();
    }
    return -1;
}

bool charging(const QJsonArray &batteries, const char *part)
{
    for (const QJsonValue &b : batteries) {
        if (b.toObject().value(QStringLiteral("part")).toString() == QLatin1String(part))
            return b.toObject().value(QStringLiteral("charging")).toBool();
    }
    return false;
}

QJsonValue findControl(const QJsonObject &settings, const char *group, const char *key)
{
    for (const QJsonValue &v : settings.value(QLatin1String(group)).toArray()) {
        if (v.toObject().value(QStringLiteral("key")).toString() == QLatin1String(key))
            return v.toObject().value(QStringLiteral("value"));
    }
    return QJsonValue(QJsonValue::Undefined);
}

QJsonArray findOptions(const QJsonObject &settings, const char *key)
{
    for (const QJsonValue &v : settings.value(QStringLiteral("choices")).toArray()) {
        if (v.toObject().value(QStringLiteral("key")).toString() == QLatin1String(key))
            return v.toObject().value(QStringLiteral("options")).toArray();
    }
    return {};
}

std::array<uint8_t, 16> bytes16(const char *hex)
{
    const QByteArray b = QByteArray::fromHex(hex);
    std::array<uint8_t, 16> out{};
    for (int i = 0; i < 16 && i < b.size(); i++)
        out[i] = uint8_t(b.at(i));
    return out;
}

QByteArray hex(const std::array<uint8_t, 16> &a)
{
    return QByteArray(reinterpret_cast<const char *>(a.data()), int(a.size())).toHex();
}

struct Session {
    FakeLink link;
    MDRConnection conn{};
    XiaomiBackend backend;

    explicit Session(const char *name)
        : backend(&conn, DeviceInfo{QString::fromLatin1(name), QStringLiteral("00:11:22:33:44:55"), {}, DeviceKind::Xiaomi})
    {
        conn.user = &link;
        conn.recv = fakeRecv;
        conn.send = fakeSend;
        conn.getLastError = fakeError;
    }

    void feed(const QByteArray &bytes)
    {
        link.incoming.append(bytes);
        backend.tick();
    }

    void authenticate()
    {
        backend.start();
        feed(frame(Response, 0x50, 0, QByteArray::fromHex("01") + QByteArray(16, '\x5a')));
        feed(frame(EarbudsRequest, 0x50, 1, QByteArray::fromHex("01000102030405060708090a0b0c0d0e0f")));
        feed(frame(EarbudsRequest, 0x51, 2, QByteArray::fromHex("0100")));
    }
};

void testChallengeResponse()
{
    const struct {
        const char *challenge;
        const char *response;
    } vectors[] = {
        {"00000000000000000000000000000000", "bca5905bc849392e7bf9fdcdc570ef77"},
        {"000102030405060708090a0b0c0d0e0f", "8713913c41434e091cad794a1b5d95c4"},
        {"ffffffffffffffffffffffffffffffff", "f26249f9a7bc42731feb945fd9e8d06f"},
        {"5a1c93e07b2f4d8866a1c3e5f7092b4d", "adb4590d9be456c957376c73eb9be16a"},
    };
    for (const auto &v : vectors) {
        const QByteArray got = hex(XiaomiBackend::challengeResponse(bytes16(v.challenge)));
        check(got == QByteArray(v.response), QStringLiteral("challenge %1 -> %2").arg(QLatin1String(v.challenge), QString::fromLatin1(got)));
    }
}

void testEncoding()
{
    check(XiaomiBackend::encodeFrame(PhoneRequest, 0xF3, 5, QByteArray::fromHex("000b")) == QByteArray::fromHex("fedcbac4f3000305000bef"),
          QStringLiteral("request frame layout"));
    check(XiaomiBackend::encodeFrame(Response, 0x0E, 7, {}) == QByteArray::fromHex("fedcba040e00020007ef"), QStringLiteral("empty response frame layout"));
    check(XiaomiBackend::encodeFrame(Response, 0x51, 2, QByteArray::fromHex("01")) == QByteArray::fromHex("fedcba04510003000201ef"),
          QStringLiteral("auth confirm response layout"));
}

void testModels()
{
    const struct {
        const char *name;
        XiaomiModel model;
    } cases[] = {
        {"Redmi Buds 3 Pro", XiaomiModel::RedmiBuds3Pro},
        {"Redmi Buds 4 Active", XiaomiModel::RedmiBuds4Active},
        {"Redmi Buds 5 Pro", XiaomiModel::RedmiBuds5Pro},
        {"Redmi Buds 6 Pro", XiaomiModel::RedmiBuds6Pro},
        {"Redmi Buds 6 Active", XiaomiModel::RedmiBuds6Active},
        {"Redmi Buds 6 Lite", XiaomiModel::RedmiBuds6Lite},
        {"Redmi Buds 6", XiaomiModel::RedmiBuds6},
        {"REDMI Buds 8 Active", XiaomiModel::RedmiBuds8Active},
        {"Xiaomi Buds 5 Pro", XiaomiModel::Unknown},
        {"Redmi Buds 5", XiaomiModel::Unknown},
    };
    for (const auto &c : cases)
        check(XiaomiBackend::modelFromName(QString::fromLatin1(c.name)) == c.model, QStringLiteral("model from name: %1").arg(QLatin1String(c.name)));
}

void testDiscovery()
{
    const QString xiaomi = QStringLiteral("0000fd2d-0000-1000-8000-00805f9b34fb");
    const QString spp = QStringLiteral("00001101-0000-1000-8000-00805f9b34fb");
    const auto match = [&](const QString &alias, const QStringList &uuids) {
        const auto services = knownServices();
        const auto it = std::find_if(services.begin(), services.end(), [&](const KnownService &s) {
            return (!s.uuid || uuids.contains(QString::fromLatin1(s.uuid), Qt::CaseInsensitive))
                && (!s.nameHint || alias.contains(QLatin1String(s.nameHint), Qt::CaseInsensitive));
        });
        return it == services.end() ? std::optional<DeviceKind>() : std::optional<DeviceKind>(it->kind);
    };
    check(match(QStringLiteral("Redmi Buds 5 Pro"), {spp, xiaomi}) == DeviceKind::Xiaomi, QStringLiteral("discovery: Redmi Buds with SPP is Xiaomi, not Galaxy"));
    check(match(QStringLiteral("REDMI Buds 8 Active"), {xiaomi}) == DeviceKind::Xiaomi, QStringLiteral("discovery: upper-case REDMI"));
    check(match(QStringLiteral("Xiaomi Buds 5"), {xiaomi}) == DeviceKind::Xiaomi, QStringLiteral("discovery: Xiaomi Buds"));
    check(match(QStringLiteral("Redmi Buds 5 Pro"), {spp}) == DeviceKind::GalaxyClassic, QStringLiteral("discovery: without the Xiaomi service nothing changes"));
    check(match(QStringLiteral("Galaxy Buds+ (A1B2)"), {spp}) == DeviceKind::GalaxyClassic, QStringLiteral("discovery: Galaxy Buds+ stays Galaxy"));
    check(!match(QStringLiteral("Xiaomi Sound Pocket"), {xiaomi}), QStringLiteral("discovery: other Xiaomi devices are ignored"));
}

void testBuds5Pro()
{
    Session s("Redmi Buds 5 Pro");
    s.backend.start();
    QList<Decoded> out = decode(s.link.outgoing);
    check(out.size() == 1 && out[0].type == PhoneRequest && out[0].opcode == 0x50 && out[0].payload.size() == 17 && out[0].payload.at(0) == 1,
          QStringLiteral("5 Pro: sends a 16-byte challenge first"));

    s.link.outgoing.clear();
    s.feed(frame(Response, 0x50, 0, QByteArray::fromHex("01") + QByteArray(16, '\x5a')));
    check(sent(s.link.outgoing, PhoneRequest, 0x51, QByteArray::fromHex("0100")), QStringLiteral("5 Pro: confirms after the challenge answer"));

    s.link.outgoing.clear();
    s.feed(frame(EarbudsRequest, 0x50, 0x21, QByteArray::fromHex("01000102030405060708090a0b0c0d0e0f")));
    check(sentWithSequence(s.link.outgoing, Response, 0x50, 0x21, QByteArray::fromHex("018713913c41434e091cad794a1b5d95c4")),
          QStringLiteral("5 Pro: answers the earbuds challenge"));
    check(!s.backend.authenticated(), QStringLiteral("5 Pro: not authenticated before confirmation"));

    s.link.outgoing.clear();
    s.feed(frame(EarbudsRequest, 0x51, 0x22, QByteArray::fromHex("0100")));
    check(s.backend.authenticated(), QStringLiteral("5 Pro: authenticated"));
    check(sentWithSequence(s.link.outgoing, Response, 0x51, 0x22, QByteArray::fromHex("01")), QStringLiteral("5 Pro: acknowledges confirmation"));
    check(sent(s.link.outgoing, PhoneRequest, 0x02, QByteArray::fromHex("ffffffff")), QStringLiteral("5 Pro: requests device info"));
    check(sent(s.link.outgoing, PhoneRequest, 0x09, QByteArray::fromHex("ffffffff")), QStringLiteral("5 Pro: requests run info"));
    for (const char *key : {"000b", "0025", "0007", "0003", "0004", "0029"})
        check(sent(s.link.outgoing, PhoneRequest, 0xF3, QByteArray::fromHex(key)), QStringLiteral("5 Pro: requests config %1").arg(QLatin1String(key)));
    check(!s.backend.ready(), QStringLiteral("5 Pro: not ready before battery"));

    s.feed(frame(Response, 0x02, 3, QByteArray::fromHex("05011234aabb 050300010002 0407e45037")));
    check(s.backend.ready(), QStringLiteral("5 Pro: ready after device info"));
    s.feed(frame(Response, 0x09, 4, QByteArray::fromHex("020901 020a00")));
    s.feed(frame(Response, 0xF3, 5, QByteArray::fromHex("04000b0102")) + frame(Response, 0xF3, 6, QByteArray::fromHex("03002500"))
           + frame(Response, 0xF3, 7, QByteArray::fromHex("03000705")) + frame(Response, 0xF3, 8, QByteArray::fromHex("03000301"))
           + frame(Response, 0xF3, 9, QByteArray::fromHex("03000400")) + frame(Response, 0xF3, 10, QByteArray::fromHex("03002901")));

    Snapshot snap = s.backend.snapshot();
    check(level(snap.batteries, "left") == 100 && charging(snap.batteries, "left"), QStringLiteral("5 Pro: left 100% charging"));
    check(level(snap.batteries, "right") == 80 && !charging(snap.batteries, "right"), QStringLiteral("5 Pro: right 80%"));
    check(level(snap.batteries, "case") == 55, QStringLiteral("5 Pro: case 55%"));
    check(snap.battery == 80, QStringLiteral("5 Pro: overall battery is the lower bud"));
    check(snap.modes == QStringList{QStringLiteral("noise-cancelling"), QStringLiteral("adaptive"), QStringLiteral("ambient"), QStringLiteral("off")},
          QStringLiteral("5 Pro: modes %1").arg(snap.modes.join(QLatin1Char(','))));
    check(snap.mode == QStringLiteral("noise-cancelling"), QStringLiteral("5 Pro: mode noise-cancelling"));
    check(snap.settings.value(QStringLiteral("model")).toString() == QStringLiteral("Redmi Buds 5 Pro"), QStringLiteral("5 Pro: model"));
    check(snap.settings.value(QStringLiteral("untested")).toBool(), QStringLiteral("5 Pro: untested"));
    check(snap.settings.value(QStringLiteral("firmware")).toString() == QStringLiteral("1.2.3.4"), QStringLiteral("5 Pro: firmware"));
    const QJsonObject eq = snap.settings.value(QStringLiteral("equalizer")).toObject();
    check(eq.value(QStringLiteral("preset")).toInt() == 5 && eq.value(QStringLiteral("presets")).toArray().size() == 5, QStringLiteral("5 Pro: equalizer bass boost"));
    check(findControl(snap.settings, "toggles", "earDetection") == QJsonValue(true), QStringLiteral("5 Pro: ear detection on"));
    check(findControl(snap.settings, "toggles", "autoAnswer") == QJsonValue(true), QStringLiteral("5 Pro: auto answer on"));
    check(findControl(snap.settings, "toggles", "multipoint") == QJsonValue(false), QStringLiteral("5 Pro: multipoint off"));
    check(findControl(snap.settings, "toggles", "adaptiveSound") == QJsonValue(true), QStringLiteral("5 Pro: adaptive sound on"));
    check(findControl(snap.settings, "choices", "ancStrength") == QJsonValue(QStringLiteral("high")), QStringLiteral("5 Pro: deep ANC is high"));
    check(findOptions(snap.settings, "ancStrength").size() == 3, QStringLiteral("5 Pro: three ANC strengths"));
    check(findControl(snap.settings, "choices", "ambientStrength").isUndefined(), QStringLiteral("5 Pro: ambient strength unknown yet"));

    s.link.outgoing.clear();
    s.feed(frame(EarbudsNotify, 0xF4, 0x30, QByteArray::fromHex("04000b0201")));
    snap = s.backend.snapshot();
    check(snap.mode == QStringLiteral("ambient"), QStringLiteral("5 Pro: notify switches to ambient"));
    check(findControl(snap.settings, "choices", "ambientStrength") == QJsonValue(QStringLiteral("enhance-voice")), QStringLiteral("5 Pro: ambient strength voice"));
    check(findOptions(snap.settings, "ambientStrength").size() == 3, QStringLiteral("5 Pro: three ambient strengths"));
    check(sentWithSequence(s.link.outgoing, Response, 0xF4, 0x30, {}), QStringLiteral("5 Pro: acknowledges config notification"));

    s.link.outgoing.clear();
    s.feed(frame(EarbudsNotify, 0x0E, 0x31, QByteArray::fromHex("04003233ff 020400")));
    snap = s.backend.snapshot();
    check(level(snap.batteries, "left") == 50 && level(snap.batteries, "right") == 51, QStringLiteral("5 Pro: status battery update"));
    check(level(snap.batteries, "case") == 55, QStringLiteral("5 Pro: case level kept while unreported"));
    check(snap.mode == QStringLiteral("off"), QStringLiteral("5 Pro: status switches mode off"));
    check(sentWithSequence(s.link.outgoing, Response, 0x0E, 0x31, {}), QStringLiteral("5 Pro: acknowledges status report"));

    s.link.outgoing.clear();
    check(!s.backend.setMode(QStringLiteral("adaptive")).isValid(), QStringLiteral("5 Pro: adaptive accepted"));
    s.backend.tick();
    check(sent(s.link.outgoing, PhoneRequest, 0x08, QByteArray::fromHex("020401")), QStringLiteral("5 Pro: adaptive turns ANC on"));
    check(sent(s.link.outgoing, PhoneRequest, 0xF2, QByteArray::fromHex("03002501")), QStringLiteral("5 Pro: adaptive enables adaptive ANC"));
    check(s.backend.snapshot().mode == QStringLiteral("adaptive"), QStringLiteral("5 Pro: mode adaptive"));

    s.link.outgoing.clear();
    QJsonObject change{
        {QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 6}}},
        {QStringLiteral("toggles"), QJsonObject{{QStringLiteral("earDetection"), false}, {QStringLiteral("multipoint"), true}}},
        {QStringLiteral("choices"), QJsonObject{{QStringLiteral("ancStrength"), QStringLiteral("low")}, {QStringLiteral("ambientStrength"), QStringLiteral("enhance-ambient")}}},
    };
    check(!s.backend.apply(change).isValid(), QStringLiteral("5 Pro: apply accepted"));
    s.backend.tick();
    check(sent(s.link.outgoing, PhoneRequest, 0xF2, QByteArray::fromHex("03000706")), QStringLiteral("5 Pro: sets treble boost preset"));
    check(sent(s.link.outgoing, PhoneRequest, 0x08, QByteArray::fromHex("020601")), QStringLiteral("5 Pro: disables ear detection"));
    check(sent(s.link.outgoing, PhoneRequest, 0xF2, QByteArray::fromHex("03000401")), QStringLiteral("5 Pro: enables multipoint"));
    check(sent(s.link.outgoing, PhoneRequest, 0xF2, QByteArray::fromHex("03002500")), QStringLiteral("5 Pro: manual strength leaves adaptive ANC"));
    check(sent(s.link.outgoing, PhoneRequest, 0xF2, QByteArray::fromHex("04000b0101")), QStringLiteral("5 Pro: sets light ANC"));
    check(sent(s.link.outgoing, PhoneRequest, 0xF2, QByteArray::fromHex("04000b0202")), QStringLiteral("5 Pro: sets ambient enhancement"));
    snap = s.backend.snapshot();
    check(snap.mode == QStringLiteral("noise-cancelling"), QStringLiteral("5 Pro: back to noise cancelling"));
    check(findControl(snap.settings, "toggles", "earDetection") == QJsonValue(false), QStringLiteral("5 Pro: ear detection off"));
    check(s.backend.apply(QJsonObject{{QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 0x15}}}}).isValid(),
          QStringLiteral("5 Pro: unknown preset rejected"));
    check(s.backend.setAmbientLevel(1).isValid(), QStringLiteral("5 Pro: ambient level unsupported"));
}

void testBuds3ProAdaptiveStrength()
{
    Session s("Redmi Buds 3 Pro");
    s.authenticate();
    s.feed(frame(Response, 0x02, 3, QByteArray::fromHex("0407 3c 3c 64")));
    s.feed(frame(Response, 0x09, 4, QByteArray::fromHex("020901")));
    s.feed(frame(Response, 0xF3, 5, QByteArray::fromHex("04000b0103")));
    Snapshot snap = s.backend.snapshot();
    check(snap.modes == QStringList{QStringLiteral("noise-cancelling"), QStringLiteral("adaptive"), QStringLiteral("ambient"), QStringLiteral("off")},
          QStringLiteral("3 Pro: modes %1").arg(snap.modes.join(QLatin1Char(','))));
    check(snap.mode == QStringLiteral("adaptive"), QStringLiteral("3 Pro: adaptive strength is adaptive mode"));
    check(findControl(snap.settings, "choices", "ancStrength") == QJsonValue(QStringLiteral("medium")), QStringLiteral("3 Pro: manual strength defaults to balanced"));

    s.link.outgoing.clear();
    check(!s.backend.setMode(QStringLiteral("noise-cancelling")).isValid(), QStringLiteral("3 Pro: noise cancelling accepted"));
    s.backend.tick();
    check(sent(s.link.outgoing, PhoneRequest, 0xF2, QByteArray::fromHex("04000b0100")), QStringLiteral("3 Pro: leaves adaptive with balanced strength"));
    check(s.backend.snapshot().mode == QStringLiteral("noise-cancelling"), QStringLiteral("3 Pro: mode noise-cancelling"));

    s.feed(frame(EarbudsNotify, 0xF4, 0x40, QByteArray::fromHex("04000b0202")));
    check(findControl(s.backend.snapshot().settings, "choices", "ambientStrength").isUndefined(), QStringLiteral("3 Pro: third ambient level rejected"));
    s.feed(frame(EarbudsNotify, 0xF4, 0x41, QByteArray::fromHex("04000b0201")));
    check(findOptions(s.backend.snapshot().settings, "ambientStrength").size() == 2, QStringLiteral("3 Pro: two ambient strengths"));
}

void testBuds6Active()
{
    Session s("Redmi Buds 6 Active");
    s.authenticate();
    check(!sent(s.link.outgoing, PhoneRequest, 0xF3, QByteArray::fromHex("000b")), QStringLiteral("6 Active: no strength query"));
    check(sent(s.link.outgoing, PhoneRequest, 0xF3, QByteArray::fromHex("0007")), QStringLiteral("6 Active: equalizer query"));
    s.feed(frame(Response, 0x02, 3, QByteArray::fromHex("0407 46 ff ff")));
    s.feed(frame(Response, 0x09, 4, QByteArray::fromHex("020901 020a00")));
    s.feed(frame(Response, 0xF3, 5, QByteArray::fromHex("03000707")));
    const Snapshot snap = s.backend.snapshot();
    check(snap.battery == 70 && level(snap.batteries, "right") < 0 && level(snap.batteries, "case") < 0, QStringLiteral("6 Active: one bud reported"));
    check(snap.modes.isEmpty() && snap.mode.isEmpty(), QStringLiteral("6 Active: no noise control"));
    check(!snap.settings.contains(QStringLiteral("toggles")), QStringLiteral("6 Active: no ear detection"));
    const QJsonArray presets = snap.settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("presets")).toArray();
    check(presets.size() == 5 && presets.last().toObject().value(QStringLiteral("key")).toString() == QStringLiteral("volume-boost"),
          QStringLiteral("6 Active: volume boost preset"));
    check(s.backend.setMode(QStringLiteral("off")).isValid(), QStringLiteral("6 Active: mode rejected"));
}

void testUnknownAndFraming()
{
    Session s("Xiaomi Buds 5");
    s.authenticate();
    const QByteArray stream = QByteArray::fromHex("00fedc11") + frame(Response, 0x02, 3, QByteArray::fromHex("0407 21 22 23"))
        + frame(Response, 0x09, 4, QByteArray::fromHex("020902"));
    for (const char c : stream)
        s.feed(QByteArray(1, c));
    check(s.backend.ready(), QStringLiteral("unknown: ready from byte-by-byte stream"));
    const Snapshot snap = s.backend.snapshot();
    check(snap.settings.value(QStringLiteral("model")).toString() == QStringLiteral("Xiaomi Buds 5"), QStringLiteral("unknown: model is the device name"));
    check(snap.modes.size() == 3 && snap.mode == QStringLiteral("ambient"), QStringLiteral("unknown: modes from reported noise control"));
    check(!snap.settings.contains(QStringLiteral("equalizer")) && !snap.settings.contains(QStringLiteral("choices")), QStringLiteral("unknown: minimal layout"));
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testChallengeResponse();
    testEncoding();
    testModels();
    testDiscovery();
    testBuds5Pro();
    testBuds3ProAdaptiveStrength();
    testBuds6Active();
    testUnknownAndFraming();
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
