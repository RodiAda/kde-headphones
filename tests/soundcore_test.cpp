#include <QCoreApplication>
#include <QJsonDocument>

#include <cstdio>
#include <cstring>
#include <memory>

#include "SoundcoreBackend.h"

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

QByteArray bytes(std::initializer_list<int> values)
{
    QByteArray out;
    for (const int v : values)
        out.append(char(v));
    return out;
}

QByteArray inbound(uint16_t command, const QByteArray &body)
{
    QByteArray out = QByteArray::fromHex("09ff000001");
    const int length = 9 + int(body.size()) + 1;
    out.append(char(command >> 8));
    out.append(char(command & 0xFF));
    out.append(char(length & 0xFF));
    out.append(char(length >> 8));
    out.append(body);
    uint8_t sum = 0;
    for (const char c : out)
        sum = uint8_t(sum + uint8_t(c));
    out.append(char(sum));
    return out;
}

QByteArray hex(const char *text)
{
    return QByteArray::fromHex(text);
}

struct Harness {
    explicit Harness(const char *name)
    {
        conn.user = &link;
        conn.recv = fakeRecv;
        conn.send = fakeSend;
        conn.getLastError = fakeError;
        backend = std::make_unique<SoundcoreBackend>(&conn, DeviceInfo{QString::fromLatin1(name), QStringLiteral("00:11:22:33:44:55"), {}, DeviceKind::Soundcore});
        backend->start();
        backend->tick();
        request = sent();
    }

    QByteArray sent()
    {
        const QByteArray out = link.outgoing;
        link.outgoing.clear();
        return out;
    }

    void feed(const QByteArray &data)
    {
        link.incoming.append(data);
        backend->tick();
    }

    QByteArray exchange(uint16_t command)
    {
        backend->tick();
        const QByteArray out = sent();
        feed(inbound(command, {}));
        return out;
    }

    Snapshot snap() { return backend->snapshot(); }

    FakeLink link;
    MDRConnection conn{};
    std::unique_ptr<SoundcoreBackend> backend;
    QByteArray request;
};

int level(const Snapshot &s, const char *part)
{
    for (const QJsonValue &b : s.batteries) {
        if (b.toObject().value(QStringLiteral("part")).toString() == QLatin1String(part))
            return b.toObject().value(QStringLiteral("level")).toInt();
    }
    return -1;
}

bool charging(const Snapshot &s, const char *part)
{
    for (const QJsonValue &b : s.batteries) {
        if (b.toObject().value(QStringLiteral("part")).toString() == QLatin1String(part))
            return b.toObject().value(QStringLiteral("charging")).toBool();
    }
    return false;
}

QJsonValue control(const Snapshot &s, const char *group, const char *key)
{
    for (const QJsonValue &v : s.settings.value(QLatin1String(group)).toArray()) {
        if (v.toObject().value(QStringLiteral("key")).toString() == QLatin1String(key))
            return v.toObject().value(QStringLiteral("value"));
    }
    return QJsonValue(QJsonValue::Undefined);
}

int toggle(const Snapshot &s, const char *key)
{
    const QJsonValue v = control(s, "toggles", key);
    return v.isBool() ? int(v.toBool()) : -1;
}

int preset(const Snapshot &s)
{
    return s.settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("preset")).toInt(-1);
}

QString bandsText(const Snapshot &s)
{
    return QString::fromUtf8(QJsonDocument(s.settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("bands")).toArray()).toJson(QJsonDocument::Compact));
}

QJsonObject change(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}

void commonChecks(Harness &h, const QString &n, const char *model)
{
    check(h.request == hex("08ee00000001010a0002"), n + QStringLiteral(": state request frame (%1)").arg(QString::fromLatin1(h.request.toHex())));
    check(h.backend->ready(), n + QStringLiteral(": state parsed"));
    const Snapshot s = h.snap();
    check(s.settings.value(QStringLiteral("model")).toString() == QLatin1String(model),
          n + QStringLiteral(": model %1 (got %2)").arg(QLatin1String(model), s.settings.value(QStringLiteral("model")).toString()));
    check(s.settings.value(QStringLiteral("untested")).toBool(), n + QStringLiteral(": untested flag"));
}

void testLifeTune()
{
    const QString n = QStringLiteral("A3029 (a3028.rs example_state_update_packet)");
    Harness h("Soundcore");
    h.feed(hex("09ff000001010146000500fefe3cb48fa08eb4748800000000000000000000000000000000000000000000000200010030322e33303330323930383645433832463132414330"));
    commonChecks(h, n, "Soundcore Life Tune");
    Snapshot s = h.snap();
    check(level(s, "main") == 100 && !charging(s, "main"), n + QStringLiteral(": battery 5/5"));
    check(s.mode == QLatin1String("off"), n + QStringLiteral(": normal mode (got %1)").arg(s.mode));
    check(s.modes == QStringList({QStringLiteral("noise-cancelling"), QStringLiteral("ambient"), QStringLiteral("off")}), n + QStringLiteral(": modes"));
    check(preset(s) == 0xFEFE, n + QStringLiteral(": custom equalizer"));
    check(bandsText(s) == QLatin1String("[-6,6,2,4,2,6,0,2]"), n + QStringLiteral(": custom bands (got %1)").arg(bandsText(s)));
    check(!s.settings.contains(QStringLiteral("toggles")), n + QStringLiteral(": no toggles"));

    h.feed(inbound(0x0103, bytes({0x02})));
    h.feed(inbound(0x0104, bytes({0x01})));
    s = h.snap();
    check(level(s, "main") == 40 && charging(s, "main"), n + QStringLiteral(": battery level and charging updates (battery_level.rs, battery_charging.rs)"));

    h.feed(hex("09ff00000106010e000202010023"));
    check(h.snap().mode == QLatin1String("off"), n + QStringLiteral(": sound mode update (sound_modes.rs it_parses_valid_packet)"));

    h.backend->setMode(QStringLiteral("noise-cancelling"));
    check(h.exchange(0x0681) == hex("08ee00000006810e00000201008e"), n + QStringLiteral(": set noise cancelling keeps the indoor scene"));
    s = h.snap();
    check(control(s, "choices", "noiseCancellingMode").toString() == QLatin1String("indoor"), n + QStringLiteral(": scene choice shown in NC"));

    h.backend->apply(change(R"({"choices":{"noiseCancellingMode":"outdoor"}})"));
    check(h.exchange(0x0681) == hex("08ee00000006810e00000101008d"), n + QStringLiteral(": outdoor NC packet (set_sound_modes.rs)"));
    h.backend->apply(change(R"({"choices":{"noiseCancellingMode":"transport"}})"));
    h.exchange(0x0681);
    h.backend->setMode(QStringLiteral("off"));
    check(h.exchange(0x0681) == hex("08ee00000006810e00020001008e"), n + QStringLiteral(": normal mode packet (set_sound_modes.rs)"));

    const QDBusError err = h.backend->apply(change(R"({"choices":{"noiseCancellingMode":"indoor"}})"));
    check(err.isValid(), n + QStringLiteral(": scene change refused outside NC"));

    h.backend->apply(change(R"({"equalizer":{"preset":21}})"));
    check(h.exchange(0x0281) == hex("08ee000000028114001500787878645a50503ca4"), n + QStringLiteral(": treble reducer packet (set_equalizer.rs)"));
    h.backend->apply(change(R"({"equalizer":{"preset":0}})"));
    check(h.exchange(0x0281) == hex("08ee00000002811400000078787878787878784d"), n + QStringLiteral(": signature packet (set_equalizer.rs)"));
    check(preset(h.snap()) == 0, n + QStringLiteral(": preset updated locally"));
}

void testLifeQ30Issue141()
{
    const QString n = QStringLiteral("A3028 (a3028 state_update_packet.rs github issue 141)");
    Harness h("Soundcore Life Q30");
    h.feed(inbound(0x0101, bytes({5, 0, 14, 0, 120, 150, 150, 140, 160, 170, 150, 160, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 48, 52, 46, 51, 51, 51, 48, 50, 56, 54, 68, 67, 56, 57, 51, 52, 52, 52, 55, 57, 56, 0, 0, 1, 1, 1, 1, 1})));
    commonChecks(h, n, "Soundcore Life Q30");
    const Snapshot s = h.snap();
    check(s.mode == QLatin1String("noise-cancelling"), n + QStringLiteral(": NC mode"));
    check(preset(s) == 14, n + QStringLiteral(": piano preset"));
}

void testLifeQ35Synthetic()
{
    const QString n = QStringLiteral("A3027 (synthetic, a3027 state_update_packet.rs layout)");
    Harness h("Soundcore Life Q35");
    QByteArray body = bytes({4, 1, 1, 0});
    body += QByteArray(8, char(120));
    body += bytes({0, 0});
    body += QByteArray(21, '\0');
    body += bytes({1, 0, 1, 0});
    body += QByteArray("04.33") + QByteArray("3027ABCDEF012345");
    body += bytes({1, 0});
    h.feed(inbound(0x0101, body));
    commonChecks(h, n, "Soundcore Life Q35");
    const Snapshot s = h.snap();
    check(level(s, "main") == 80 && charging(s, "main"), n + QStringLiteral(": battery 4/5 charging"));
    check(s.mode == QLatin1String("ambient"), n + QStringLiteral(": transparency"));
    check(toggle(s, "wearDetection") == 1, n + QStringLiteral(": wear detection on"));
    check(!s.settings.contains(QStringLiteral("choices")), n + QStringLiteral(": no transparency choice on Q35"));
    h.backend->apply(change(R"({"toggles":{"wearDetection":false}})"));
    check(h.exchange(0x0181) == hex("08ee00000001810b000083"), n + QStringLiteral(": wear detection off packet"));
    check(toggle(h.snap(), "wearDetection") == 0, n + QStringLiteral(": wear detection updated"));
}

const char *SpaceQ45State =
    "03ff30322e3334333034303141314132464343454545381100968c6e6e8296a0aa78007a7a7677797a7a7e780004040707005000010005ff0000ff00310001ff000100005a000101"
    "0100113201018f8f808a96968f8f3c3c8f8f808a96968f8f3c3c64988ccd008f8f808a96968f8f3c3c000000000000000000007a7a77797a7a797b7200110000000000000000";

void testSpaceQ45()
{
    const QString n = QStringLiteral("A3040 (a3040.rs test_with_known_good_packet)");
    Harness h("Soundcore Space Q45");
    h.feed(inbound(0x0101, hex(SpaceQ45State)));
    commonChecks(h, n, "Soundcore Space Q45");
    Snapshot s = h.snap();
    check(level(s, "main") == 60, n + QStringLiteral(": battery 3/5 (got %1)").arg(level(s, "main")));
    check(s.mode == QLatin1String("adaptive"), n + QStringLiteral(": adaptive NC (got %1)").arg(s.mode));
    check(s.modes.contains(QStringLiteral("adaptive")), n + QStringLiteral(": adaptive mode offered"));
    check(preset(s) == 17, n + QStringLiteral(": rock preset"));
    check(toggle(s, "multipoint") == 1, n + QStringLiteral(": dual connections on"));
    check(toggle(s, "voicePrompts") == 1, n + QStringLiteral(": voice prompts on"));
    bool hasBassBooster = false;
    for (const QJsonValue &p : s.settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("presets")).toArray())
        hasBassBooster |= p.toObject().value(QStringLiteral("key")).toString() == QLatin1String("bass-booster");
    check(!hasBassBooster, n + QStringLiteral(": no bass booster preset"));

    h.backend->setMode(QStringLiteral("off"));
    check(h.exchange(0x0681) == SoundcoreBackend::frame(0x0681, bytes({2, 80, 0, 1, 0, 5})), n + QStringLiteral(": normal mode body (set_ambient_sound_mode)"));

    h.backend->setMode(QStringLiteral("noise-cancelling"));
    h.backend->tick();
    check(h.sent() == SoundcoreBackend::frame(0x0681, bytes({0, 80, 0, 1, 0, 5})), n + QStringLiteral(": NC migration step 1"));
    h.backend->tick();
    check(h.sent().isEmpty(), n + QStringLiteral(": step 2 waits for the ack"));
    h.feed(inbound(0x0681, {}));
    check(h.exchange(0x0681) == SoundcoreBackend::frame(0x0681, bytes({0, 80, 0, 0, 0, 5})), n + QStringLiteral(": NC migration step 2"));
    s = h.snap();
    check(control(s, "sliders", "ancStrength").toInt() == 5, n + QStringLiteral(": NC strength 5"));
    h.backend->apply(change(R"({"sliders":{"ancStrength":2}})"));
    check(h.exchange(0x0681) == SoundcoreBackend::frame(0x0681, bytes({0, 0x20, 0, 0, 0, 5})), n + QStringLiteral(": NC strength 2"));

    h.backend->apply(change(R"({"equalizer":{"preset":0}})"));
    const QByteArray signature = h.exchange(0x0387);
    check(signature ==SoundcoreBackend::frame(0x0387, bytes({0, 0, 17, 0, 120, 120, 120, 120, 120, 120, 120, 120, 120, 0, 120, 120, 120, 120, 120, 120, 120, 120, 120, 0, 0, 1, 0,
                                                                       143, 143, 128, 138, 150, 150, 143, 143, 60, 60, 143, 143, 128, 138, 150, 150, 143, 143, 60, 60, 100, 152, 140, 205, 0,
                                                                       143, 143, 128, 138, 150, 150, 143, 143, 60, 60, 143, 143, 128, 138, 150, 150, 143, 143, 60, 60, 120, 120, 120, 120, 120,
                                                                       120, 120, 120, 120, 0, 120, 120, 120, 120, 120, 120, 120, 120, 120, 0, 0})),
          n + QStringLiteral(": signature preset packet (set_equalizer_soundcore_signature)"));
    h.backend->apply(change(R"({"equalizer":{"bands":[6,0,0,0,0,0,0,-6]}})"));
    check(h.exchange(0x0387) == SoundcoreBackend::frame(0x0387, bytes({254, 254, 17, 0, 180, 120, 120, 120, 120, 120, 120, 60, 120, 120, 180, 120, 120, 120, 120, 120, 120, 60, 120, 120, 0, 1, 0,
                                                                       143, 143, 128, 138, 150, 150, 143, 143, 60, 60, 143, 143, 128, 138, 150, 150, 143, 143, 60, 60, 100, 152, 140, 205, 0,
                                                                       143, 143, 128, 138, 150, 150, 143, 143, 60, 60, 143, 143, 128, 138, 150, 150, 143, 143, 60, 60, 128, 116, 121, 119, 121,
                                                                       119, 124, 111, 120, 0, 128, 116, 121, 119, 121, 119, 124, 111, 120, 0, 0})),
          n + QStringLiteral(": custom equalizer packet with DRC (set_equalizer_custom)"));
    check(preset(h.snap()) == 0xFEFE && bandsText(h.snap()) == QLatin1String("[6,0,0,0,0,0,0,-6]"), n + QStringLiteral(": custom bands stored"));
}

void testLifeP3()
{
    const QString n = QStringLiteral("A3939 (a3933 state_update_packet.rs it_parses_packet)");
    Harness h("Soundcore");
    h.feed(hex("09ff00000101018e0001010404000030322e363130322e363133393339324137464343324631324143000078787878787878787878ffffffffffffffffffffffffffffffffff"
               "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff016301520166015401010100070000000affff00ff00000033ffffffff66"));
    commonChecks(h, n, "Soundcore Life P3");
    Snapshot s = h.snap();
    check(level(s, "left") == 80 && level(s, "right") == 80 && level(s, "case") == 0, n + QStringLiteral(": batteries 4/5, case 0"));
    check(s.mode == QLatin1String("noise-cancelling"), n + QStringLiteral(": NC mode"));
    check(control(s, "choices", "noiseCancellingMode").toString() == QLatin1String("transport"), n + QStringLiteral(": transport scene"));
    check(toggle(s, "touchTone") == 0 && toggle(s, "wearDetection") == 0 && toggle(s, "lowLatency") == 0, n + QStringLiteral(": flags"));
    check(preset(s) == 0 && bandsText(s) == QLatin1String("[0,0,0,0,0,0,0,0]"), n + QStringLiteral(": signature preset"));

    h.backend->apply(change(R"({"equalizer":{"preset":21}})"));
    check(h.exchange(0x0281) == hex("08ee000000028120001500787878645a50503c7800787878645a50503c7800a2"),
          n + QStringLiteral(": two-channel ten-band treble reducer (a3933.rs it_resets_eq_band_9_and_10_values)"));
    h.backend->setMode(QStringLiteral("ambient"));
    h.exchange(0x0681);
    s = h.snap();
    check(control(s, "choices", "transparencyMode").toString() == QLatin1String("full"), n + QStringLiteral(": transparency choice"));
    h.backend->apply(change(R"({"choices":{"transparencyMode":"vocal"}})"));
    check(h.exchange(0x0681) == SoundcoreBackend::frame(0x0681, bytes({1, 0, 1, 10})), n + QStringLiteral(": vocal transparency"));

    h.feed(inbound(0x0102, bytes({1, 0})));
    s = h.snap();
    check(level(s, "left") < 0 && level(s, "right") == 80, n + QStringLiteral(": only the host earbud while TWS is disconnected"));
    h.feed(inbound(0x0103, bytes({3, 4})));
    h.feed(inbound(0x0102, bytes({1, 1})));
    check(level(h.snap(), "left") == 60, n + QStringLiteral(": dual battery update (battery_level.rs)"));
}

void testLifeNote3S()
{
    const QString n = QStringLiteral("A3945 (a3945.rs it_parses_settings_correctly)");
    Harness h("Soundcore Life Note 3S");
    h.feed(inbound(0x0101, bytes({0, 1, 4, 5, 0, 0, 48, 48, 46, 48, 48, 48, 48, 46, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 0, 0, 120, 120, 120,
                                   120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 1, 0x66, 1, 0x55, 1, 0x33, 1, 0x22, 1, 0x1, 1, 0x0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0})));
    commonChecks(h, n, "Soundcore Life Note 3S");
    const Snapshot s = h.snap();
    check(level(s, "left") == 80 && level(s, "right") == 100, n + QStringLiteral(": batteries"));
    check(s.modes.isEmpty() && s.mode.isEmpty(), n + QStringLiteral(": no sound modes"));
    check(toggle(s, "touchTone") == 0 && toggle(s, "wearDetection") == 0 && toggle(s, "lowLatency") == 0, n + QStringLiteral(": flags"));
    check(h.backend->setMode(QStringLiteral("ambient")).isValid(), n + QStringLiteral(": setMode refused"));
    h.backend->apply(change(R"({"toggles":{"lowLatency":true}})"));
    check(h.exchange(0x0187) == SoundcoreBackend::frame(0x0187, bytes({1})), n + QStringLiteral(": gaming mode packet"));
}

void testLifeDot2NcSynthetic()
{
    const QString n = QStringLiteral("A3931 (synthetic, a3931 state_update_packet.rs layout)");
    Harness h("Soundcore Life Dot 2 NC");
    QByteArray body = bytes({1, 1, 3, 5, 1, 0, 0x15, 0});
    body += QByteArray(16, char(120));
    body += QByteArray(12, '\0');
    body += bytes({0, 2, 0, 0, 0, 1, 0, 0});
    h.feed(inbound(0x0101, body));
    commonChecks(h, n, "Soundcore Life Dot 2 NC");
    const Snapshot s = h.snap();
    check(level(s, "left") == 60 && charging(s, "left") && level(s, "right") == 100, n + QStringLiteral(": batteries"));
    check(control(s, "choices", "noiseCancellingMode").toString() == QLatin1String("indoor"), n + QStringLiteral(": indoor scene"));
    check(toggle(s, "touchTone") == 1, n + QStringLiteral(": touch tone"));
    h.backend->apply(change(R"({"equalizer":{"preset":21}})"));
    const QByteArray out = h.exchange(0x0283);
    const QByteArray plain = hex("1500787878645a50503c787878645a50503c");
    check(out.size() == 9 + 34 + 1 && out.mid(5, 2) == hex("0283") && out.mid(9, plain.size()) == plain,
          n + QStringLiteral(": DRC equalizer packet starts with the plain configuration (set_equalizer.rs it_sends_second_channel_if_present)"));
}

void testSpaceA40()
{
    const QString n = QStringLiteral("A3936 (a3936.rs it_parses_settings_correctly)");
    Harness h("Soundcore Space A40");
    h.feed(inbound(0x0101, bytes({1, 1, 5, 3, 1, 1, 48, 52, 46, 49, 57, 48, 52, 46, 49, 57, 51, 57, 51, 54, 97, 52, 55, 55, 53, 56, 51, 100, 100, 97, 57, 101, 0, 0, 120,
                                   120, 120, 120, 120, 120, 120, 120, 120, 0, 120, 120, 120, 120, 120, 120, 120, 120, 120, 0, 255, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 255,
                                   255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                   0, 0, 0, 14, 0, 17, 0, 0, 17, 99, 17, 102, 17, 68, 17, 73, 7, 2, 0x30, 0, 1, 0, 0, 0, 8, 49, 0, 1, 1, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                   255, 255, 255})));
    commonChecks(h, n, "Soundcore Space A40");
    Snapshot s = h.snap();
    check(level(s, "left") == 100 && charging(s, "left") && level(s, "right") == 60 && charging(s, "right"), n + QStringLiteral(": batteries"));
    check(level(s, "case") == 80, n + QStringLiteral(": case 8/10 (got %1)").arg(level(s, "case")));
    check(s.mode == QLatin1String("off"), n + QStringLiteral(": normal mode"));
    check(toggle(s, "touchTone") == 0 && toggle(s, "lowLatency") == 0 && toggle(s, "multipoint") == 1, n + QStringLiteral(": flags"));
    check(preset(s) == 0, n + QStringLiteral(": signature preset"));

    h.backend->setMode(QStringLiteral("noise-cancelling"));
    check(h.exchange(0x0681) == SoundcoreBackend::frame(0x0681, bytes({0, 0x30, 0, 0, 0, 0})), n + QStringLiteral(": manual NC in one packet"));
    s = h.snap();
    check(control(s, "choices", "ancStrength").toString() == QLatin1String("high"), n + QStringLiteral(": strong NC"));
    h.backend->apply(change(R"({"choices":{"ancStrength":"low"}})"));
    check(h.exchange(0x0681) == SoundcoreBackend::frame(0x0681, bytes({0, 0x10, 0, 0, 0, 0})), n + QStringLiteral(": weak NC"));

    h.backend->apply(change(R"({"equalizer":{"preset":21}})"));
    const QByteArray out = h.exchange(0x0387);
    const QByteArray body = out.mid(9, out.size() - 10);
    const QByteArray channel = hex("787878645a50503c7800");
    const QByteArray absent(20, char(0xFF));
    check(out.mid(5, 2) == hex("0387") && body.size() == 2 + 2 + 20 + 3 + 20 + 4 + 1 + 20 + 20, n + QStringLiteral(": hear ID equalizer packet size"));
    check(body.left(4) == hex("15000000") && body.mid(4, 20) == channel + channel && body.mid(24, 3) == hex("00ff00"),
          n + QStringLiteral(": preset, genre, channels, gender and age range"));
    check(body.mid(27, 20) == absent && body.mid(47, 5) == QByteArray(5, '\0') && body.mid(52, 20) == absent && body.mid(72, 10) == body.mid(82, 10),
          n + QStringLiteral(": absent hear ID curves and DRC for both channels"));
}

const int Liberty4NcState[] = {
    1, 1, 2, 4, 0, 0, 48, 53, 46, 56, 56, 48, 53, 46, 56, 56, 51, 57, 52, 55, 66, 54, 54, 50, 70, 68, 67, 67, 69, 69, 69, 56, 48, 48, 46, 48, 48, 254, 254, 120, 120, 120, 120,
    120, 120, 120, 120, 120, 120, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 1, 112, 117, 140, 148, 150, 142, 134, 131, 60, 60, 112, 117, 140, 148, 150, 142, 134, 131,
    60, 60, 104, 100, 34, 64, 0, 112, 117, 140, 148, 150, 142, 134, 131, 60, 60, 112, 117, 140, 148, 150, 142, 134, 131, 60, 60, 0, 0, 18, 17, 102, 17, 101, 17, 50, 17, 51,
    17, 68, 17, 68, 0, 33, 0, 32, 0x35, 0, 0x50, 1, 0, 1, 1, 3, 0, 0, 0, 0, 255, 1, 2, 49, 1, 0, 0, 1, 0, 0, 1, 0, 1, 80, 1, 0, 0, 1, 1, 1, 1, 255,
};

QByteArray liberty4NcState()
{
    QByteArray out;
    for (const int v : Liberty4NcState)
        out.append(char(v));
    return out;
}

void testLiberty4Nc()
{
    const QString n = QStringLiteral("A3947 (a3947.rs test_with_known_good_packet)");
    Harness h("Soundcore Liberty 4 NC");
    h.feed(inbound(0x0101, liberty4NcState()));
    commonChecks(h, n, "Soundcore Liberty 4 NC");
    Snapshot s = h.snap();
    check(level(s, "left") == 40 && level(s, "right") == 80 && level(s, "case") == 40, n + QStringLiteral(": batteries 2/5, 4/5, case 2/5"));
    check(s.mode == QLatin1String("noise-cancelling"), n + QStringLiteral(": manual NC"));
    check(control(s, "sliders", "ancStrength").toInt() == 5, n + QStringLiteral(": NC strength 5"));
    check(toggle(s, "touchTone") == 1 && toggle(s, "lowLatency") == 0, n + QStringLiteral(": touch tone on, gaming off"));
    check(toggle(s, "surroundSound") == 0 && toggle(s, "earDetection") == 0 && toggle(s, "touchControls") == 0,
          n + QStringLiteral(": surround off, auto play/pause off, touch locked"));
    check(preset(s) == 0xFEFE, n + QStringLiteral(": custom equalizer"));

    h.backend->setMode(QStringLiteral("off"));
    check(h.exchange(0x0681) == SoundcoreBackend::frame(0x0681, bytes({2, 80, 1, 0, 1, 1, 3})), n + QStringLiteral(": normal mode body (test_set_sound_mode_normal)"));
    h.backend->apply(change(R"({"toggles":{"lowLatency":true}})"));
    check(h.exchange(0x1085) == SoundcoreBackend::frame(0x1085, bytes({1})), n + QStringLiteral(": gaming mode packet (set_gaming_mode)"));
    h.backend->apply(change(R"({"toggles":{"touchControls":true}})"));
    check(h.exchange(0x0486) == SoundcoreBackend::frame(0x0486, bytes({0})), n + QStringLiteral(": touch lock off packet (set_touch_lock)"));
    h.backend->apply(change(R"({"toggles":{"surroundSound":true}})"));
    check(h.exchange(0x0286) == SoundcoreBackend::frame(0x0286, bytes({1})), n + QStringLiteral(": surround sound packet (set_surround_sound)"));

    QByteArray modified = liberty4NcState();
    modified[85] = char(2);
    modified[106] = char(6);
    Harness g("Soundcore Liberty 4 NC");
    g.feed(inbound(0x0101, modified));
    g.backend->apply(change(R"({"equalizer":{"bands":[6,0,0,0,0,0,0,-6]}})"));
    const QByteArray expected = bytes({8, 238, 0, 0, 0, 3, 135, 103, 0, 254, 254, 6, 0, 180, 120, 120, 120, 120, 120, 120, 60, 120, 120, 180, 120, 120, 120, 120, 120, 120, 60, 120, 120,
                                       0, 0, 0, 112, 117, 140, 148, 150, 142, 134, 131, 60, 60, 112, 117, 140, 148, 150, 142, 134, 131, 60, 60, 104, 100, 34, 64, 2, 112, 117, 140, 148, 150, 142,
                                       134, 131, 60, 60, 112, 117, 140, 148, 150, 142, 134, 131, 60, 60, 128, 116, 121, 119, 121, 119, 124, 111, 120, 0, 128, 116, 121, 119, 121, 119, 124, 111,
                                       120, 0, 0, 143});
    const QByteArray out = g.exchange(0x0387);
    check(out == expected, n + QStringLiteral(": equalizer with hear ID off (set_equalizer_configuration.rs matches_known_good_packet)"));
}

void testP30i()
{
    const QString n = QStringLiteral("A3959 (a3959.rs github issue 149)");
    Harness h("soundcore P30i");
    h.feed(inbound(0x0101, bytes({1, 1, 5, 6, 255, 255, 48, 49, 46, 54, 52, 48, 49, 46, 54, 52, 51, 57, 53, 57, 68, 69, 68, 54, 54, 57, 50, 68, 66, 54, 70, 52, 254, 254, 101,
                                   120, 161, 171, 171, 152, 144, 179, 120, 120, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 241, 240, 102, 102, 242, 243, 68, 68, 51, 0, 85, 0, 0, 1, 255, 1, 49, 1,
                                   1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})));
    commonChecks(h, n, "Soundcore P30i");
    Snapshot s = h.snap();
    check(level(s, "left") == 50 && level(s, "right") == 60 && !charging(s, "left") && !charging(s, "right"), n + QStringLiteral(": batteries 5/10, 6/10"));
    check(s.mode == QLatin1String("noise-cancelling"), n + QStringLiteral(": manual NC"));
    check(control(s, "sliders", "ancStrength").toInt() == 5, n + QStringLiteral(": NC strength 5"));
    check(toggle(s, "touchTone") == 1 && toggle(s, "multipoint") == 1 && toggle(s, "surroundSound") == 0 && toggle(s, "lowLatency") == 0, n + QStringLiteral(": flags"));
    check(bandsText(s) == QLatin1String("[-2,0,4,5,5,3,2,6]"), n + QStringLiteral(": custom bands (got %1)").arg(bandsText(s)));

    h.backend->apply(change(R"({"sliders":{"ancStrength":2}})"));
    check(h.exchange(0x0681) == SoundcoreBackend::frame(0x0681, bytes({0, 37, 0, 0, 1, 255, 1})), n + QStringLiteral(": NC strength 2 (set_manual_noise_canceling)"));
    h.backend->setMode(QStringLiteral("ambient"));
    check(h.exchange(0x0681) == SoundcoreBackend::frame(0x0681, bytes({1, 37, 1, 0, 1, 255, 1})), n + QStringLiteral(": ambient mode writes both mode bytes"));
    check(h.backend->apply(change(R"({"sliders":{"ancStrength":3}})")).isValid(), n + QStringLiteral(": NC strength refused outside NC"));
    h.backend->apply(change(R"({"equalizer":{"preset":0}})"));
    const QByteArray out = h.exchange(0x0283);
    check(out.mid(9, out.size() - 10) == bytes({0, 0, 120, 120, 120, 120, 120, 120, 120, 120, 120, 0, 120, 120, 120, 120, 120, 120, 120, 120, 120, 0}),
          n + QStringLiteral(": one-channel DRC signature packet"));
}

void testLibertyAir2ProSynthetic()
{
    const QString n = QStringLiteral("A3951 (synthetic, a3951 state_update_packet.rs layout)");
    Harness h("Soundcore Liberty Air 2 Pro");
    QByteArray body = bytes({0, 1, 5, 4, 0, 1, 0, 0});
    body += QByteArray(16, char(120));
    body += bytes({1, 255, 1});
    body += QByteArray(16, char(100));
    body += bytes({0, 0, 0, 0, 0, 0});
    body += QByteArray(16, char(110));
    body += QByteArray(12, '\0');
    body += bytes({1, 0, 1, 0, 0, 1, 0});
    h.feed(inbound(0x0101, body));
    commonChecks(h, n, "Soundcore Liberty Air 2 Pro");
    const Snapshot s = h.snap();
    check(level(s, "left") == 100 && level(s, "right") == 80 && charging(s, "right"), n + QStringLiteral(": batteries"));
    check(s.mode == QLatin1String("ambient") && control(s, "choices", "transparencyMode").toString() == QLatin1String("vocal"), n + QStringLiteral(": vocal transparency"));
    check(toggle(s, "wearDetection") == 1 && toggle(s, "touchTone") == 0, n + QStringLiteral(": flags"));
    h.backend->apply(change(R"({"equalizer":{"preset":0}})"));
    const QByteArray expected = bytes({0x08, 0xee, 0x00, 0x00, 0x00, 0x03, 0x86, 0x54, 0x00, 0x00, 0x00, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78,
                                       0x78, 0x78, 0x78, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x78, 0x78, 0x78, 0x78, 0x78,
                                       0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0xb1});
    check(h.exchange(0x0386) == expected, n + QStringLiteral(": equalizer without hear ID (set_equalizer_and_custom_hear_id.rs it_matches_a_generated_packet_with_no_hear_id)"));
}

void testFraming()
{
    const QString n = QStringLiteral("framing");
    Harness h("Soundcore Life Q30");
    QByteArray good = inbound(0x0101, bytes({5, 0, 14, 0, 120, 150, 150, 140, 160, 170, 150, 160, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                             0, 0, 0, 0, 0, 48, 52, 46, 51, 51, 51, 48, 50, 56, 54, 68, 67, 56, 57, 51, 52, 52, 52, 55, 57, 56}));
    QByteArray bad = good;
    bad[bad.size() - 1] = char(bad.at(bad.size() - 1) + 1);
    h.feed(hex("0102") + bad);
    check(!h.backend->ready(), n + QStringLiteral(": bad checksum ignored"));
    h.feed(good.left(20));
    check(!h.backend->ready(), n + QStringLiteral(": partial frame waits"));
    h.feed(good.mid(20));
    check(h.backend->ready(), n + QStringLiteral(": split frame reassembled"));

    Harness u("Some Headphones");
    u.feed(inbound(0x0101, bytes({1, 1, 1})));
    check(!u.backend->ready() && u.backend->errorString().contains(QStringLiteral("unsupported")), n + QStringLiteral(": unknown model is reported"));
    check(SoundcoreBackend::familyFromName(QStringLiteral("soundcore Life Note 3S")) == SoundcoreFamily::LifeNote3S
              && SoundcoreBackend::familyFromName(QStringLiteral("Soundcore Life Note 3")) == SoundcoreFamily::LifeNote3
              && SoundcoreBackend::familyFromName(QStringLiteral("Soundcore Life Tune Pro")) == SoundcoreFamily::LifeQ35
              && SoundcoreBackend::familyFromName(QStringLiteral("Galaxy Buds2")) == SoundcoreFamily::Unknown,
          n + QStringLiteral(": names"));
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testLifeTune();
    testLifeQ30Issue141();
    testLifeQ35Synthetic();
    testSpaceQ45();
    testLifeP3();
    testLifeNote3S();
    testLifeDot2NcSynthetic();
    testSpaceA40();
    testLiberty4Nc();
    testP30i();
    testLibertyAir2ProSynthetic();
    testFraming();
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
