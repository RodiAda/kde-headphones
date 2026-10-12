#include <QCoreApplication>
#include <QJsonDocument>
#include <QMap>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

#include "BoseBackend.h"

namespace {

struct FakeDevice {
    QMap<QString, QByteArray> responses;
    QByteArray incoming;
    QList<QByteArray> sent;
    bool trickle = false;
    bool gap = false;
};

QString key(int block, int function, int op)
{
    return QStringLiteral("%1.%2.%3").arg(block).arg(function).arg(op);
}

QByteArray frame(int block, int function, int op, const QByteArray &payload = {})
{
    return BoseBackend::packet(uint8_t(block), uint8_t(function), uint8_t(op), payload);
}

QByteArray hex(const char *s)
{
    return QByteArray::fromHex(s);
}

MDRResult fakeRecv(void *user, char *dst, int size, int *received)
{
    auto *dev = static_cast<FakeDevice *>(user);
    if (dev->incoming.isEmpty())
        return MDR_RESULT_INPROGRESS;
    if (dev->trickle) {
        dev->gap = !dev->gap;
        if (dev->gap)
            return MDR_RESULT_INPROGRESS;
    }
    const int n = std::min<int>(dev->trickle ? 1 : size, int(dev->incoming.size()));
    std::memcpy(dst, dev->incoming.constData(), n);
    dev->incoming.remove(0, n);
    *received = n;
    return MDR_RESULT_OK;
}

MDRResult fakeSend(void *user, const char *src, int size, int *sent)
{
    auto *dev = static_cast<FakeDevice *>(user);
    QByteArray data(src, size);
    while (data.size() >= 4) {
        const int length = uint8_t(data.at(3));
        const QByteArray f = data.left(4 + length);
        data.remove(0, 4 + length);
        dev->sent.append(f);
        const int block = uint8_t(f.at(0));
        const int function = uint8_t(f.at(1));
        const int op = uint8_t(f.at(2)) & 0x0F;
        const QString k = key(block, function, op);
        if (dev->responses.contains(k))
            dev->incoming.append(dev->responses.value(k));
        else if (op == 1)
            dev->incoming.append(frame(block, function, 4, QByteArray(1, char(4))));
        else if (op == 2)
            dev->incoming.append(frame(block, function, 3));
        else
            dev->incoming.append(frame(block, function, 6));
    }
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

void status(FakeDevice &dev, int block, int function, const QByteArray &payload)
{
    dev.responses.insert(key(block, function, 1), frame(block, function, 3, payload));
}

QByteArray modeConfig(int index, int prompt, bool editable, bool configured, const char *name, int size)
{
    QByteArray p(size, '\0');
    p[0] = char(index);
    p[1] = char(prompt >> 8);
    p[2] = char(prompt & 0xFF);
    p[3] = char(editable);
    p[4] = char(configured);
    const QByteArray n(name);
    for (int i = 0; i < n.size(); i++)
        p[6 + i] = n.at(i);
    return frame(31, 6, 3, p);
}

void getAll(FakeDevice &dev, const QList<QByteArray> &modes)
{
    QByteArray r = frame(31, 1, 7);
    for (const QByteArray &m : modes)
        r.append(m);
    r.append(frame(31, 1, 6));
    dev.responses.insert(key(31, 1, 5), r);
}

struct Harness {
    FakeDevice dev;
    MDRConnection conn{};
    std::unique_ptr<BoseBackend> backend;

    void open(const char *name)
    {
        conn.user = &dev;
        conn.recv = fakeRecv;
        conn.send = fakeSend;
        conn.getLastError = fakeError;
        backend = std::make_unique<BoseBackend>(&conn, DeviceInfo{QString::fromLatin1(name), QStringLiteral("00:11:22:33:44:55"), {}, DeviceKind::Bose});
        backend->start();
        for (int i = 0; i < 20000 && !backend->ready(); i++)
            backend->tick();
    }

    QList<QByteArray> run(const std::function<QDBusError()> &command, QString *error = nullptr)
    {
        dev.sent.clear();
        const QDBusError e = command();
        if (error)
            *error = e.isValid() ? e.message() : QString();
        for (int i = 0; i < 200; i++)
            backend->tick();
        return dev.sent;
    }
};

QJsonObject find(const QJsonObject &settings, const char *group, const char *k)
{
    for (const QJsonValue &v : settings.value(QLatin1String(group)).toArray()) {
        if (v.toObject().value(QStringLiteral("key")).toString() == QLatin1String(k))
            return v.toObject();
    }
    return {};
}

int level(const QJsonArray &batteries, const char *part)
{
    for (const QJsonValue &b : batteries) {
        if (b.toObject().value(QStringLiteral("part")).toString() == QLatin1String(part))
            return b.toObject().value(QStringLiteral("level")).toInt();
    }
    return -1;
}

QStringList strings(const QJsonValue &v)
{
    QStringList out;
    for (const QJsonValue &s : v.toArray())
        out.append(s.toString());
    return out;
}

void expectSent(const QList<QByteArray> &sent, const QList<QByteArray> &expected, const QString &what)
{
    QStringList got;
    for (const QByteArray &f : sent)
        got.append(QString::fromLatin1(f.toHex(' ')));
    bool ok = true;
    for (const QByteArray &e : expected) {
        if (!sent.contains(e))
            ok = false;
    }
    check(ok, what + QStringLiteral(" (sent: %1)").arg(got.join(QStringLiteral(" | "))));
}

void testUltra2(bool trickle)
{
    const QString n = trickle ? QStringLiteral("QC Ultra 2 (byte by byte)") : QStringLiteral("QC Ultra 2");
    Harness h;
    h.dev.trickle = trickle;
    status(h.dev, 0, 1, hex("312e322e30"));
    status(h.dev, 0, 3, hex("408201"));
    status(h.dev, 2, 2, hex("50ffff00"));
    status(h.dev, 1, 3, hex("41000081020000"));
    status(h.dev, 1, 5, hex("0b0003"));
    status(h.dev, 1, 7, hex("f60a0000f60afe01f60afa02"));
    status(h.dev, 1, 10, hex("07"));
    status(h.dev, 1, 11, hex("01020f"));
    status(h.dev, 1, 24, hex("01"));
    status(h.dev, 1, 27, hex("01"));
    status(h.dev, 31, 3, hex("00"));
    status(h.dev, 31, 10, hex("0300010001"));
    getAll(h.dev, {
        modeConfig(0, 0x0001, false, false, "Quiet", 48),
        modeConfig(1, 0x0002, false, false, "Aware", 48),
        modeConfig(2, 0x0022, false, false, "Immersion", 48),
        modeConfig(3, 0x0024, false, false, "Cinema", 48),
        modeConfig(4, 0x000a, true, true, "Home", 48),
        modeConfig(5, 0x0000, true, false, "None", 48),
    });
    h.open("Fargo");
    check(h.backend->ready(), n + QStringLiteral(": ready"));
    if (!h.backend->ready())
        return;

    const Snapshot s = h.backend->snapshot();
    const QJsonObject o = s.settings;
    check(o.value(QStringLiteral("model")).toString() == QLatin1String("Bose QuietComfort Ultra Headphones (2nd Gen)"),
          n + QStringLiteral(": model from product id (got %1)").arg(o.value(QStringLiteral("model")).toString()));
    check(o.value(QStringLiteral("untested")).toBool(), n + QStringLiteral(": untested flag"));
    check(level(s.batteries, "main") == 80 && s.battery == 80, n + QStringLiteral(": battery 80"));
    check(s.modes == QStringList{QStringLiteral("noise-cancelling"), QStringLiteral("ambient"), QStringLiteral("off")},
          n + QStringLiteral(": modes (got %1)").arg(s.modes.join(QLatin1Char(','))));
    check(s.mode == QLatin1String("noise-cancelling"), n + QStringLiteral(": mode quiet (got %1)").arg(s.mode));
    check(find(o, "toggles", "voicePrompts").value(QStringLiteral("value")) == false, n + QStringLiteral(": voice prompts off"));
    check(find(o, "toggles", "multipoint").value(QStringLiteral("value")) == true, n + QStringLiteral(": multipoint on"));
    check(find(o, "toggles", "wearDetection").value(QStringLiteral("value")) == true, n + QStringLiteral(": wear detection on"));
    check(find(o, "toggles", "autoAnswer").value(QStringLiteral("value")) == true, n + QStringLiteral(": auto answer on"));
    check(find(o, "toggles", "windBlock").value(QStringLiteral("value")) == false, n + QStringLiteral(": wind block off"));
    const QJsonObject anc = find(o, "sliders", "ancStrength");
    check(anc.value(QStringLiteral("value")).toInt() == 7 && anc.value(QStringLiteral("max")).toInt() == 10,
          n + QStringLiteral(": anc strength 7 of 10"));
    check(find(o, "sliders", "eqBass").value(QStringLiteral("value")).toInt() == 0, n + QStringLiteral(": eq bass 0"));
    check(find(o, "sliders", "eqMid").value(QStringLiteral("value")).toInt() == -2, n + QStringLiteral(": eq mid -2"));
    const QJsonObject treble = find(o, "sliders", "eqTreble");
    check(treble.value(QStringLiteral("value")).toInt() == -6 && treble.value(QStringLiteral("min")).toInt() == -10
              && treble.value(QStringLiteral("max")).toInt() == 10,
          n + QStringLiteral(": eq treble -6 in -10..10"));
    check(find(o, "choices", "spatialAudio").value(QStringLiteral("value")).toString() == QLatin1String("still"), n + QStringLiteral(": spatial still"));
    check(find(o, "choices", "sidetone").value(QStringLiteral("value")).toString() == QLatin1String("medium"), n + QStringLiteral(": sidetone medium"));
    const QJsonObject mode = find(o, "choices", "audioMode");
    check(mode.value(QStringLiteral("value")).toString() == QLatin1String("Quiet")
              && strings(mode.value(QStringLiteral("options")))
                  == QStringList{QStringLiteral("Quiet"), QStringLiteral("Aware"), QStringLiteral("Immersion"), QStringLiteral("Cinema"), QStringLiteral("Home")},
          n + QStringLiteral(": audio modes from the mode dump (got %1)").arg(strings(mode.value(QStringLiteral("options"))).join(QLatin1Char(','))));
    check(find(o, "choices", "autoPowerOff").isEmpty(), n + QStringLiteral(": no auto power off without a status"));
    if (trickle)
        return;

    expectSent(h.run([&] { return h.backend->setMode(QStringLiteral("ambient")); }), {hex("1f03050201 00")}, n + QStringLiteral(": aware via START [31.3]"));
    expectSent(h.run([&] { return h.backend->setMode(QStringLiteral("off")); }), {hex("1f0a02050300010000")}, n + QStringLiteral(": anc off via [31.10]"));
    expectSent(h.run([&] { return h.backend->apply(QJsonObject{{QStringLiteral("sliders"), QJsonObject{{QStringLiteral("ancStrength"), 2}}}}); }),
               {hex("1f0a02050800010001")}, n + QStringLiteral(": anc strength 2 is raw cnc 8"));
    expectSent(h.run([&] { return h.backend->apply(QJsonObject{{QStringLiteral("sliders"), QJsonObject{{QStringLiteral("eqBass"), -3}}}}); }),
               {hex("01070202fd00")}, n + QStringLiteral(": eq bass -3"));
    expectSent(h.run([&] { return h.backend->apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("multipoint"), false}}}}); }),
               {hex("010a020100")}, n + QStringLiteral(": multipoint off"));
    expectSent(h.run([&] { return h.backend->apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("audioMode"), QStringLiteral("Immersion")}}}}); }),
               {hex("1f0305020200")}, n + QStringLiteral(": immersion via START [31.3]"));
    const QList<QByteArray> spatial = h.run([&] {
        return h.backend->apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("spatialAudio"), QStringLiteral("motion")}}}});
    });
    check(!spatial.isEmpty() && spatial.constFirst().left(4) == hex("1f0a0205") && uint8_t(spatial.constFirst().at(6)) == 2,
          n + QStringLiteral(": spatial motion is 2"));
    expectSent(h.run([&] { return h.backend->apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("sidetone"), QStringLiteral("high")}}}}); }),
               {hex("010b02020101")}, n + QStringLiteral(": sidetone high"));
    QString error;
    h.run([&] { return h.backend->setAmbientLevel(3); }, &error);
    check(!error.isEmpty(), n + QStringLiteral(": ambient level unsupported"));
}

void testQc35()
{
    const QString n = QStringLiteral("QC35 II");
    Harness h;
    status(h.dev, 0, 1, hex("312e302e30"));
    status(h.dev, 0, 3, hex("402002"));
    status(h.dev, 2, 2, hex("46"));
    status(h.dev, 1, 3, hex("21000000de"));
    status(h.dev, 1, 4, hex("14"));
    status(h.dev, 1, 6, hex("010b"));
    status(h.dev, 1, 11, hex("01030f"));
    h.dev.responses.insert(key(31, 3, 1), frame(31, 3, 4, hex("03")));
    h.open("Bose QC35 II");
    check(h.backend->ready(), n + QStringLiteral(": ready"));
    if (!h.backend->ready())
        return;

    const Snapshot s = h.backend->snapshot();
    const QJsonObject o = s.settings;
    check(o.value(QStringLiteral("model")).toString() == QLatin1String("Bose QuietComfort 35 II"), n + QStringLiteral(": model"));
    check(level(s.batteries, "main") == 70, n + QStringLiteral(": one-byte battery 70"));
    check(s.modes == QStringList{QStringLiteral("noise-cancelling"), QStringLiteral("off")}, n + QStringLiteral(": modes nc/off"));
    check(s.mode == QLatin1String("noise-cancelling"), n + QStringLiteral(": mode nc"));
    check(find(o, "toggles", "voicePrompts").value(QStringLiteral("value")) == true, n + QStringLiteral(": voice prompts on"));
    check(o.value(QStringLiteral("sliders")).toArray().isEmpty(), n + QStringLiteral(": no sliders"));
    check(find(o, "choices", "ancLevel").value(QStringLiteral("value")).toString() == QLatin1String("high"), n + QStringLiteral(": anr high"));
    check(find(o, "choices", "sidetone").value(QStringLiteral("value")).toString() == QLatin1String("low"), n + QStringLiteral(": sidetone low"));
    const QJsonObject off = find(o, "choices", "autoPowerOff");
    check(off.value(QStringLiteral("value")).toString() == QLatin1String("20-min")
              && strings(off.value(QStringLiteral("options")))
                  == QStringList{QStringLiteral("never"), QStringLiteral("5-min"), QStringLiteral("20-min"), QStringLiteral("40-min"),
                                 QStringLiteral("60-min"), QStringLiteral("180-min")},
          n + QStringLiteral(": auto power off 20 min"));
    check(find(o, "choices", "audioMode").isEmpty(), n + QStringLiteral(": no audio modes block"));

    expectSent(h.run([&] { return h.backend->setMode(QStringLiteral("off")); }), {hex("0106020100")}, n + QStringLiteral(": anr off"));
    expectSent(h.run([&] { return h.backend->setMode(QStringLiteral("noise-cancelling")); }), {hex("0106020101")}, n + QStringLiteral(": anr high"));
    expectSent(h.run([&] { return h.backend->apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("ancLevel"), QStringLiteral("low")}}}}); }),
               {hex("0106020103")}, n + QStringLiteral(": anr low"));
    expectSent(h.run([&] { return h.backend->apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("autoPowerOff"), QStringLiteral("60-min")}}}}); }),
               {hex("010402013c")}, n + QStringLiteral(": auto off 60 min"));
    expectSent(h.run([&] { return h.backend->apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("voicePrompts"), false}}}}); }),
               {hex("0103020101")}, n + QStringLiteral(": voice prompts off keeps the language"));
    QString error;
    h.run([&] { return h.backend->setMode(QStringLiteral("ambient")); }, &error);
    check(!error.isEmpty(), n + QStringLiteral(": ambient rejected"));
}

void testUltraEarbuds2()
{
    const QString n = QStringLiteral("QC Ultra Earbuds 2");
    Harness h;
    status(h.dev, 0, 3, hex("406201"));
    status(h.dev, 2, 2, hex("3cffff013cffff023cffff0450ffff03"));
    status(h.dev, 1, 24, hex("00"));
    status(h.dev, 31, 3, hex("01"));
    status(h.dev, 31, 10, hex("0a00000101"));
    h.open("Bose QC Ultra Earbuds");
    check(h.backend->ready(), n + QStringLiteral(": ready"));
    if (!h.backend->ready())
        return;
    const Snapshot s = h.backend->snapshot();
    const QJsonObject o = s.settings;
    check(o.value(QStringLiteral("model")).toString() == QLatin1String("Bose QuietComfort Ultra Earbuds (2nd Gen)"), n + QStringLiteral(": model"));
    check(level(s.batteries, "left") == 60 && level(s.batteries, "right") == 60 && level(s.batteries, "case") == 80 && s.battery == 60,
          n + QStringLiteral(": left/right/case batteries"));
    check(level(s.batteries, "main") == -1, n + QStringLiteral(": no main battery"));
    check(s.mode == QLatin1String("ambient"), n + QStringLiteral(": mode aware (got %1)").arg(s.mode));
    check(find(o, "toggles", "earDetection").value(QStringLiteral("value")) == false, n + QStringLiteral(": ear detection off"));
    check(find(o, "toggles", "windBlock").value(QStringLiteral("value")) == true, n + QStringLiteral(": wind block on"));
    check(find(o, "sliders", "ancStrength").value(QStringLiteral("value")).toInt() == 0, n + QStringLiteral(": anc strength 0"));
    expectSent(h.run([&] { return h.backend->setMode(QStringLiteral("noise-cancelling")); }), {hex("1f0305020000")}, n + QStringLiteral(": quiet via START [31.3]"));
}

void testNc700()
{
    const QString n = QStringLiteral("NC 700");
    Harness h;
    status(h.dev, 0, 3, hex("402401"));
    status(h.dev, 2, 2, hex("5a"));
    status(h.dev, 1, 4, hex("3c"));
    status(h.dev, 1, 5, hex("0b0301"));
    status(h.dev, 1, 10, hex("07"));
    h.open("Bose NC 700 HP");
    check(h.backend->ready(), n + QStringLiteral(": ready"));
    if (!h.backend->ready())
        return;
    const Snapshot s = h.backend->snapshot();
    const QJsonObject o = s.settings;
    check(s.modes == QStringList{QStringLiteral("noise-cancelling"), QStringLiteral("ambient")}, n + QStringLiteral(": modes nc/ambient"));
    check(s.mode == QLatin1String("noise-cancelling"), n + QStringLiteral(": mode nc"));
    check(find(o, "sliders", "ancStrength").value(QStringLiteral("value")).toInt() == 7, n + QStringLiteral(": anc strength 7 from inverted raw 3"));
    check(strings(find(o, "choices", "autoPowerOff").value(QStringLiteral("options"))).contains(QStringLiteral("10-min")),
          n + QStringLiteral(": NC 700 auto off offers 10 min"));
    const QList<QByteArray> ambient = h.run([&] { return h.backend->setMode(QStringLiteral("ambient")); });
    check(ambient.count(hex("010502020a01")) == 3, n + QStringLiteral(": ambient is raw 10 sent three times"));
    const QList<QByteArray> strength = h.run([&] {
        return h.backend->apply(QJsonObject{{QStringLiteral("sliders"), QJsonObject{{QStringLiteral("ancStrength"), 4}}}});
    });
    check(strength.count(hex("0105020206 01")) == 3, n + QStringLiteral(": strength 4 is raw 6 sent three times"));
}

void testQc45ByName()
{
    const QString n = QStringLiteral("QC45 (name only)");
    Harness h;
    status(h.dev, 2, 2, hex("64ffff00"));
    status(h.dev, 1, 5, hex("0b0003"));
    status(h.dev, 31, 3, hex("02"));
    getAll(h.dev, {
        frame(31, 6, 3, hex("0000010000015175696574000000000000000000000000000000000000000000000000000000000000000000000000")),
        frame(31, 6, 3, hex("0100020000014177617265000000000000000000000000000000000000000000000000000000000000000a00000000")),
        frame(31, 6, 3, hex("02000c0101014d75736963000000000000000000000000000000000000000000000000000000000000090500000000")),
    });
    h.open("Bose QC45");
    check(h.backend->ready(), n + QStringLiteral(": ready"));
    if (!h.backend->ready())
        return;
    const Snapshot s = h.backend->snapshot();
    const QJsonObject o = s.settings;
    check(o.value(QStringLiteral("model")).toString() == QLatin1String("Bose QuietComfort 45"), n + QStringLiteral(": model from name"));
    check(s.modes == QStringList{QStringLiteral("noise-cancelling"), QStringLiteral("ambient")}, n + QStringLiteral(": modes nc/ambient"));
    check(find(o, "sliders", "ancStrength").isEmpty(), n + QStringLiteral(": no anc slider, [1.5] is read only"));
    const QJsonObject mode = find(o, "choices", "audioMode");
    check(mode.value(QStringLiteral("value")).toString() == QLatin1String("Music")
              && strings(mode.value(QStringLiteral("options"))) == QStringList{QStringLiteral("Quiet"), QStringLiteral("Aware"), QStringLiteral("Music")},
          n + QStringLiteral(": 47-byte mode dump"));
    expectSent(h.run([&] { return h.backend->setMode(QStringLiteral("ambient")); }), {hex("1f0305020100")}, n + QStringLiteral(": aware via START [31.3]"));
}

void testNoBattery()
{
    Harness h;
    h.open("Bose QC35");
    check(!h.backend->ready(), QStringLiteral("silent device: not ready"));
}

void testNames()
{
    check(BoseBackend::modelForName(QStringLiteral("Bose QC Earbuds II"))->productId == 0x4064, QStringLiteral("name: QC Earbuds II"));
    check(BoseBackend::modelForName(QStringLiteral("Bose QC Earbuds"))->productId == 0x402F, QStringLiteral("name: QC Earbuds"));
    check(BoseBackend::modelForName(QStringLiteral("Bose QC Ultra Headphones"))->productId == 0x4066, QStringLiteral("name: QC Ultra Headphones"));
    check(BoseBackend::modelForName(QStringLiteral("Bose QC 35 II"))->productId == 0x4020, QStringLiteral("name: QC 35 II"));
    check(BoseBackend::modelForName(QStringLiteral("LE-Bose NC 700 HP"))->productId == 0x4024, QStringLiteral("name: NC 700"));
    check(BoseBackend::modelForName(QStringLiteral("Galaxy Buds2 Pro")) == nullptr, QStringLiteral("name: Galaxy is not Bose"));
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testUltra2(false);
    testUltra2(true);
    testQc35();
    testUltraEarbuds2();
    testNc700();
    testQc45ByName();
    testNames();
    testNoBattery();
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
