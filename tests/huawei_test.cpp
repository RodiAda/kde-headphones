#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QThread>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "HuaweiBackend.h"
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

QByteArray hex(const char *s)
{
    return QByteArray::fromHex(QByteArray(s).replace(' ', ""));
}

QByteArray frame(uint16_t id, const QList<std::pair<int, QByteArray>> &tlv)
{
    return HuaweiBackend::encodePacket(id, tlv);
}

QByteArray fragment(uint8_t flag, uint8_t index, const QByteArray &data)
{
    const int length = int(data.size()) + 2;
    QByteArray out;
    out.append(char(0x5A));
    out.append(char(length >> 8));
    out.append(char(length & 0xFF));
    out.append(char(flag));
    out.append(char(index));
    out.append(data);
    const uint16_t crc = HuaweiBackend::crc16(out);
    out.append(char(crc >> 8));
    out.append(char(crc & 0xFF));
    return out;
}

const QByteArray StatusOk = hex("000186a0");
const QByteArray StatusUnsupported = hex("000186a3");

namespace Vector {
const char *const InfoFb5 =
    "5a0076000107020201410309484c31524845494e4d07204861726d6f6e794f5320352e302e302e3230382846303031483030334339302909105445535453455249414c303030"
    "3030310a0f42544654303031332d3030303134310f08425446543030313318105445535442554453455249414c303031190100a983";
const char *const InfoFb6 =
    "5a009700010702020155030d48523148364d525f5665722e4107204861726d6f6e794f5320362e312e302e3335382846303033483030334339302909105445535453455249"
    "414c3030303030310a0f42544654303032302d3030303135350f08425446543030323018255445535442554453455249414c3030315445535442554453455249414c303031"
    "54455354421901021b0611223344556632c3";
const char *const InfoPro3 =
    "5a009800010702020149030e484c3153414b4d325f5665722e4107204861726d6f6e794f5320352e302e302e3233322846303033483030334339302909105445535453455249"
    "414c3030303030310a0f42544654303031382d3030303134390f08425446543030313818255445535442554453455249414c3030315445535442554453455249414c303031"
    "54455354421901001b061122334455668051";
const char *const BatteryFb5 = "5a0014000108010164020364645903030000000402140aa588";
const char *const BatteryFb6 = "5a001b0001080101640203646464030300000004020a140502000006010a09ce";
const char *const BatteryPro3 = "5a0018000108010164020364642d030300000004020a1405020000c109";
const char *const BatteryCaseCharging = "5a0010000108010164020364646403030000015031";
const char *const BatteryPush = "5a001000012701015a0203645a300303000001e79e";
const char *const BatterySingle = "5a0009000108010140030100ed2e";
const char *const AncFb5 = "5a0007002b2a010203004062";
const char *const AncFb6 = "5a0007002b2a010200001531";
const char *const AncPro3Ultra = "5a0007002b2a010202016372";
const char *const AncPro3VoiceBoost = "5a0007002b2a010201020642";
const char *const AncWriteAck = "5a0006002b040201003171";
const char *const EqualizerFb5 = "5a0014002b4a0101010201010304010203090401010800742d";
const char *const EqualizerFb6 = "5a0016002b4a0101010201000306000203090b0c0401000800c3a5";
const char *const EqualizerPro3 = "5a0014002b4a01010102010503040203050904010508006e8b";
const char *const WearOn = "5a0006002b11010101dfe2";
const char *const WearOff = "5a0006002b11010100cfc3";
const char *const WearWriteAck = "5a0009002b107f04000186a0729d";
const char *const MultipointOn = "5a0006002b2f0101015151";
const char *const LowLatencyOff = "5a0006002b6c020100ed60";
const char *const LowLatencyWriteAck = "5a0009002b6c7f04000186a0a497";

const char *const BatteryRequest = "5a0009000108010002000300fbb9";
const char *const AncReadRequest = "5a0005002b2a0100427e";
const char *const AncWriteAwareness = "5a0007002b0401020200b44f";
const char *const AncWriteDynamic = "5a0007002b0401020103d17f";
const char *const AncWriteOffKeepLevel = "5a0007002b0401020003e24e";
const char *const WearReadRequest = "5a0005002b110100772a";
const char *const WearWriteOn = "5a0006002b10010101a956";
const char *const EqualizerSelect9 = "5a0006002b49010109ae12";
const char *const LowLatencyReadRequest = "5a0005002b6c0200b820";
const char *const LowLatencyWriteOn = "5a0006002b6c010101a411";
const char *const AutoPauseReadResponse = "5a0006002b11010100cfc3";
}

struct Harness {
    FakeLink link;
    MDRConnection conn{};
    QHash<uint16_t, QByteArray> replies;
    QList<QByteArray> sent;
    std::unique_ptr<HuaweiBackend> backend;

    explicit Harness(const QString &name)
    {
        conn.user = &link;
        conn.recv = fakeRecv;
        conn.send = fakeSend;
        conn.getLastError = fakeError;
        backend = std::make_unique<HuaweiBackend>(&conn, DeviceInfo{name, QStringLiteral("00:11:22:33:44:55"), {}, DeviceKind::Huawei});
    }

    void reply(const char *h)
    {
        const QByteArray f = hex(h);
        replies.insert(uint16_t((uint8_t(f.at(4)) << 8) | uint8_t(f.at(5))), f);
    }

    void reply(uint16_t id, const QByteArray &f) { replies.insert(id, f); }

    bool step()
    {
        const bool ok = backend->tick();
        QByteArray out = link.outgoing;
        link.outgoing.clear();
        while (out.size() >= 6 && uint8_t(out.at(0)) == 0x5A) {
            const int total = ((uint8_t(out.at(1)) << 8) | uint8_t(out.at(2))) + 5;
            const QByteArray f = out.left(total);
            out.remove(0, total);
            sent.append(f);
            const uint16_t id = uint16_t((uint8_t(f.at(4)) << 8) | uint8_t(f.at(5)));
            if (replies.contains(id))
                link.incoming.append(replies.value(id));
        }
        return ok;
    }

    bool runUntil(const std::function<bool()> &done, qint64 timeoutMs = 3000)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            if (!step())
                return false;
            if (done())
                return true;
            QThread::msleep(5);
        }
        return done();
    }

    bool settle(qint64 ms = 1500)
    {
        return runUntil([] { return false; }, ms) || true;
    }

    bool sentFrame(const QByteArray &f) const { return sent.contains(f); }

    bool start()
    {
        backend->start();
        return runUntil([this] { return backend->ready(); });
    }
};

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

QJsonValue control(const QJsonObject &settings, const char *group, const char *key)
{
    for (const QJsonValue &c : settings.value(QLatin1String(group)).toArray()) {
        if (c.toObject().value(QStringLiteral("key")).toString() == QLatin1String(key))
            return c.toObject().value(QStringLiteral("value"));
    }
    return QJsonValue(QJsonValue::Undefined);
}

QStringList choiceOptions(const QJsonObject &settings, const char *key)
{
    for (const QJsonValue &c : settings.value(QStringLiteral("choices")).toArray()) {
        if (c.toObject().value(QStringLiteral("key")).toString() == QLatin1String(key)) {
            QStringList out;
            for (const QJsonValue &o : c.toObject().value(QStringLiteral("options")).toArray())
                out << o.toString();
            return out;
        }
    }
    return {};
}

QString presets(const QJsonObject &settings)
{
    QStringList out;
    for (const QJsonValue &p : settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("presets")).toArray())
        out << QStringLiteral("%1:%2").arg(p.toObject().value(QStringLiteral("id")).toInt()).arg(p.toObject().value(QStringLiteral("key")).toString());
    return out.join(u',');
}

int preset(const QJsonObject &settings)
{
    return settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("preset")).toInt(-100);
}

void testEncoding()
{
    using namespace Vector;
    check(HuaweiBackend::encodePacket(0x0108, {{1, {}}, {2, {}}, {3, {}}}) == hex(BatteryRequest), QStringLiteral("encode battery read"));
    check(HuaweiBackend::encodePacket(0x2B2A, {{1, {}}}) == hex(AncReadRequest), QStringLiteral("encode noise control read"));
    check(HuaweiBackend::encodePacket(0x2B04, {{1, hex("0200")}}) == hex(AncWriteAwareness), QStringLiteral("encode awareness write"));
    check(HuaweiBackend::encodePacket(0x2B04, {{1, hex("0103")}}) == hex(AncWriteDynamic), QStringLiteral("encode dynamic cancelling write"));
    check(HuaweiBackend::encodePacket(0x2B04, {{1, hex("0003")}}) == hex(AncWriteOffKeepLevel), QStringLiteral("encode off write"));
    check(HuaweiBackend::encodePacket(0x2B11, {{1, {}}}) == hex(WearReadRequest), QStringLiteral("encode wear detection read"));
    check(HuaweiBackend::encodePacket(0x2B10, {{1, hex("01")}}) == hex(WearWriteOn), QStringLiteral("encode wear detection write"));
    check(HuaweiBackend::encodePacket(0x2B49, {{1, hex("09")}}) == hex(EqualizerSelect9), QStringLiteral("encode equalizer select"));
    check(HuaweiBackend::encodePacket(0x2B6C, {{2, {}}}) == hex(LowLatencyReadRequest), QStringLiteral("encode low latency read"));
    check(HuaweiBackend::encodePacket(0x2B6C, {{1, hex("01")}}) == hex(LowLatencyWriteOn), QStringLiteral("encode low latency write"));

    const char *const received[] = {InfoFb5, InfoFb6, InfoPro3, BatteryFb5, BatteryFb6, BatteryPro3, BatteryCaseCharging, BatteryPush, BatterySingle,
                                    AncFb5, AncFb6, AncPro3Ultra, AncPro3VoiceBoost, AncWriteAck, EqualizerFb5, EqualizerFb6, EqualizerPro3,
                                    WearOn, WearOff, WearWriteAck, MultipointOn, LowLatencyOff, LowLatencyWriteAck, AutoPauseReadResponse};
    int good = 0;
    for (const char *v : received) {
        const QByteArray f = hex(v);
        if (HuaweiBackend::crc16(f) == 0 && HuaweiBackend::encodeFrame(f.mid(4, f.size() - 6)) == f)
            good++;
    }
    check(good == int(std::size(received)), QStringLiteral("captured frames re-encode byte for byte (%1/%2)").arg(good).arg(std::size(received)));

    const auto info = HuaweiBackend::decodePayload(hex(InfoFb5).mid(4, hex(InfoFb5).size() - 6));
    check(info && info->id == 0x0107 && info->value(10) == "BTFT0013-000141" && info->value(15) == "BTFT0013",
          QStringLiteral("decode device info TLVs"));
}

void testModelNames()
{
    struct Case {
        const char *name;
        HuaweiModel model;
    };
    const Case cases[] = {
        {"HUAWEI FreeBuds 5", HuaweiModel::FreeBuds5},
        {"HUAWEI FreeBuds 5i", HuaweiModel::FreeBuds5i},
        {"HUAWEI FreeBuds 4E", HuaweiModel::Unknown},
        {"HUAWEI FreeBuds 4", HuaweiModel::FreeBuds4},
        {"HUAWEI FreeBuds Pro", HuaweiModel::FreeBudsPro},
        {"HUAWEI FreeBuds Pro 2 +", HuaweiModel::FreeBudsPro2},
        {"HUAWEI FreeBuds Pro 3", HuaweiModel::FreeBudsPro3},
        {"HUAWEI FreeBuds SE 4 ANC", HuaweiModel::FreeBudsSe4},
        {"HUAWEI FreeBuds SE", HuaweiModel::FreeBudsSe},
        {"HUAWEI FreeLace Pro", HuaweiModel::FreeLacePro},
        {"HUAWEI FreeLace Pro 2", HuaweiModel::FreeLacePro2},
        {"HUAWEI FreeClip 2", HuaweiModel::FreeClip2},
        {"HONOR Earbuds 2 Lite", HuaweiModel::HonorEarbuds2},
    };
    for (const Case &c : cases)
        check(HuaweiBackend::modelFromName(QString::fromLatin1(c.name)) == c.model, QStringLiteral("model from name: %1").arg(QLatin1String(c.name)));
    check(HuaweiBackend::modelFromInfo(QStringLiteral("BTFT0020"), QString()) == HuaweiModel::FreeBuds6, QStringLiteral("model from SKU"));
    check(HuaweiBackend::modelFromInfo(QString(), QStringLiteral("00016D")) == HuaweiModel::FreeBudsPro5, QStringLiteral("model from model id"));
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
    check(match(QStringLiteral("HUAWEI FreeBuds Pro 3")) == DeviceKind::Huawei, QStringLiteral("discovery: FreeBuds is Huawei"));
    check(match(QStringLiteral("HUAWEI FreeLace Pro")) == DeviceKind::Huawei, QStringLiteral("discovery: FreeLace is Huawei"));
    check(match(QStringLiteral("HONOR Earbuds 2")) == DeviceKind::Huawei, QStringLiteral("discovery: HONOR Earbuds is Huawei"));
    check(match(QStringLiteral("Galaxy Buds+ (A1B2)")) == DeviceKind::GalaxyClassic, QStringLiteral("discovery: Galaxy Buds+ stays Galaxy"));
    check(match(QStringLiteral("Galaxy Buds Live (A1B2)")) == DeviceKind::GalaxyClassic, QStringLiteral("discovery: Galaxy Buds Live stays Galaxy"));
    check(!match(QStringLiteral("Car kit")), QStringLiteral("discovery: other SPP devices are ignored"));
}

void testFreeBuds5()
{
    using namespace Vector;
    Harness h(QStringLiteral("Kitchen earphones"));
    h.reply(InfoFb5);
    h.reply(BatteryFb5);
    h.reply(AncFb5);
    h.reply(WearOn);
    h.reply(EqualizerFb5);
    h.reply(MultipointOn);
    h.reply(AncWriteAck);
    h.reply(WearWriteAck);
    h.reply(0x2B49, frame(0x2B49, {{0x7F, StatusOk}}));
    h.reply(0x2B2E, frame(0x2B2E, {{0x7F, StatusOk}}));
    const QString n = QStringLiteral("FreeBuds 5");

    check(h.start(), n + QStringLiteral(": ready"));
    check(h.backend->model() == HuaweiModel::FreeBuds5, n + QStringLiteral(": identified from device info"));
    check(h.sentFrame(hex(BatteryRequest)), n + QStringLiteral(": battery read sent"));
    h.runUntil([&] { return h.sentFrame(HuaweiBackend::encodePacket(0x2B2F, {{1, {}}})); });
    h.step();

    Snapshot s = h.backend->snapshot();
    check(s.settings.value(QStringLiteral("model")).toString() == QLatin1String("HUAWEI FreeBuds 5"), n + QStringLiteral(": model name"));
    check(s.settings.value(QStringLiteral("untested")).toBool(), n + QStringLiteral(": untested flag"));
    check(level(s.batteries, "left") == 100 && level(s.batteries, "right") == 100 && level(s.batteries, "case") == 89,
          n + QStringLiteral(": batteries 100/100/89"));
    check(s.battery == 100, n + QStringLiteral(": overall battery"));
    check(s.modes == QStringList{ModeNc, ModeOff}, n + QStringLiteral(": modes nc/off (got %1)").arg(s.modes.join(u',')));
    check(s.mode == ModeOff, n + QStringLiteral(": mode off"));
    check(presets(s.settings) == QLatin1String("1:default,2:bass-boost,3:treble-boost,9:vocal"),
          n + QStringLiteral(": presets (got %1)").arg(presets(s.settings)));
    check(preset(s.settings) == 1, n + QStringLiteral(": active preset 1"));
    check(control(s.settings, "toggles", "wearDetection") == QJsonValue(true), n + QStringLiteral(": wear detection on"));
    check(control(s.settings, "toggles", "multipoint") == QJsonValue(true), n + QStringLiteral(": multipoint on"));
    check(control(s.settings, "toggles", "lowLatency").isUndefined(), n + QStringLiteral(": no low latency"));
    check(control(s.settings, "toggles", "voiceBoost").isUndefined(), n + QStringLiteral(": no voice boost"));
    check(!s.settings.contains(QStringLiteral("choices")), n + QStringLiteral(": no strength while off"));
    check(h.backend->setAmbientLevel(3).type() == QDBusError::NotSupported, n + QStringLiteral(": ambient level unsupported"));
    check(h.backend->setMode(ModeAmbient).isValid(), n + QStringLiteral(": ambient mode refused"));

    h.reply(0x2B2A, frame(0x2B2A, {{1, hex("0301")}}));
    check(!h.backend->setMode(ModeNc).isValid(), n + QStringLiteral(": set noise cancelling"));
    check(h.runUntil([&] { return h.sentFrame(HuaweiBackend::encodePacket(0x2B04, {{1, hex("01ff")}})); }),
          n + QStringLiteral(": mode write [01 ff]"));
    h.settle(600);
    s = h.backend->snapshot();
    check(s.mode == ModeNc, n + QStringLiteral(": mode noise cancelling after read-back"));
    check(choiceOptions(s.settings, "ancStrength") == QStringList{QStringLiteral("comfort"), QStringLiteral("normal"), QStringLiteral("dynamic")},
          n + QStringLiteral(": strength options"));
    check(control(s.settings, "choices", "ancStrength") == QJsonValue(QStringLiteral("dynamic")), n + QStringLiteral(": strength dynamic"));

    h.sent.clear();
    check(!h.backend->apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("ancStrength"), QStringLiteral("dynamic")}}}}).isValid(),
          n + QStringLiteral(": apply strength"));
    check(h.runUntil([&] { return h.sentFrame(hex(AncWriteDynamic)); }), n + QStringLiteral(": strength write matches capture"));
    check(h.backend->apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("ancStrength"), QStringLiteral("ultra")}}}}).isValid(),
          n + QStringLiteral(": unsupported strength refused"));

    check(!h.backend->apply(QJsonObject{{QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 9}}}}).isValid(),
          n + QStringLiteral(": apply preset 9"));
    check(h.runUntil([&] { return h.sentFrame(hex(EqualizerSelect9)); }), n + QStringLiteral(": preset write matches capture"));
    check(preset(h.backend->snapshot().settings) == 9, n + QStringLiteral(": preset 9 shown"));
    check(h.backend->apply(QJsonObject{{QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 7}}}}).isValid(),
          n + QStringLiteral(": unknown preset refused"));

    h.reply(WearOff);
    check(!h.backend->apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("wearDetection"), false}}}}).isValid(),
          n + QStringLiteral(": apply wear detection off"));
    check(h.runUntil([&] { return h.sentFrame(HuaweiBackend::encodePacket(0x2B10, {{1, hex("00")}})); }), n + QStringLiteral(": wear write off"));
    h.settle(600);
    check(control(h.backend->snapshot().settings, "toggles", "wearDetection") == QJsonValue(false), n + QStringLiteral(": wear detection off"));
    h.reply(WearOn);
    h.backend->apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("wearDetection"), true}}}});
    check(h.runUntil([&] { return h.sentFrame(hex(WearWriteOn)); }), n + QStringLiteral(": wear write on matches capture"));

    h.link.incoming.append(hex(BatteryPush));
    h.step();
    s = h.backend->snapshot();
    check(level(s.batteries, "right") == 90 && level(s.batteries, "case") == 48 && charging(s.batteries, "case"),
          n + QStringLiteral(": battery push applied"));
}

void testFreeBuds6()
{
    using namespace Vector;
    Harness h(QStringLiteral("HUAWEI FreeBuds 6"));
    h.reply(InfoFb6);
    h.reply(BatteryCaseCharging);
    h.reply(AncFb6);
    h.reply(WearOn);
    h.reply(EqualizerFb6);
    h.reply(MultipointOn);
    const QString n = QStringLiteral("FreeBuds 6");
    check(h.start(), n + QStringLiteral(": ready"));
    h.runUntil([&] { return h.sentFrame(HuaweiBackend::encodePacket(0x2B2F, {{1, {}}})); });
    h.step();
    const Snapshot s = h.backend->snapshot();
    check(h.backend->model() == HuaweiModel::FreeBuds6, n + QStringLiteral(": model"));
    check(level(s.batteries, "case") == 100 && charging(s.batteries, "case") && !charging(s.batteries, "left"), n + QStringLiteral(": case charging"));
    check(s.mode == ModeOff, n + QStringLiteral(": mode off"));
    check(presets(s.settings) == QLatin1String("0:default,2:bass-boost,3:treble-boost,9:vocal,11:balanced,12:classical"),
          n + QStringLiteral(": presets (got %1)").arg(presets(s.settings)));
    check(preset(s.settings) == 0, n + QStringLiteral(": active preset 0"));

    Harness b(QStringLiteral("HUAWEI FreeBuds 6"));
    b.reply(InfoFb6);
    b.reply(BatteryFb6);
    check(b.start(), n + QStringLiteral(": ready with extended battery TLVs"));
    check(level(b.backend->snapshot().batteries, "case") == 100, n + QStringLiteral(": extended battery parsed"));
}

void testFreeBudsPro3()
{
    using namespace Vector;
    Harness h(QStringLiteral("HUAWEI FreeBuds Pro 3"));
    h.reply(InfoPro3);
    h.reply(BatteryPro3);
    h.reply(AncPro3VoiceBoost);
    h.reply(WearOn);
    h.reply(EqualizerPro3);
    h.reply(MultipointOn);
    h.reply(AncWriteAck);
    const QString n = QStringLiteral("FreeBuds Pro 3");
    check(h.start(), n + QStringLiteral(": ready"));
    h.runUntil([&] { return h.sentFrame(HuaweiBackend::encodePacket(0x2B2F, {{1, {}}})); });
    h.step();
    Snapshot s = h.backend->snapshot();
    check(level(s.batteries, "case") == 45, n + QStringLiteral(": case battery 45"));
    check(s.modes == QStringList{ModeNc, ModeAmbient, ModeOff}, n + QStringLiteral(": modes nc/ambient/off"));
    check(s.mode == ModeAmbient, n + QStringLiteral(": mode ambient"));
    check(control(s.settings, "toggles", "voiceBoost") == QJsonValue(true), n + QStringLiteral(": voice boost on"));
    check(presets(s.settings) == QLatin1String("2:bass-boost,3:treble-boost,5:default,9:vocal"), n + QStringLiteral(": presets"));
    check(preset(s.settings) == 5, n + QStringLiteral(": active preset 5"));

    h.reply(0x2B2A, frame(0x2B2A, {{1, hex("0202")}}));
    h.backend->apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("voiceBoost"), false}}}});
    check(h.runUntil([&] { return h.sentFrame(HuaweiBackend::encodePacket(0x2B04, {{1, hex("0202")}})); }), n + QStringLiteral(": voice boost off write"));
    h.settle(600);
    check(control(h.backend->snapshot().settings, "toggles", "voiceBoost") == QJsonValue(false), n + QStringLiteral(": voice boost off read back"));

    h.reply(AncPro3Ultra);
    h.backend->setMode(ModeNc);
    check(h.runUntil([&] { return h.sentFrame(HuaweiBackend::encodePacket(0x2B04, {{1, hex("01ff")}})); }), n + QStringLiteral(": mode write [01 ff]"));
    h.settle(600);
    s = h.backend->snapshot();
    check(s.mode == ModeNc && control(s.settings, "choices", "ancStrength") == QJsonValue(QStringLiteral("ultra")), n + QStringLiteral(": ultra read back"));
    check(choiceOptions(s.settings, "ancStrength")
              == QStringList{QStringLiteral("comfort"), QStringLiteral("normal"), QStringLiteral("ultra"), QStringLiteral("dynamic")},
          n + QStringLiteral(": four strength levels"));
    check(control(s.settings, "toggles", "voiceBoost").isUndefined(), n + QStringLiteral(": voice boost hidden outside awareness"));

    h.sent.clear();
    h.backend->setMode(ModeNc);
    check(h.runUntil([&] { return h.sentFrame(HuaweiBackend::encodePacket(0x2B04, {{1, hex("0102")}})); }),
          n + QStringLiteral(": reselecting the mode keeps the level"));
}

void testFreeBudsPro5()
{
    using namespace Vector;
    Harness h(QStringLiteral("HUAWEI FreeBuds Pro 5"));
    h.reply(0x0107, frame(0x0107, {{0x7F, StatusUnsupported}}));
    h.reply(BatteryPro3);
    h.reply(0x2B2A, frame(0x2B2A, {{1, hex("0301")}}));
    h.reply(LowLatencyOff);
    h.reply(0x2B11, frame(0x2B11, {{0x7F, StatusUnsupported}}));
    h.reply(0x2B4A, frame(0x2B4A, {{0x7F, StatusUnsupported}}));
    h.reply(0x2B2F, frame(0x2B2F, {{0x7F, StatusUnsupported}}));
    const QString n = QStringLiteral("FreeBuds Pro 5");
    check(h.start(), n + QStringLiteral(": ready"));
    check(h.runUntil([&] { return control(h.backend->snapshot().settings, "toggles", "lowLatency") == QJsonValue(false); }),
          n + QStringLiteral(": low latency off"));
    check(control(h.backend->snapshot().settings, "choices", "ancStrength") == QJsonValue(QStringLiteral("dual-engine")),
          n + QStringLiteral(": level 3 is dual engine"));
    h.reply(LowLatencyWriteAck);
    h.backend->apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("lowLatency"), true}}}});
    check(h.runUntil([&] { return h.sentFrame(hex(LowLatencyWriteOn)); }), n + QStringLiteral(": low latency write matches capture"));
    check(control(h.backend->snapshot().settings, "toggles", "lowLatency") == QJsonValue(true), n + QStringLiteral(": low latency on"));
    const QJsonObject settings = h.backend->snapshot().settings;
    check(control(settings, "toggles", "wearDetection").isUndefined() && control(settings, "toggles", "multipoint").isUndefined()
              && !settings.contains(QStringLiteral("equalizer")),
          n + QStringLiteral(": settings refused by the earbuds stay hidden"));
}

void testStudio()
{
    using namespace Vector;
    Harness h(QStringLiteral("HUAWEI FreeBuds Studio"));
    h.reply(0x0107, frame(0x0107, {{0x7F, StatusUnsupported}}));
    h.reply(BatterySingle);
    h.reply(AncFb6);
    h.reply(AutoPauseReadResponse);
    const QString n = QStringLiteral("FreeBuds Studio");
    check(h.start(), n + QStringLiteral(": ready"));
    h.runUntil([&] { return h.sentFrame(hex(WearReadRequest)); });
    h.step();
    const Snapshot s = h.backend->snapshot();
    check(s.batteries.size() == 1 && level(s.batteries, "main") == 64 && !charging(s.batteries, "main"), n + QStringLiteral(": single battery 64"));
    check(s.battery == 64, n + QStringLiteral(": overall battery"));
    check(control(s.settings, "toggles", "wearDetection") == QJsonValue(false), n + QStringLiteral(": wear detection off"));
    check(!s.settings.contains(QStringLiteral("equalizer")), n + QStringLiteral(": no equalizer"));
}

void testUnknownAndFraming()
{
    using namespace Vector;
    Harness h(QStringLiteral("HUAWEI FreeArc"));
    const QString n = QStringLiteral("framing");
    h.backend->start();
    h.step();
    check(h.sentFrame(HuaweiBackend::encodePacket(0x0107, {{1, {}}, {2, {}}, {3, {}}, {4, {}}, {5, {}}, {6, {}}, {7, {}}, {8, {}}, {9, {}},
                                                            {10, {}}, {15, {}}, {24, {}}, {27, {}}})),
          n + QStringLiteral(": device info read sent first"));

    QByteArray corrupt = hex(BatteryFb5);
    corrupt[10] = char(0x11);
    h.link.incoming = hex("00ff7f") + corrupt;
    h.step();
    check(!h.backend->ready(), n + QStringLiteral(": corrupted CRC ignored"));

    const QByteArray payload = hex(BatteryPush).mid(4, hex(BatteryPush).size() - 6);
    const QByteArray first = fragment(1, 0, payload.left(7));
    const QByteArray last = fragment(3, 1, payload.mid(7));
    h.link.incoming = first + last.left(4);
    h.step();
    check(!h.backend->ready(), n + QStringLiteral(": partial fragment buffered"));
    h.link.incoming = last.mid(4);
    h.step();
    check(h.backend->ready(), n + QStringLiteral(": fragmented battery reassembled"));
    const Snapshot s = h.backend->snapshot();
    check(level(s.batteries, "left") == 100 && level(s.batteries, "right") == 90 && level(s.batteries, "case") == 48,
          n + QStringLiteral(": fragmented levels"));
    check(s.modes.isEmpty() && s.mode.isEmpty(), n + QStringLiteral(": unknown model has no noise control"));
    check(s.settings.value(QStringLiteral("model")).toString() == QLatin1String("HUAWEI FreeBuds"), n + QStringLiteral(": unknown model name"));

    h.link.incoming = h.link.outgoing;
    for (const QByteArray &f : std::as_const(h.sent))
        h.link.incoming.append(f);
    h.step();
    check(h.link.incoming.isEmpty() && h.backend->ready(), n + QStringLiteral(": own frames are consumed by the parser"));
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testEncoding();
    testModelNames();
    testDiscovery();
    testFreeBuds5();
    testFreeBuds6();
    testFreeBudsPro3();
    testFreeBudsPro5();
    testStudio();
    testUnknownAndFraming();
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
