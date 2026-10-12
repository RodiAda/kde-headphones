#include <QCoreApplication>

#include <cstdio>
#include <cstring>
#include <optional>

#include "JabraBackend.h"
#include "Registry.h"

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

QByteArray reply(uint8_t seq, uint16_t feature, const QByteArray &payload)
{
    QByteArray f = QByteArray::fromHex("0904");
    f.append(char(seq));
    f.append(char(0xC0 | (6 + payload.size())));
    f.append(char(feature >> 8));
    f.append(char(feature & 0xFF));
    f.append(payload);
    return f;
}

QByteArray event(uint16_t feature, const QByteArray &payload)
{
    QByteArray f = QByteArray::fromHex("0904");
    f.append(char(0));
    f.append(char(6 + payload.size()));
    f.append(char(feature >> 8));
    f.append(char(feature & 0xFF));
    f.append(payload);
    return f;
}

struct Request {
    uint8_t seq;
    uint8_t type;
    uint16_t feature;
    QByteArray data;
};

QList<Request> decode(const QByteArray &bytes)
{
    QList<Request> out;
    qsizetype i = 0;
    while (i + 6 <= bytes.size()) {
        if (uint8_t(bytes.at(i)) != 0x04 || uint8_t(bytes.at(i + 1)) != 0x09)
            return {};
        const int length = uint8_t(bytes.at(i + 3)) & 0x3F;
        if (length < 6 || i + length > bytes.size())
            return {};
        out.append({uint8_t(bytes.at(i + 2)), uint8_t(uint8_t(bytes.at(i + 3)) & 0xC0),
                    uint16_t((uint8_t(bytes.at(i + 4)) << 8) | uint8_t(bytes.at(i + 5))), bytes.mid(i + 6, length - 6)});
        i += length;
    }
    return out;
}

std::optional<Request> find(const QByteArray &bytes, uint8_t type, uint16_t feature, const QByteArray &data = {}, bool anyData = false)
{
    for (const Request &r : decode(bytes)) {
        if (r.type == type && r.feature == feature && (anyData || r.data == data))
            return r;
    }
    return std::nullopt;
}

bool queried(const QByteArray &bytes, uint16_t feature, const QByteArray &data = {})
{
    return find(bytes, 0x40, feature, data).has_value();
}

bool commanded(const QByteArray &bytes, uint16_t feature, const QByteArray &data)
{
    return find(bytes, 0x80, feature, data).has_value();
}

uint8_t seqOf(const QByteArray &bytes, uint16_t feature)
{
    const auto r = find(bytes, 0x40, feature, {}, true);
    return r ? r->seq : 0;
}

int level(const QJsonArray &batteries, const char *part)
{
    for (const QJsonValue &b : batteries) {
        if (b.toObject().value(QStringLiteral("part")).toString() == QLatin1String(part))
            return b.toObject().value(QStringLiteral("level")).toInt();
    }
    return -1;
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

struct Session {
    FakeLink link;
    MDRConnection conn{};
    JabraBackend backend;

    explicit Session(const char *name)
        : backend(&conn, DeviceInfo{QString::fromLatin1(name), QStringLiteral("00:11:22:33:44:55"), {}, DeviceKind::Jabra})
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
};

const QByteArray SpeechCurve = QByteArray::fromHex("0500000000b4fef206c202ee000006760bb8000006292ee0003d063d5910ff880828");
const QByteArray FlatCurve = QByteArray::fromHex("0500000000b4000006c202ee000006760bb8000006292ee00000063d591000000828");
const QByteArray BassCurve = QByteArray::fromHex("0500000000b4010e06c202eeff8a06760bb8000006292ee00000063d591000000828");

void testEncoding()
{
    const struct {
        uint8_t seq;
        uint8_t type;
        uint16_t feature;
        const char *data;
        const char *expected;
    } frames[] = {
        {0x01, 0x40, 0x0200, "", "0409014602 00"},
        {0x05, 0x40, 0x1202, "", "0409054612 02"},
        {0x07, 0x40, 0x13BE, "01", "0409074713be01"},
        {0x08, 0x80, 0x13BE, "0104", "0409088813be0104"},
        {0x09, 0x80, 0x131E, "14", "04090987131e14"},
        {0x0A, 0x80, 0x0D4C, "00000210", "04090a8a0d4c00000210"},
        {0x0B, 0x40, 0xFC00, "", "04090b46fc00"},
    };
    for (const auto &f : frames) {
        const QByteArray got = JabraBackend::encodeFrame(f.seq, f.type, f.feature, QByteArray::fromHex(f.data));
        const QByteArray want = QByteArray::fromHex(QByteArray(f.expected).replace(' ', ""));
        check(got == want, QStringLiteral("encode %1/%2 -> %3").arg(f.feature, 4, 16).arg(QLatin1String(f.data), QString::fromLatin1(got.toHex(' '))));
    }
    const QByteArray curve = JabraBackend::encodeFrame(0x10, 0x80, 0x137D, SpeechCurve);
    check(curve.size() == 40 && uint8_t(curve.at(3)) == 0xA8, QStringLiteral("equalizer curve frame is 40 bytes with type a8"));
}

void testModels()
{
    const struct {
        const char *name;
        JabraModel model;
    } names[] = {
        {"Jabra Evolve2 55", JabraModel::Evolve2_55},
        {"jabra evolve2 65", JabraModel::Evolve2_65},
        {"Jabra Evolve2 85", JabraModel::Evolve2_85},
        {"Jabra EVOLVE 65", JabraModel::Evolve65},
        {"Jabra Elite 85t", JabraModel::Unknown},
        {"Jabra Evolve2 Buds", JabraModel::Unknown},
    };
    for (const auto &n : names)
        check(JabraBackend::modelFromName(QString::fromLatin1(n.name)) == n.model, QStringLiteral("model of \"%1\"").arg(QLatin1String(n.name)));
}

void testDiscovery()
{
    const QString spp = QStringLiteral("00001101-0000-1000-8000-00805F9B34FB");
    const auto match = [&](const QString &alias) {
        const auto services = knownServices();
        const auto it = std::find_if(services.begin(), services.end(), [&](const KnownService &s) {
            return (!s.uuid || QString::fromLatin1(s.uuid).compare(spp, Qt::CaseInsensitive) == 0)
                && (!s.nameHint || alias.contains(QLatin1String(s.nameHint), Qt::CaseInsensitive));
        });
        return it == services.end() ? std::optional<DeviceKind>() : std::optional<DeviceKind>(it->kind);
    };
    check(match(QStringLiteral("Jabra Evolve2 55")) == DeviceKind::Jabra, QStringLiteral("discovery: Evolve2 55 is Jabra"));
    check(match(QStringLiteral("Jabra Evolve2 Buds")) == DeviceKind::Jabra, QStringLiteral("discovery: Evolve2 Buds is Jabra, not Galaxy"));
    check(match(QStringLiteral("Galaxy Buds+ (A1B2)")) == DeviceKind::GalaxyClassic, QStringLiteral("discovery: Galaxy Buds+ stays Galaxy"));
    check(match(QStringLiteral("Bose QC35 II")) == DeviceKind::Bose, QStringLiteral("discovery: Bose stays Bose"));
    check(!match(QStringLiteral("Car kit")), QStringLiteral("discovery: other SPP devices are ignored"));
}

void testEvolve2_55()
{
    Session s("Jabra Evolve2 55");
    s.backend.start();
    s.backend.tick();
    const QByteArray out = s.link.outgoing;
    check(queried(out, 0x0200) && queried(out, 0x0203) && queried(out, 0x1202), QStringLiteral("Evolve2 55: product, firmware and battery asked"));
    check(queried(out, 0x13BE, QByteArray::fromHex("01")), QStringLiteral("Evolve2 55: ANC mode asked with param 1"));
    check(!queried(out, 0x1387), QStringLiteral("Evolve2 55: Evolve2 85 noise mode not asked"));
    check(queried(out, 0x137E) && queried(out, 0x137D, QByteArray::fromHex("00")), QStringLiteral("Evolve2 55: equalizer asked"));
    check(queried(out, 0x131E) && queried(out, 0x133C) && queried(out, 0x133F) && queried(out, 0x137C) && queried(out, 0x1315)
              && queried(out, 0x1390) && queried(out, 0x133A),
          QStringLiteral("Evolve2 55: settings asked"));
    check(commanded(out, 0x0D4C, QByteArray::fromHex("00000000")) && commanded(out, 0x0D4C, QByteArray::fromHex("00000010"))
              && commanded(out, 0x0D4C, QByteArray::fromHex("00000210")),
          QStringLiteral("Evolve2 55: notifications subscribed"));
    check(!s.backend.ready(), QStringLiteral("Evolve2 55: not ready before battery"));

    QByteArray in;
    in += reply(seqOf(out, 0x0200), 0x0200, QByteArray("\x10" "Jabra Evolve2 55", 17));
    in += reply(seqOf(out, 0x0203), 0x0203, QByteArray("\x05" "1.2.3", 6));
    in += reply(seqOf(out, 0x13BE), 0x13BE, QByteArray::fromHex("04"));
    in += reply(seqOf(out, 0x137E), 0x137E, QByteArray::fromHex("01"));
    in += reply(seqOf(out, 0x137D), 0x137D, SpeechCurve);
    in += reply(seqOf(out, 0x131E), 0x131E, QByteArray::fromHex("14"));
    in += reply(seqOf(out, 0x133C), 0x133C, QByteArray::fromHex("00"));
    in += reply(seqOf(out, 0x133F), 0x133F, QByteArray::fromHex("ff"));
    in += reply(seqOf(out, 0x137C), 0x137C, QByteArray::fromHex("00f7"));
    in += reply(seqOf(out, 0x1315), 0x1315, QByteArray::fromHex("01"));
    in += reply(seqOf(out, 0x1390), 0x1390, QByteArray::fromHex("03"));
    in += reply(seqOf(out, 0x133A), 0x133A, QByteArray::fromHex("01"));
    s.feed(QByteArray::fromHex("ff0004") + in.left(10));
    s.feed(in.mid(10));
    check(!s.backend.ready(), QStringLiteral("Evolve2 55: still not ready without battery"));

    s.feed(reply(seqOf(out, 0x1202), 0x1202, QByteArray::fromHex("04500000")));
    check(s.backend.ready(), QStringLiteral("Evolve2 55: ready after battery"));
    Snapshot snap = s.backend.snapshot();
    check(snap.battery == 80 && level(snap.batteries, "main") == 80, QStringLiteral("Evolve2 55: battery 80%"));
    check(snap.modes == QStringList{QStringLiteral("noise-cancelling"), QStringLiteral("ambient"), QStringLiteral("off")},
          QStringLiteral("Evolve2 55: modes %1").arg(snap.modes.join(QLatin1Char(','))));
    check(snap.mode == QStringLiteral("noise-cancelling"), QStringLiteral("Evolve2 55: ANC on (got %1)").arg(snap.mode));
    check(snap.settings.value(QStringLiteral("model")).toString() == QStringLiteral("Jabra Evolve2 55"), QStringLiteral("Evolve2 55: model"));
    check(snap.settings.value(QStringLiteral("untested")).toBool(), QStringLiteral("Evolve2 55: untested flag"));
    check(snap.settings.value(QStringLiteral("firmware")).toString() == QStringLiteral("1.2.3"), QStringLiteral("Evolve2 55: firmware"));
    const QJsonObject eq = snap.settings.value(QStringLiteral("equalizer")).toObject();
    check(eq.value(QStringLiteral("preset")).toInt() == 1 && eq.value(QStringLiteral("presets")).toArray().size() == 6,
          QStringLiteral("Evolve2 55: equalizer speech among 6 presets"));
    check(findControl(snap.settings, "toggles", "muteReminder") == QJsonValue(true), QStringLiteral("Evolve2 55: mute reminder on"));
    check(findControl(snap.settings, "toggles", "autoRejectCalls") == QJsonValue(false), QStringLiteral("Evolve2 55: auto reject off"));
    check(findControl(snap.settings, "toggles", "buttonSounds") == QJsonValue(true), QStringLiteral("Evolve2 55: button sounds on"));
    check(findControl(snap.settings, "toggles", "sidetone") == QJsonValue(true), QStringLiteral("Evolve2 55: sidetone on"));
    check(findControl(snap.settings, "sliders", "sidetoneLevel").toInt(-1) == 0, QStringLiteral("Evolve2 55: sidetone level 0 from f7"));
    check(findControl(snap.settings, "choices", "voicePrompts").toString() == QStringLiteral("voice"), QStringLiteral("Evolve2 55: voice guidance"));
    check(findControl(snap.settings, "choices", "autoPowerOff").toString() == QStringLiteral("15-min"), QStringLiteral("Evolve2 55: sleep 15 min"));
    check(findOptions(snap.settings, "autoPowerOff").contains(QStringLiteral("never")), QStringLiteral("Evolve2 55: sleep can be disabled"));
    check(findControl(snap.settings, "choices", "callEqualizer").toString() == QStringLiteral("bass"), QStringLiteral("Evolve2 55: call equalizer bass"));

    s.feed(event(0x0D4C, QByteArray::fromHex("090102")));
    check(s.backend.snapshot().mode == QStringLiteral("ambient"), QStringLiteral("Evolve2 55: HearThrough from notification"));
    s.feed(event(0x0D4C, QByteArray::fromHex("090101")) + event(0x1202, QByteArray::fromHex("01410000")));
    snap = s.backend.snapshot();
    check(snap.mode == QStringLiteral("off") && snap.battery == 65, QStringLiteral("Evolve2 55: off and battery from notifications"));
    check(snap.batteries.at(0).toObject().value(QStringLiteral("charging")).toBool(), QStringLiteral("Evolve2 55: charging flag"));

    s.link.outgoing.clear();
    s.backend.setMode(QStringLiteral("noise-cancelling"));
    s.backend.apply(QJsonObject{
        {QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 2}}},
        {QStringLiteral("toggles"), QJsonObject{{QStringLiteral("muteReminder"), false}, {QStringLiteral("autoRejectCalls"), true}, {QStringLiteral("buttonSounds"), false}}},
        {QStringLiteral("sliders"), QJsonObject{{QStringLiteral("sidetoneLevel"), 5}}},
        {QStringLiteral("choices"), QJsonObject{{QStringLiteral("voicePrompts"), QStringLiteral("off")}, {QStringLiteral("autoPowerOff"), QStringLiteral("60-min")}, {QStringLiteral("callEqualizer"), QStringLiteral("treble")}}},
    });
    s.backend.tick();
    const QByteArray cmd = s.link.outgoing;
    check(commanded(cmd, 0x13BE, QByteArray::fromHex("0104")), QStringLiteral("Evolve2 55: ANC on command"));
    check(commanded(cmd, 0x137E, QByteArray::fromHex("01")) && commanded(cmd, 0x137D, BassCurve) && !commanded(cmd, 0x137E, QByteArray::fromHex("00")),
          QStringLiteral("Evolve2 55: bass preset curve"));
    check(commanded(cmd, 0x131E, QByteArray::fromHex("00")), QStringLiteral("Evolve2 55: mute reminder off command"));
    check(commanded(cmd, 0x133C, QByteArray::fromHex("01")), QStringLiteral("Evolve2 55: auto reject on command"));
    check(commanded(cmd, 0x133F, QByteArray::fromHex("00")), QStringLiteral("Evolve2 55: button sounds off command"));
    check(commanded(cmd, 0x137C, QByteArray::fromHex("0006")), QStringLiteral("Evolve2 55: sidetone level 5 keeps sidetone on"));
    check(commanded(cmd, 0x133A, QByteArray::fromHex("02")), QStringLiteral("Evolve2 55: guidance none command"));
    check(commanded(cmd, 0x1390, QByteArray::fromHex("0c")), QStringLiteral("Evolve2 55: sleep 60 min command"));
    check(commanded(cmd, 0x1315, QByteArray::fromHex("02")), QStringLiteral("Evolve2 55: call equalizer treble command"));
    snap = s.backend.snapshot();
    check(snap.mode == QStringLiteral("noise-cancelling") && snap.settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("preset")).toInt() == 2,
          QStringLiteral("Evolve2 55: optimistic state"));

    s.link.outgoing.clear();
    s.backend.apply(QJsonObject{
        {QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 0}}},
        {QStringLiteral("toggles"), QJsonObject{{QStringLiteral("sidetone"), false}}},
    });
    s.backend.setMode(QStringLiteral("ambient"));
    s.backend.tick();
    check(commanded(s.link.outgoing, 0x137D, FlatCurve) && commanded(s.link.outgoing, 0x137E, QByteArray::fromHex("00")),
          QStringLiteral("Evolve2 55: equalizer off writes a flat curve and disables it"));
    check(commanded(s.link.outgoing, 0x137C, QByteArray::fromHex("0106")), QStringLiteral("Evolve2 55: sidetone off command"));
    check(commanded(s.link.outgoing, 0x13BE, QByteArray::fromHex("0102")), QStringLiteral("Evolve2 55: HearThrough command"));
    check(s.backend.snapshot().settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("preset")).toInt() == 0,
          QStringLiteral("Evolve2 55: equalizer off"));

    check(s.backend.apply(QJsonObject{{QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 9}}}}).isValid(),
          QStringLiteral("Evolve2 55: unknown preset rejected"));
    check(s.backend.apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("autoPowerOff"), QStringLiteral("7-min")}}}}).isValid(),
          QStringLiteral("Evolve2 55: unknown sleep timeout rejected"));
    check(s.backend.setMode(QStringLiteral("adaptive")).isValid(), QStringLiteral("Evolve2 55: adaptive rejected"));
    check(s.backend.setAmbientLevel(2).isValid(), QStringLiteral("Evolve2 55: ambient level unsupported"));
}

void testEvolve2_85()
{
    Session s("Jabra Evolve2 85");
    s.backend.start();
    s.backend.tick();
    const QByteArray out = s.link.outgoing;
    check(queried(out, 0x1387) && !queried(out, 0x13BE, QByteArray::fromHex("01")), QStringLiteral("Evolve2 85: noise mode asked"));
    s.feed(reply(seqOf(out, 0x1387), 0x1387, QByteArray::fromHex("03")) + reply(seqOf(out, 0x1202), 0x1202, QByteArray::fromHex("00320000")));
    Snapshot snap = s.backend.snapshot();
    check(snap.mode == QStringLiteral("ambient") && snap.battery == 50, QStringLiteral("Evolve2 85: HearThrough, 50%"));
    s.link.outgoing.clear();
    s.backend.setMode(QStringLiteral("off"));
    s.backend.setMode(QStringLiteral("noise-cancelling"));
    s.backend.tick();
    check(commanded(s.link.outgoing, 0x1387, QByteArray::fromHex("00")) && commanded(s.link.outgoing, 0x1387, QByteArray::fromHex("01")),
          QStringLiteral("Evolve2 85: off and ANC commands"));
}

void testEvolve65()
{
    Session s("Jabra EVOLVE 65");
    s.backend.start();
    s.backend.tick();
    const QByteArray out = s.link.outgoing;
    check(!queried(out, 0x13BE, QByteArray::fromHex("01")) && !queried(out, 0x1387), QStringLiteral("Evolve 65: no noise control asked"));
    const uint8_t batterySeq = seqOf(out, 0x1202);
    QByteArray error = QByteArray::fromHex("0904");
    error.append(char(batterySeq));
    error.append(QByteArray::fromHex("cb1202"));
    s.feed(error + reply(seqOf(out, 0x1315), 0x1315, QByteArray::fromHex("01")) + reply(seqOf(out, 0x137D), 0x137D, QByteArray::fromHex("00")));
    check(s.backend.ready(), QStringLiteral("Evolve 65: ready after the battery error"));
    const Snapshot snap = s.backend.snapshot();
    check(snap.battery == -1 && snap.batteries.isEmpty(), QStringLiteral("Evolve 65: no battery"));
    check(snap.modes.isEmpty(), QStringLiteral("Evolve 65: no modes"));
    check(findControl(snap.settings, "choices", "callEqualizer").toString() == QStringLiteral("treble"), QStringLiteral("Evolve 65: call equalizer swapped"));
    check(!snap.settings.contains(QStringLiteral("equalizer")), QStringLiteral("Evolve 65: no equalizer without a curve"));
    s.link.outgoing.clear();
    s.backend.apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("callEqualizer"), QStringLiteral("bass")}}}});
    s.backend.tick();
    check(commanded(s.link.outgoing, 0x1315, QByteArray::fromHex("02")), QStringLiteral("Evolve 65: bass written as 2"));
}

void testUnknownEarbuds()
{
    Session s("Office earbuds");
    s.backend.start();
    s.backend.tick();
    const QByteArray out = s.link.outgoing;
    QByteArray nak = QByteArray::fromHex("0904");
    nak.append(char(seqOf(out, 0x13BE)));
    nak.append(QByteArray::fromHex("c6fe05"));
    QByteArray battery = QByteArray::fromHex("80500000030a0005140004320015");
    s.feed(reply(seqOf(out, 0x0200), 0x0200, QByteArray("\x0b" "Elite 10 G2", 12)) + nak + reply(seqOf(out, 0x1202), 0x1202, battery));
    const Snapshot snap = s.backend.snapshot();
    check(s.backend.ready(), QStringLiteral("Unknown: ready"));
    check(level(snap.batteries, "left") == 10 && level(snap.batteries, "right") == 20 && level(snap.batteries, "case") == 50 && snap.battery == 10,
          QStringLiteral("Unknown: left/right/case from multi-battery"));
    check(level(snap.batteries, "main") == -1, QStringLiteral("Unknown: no main battery with parts"));
    check(snap.modes.isEmpty(), QStringLiteral("Unknown: no modes after NAK"));
    check(snap.settings.value(QStringLiteral("model")).toString() == QStringLiteral("Jabra Elite 10 G2"), QStringLiteral("Unknown: model from product name"));
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testEncoding();
    testModels();
    testDiscovery();
    testEvolve2_55();
    testEvolve2_85();
    testEvolve65();
    testUnknownEarbuds();
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
