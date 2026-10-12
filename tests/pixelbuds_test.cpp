#include <QCoreApplication>
#include <QThread>

#include <cstdio>
#include <cstring>

#include "PixelBudsBackend.h"

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

QString hex(const QByteArray &data)
{
    return QString::fromLatin1(data.toHex(' '));
}

constexpr uint32_t Response = 1;
constexpr uint32_t ServerError = 5;
constexpr uint32_t ServerStream = 7;

const uint32_t Service = maestro::idHash("maestro_pw.Maestro");
const uint32_t GetSoftwareInfo = maestro::idHash("GetSoftwareInfo");
const uint32_t SubscribeRuntimeInfo = maestro::idHash("SubscribeRuntimeInfo");
const uint32_t SubscribeToSettingsChanges = maestro::idHash("SubscribeToSettingsChanges");
const uint32_t ReadSetting = maestro::idHash("ReadSetting");
const uint32_t WriteSetting = maestro::idHash("WriteSetting");

QByteArray devicePacket(uint32_t type, uint32_t channel, uint32_t method, const char *payloadHex, uint32_t status = 0, uint32_t callId = 0)
{
    maestro::RpcPacket p;
    p.type = type;
    p.channel = channel;
    p.service = Service;
    p.method = method;
    p.payload = QByteArray::fromHex(payloadHex);
    p.status = status;
    p.callId = callId;
    return maestro::encodeFrame({maestro::channelAddress(channel), 0x03, p.encode()});
}

QList<maestro::RpcPacket> sentPackets(FakeLink &link, QList<uint32_t> *addresses = nullptr)
{
    QList<maestro::RpcPacket> packets;
    for (const QByteArray &raw : maestro::takeFrames(link.outgoing)) {
        const auto frame = maestro::decodeFrame(raw);
        if (!frame)
            continue;
        if (const auto p = maestro::RpcPacket::decode(frame->data)) {
            packets.append(*p);
            if (addresses)
                addresses->append(frame->address);
        }
    }
    link.outgoing.clear();
    return packets;
}

void testReferenceVectors()
{
    check(maestro::crc32("test test test") == 0x235b6a02, QStringLiteral("crc32 of 'test test test'"));
    check(maestro::crc32("1234321") == 0xd981751c, QStringLiteral("crc32 of '1234321'"));
    check(maestro::idHash("maestro_pw.Maestro") == 0x7ede71ea, QStringLiteral("service hash"));
    check(maestro::idHash("GetSoftwareInfo") == 0x7199fa44, QStringLiteral("GetSoftwareInfo hash"));
    check(maestro::idHash("SubscribeToSettingsChanges") == 0x2821adf5, QStringLiteral("SubscribeToSettingsChanges hash"));

    const QByteArray empty = maestro::encodeFrame({0x010203, 0x03, {}});
    check(empty == QByteArray::fromHex("7e060809038b3bf7427e"), QStringLiteral("hdlc empty frame %1").arg(hex(empty)));
    const QByteArray escaped = maestro::encodeFrame({0x010203, 0x03, QByteArray::fromHex("0506077d7e7fff")});
    const QByteArray expected = QByteArray::fromHex("7e06080903050607" "7d5d7d5e7fffe62d17c67e");
    check(escaped == expected, QStringLiteral("hdlc escaped frame %1").arg(hex(escaped)));

    QByteArray stream = expected + QByteArray::fromHex("0201");
    const QList<QByteArray> frames = maestro::takeFrames(stream);
    const auto frame = frames.size() == 1 ? maestro::decodeFrame(frames.first()) : std::nullopt;
    check(frame && frame->address == 0x010203 && frame->control == 0x03 && frame->data == QByteArray::fromHex("0506077d7e7fff"),
          QStringLiteral("hdlc frame decodes"));

    QByteArray corrupted = expected;
    corrupted[6] = 0x08;
    const QList<QByteArray> bad = maestro::takeFrames(corrupted);
    check(bad.size() == 1 && !maestro::decodeFrame(bad.first()), QStringLiteral("hdlc frame with a bad checksum is dropped"));

    check(maestro::channelAddress(18) == ((10u << 6) | (2u << 10)), QStringLiteral("address of channel 18"));
    check(maestro::channelAddress(26) == ((13u << 6) | (4u << 10)), QStringLiteral("address of channel 26"));
    check(maestro::channelAddress(17) == 0 && maestro::channelAddress(28) == 0, QStringLiteral("no address outside channels 18..27"));

    maestro::RpcPacket request;
    request.channel = 21;
    request.service = Service;
    request.method = GetSoftwareInfo;
    check(request.encode() == QByteArray::fromHex("10151dea71de7e2544fa9971"), QStringLiteral("rpc request encoding %1").arg(hex(request.encode())));
}

struct ReadReply {
    int id;
    uint32_t type;
    uint32_t status;
    const char *payload;
};

const ReadReply ReadReplies[] = {
    {13, Response, 0, "22026802"},
    {12, Response, 0, "220862060801100118" "01"},
    {2, Response, 0, "22021001"},
    {4, Response, 0, "22022001"},
    {11, Response, 0, "22025800"},
    {22, Response, 0, "2203b00101"},
    {15, Response, 0, "22027801"},
    {16, Response, 0, "220d82010a150000c03f25000000c0"},
    {17, Response, 0, "2203880114"},
    {19, ServerError, 2, ""},
};

const char *const RuntimeInfo = "10e807" "3212" "0a0408371001" "120408501002" "1a04085a1001" "3a020801";

QJsonValue findKey(const QJsonArray &list, const char *key)
{
    for (const QJsonValue &v : list) {
        if (v.toObject().value(QStringLiteral("key")).toString() == QLatin1String(key))
            return v;
    }
    return QJsonValue(QJsonValue::Undefined);
}

int level(const QJsonArray &batteries, const char *part, bool *charging = nullptr)
{
    for (const QJsonValue &b : batteries) {
        const QJsonObject o = b.toObject();
        if (o.value(QStringLiteral("part")).toString() == QLatin1String(part)) {
            if (charging)
                *charging = o.value(QStringLiteral("charging")).toBool();
            return o.value(QStringLiteral("level")).toInt();
        }
    }
    return -1;
}

bool answerReads(PixelBudsBackend &backend, FakeLink &link, uint32_t channel)
{
    for (int round = 0; round < 32; round++) {
        backend.tick();
        const QList<maestro::RpcPacket> sent = sentPackets(link);
        const maestro::RpcPacket *read = nullptr;
        for (const maestro::RpcPacket &p : sent) {
            if (p.method == ReadSetting)
                read = &p;
        }
        if (!read)
            return true;
        const QByteArray msg = read->payload;
        if (msg.size() != 2 || uint8_t(msg.at(0)) != 0x20 || read->channel != channel)
            return false;
        const int id = uint8_t(msg.at(1));
        const ReadReply *reply = nullptr;
        for (const ReadReply &r : ReadReplies) {
            if (r.id == id)
                reply = &r;
        }
        if (!reply)
            return false;
        link.incoming.append(devicePacket(reply->type, channel, ReadSetting, reply->payload, reply->status));
    }
    return false;
}

void testSession()
{
    FakeLink link;
    MDRConnection conn{};
    conn.user = &link;
    conn.recv = fakeRecv;
    conn.send = fakeSend;
    conn.getLastError = fakeError;

    PixelBudsBackend backend(&conn, DeviceInfo{QStringLiteral("Pixel Buds Pro"), QStringLiteral("00:11:22:33:44:55"), {}, DeviceKind::PixelBuds});
    check(backend.start(), QStringLiteral("start"));
    backend.tick();
    check(link.outgoing.isEmpty(), QStringLiteral("nothing is sent before the greeting"));

    link.incoming = QByteArray::fromHex("00ff7e") + devicePacket(Response, 21, GetSoftwareInfo, "", 0, 0xffffffff);
    backend.tick();
    QList<uint32_t> addresses;
    const QList<maestro::RpcPacket> opening = sentPackets(link, &addresses);
    check(opening.size() == 3, QStringLiteral("three requests after the greeting (got %1)").arg(opening.size()));
    if (opening.size() == 3) {
        check(opening[0].method == SubscribeRuntimeInfo && opening[1].method == SubscribeToSettingsChanges && opening[2].method == ReadSetting,
              QStringLiteral("subscriptions then the first read"));
        bool allOnChannel = true;
        for (int i = 0; i < opening.size(); i++)
            allOnChannel = allOnChannel && opening[i].channel == 21 && opening[i].service == Service && opening[i].type == 0
                && addresses[i] == maestro::channelAddress(21);
        check(allOnChannel, QStringLiteral("requests go to channel 21 and its address"));
        check(opening[2].payload == QByteArray::fromHex("200d"), QStringLiteral("first read asks for the ANC state"));
        link.incoming.append(devicePacket(Response, 21, ReadSetting, "22026802"));
    }
    check(answerReads(backend, link, 21), QStringLiteral("all settings read one after another"));
    check(!backend.ready(), QStringLiteral("not ready before runtime info"));

    link.incoming = devicePacket(ServerStream, 21, SubscribeRuntimeInfo, RuntimeInfo);
    backend.tick();
    check(backend.ready(), QStringLiteral("ready after runtime info"));
    if (!backend.ready())
        return;

    Snapshot s = backend.snapshot();
    bool charging = false;
    check(level(s.batteries, "left", &charging) == 80 && charging, QStringLiteral("left 80% charging"));
    check(level(s.batteries, "right", &charging) == 90 && !charging, QStringLiteral("right 90% not charging"));
    check(level(s.batteries, "case", &charging) == 55 && !charging, QStringLiteral("case 55%"));
    check(s.battery == 80, QStringLiteral("overall battery is the lower bud"));
    check(s.mode == QLatin1String("noise-cancelling"), QStringLiteral("mode noise-cancelling (got %1)").arg(s.mode));
    check(s.modes == QStringList({QStringLiteral("noise-cancelling"), QStringLiteral("ambient"), QStringLiteral("off")}),
          QStringLiteral("modes without adaptive (got %1)").arg(s.modes.join(QLatin1Char(','))));
    check(s.settings.value(QStringLiteral("untested")).toBool(), QStringLiteral("marked untested"));
    check(s.settings.value(QStringLiteral("model")).toString() == QLatin1String("Pixel Buds Pro"), QStringLiteral("model name"));

    const QJsonArray toggles = s.settings.value(QStringLiteral("toggles")).toArray();
    const auto toggle = [&](const char *key) { return findKey(toggles, key).toObject().value(QStringLiteral("value")); };
    check(toggle("earDetection") == QJsonValue(true), QStringLiteral("ear detection on"));
    check(toggle("touchControls") == QJsonValue(true), QStringLiteral("touch controls on"));
    check(toggle("multipoint") == QJsonValue(false), QStringLiteral("multipoint off"));
    check(toggle("conversationAwareness") == QJsonValue(true), QStringLiteral("conversation detection on"));
    check(toggle("volumeEq") == QJsonValue(true), QStringLiteral("volume eq on"));
    check(findKey(toggles, "monoAudio").isUndefined(), QStringLiteral("unreadable mono setting is hidden"));

    const QJsonArray sliders = s.settings.value(QStringLiteral("sliders")).toArray();
    const auto slider = [&](const char *key) { return findKey(sliders, key).toObject(); };
    check(slider("balance").value(QStringLiteral("value")).toInt() == -10, QStringLiteral("balance -10 from raw 20"));
    check(slider("eqBass").value(QStringLiteral("value")).toInt() == 2 && slider("eqTreble").value(QStringLiteral("value")).toInt() == -2
              && slider("eqLowBass").value(QStringLiteral("value")).toInt() == 0 && slider("eqMid").value(QStringLiteral("min")).toInt() == -6
              && slider("eqUpperTreble").value(QStringLiteral("max")).toInt() == 6,
          QStringLiteral("equalizer bands"));

    const auto expectWrite = [&](const char *what, const char *payloadHex) {
        backend.tick();
        const QList<maestro::RpcPacket> sent = sentPackets(link);
        const bool ok = sent.size() == 1 && sent[0].method == WriteSetting && sent[0].channel == 21
            && sent[0].payload == QByteArray::fromHex(payloadHex);
        check(ok, QStringLiteral("%1 encodes as %2 (got %3)").arg(QLatin1String(what), QLatin1String(payloadHex),
                                                                     sent.isEmpty() ? QString() : hex(sent[0].payload)));
    };

    backend.setMode(QStringLiteral("ambient"));
    expectWrite("mode ambient", "22026803");
    backend.setMode(QStringLiteral("off"));
    expectWrite("mode off", "22026801");
    check(backend.setMode(QStringLiteral("adaptive")).isValid(), QStringLiteral("adaptive rejected on this model"));
    check(backend.setAmbientLevel(3).isValid(), QStringLiteral("ambient level unsupported"));

    backend.apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("earDetection"), false}}}});
    expectWrite("ear detection off", "22021000");
    backend.apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("multipoint"), true}}}});
    expectWrite("multipoint on", "22025801");
    backend.apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("conversationAwareness"), false}}}});
    expectWrite("conversation detection off", "2203b00100");
    backend.apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("monoAudio"), true}}}});
    backend.tick();
    check(sentPackets(link).isEmpty(), QStringLiteral("unknown setting is not written"));
    backend.apply(QJsonObject{{QStringLiteral("sliders"), QJsonObject{{QStringLiteral("balance"), 30}}}});
    expectWrite("balance +30", "220388013b");
    backend.apply(QJsonObject{{QStringLiteral("sliders"), QJsonObject{{QStringLiteral("balance"), -30}}}});
    expectWrite("balance -30", "220388013c");
    backend.apply(QJsonObject{{QStringLiteral("sliders"), QJsonObject{{QStringLiteral("eqBass"), 3}}}});
    expectWrite("equalizer bass +3", "220d82010a1500004040250000" "00c0");

    s = backend.snapshot();
    check(s.mode == QLatin1String("off"), QStringLiteral("mode follows the command"));
    check(findKey(s.settings.value(QStringLiteral("toggles")).toArray(), "multipoint").toObject().value(QStringLiteral("value")) == QJsonValue(true),
          QStringLiteral("toggle follows the command"));

    link.incoming = devicePacket(ServerStream, 21, SubscribeToSettingsChanges, "22026804")
        + devicePacket(ServerStream, 21, SubscribeRuntimeInfo, "320c" "120408321001" "1a0408141002");
    backend.tick();
    s = backend.snapshot();
    check(s.mode == QLatin1String("adaptive") && s.modes.contains(QLatin1String("adaptive")), QStringLiteral("adaptive state from a change notification"));
    check(level(s.batteries, "left") == 50 && level(s.batteries, "right", &charging) == 20 && charging && level(s.batteries, "case") == -1,
          QStringLiteral("battery update without the case"));

    link.incoming = devicePacket(ServerStream, 22, SubscribeToSettingsChanges, "22026801") + QByteArray::fromHex("7e0102037e7e");
    backend.tick();
    check(backend.snapshot().mode == QLatin1String("adaptive"), QStringLiteral("other channels and garbage are ignored"));
}

void testProbe()
{
    FakeLink link;
    MDRConnection conn{};
    conn.user = &link;
    conn.recv = fakeRecv;
    conn.send = fakeSend;
    conn.getLastError = fakeError;

    PixelBudsBackend backend(&conn, DeviceInfo{QStringLiteral("Pixel Buds Pro 2"), {}, {}, DeviceKind::PixelBuds});
    backend.start();
    QThread::msleep(1600);
    backend.tick();
    const QList<maestro::RpcPacket> probes = sentPackets(link);
    QList<uint32_t> channels;
    for (const maestro::RpcPacket &p : probes) {
        if (p.method == GetSoftwareInfo)
            channels.append(p.channel);
    }
    check(channels == QList<uint32_t>({18, 19, 21, 23, 24, 26}), QStringLiteral("probes the six maestro channels"));

    link.incoming = devicePacket(ServerError, 18, GetSoftwareInfo, "", 5) + devicePacket(Response, 24, GetSoftwareInfo, "");
    backend.tick();
    const QList<maestro::RpcPacket> after = sentPackets(link);
    check(!after.isEmpty() && after.first().channel == 24, QStringLiteral("errors are skipped, the answering channel is used"));
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testReferenceVectors();
    testSession();
    testProbe();
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
