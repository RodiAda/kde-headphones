#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cstdio>
#include <optional>

#include "JblBackend.h"
#include "Registry.h"

namespace {

int failures = 0;

void check(bool ok, const QString &what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        failures++;
}

QByteArray hex(const char *text)
{
    return QByteArray::fromHex(text);
}

DeviceInfo device(const QString &name)
{
    DeviceInfo d;
    d.name = name;
    d.address = QStringLiteral("44:F0:A9:E7:6F:5F");
    d.kind = DeviceKind::Jbl;
    d.ownTransport = true;
    return d;
}

QJsonObject find(const QJsonObject &settings, const QString &group, const QString &key)
{
    for (const QJsonValue &v : settings.value(group).toArray()) {
        if (v.toObject().value(QStringLiteral("key")).toString() == key)
            return v.toObject();
    }
    return {};
}

QJsonObject battery(const Snapshot &s, const QString &part)
{
    for (const QJsonValue &v : s.batteries) {
        if (v.toObject().value(QStringLiteral("part")).toString() == part)
            return v.toObject();
    }
    return {};
}

bool sent(const QList<QByteArray> &out, const QByteArray &packet)
{
    return out.contains(packet);
}

namespace Vector {
const char *BatteryM2 = "aa250d0100005a5affffffffffffffff";
const char *NoiseAncM2 = "aa910712010102000300";
const char *NoiseAmbientM2 = "aa910712010002010300";
const char *NoiseTalkThruM2 = "aa910712010002000301";
const char *NoiseOffM2 = "aa910712010002000300";
const char *AncOn = "aa22023101";
const char *AmbientOff = "aa22023200";
const char *AutoOffM2 = "aa220433001e00";
const char *AutoPlayOn = "aa22023801";
const char *AncTuningM2 = "aa910922010104070501a107";
const char *VoicePromptsWithBattery = "aa93020501aa250d0100005a5affffffffffffffff";
const char *LivePro2TalkThru = "aa22023101aa22023201";
const char *LivePro2Ambient = "aa22023100aa22023202";
const char *EqPresetUser = "aa22023404";
const char *AckRefused = "aa00023104";
const char *AckOk = "aa00023300";
const char *SpatialMusicOn = "aa9d03020101";
const char *NameM2 = "aa12104a424c20544f5552204f4e45204d3200";
}

void testEncoding()
{
    check(JblBackend::nameRequest() == hex("aa1100"), QStringLiteral("encode: name request aa 11 00"));
    check(JblBackend::batteryRequest() == hex("aa2500"), QStringLiteral("encode: battery request aa 25 00"));
    check(JblBackend::batteryRequestLevels() == hex("aa250101"), QStringLiteral("encode: battery request aa 25 01 01"));
    check(JblBackend::batteryRequestLegacy() == hex("aa250100"), QStringLiteral("encode: battery request aa 25 01 00"));
    check(JblBackend::statusRequest(0x31) == hex("aa210131"), QStringLiteral("encode: status request aa 21 01 31"));
    check(JblBackend::statusRequest(0x41) == hex("aa210141"), QStringLiteral("encode: in-ear request aa 21 01 41"));
    check(JblBackend::noiseRequest() == hex("aa910111"), QStringLiteral("encode: noise request aa 91 01 11"));
    check(JblBackend::ancLevelRequest() == hex("aa910121"), QStringLiteral("encode: anc tuning request aa 91 01 21"));
    check(JblBackend::noiseSlotsCommand(true, false, false) == hex("aa910710010102000300"), QStringLiteral("encode: slots ANC"));
    check(JblBackend::noiseSlotsCommand(false, true, false) == hex("aa910710010002010300"), QStringLiteral("encode: slots ambient"));
    check(JblBackend::noiseSlotsCommand(false, false, true) == hex("aa910710010002000301"), QStringLiteral("encode: slots TalkThru"));
    check(JblBackend::ancSwitchCommand(false) == hex("aa310100"), QStringLiteral("encode: ANC switch off aa 31 01 00"));
    check(JblBackend::ancSwitchCommand(true) == hex("aa310101"), QStringLiteral("encode: ANC switch on aa 31 01 01"));
    check(JblBackend::ancLevelCommand(5) == hex("aa9103200405"), QStringLiteral("encode: ANC level aa 91 03 20 04 05"));
    check(JblBackend::ancLevelCommand(12) == hex("aa9103200407"), QStringLiteral("encode: ANC level clamped to 7"));
    check(JblBackend::autoOffCommand(true, 60) == hex("aa3303013c00"), QStringLiteral("encode: auto-off 60 min aa 33 03 01 3c 00"));
    check(JblBackend::autoPlayCommand(false) == hex("aa350100"), QStringLiteral("encode: auto play off aa 35 01 00"));
    check(JblBackend::eqPresetCommand(5) == hex("aa400105"), QStringLiteral("encode: EQ preset rock aa 40 01 05"));
    check(JblBackend::spatialCommand(true, 1) == hex("aa9d03000101"), QStringLiteral("encode: spatial on music aa 9d 03 00 01 01"));
}

void testFraming()
{
    QByteArray buffer = hex(Vector::VoicePromptsWithBattery);
    const QList<JblFrame> frames = JblBackend::splitFrames(buffer);
    check(frames.size() == 2, QStringLiteral("framing: glued notification splits into %1 frames").arg(frames.size()));
    check(frames.size() == 2 && frames[0].command == 0x93 && frames[0].payload == hex("0501"), QStringLiteral("framing: first frame is aa 93"));
    check(frames.size() == 2 && frames[1].command == 0x25 && frames[1].payload.size() == 13, QStringLiteral("framing: second frame is battery"));
    check(buffer.isEmpty(), QStringLiteral("framing: buffer consumed"));

    QByteArray partial = hex("aa9107120101");
    check(JblBackend::splitFrames(partial).isEmpty() && partial.size() == 6, QStringLiteral("framing: incomplete frame kept"));
    partial.append(hex("02000300"));
    const QList<JblFrame> rest = JblBackend::splitFrames(partial);
    check(rest.size() == 1 && rest[0].command == 0x91, QStringLiteral("framing: frame completes across notifications"));

    QByteArray junk = hex("0102aa22023101");
    const QList<JblFrame> resync = JblBackend::splitFrames(junk);
    check(resync.size() == 1 && resync[0].command == 0x22, QStringLiteral("framing: resyncs on header"));
}

void testModels()
{
    check(JblBackend::schemeFromName(QStringLiteral("JBL LIVE PRO 2 TWS")) == JblBackend::AncScheme::Status, QStringLiteral("model: Live Pro 2 uses status fields"));
    check(JblBackend::schemeFromName(QStringLiteral("JBL TOUR ONE M2")) == JblBackend::AncScheme::Slots, QStringLiteral("model: Tour One M2 uses slots"));
    check(!JblBackend::earbudsFromName(QStringLiteral("JBL TOUR ONE M2")), QStringLiteral("model: Tour One M2 is headphones"));
    check(!JblBackend::earbudsFromName(QStringLiteral("JBL Live 770NC")), QStringLiteral("model: Live 770NC is headphones"));
    check(JblBackend::earbudsFromName(QStringLiteral("JBL LIVE PRO 2 TWS")), QStringLiteral("model: Live Pro 2 is earbuds"));
    check(JblBackend::earbudsFromName(QStringLiteral("JBL Tune Flex 2")), QStringLiteral("model: Tune Flex 2 is earbuds"));
    check(JblBackend::earbudsFromName(QStringLiteral("JBL Live Beam 3")), QStringLiteral("model: Live Beam 3 is earbuds"));
}

void testTourOneM2()
{
    JblBackend b(nullptr, device(QStringLiteral("JBL TOUR ONE M2")));
    b.requestState();
    const QList<QByteArray> initial = b.takeOutgoing();
    check(sent(initial, hex("aa1100")) && sent(initial, hex("aa2500")) && sent(initial, hex("aa910111")) && sent(initial, hex("aa210131"))
              && sent(initial, hex("aa210132")) && sent(initial, hex("aa910121")) && sent(initial, hex("aa210133")) && sent(initial, hex("aa210138")),
          QStringLiteral("M2: initial requests"));
    check(!sent(initial, hex("aa210134")), QStringLiteral("M2: legacy EQ field not requested"));
    check(!b.ready(), QStringLiteral("M2: not ready before battery"));

    b.handleData(hex(Vector::BatteryM2));
    check(b.ready(), QStringLiteral("M2: ready after battery"));
    Snapshot s = b.snapshot();
    check(s.battery == 90 && battery(s, QStringLiteral("main")).value(QStringLiteral("level")).toInt() == 90 && s.batteries.size() == 1,
          QStringLiteral("M2: battery main 90%"));
    check(!battery(s, QStringLiteral("main")).value(QStringLiteral("charging")).toBool(), QStringLiteral("M2: not charging"));
    check(s.modes.isEmpty() && s.mode.isEmpty(), QStringLiteral("M2: no modes before noise state"));
    check(s.settings.value(QStringLiteral("untested")).toBool() && s.settings.value(QStringLiteral("model")).toString() == QLatin1String("JBL TOUR ONE M2"),
          QStringLiteral("M2: model and untested"));

    b.handleData(hex(Vector::NoiseAncM2));
    s = b.snapshot();
    check(s.modes == QStringList{QStringLiteral("noise-cancelling"), QStringLiteral("ambient"), QStringLiteral("off")}, QStringLiteral("M2: modes"));
    check(s.mode == QLatin1String("noise-cancelling"), QStringLiteral("M2: ANC slot -> noise-cancelling"));
    b.handleData(hex(Vector::NoiseAmbientM2));
    check(b.snapshot().mode == QLatin1String("ambient"), QStringLiteral("M2: ambient slot -> ambient"));
    check(find(b.snapshot().settings, QStringLiteral("choices"), QStringLiteral("ambientType")).value(QStringLiteral("value")).toString()
              == QLatin1String("ambientAware"),
          QStringLiteral("M2: ambient type ambientAware"));
    b.handleData(hex(Vector::NoiseTalkThruM2));
    s = b.snapshot();
    check(s.mode == QLatin1String("ambient") && b.noise() == JblBackend::Noise::TalkThru, QStringLiteral("M2: TalkThru slot -> ambient (talk-through)"));
    check(find(s.settings, QStringLiteral("choices"), QStringLiteral("ambientType")).value(QStringLiteral("value")).toString() == QLatin1String("talkThru"),
          QStringLiteral("M2: ambient type talkThru"));
    b.handleData(hex(Vector::NoiseOffM2));
    check(b.snapshot().mode == QLatin1String("off"), QStringLiteral("M2: all slots zero -> off"));

    b.handleData(hex(Vector::AncOn));
    b.handleData(hex(Vector::AmbientOff));
    check(b.snapshot().mode == QLatin1String("noise-cancelling"), QStringLiteral("M2: status fields 31=01 32=00 -> noise-cancelling"));

    b.handleData(hex(Vector::AutoOffM2) + hex(Vector::AutoPlayOn));
    b.handleData(hex(Vector::AncTuningM2));
    b.handleData(hex(Vector::SpatialMusicOn));
    b.handleData(hex(Vector::NameM2));
    s = b.snapshot();
    const QJsonObject autoOff = find(s.settings, QStringLiteral("choices"), QStringLiteral("autoPowerOff"));
    check(autoOff.value(QStringLiteral("value")).toString() == QLatin1String("off"), QStringLiteral("M2: auto-off reported off"));
    check(autoOff.value(QStringLiteral("options")).toArray()
              == QJsonArray{QStringLiteral("off"), QStringLiteral("30-minutes"), QStringLiteral("60-minutes"), QStringLiteral("120-minutes")},
          QStringLiteral("M2: auto-off options"));
    check(find(s.settings, QStringLiteral("toggles"), QStringLiteral("wearDetection")).value(QStringLiteral("value")).toBool(),
          QStringLiteral("M2: auto play/pause as wearDetection on"));
    const QJsonObject strength = find(s.settings, QStringLiteral("sliders"), QStringLiteral("ancStrength"));
    check(strength.value(QStringLiteral("value")).toInt() == 7 && strength.value(QStringLiteral("min")).toInt() == 1 && strength.value(QStringLiteral("max")).toInt() == 7,
          QStringLiteral("M2: ancStrength 7 of 1..7 from key 04"));
    check(find(s.settings, QStringLiteral("toggles"), QStringLiteral("spatialAudio")).value(QStringLiteral("value")).toBool(), QStringLiteral("M2: spatial on"));
    check(!s.settings.contains(QStringLiteral("equalizer")), QStringLiteral("M2: no legacy equalizer"));
    check(s.settings.value(QStringLiteral("model")).toString() == QLatin1String("JBL TOUR ONE M2"), QStringLiteral("M2: name from aa 12"));

    b.takeOutgoing();
    check(!b.setMode(QStringLiteral("noise-cancelling")).isValid(), QStringLiteral("M2: setMode accepted"));
    check(b.takeOutgoing() == QList<QByteArray>{hex("aa910710010102000300")}, QStringLiteral("M2: setMode NC writes slots"));
    b.setMode(QStringLiteral("off"));
    check(b.takeOutgoing() == QList<QByteArray>{hex("aa910710010002000300")}, QStringLiteral("M2: setMode off writes zero slots"));
    b.setMode(QStringLiteral("ambient"));
    check(b.takeOutgoing() == QList<QByteArray>{hex("aa910710010002000301")}, QStringLiteral("M2: setMode ambient uses last pass-through (TalkThru)"));
    b.apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("ambientType"), QStringLiteral("ambientAware")}}}});
    check(b.takeOutgoing() == QList<QByteArray>{hex("aa910710010002010300")}, QStringLiteral("M2: switching ambient type while in ambient"));
    b.flushRefreshNow();
    const QList<QByteArray> refresh = b.takeOutgoing();
    check(sent(refresh, hex("aa210131")) && sent(refresh, hex("aa210132")) && sent(refresh, hex("aa910111")), QStringLiteral("M2: noise re-read after write"));

    b.apply(QJsonObject{{QStringLiteral("sliders"), QJsonObject{{QStringLiteral("ancStrength"), 3}}},
                        {QStringLiteral("toggles"), QJsonObject{{QStringLiteral("wearDetection"), false}, {QStringLiteral("spatialAudio"), false}}},
                        {QStringLiteral("choices"), QJsonObject{{QStringLiteral("autoPowerOff"), QStringLiteral("60-minutes")}}}});
    const QList<QByteArray> writes = b.takeOutgoing();
    check(sent(writes, hex("aa9103200403")), QStringLiteral("M2: ancStrength write"));
    check(sent(writes, hex("aa350100")), QStringLiteral("M2: auto play write"));
    check(sent(writes, hex("aa9d03000001")), QStringLiteral("M2: spatial off keeps music mode"));
    check(sent(writes, hex("aa3303013c00")), QStringLiteral("M2: auto-off write"));
    check(b.apply(QJsonObject{{QStringLiteral("sliders"), QJsonObject{{QStringLiteral("ancStrength"), 9}}}}).isValid(), QStringLiteral("M2: ancStrength out of range rejected"));
    check(b.apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("autoPowerOff"), QStringLiteral("soon")}}}}).isValid(),
          QStringLiteral("M2: bad auto-off rejected"));
    b.apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("autoPowerOff"), QStringLiteral("off")}}}});
    check(sent(b.takeOutgoing(), hex("aa3303003c00")), QStringLiteral("M2: auto-off off keeps minutes"));
    check(b.setAmbientLevel(3).isValid(), QStringLiteral("M2: ambient level unsupported"));

    b.handleData(hex(Vector::AckOk));
    check(b.takeOutgoing().isEmpty(), QStringLiteral("M2: positive ack triggers nothing"));
}

void testLivePro2()
{
    JblBackend b(nullptr, device(QStringLiteral("JBL LIVE PRO 2 TWS")));
    b.requestState();
    const QList<QByteArray> initial = b.takeOutgoing();
    check(!sent(initial, hex("aa910111")), QStringLiteral("Live Pro 2: unreliable aa 91 01 11 not used"));
    check(sent(initial, hex("aa210134")) && sent(initial, hex("aa210141")), QStringLiteral("Live Pro 2: EQ and in-ear requested"));

    b.handleData(hex("aa250d010001503c0128ffffffffffffff"));
    Snapshot s = b.snapshot();
    check(battery(s, QStringLiteral("left")).value(QStringLiteral("level")).toInt() == 80, QStringLiteral("Live Pro 2: left 80"));
    check(battery(s, QStringLiteral("right")).value(QStringLiteral("level")).toInt() == 60
              && battery(s, QStringLiteral("right")).value(QStringLiteral("charging")).toBool(),
          QStringLiteral("Live Pro 2: right 60 charging"));
    check(battery(s, QStringLiteral("case")).value(QStringLiteral("level")).toInt() == 40
              && battery(s, QStringLiteral("case")).value(QStringLiteral("charging")).toBool(),
          QStringLiteral("Live Pro 2: case 40 charging"));
    check(!battery(s, QStringLiteral("left")).value(QStringLiteral("charging")).toBool() && s.battery == 60, QStringLiteral("Live Pro 2: overall min"));

    b.handleData(hex(Vector::LivePro2TalkThru));
    check(b.noise() == JblBackend::Noise::TalkThru && b.snapshot().mode == QLatin1String("ambient"), QStringLiteral("Live Pro 2: 32=01 outranks 31=01"));
    b.handleData(hex(Vector::LivePro2Ambient));
    check(b.noise() == JblBackend::Noise::Ambient, QStringLiteral("Live Pro 2: 32=02 is ambient aware"));
    b.handleData(hex(Vector::NoiseAncM2));
    check(b.noise() == JblBackend::Noise::Ambient, QStringLiteral("Live Pro 2: aa 91 slot reply ignored"));
    b.handleData(hex("aa22023101aa22023200"));
    check(b.snapshot().mode == QLatin1String("noise-cancelling"), QStringLiteral("Live Pro 2: 31=01 32=00 -> noise-cancelling"));

    b.handleData(hex(Vector::EqPresetUser));
    s = b.snapshot();
    const QJsonObject eq = s.settings.value(QStringLiteral("equalizer")).toObject();
    check(eq.value(QStringLiteral("preset")).toInt() == -1 && eq.value(QStringLiteral("presets")).toArray().size() == 8,
          QStringLiteral("Live Pro 2: user EQ shows no preset, 8 presets offered"));
    b.handleData(hex("aa22023405"));
    check(b.snapshot().settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("preset")).toInt() == 5, QStringLiteral("Live Pro 2: rock preset"));

    b.takeOutgoing();
    b.setMode(QStringLiteral("off"));
    check(b.takeOutgoing() == QList<QByteArray>{hex("aa310100")}, QStringLiteral("Live Pro 2: off is aa 31 01 00"));
    b.setMode(QStringLiteral("noise-cancelling"));
    check(b.takeOutgoing() == QList<QByteArray>{hex("aa310101")}, QStringLiteral("Live Pro 2: NC is aa 31 01 01"));
    b.setMode(QStringLiteral("ambient"));
    check(b.takeOutgoing() == QList<QByteArray>{hex("aa910710010002010300")}, QStringLiteral("Live Pro 2: ambient via slots with ANC 00"));
    b.apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("ambientType"), QStringLiteral("talkThru")}}}});
    check(b.takeOutgoing() == QList<QByteArray>{hex("aa910710010002000301")}, QStringLiteral("Live Pro 2: TalkThru via slots"));
    b.flushRefreshNow();
    const QList<QByteArray> refresh = b.takeOutgoing();
    check(sent(refresh, hex("aa210131")) && !sent(refresh, hex("aa910111")), QStringLiteral("Live Pro 2: re-read via status fields"));

    b.apply(QJsonObject{{QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 2}}}});
    check(b.takeOutgoing() == QList<QByteArray>{hex("aa400102")}, QStringLiteral("Live Pro 2: EQ write"));
    check(b.apply(QJsonObject{{QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 4}}}}).isValid(), QStringLiteral("Live Pro 2: user preset not writable"));
    b.flushRefreshNow();
    check(sent(b.takeOutgoing(), hex("aa210134")), QStringLiteral("Live Pro 2: EQ re-read"));

    b.handleData(hex(Vector::AckRefused));
    check(sent(b.takeOutgoing(), hex("aa210131")), QStringLiteral("Live Pro 2: refused ANC write re-reads state"));
}

void testBatteryEdges()
{
    JblBackend b(nullptr, device(QStringLiteral("JBL Live 770NC")));
    b.handleData(hex("aa250d010000ffffffffffffffffffff"));
    check(!b.ready(), QStringLiteral("battery: absent cells do not make ready"));
    b.handleData(hex("aa250d020000505affffffffffffffff"));
    check(!b.ready(), QStringLiteral("battery: sub-command other than 01 ignored"));
    b.handleData(hex("aa250d010000d0d2ffffffffffffffff"));
    const Snapshot s = b.snapshot();
    check(b.ready() && s.battery == 82 && battery(s, QStringLiteral("main")).value(QStringLiteral("charging")).toBool(),
          QStringLiteral("battery: bit 7 is charging, low 7 bits level"));

    JblBackend c(nullptr, device(QStringLiteral("JBL Live 770NC")));
    c.handleData(hex("aa250d0100005a5a01ffffffffffffff"));
    check(c.ready() && !c.earbuds(), QStringLiteral("battery: headphones stay headphones when the case slot is ff"));
    c.handleData(hex("aa250d0100005a5a0032ffffffffffff"));
    check(c.earbuds() && battery(c.snapshot(), QStringLiteral("case")).value(QStringLiteral("level")).toInt() == 50,
          QStringLiteral("battery: a reported case switches to earbuds"));
}

void testDiscovery()
{
    const auto match = [](const QString &alias, const QStringList &uuids) {
        const auto services = knownServices();
        const auto it = std::find_if(services.begin(), services.end(), [&](const KnownService &s) {
            return (!s.uuid || uuids.contains(QString::fromLatin1(s.uuid), Qt::CaseInsensitive))
                && (!s.nameHint || alias.contains(QLatin1String(s.nameHint), Qt::CaseInsensitive));
        });
        return it == services.end() ? std::optional<KnownService>() : std::optional<KnownService>(*it);
    };
    const QStringList m2Uuids{QStringLiteral("00001101-0000-1000-8000-00805f9b34fb"), QStringLiteral("df21fe2c-2515-4fdb-8886-f12c4d67927c"),
                              QStringLiteral("931c7e8a-540f-4686-b798-e8df0a2ad9f7")};
    const auto m2 = match(QStringLiteral("JBL TOUR ONE M2"), m2Uuids);
    check(m2 && m2->kind == DeviceKind::Jbl && m2->ownTransport, QStringLiteral("discovery: Tour One M2 is JBL with own transport"));
    const auto buds = match(QStringLiteral("JBL Tune Buds"), m2Uuids);
    check(buds && buds->kind == DeviceKind::Jbl, QStringLiteral("discovery: JBL Tune Buds is not taken by Galaxy"));
    const auto galaxy = match(QStringLiteral("Galaxy Buds+ (A1B2)"), {QStringLiteral("00001101-0000-1000-8000-00805F9B34FB")});
    check(galaxy && galaxy->kind == DeviceKind::GalaxyClassic, QStringLiteral("discovery: Galaxy Buds+ stays Galaxy"));
    check(!match(QStringLiteral("Car kit"), {}), QStringLiteral("discovery: unrelated device ignored"));
    check(createBackend(nullptr, device(QStringLiteral("JBL TOUR ONE M2")))->vendor() == QLatin1String("jbl"), QStringLiteral("discovery: factory builds JBL backend"));
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testEncoding();
    testFraming();
    testModels();
    testTourOneM2();
    testLivePro2();
    testBatteryEdges();
    testDiscovery();
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
