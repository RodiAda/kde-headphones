#include <QCoreApplication>

#include <cstdio>
#include <cstring>

#include "NothingBackend.h"

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

uint16_t modbus(const QByteArray &data)
{
    uint16_t crc = 0xFFFF;
    for (const char c : data) {
        crc ^= uint8_t(c);
        for (int i = 0; i < 8; i++)
            crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0xA001) : uint16_t(crc >> 1);
    }
    return crc;
}

QByteArray frame(uint16_t command, const QByteArray &payload)
{
    QByteArray f = QByteArray::fromHex("556001");
    f.append(char(command & 0xFF));
    f.append(char(command >> 8));
    f.append(char(payload.size() & 0xFF));
    f.append(char(payload.size() >> 8));
    f.append(char(1));
    f.append(payload);
    const uint16_t crc = modbus(f);
    f.append(char(crc & 0xFF));
    f.append(char(crc >> 8));
    return f;
}

struct Decoded {
    uint16_t command;
    QByteArray payload;
};

QList<Decoded> decode(const QByteArray &bytes)
{
    QList<Decoded> out;
    qsizetype i = 0;
    while (i + 10 <= bytes.size()) {
        const int length = uint8_t(bytes.at(i + 5)) | (uint8_t(bytes.at(i + 6)) << 8);
        const QByteArray whole = bytes.mid(i, 8 + length + 2);
        const uint16_t crc = uint8_t(whole.at(whole.size() - 2)) | (uint8_t(whole.at(whole.size() - 1)) << 8);
        if (uint8_t(whole.at(0)) != 0x55 || modbus(whole.left(whole.size() - 2)) != crc)
            return {};
        out.append({uint16_t(uint8_t(whole.at(3)) | (uint8_t(whole.at(4)) << 8)), whole.mid(8, length)});
        i += whole.size();
    }
    return out;
}

bool sent(const QByteArray &bytes, uint16_t command, const QByteArray &payload)
{
    const QList<Decoded> frames = decode(bytes);
    return std::any_of(frames.begin(), frames.end(), [&](const Decoded &d) { return d.command == command && d.payload == payload; });
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

struct Session {
    FakeLink link;
    MDRConnection conn{};
    NothingBackend backend;

    explicit Session(const char *name)
        : backend(&conn, DeviceInfo{QString::fromLatin1(name), QStringLiteral("00:11:22:33:44:55"), {}, DeviceKind::Nothing})
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

void testCrcAndEncoding()
{
    check(NothingBackend::crc16(QByteArray("123456789")) == 0x4B37, QStringLiteral("CRC-16/MODBUS check value"));
    const struct {
        uint16_t command;
        const char *payload;
        const char *expected;
    } frames[] = {
        {0xC006, "", "55600106c000000191 1f"},
        {0xC007, "", "55600107c0000001acdf"},
        {0xC01E, "", "5560011ec0000001b11d"},
        {0xC041, "", "55600141c00000012510"},
        {0xF00F, "010500", "5560010ff0030001010500fb17"},
        {0xF00F, "010700", "5560010ff0030001010700fa77"},
        {0xF00F, "010400", "5560010ff0030001010400fa87"},
        {0xF00F, "010300", "5560010ff0030001010300f8b7"},
        {0xF00F, "010200", "5560010ff0030001010200f927"},
        {0xF00F, "010100", "5560010ff0030001010100f9d7"},
        {0xF040, "01", "55600140f00100010150 32"},
        {0xF040, "02", "55600140f00100010210 33"},
    };
    for (const auto &f : frames) {
        const QByteArray got = NothingBackend::encodeFrame(f.command, 1, QByteArray::fromHex(f.payload));
        const QByteArray want = QByteArray::fromHex(QByteArray(f.expected).replace(' ', ""));
        check(got == want, QStringLiteral("encode %1 %2 -> %3").arg(f.command, 4, 16).arg(QLatin1String(f.payload), QString::fromLatin1(got.toHex(' '))));
    }
}

void testModels()
{
    const struct {
        const char *name;
        NothingModel model;
    } names[] = {
        {"Nothing ear (1)", NothingModel::Ear1},
        {"Ear (2)", NothingModel::Ear2},
        {"Nothing Ear (2)", NothingModel::Ear2},
        {"Ear (stick)", NothingModel::EarStick},
        {"Nothing Ear (a)", NothingModel::EarA},
        {"Nothing Ear", NothingModel::Ear},
        {"Nothing Ear (open)", NothingModel::EarOpen},
        {"Nothing Ear (3)", NothingModel::Ear3},
        {"Nothing Ear (3a)", NothingModel::Ear3A},
        {"Nothing Headphone (1)", NothingModel::Headphone1},
        {"CMF Buds", NothingModel::CmfBuds},
        {"CMF Buds Pro", NothingModel::CmfBudsPro},
        {"CMF Buds Pro 2", NothingModel::CmfBudsPro2},
        {"CMF Buds 2", NothingModel::CmfBuds2},
        {"CMF Buds 2a", NothingModel::CmfBuds2A},
        {"CMF Buds 2 Plus", NothingModel::CmfBuds2Plus},
        {"CMF Neckband Pro", NothingModel::CmfNeckbandPro},
        {"CMF Headphone Pro", NothingModel::CmfHeadphonePro},
        {"Nothing Ear (9)", NothingModel::Unknown},
        {"My earbuds", NothingModel::Unknown},
    };
    for (const auto &n : names)
        check(NothingBackend::modelFromName(QString::fromLatin1(n.name)) == n.model, QStringLiteral("model of \"%1\"").arg(QLatin1String(n.name)));
}

void testCmfBuds2()
{
    Session s("CMF Buds 2");
    s.backend.start();
    s.backend.tick();
    check(s.link.outgoing == QByteArray::fromHex("55600106c0000001911f"), QStringLiteral("CMF Buds 2: device info asked first"));
    s.link.outgoing.clear();

    s.feed(frame(0x4006, QByteArray("\x04" "2,2,1.0.1.52\n\n2,4,0000\n", 24)));
    check(sent(s.link.outgoing, 0xC007, {}) && sent(s.link.outgoing, 0xC01E, {}) && sent(s.link.outgoing, 0xC050, {})
              && sent(s.link.outgoing, 0xC00E, {}) && sent(s.link.outgoing, 0xC041, {}) && sent(s.link.outgoing, 0xC04E, {}),
          QStringLiteral("CMF Buds 2: queries after device info"));
    check(!s.backend.ready(), QStringLiteral("CMF Buds 2: not ready before battery"));

    s.feed(QByteArray::fromHex("5560010740050001020264036432 89"
                               "5560011e40060001010500020100b62a"
                               "5560014140010001025038"
                               "556001294001000100d911"));
    check(s.backend.ready(), QStringLiteral("CMF Buds 2: ready"));
    Snapshot snap = s.backend.snapshot();
    check(level(snap.batteries, "left") == 100 && level(snap.batteries, "right") == 100, QStringLiteral("CMF Buds 2: buds 100%"));
    check(level(snap.batteries, "case") == -1, QStringLiteral("CMF Buds 2: no case while closed"));
    check(snap.mode == QStringLiteral("off"), QStringLiteral("CMF Buds 2: mode off (got %1)").arg(snap.mode));
    check(snap.modes == QStringList{QStringLiteral("noise-cancelling"), QStringLiteral("adaptive"), QStringLiteral("ambient"), QStringLiteral("off")},
          QStringLiteral("CMF Buds 2: modes %1").arg(snap.modes.join(QLatin1Char(','))));
    check(findControl(snap.settings, "choices", "ancStrength").toString() == QStringLiteral("high"), QStringLiteral("CMF Buds 2: strength high"));
    check(findControl(snap.settings, "toggles", "lowLatency") == QJsonValue(false), QStringLiteral("CMF Buds 2: low latency off"));
    check(snap.settings.value(QStringLiteral("untested")).toBool(), QStringLiteral("CMF Buds 2: untested flag"));
    check(snap.settings.value(QStringLiteral("firmware")).toString() == QStringLiteral("1.0.1.52"), QStringLiteral("CMF Buds 2: firmware"));
    check(!snap.settings.contains(QStringLiteral("equalizer")), QStringLiteral("CMF Buds 2: no equalizer before it answers"));

    s.feed(QByteArray::fromHex("55200101e007000003026403640455 2d75"));
    snap = s.backend.snapshot();
    check(level(snap.batteries, "case") == 85, QStringLiteral("CMF Buds 2: case 85% from event"));

    s.feed(QByteArray::fromHex("55200103e00600000103000203 00b8a4"));
    snap = s.backend.snapshot();
    check(snap.mode == QStringLiteral("noise-cancelling") && findControl(snap.settings, "choices", "ancStrength").toString() == QStringLiteral("low"),
          QStringLiteral("CMF Buds 2: event to low noise cancelling"));

    s.feed(QByteArray::fromHex("55600141400100010110 39"));
    check(findControl(s.backend.snapshot().settings, "toggles", "lowLatency") == QJsonValue(true), QStringLiteral("CMF Buds 2: low latency on"));

    s.feed(frame(0x4050, QByteArray::fromHex("0300")));
    snap = s.backend.snapshot();
    check(snap.settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("preset")).toInt() == 3, QStringLiteral("CMF Buds 2: equalizer pop"));

    s.link.outgoing.clear();
    s.backend.setMode(QStringLiteral("ambient"));
    s.backend.tick();
    check(sent(s.link.outgoing, 0xF00F, QByteArray::fromHex("010700")), QStringLiteral("CMF Buds 2: transparency command"));
    check(s.backend.snapshot().mode == QStringLiteral("ambient"), QStringLiteral("CMF Buds 2: mode ambient"));

    s.link.outgoing.clear();
    s.backend.setMode(QStringLiteral("noise-cancelling"));
    s.backend.apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("ancStrength"), QStringLiteral("medium")}}}});
    s.backend.apply(QJsonObject{{QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 1}}}});
    s.backend.apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("lowLatency"), false}}}});
    s.backend.tick();
    check(sent(s.link.outgoing, 0xF00F, QByteArray::fromHex("010300")), QStringLiteral("CMF Buds 2: noise cancelling with the last strength"));
    check(sent(s.link.outgoing, 0xF00F, QByteArray::fromHex("010200")), QStringLiteral("CMF Buds 2: medium strength command"));
    check(sent(s.link.outgoing, 0xF01D, QByteArray::fromHex("0100")), QStringLiteral("CMF Buds 2: listening mode rock command"));
    check(sent(s.link.outgoing, 0xF040, QByteArray::fromHex("02")), QStringLiteral("CMF Buds 2: low latency off command"));
    check(s.backend.apply(QJsonObject{{QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 9}}}}).isValid(),
          QStringLiteral("CMF Buds 2: unknown preset rejected"));
}

void testHeadphonePro()
{
    Session s("CMF Headphone Pro");
    s.backend.start();
    s.feed(QByteArray::fromHex("5560014140010 0ff009199"
                               "55600609 7c0300ff000400717d"
                               "550003 19e00200000100"
                               "550003 03e00600000104000204 00"));
    check(!s.backend.ready(), QStringLiteral("Headphone Pro: not ready before battery"));
    s.feed(QByteArray::fromHex("55600107400300010106 0f0b8e"));
    check(s.backend.ready(), QStringLiteral("Headphone Pro: ready"));
    const Snapshot snap = s.backend.snapshot();
    check(level(snap.batteries, "main") == 15 && snap.battery == 15 && level(snap.batteries, "left") == -1, QStringLiteral("Headphone Pro: one battery 15%"));
    check(snap.mode == QStringLiteral("adaptive"), QStringLiteral("Headphone Pro: adaptive from a frame without CRC (got %1)").arg(snap.mode));
    check(findControl(snap.settings, "toggles", "lowLatency") == QJsonValue(false), QStringLiteral("Headphone Pro: low latency off"));
}

void testClassicEqualizer()
{
    Session s("Nothing Ear (2)");
    s.backend.start();
    s.feed(QByteArray::fromHex("00ff55") + frame(0x4007, QByteArray::fromHex("030255030f0455")));
    QByteArray bad = frame(0x401E, QByteArray::fromHex("010100"));
    bad[bad.size() - 1] = char(bad.at(bad.size() - 1) ^ 0xFF);
    s.feed(bad + frame(0x401F, QByteArray::fromHex("03")) + frame(0x400E, QByteArray::fromHex("010101")));
    check(s.backend.ready(), QStringLiteral("Ear (2): ready after garbage"));
    Snapshot snap = s.backend.snapshot();
    check(level(snap.batteries, "left") == 85 && level(snap.batteries, "right") == 15 && level(snap.batteries, "case") == 85 && snap.battery == 15,
          QStringLiteral("Ear (2): batteries 85/15/85"));
    check(snap.modes.isEmpty(), QStringLiteral("Ear (2): frame with a broken CRC ignored"));
    check(snap.settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("preset")).toInt() == 3, QStringLiteral("Ear (2): more bass"));
    check(findControl(snap.settings, "toggles", "earDetection") == QJsonValue(true), QStringLiteral("Ear (2): ear detection on"));

    s.link.outgoing.clear();
    s.backend.apply(QJsonObject{
        {QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 0}}},
        {QStringLiteral("toggles"), QJsonObject{{QStringLiteral("earDetection"), false}}},
    });
    s.backend.tick();
    check(sent(s.link.outgoing, 0xF010, QByteArray::fromHex("0000")), QStringLiteral("Ear (2): balanced command"));
    check(sent(s.link.outgoing, 0xF004, QByteArray::fromHex("010100")), QStringLiteral("Ear (2): ear detection off command"));

    s.feed(frame(0xE001, QByteArray::fromHex("0202d00364")));
    snap = s.backend.snapshot();
    check(level(snap.batteries, "left") == 80 && level(snap.batteries, "case") == 85, QStringLiteral("Ear (2): charging left bud keeps case level"));
    bool charging = false;
    for (const QJsonValue &b : snap.batteries) {
        if (b.toObject().value(QStringLiteral("part")).toString() == QStringLiteral("left"))
            charging = b.toObject().value(QStringLiteral("charging")).toBool();
    }
    check(charging, QStringLiteral("Ear (2): charging bit"));
}

void testOneStrength()
{
    Session s("CMF Buds 2a");
    s.backend.start();
    s.feed(frame(0x4007, QByteArray::fromHex("0102 32")) + frame(0x401E, QByteArray::fromHex("010100")));
    const Snapshot snap = s.backend.snapshot();
    check(snap.modes == QStringList{QStringLiteral("noise-cancelling"), QStringLiteral("ambient"), QStringLiteral("off")},
          QStringLiteral("Buds 2a: modes %1").arg(snap.modes.join(QLatin1Char(','))));
    check(!snap.settings.contains(QStringLiteral("choices")), QStringLiteral("Buds 2a: no strength choice"));
    check(s.backend.setMode(QStringLiteral("adaptive")).isValid(), QStringLiteral("Buds 2a: adaptive rejected"));
    check(s.backend.setAmbientLevel(2).isValid(), QStringLiteral("Buds 2a: ambient level unsupported"));
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testCrcAndEncoding();
    testModels();
    testCmfBuds2();
    testHeadphonePro();
    testClassicEqualizer();
    testOneStrength();
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
