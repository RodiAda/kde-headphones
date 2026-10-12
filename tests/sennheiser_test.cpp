#include <QCoreApplication>
#include <QMap>

#include <cstdio>
#include <cstring>
#include <functional>

#include "SennheiserBackend.h"

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

QByteArray hex(const char *text)
{
    return QByteArray::fromHex(QByteArray(text).replace(' ', ""));
}

QByteArray frame(uint16_t command, const QByteArray &payload)
{
    QByteArray f = hex("ff 03 00");
    f.append(char(payload.size()));
    f.append(hex("04 95"));
    f.append(char(command >> 8));
    f.append(char(command & 0xFF));
    f.append(payload);
    return f;
}

struct Sent {
    uint16_t command;
    QByteArray payload;
    QByteArray raw;
};

QList<Sent> decode(const QByteArray &bytes)
{
    QList<Sent> out;
    qsizetype i = 0;
    while (i + 8 <= bytes.size()) {
        const int length = uint8_t(bytes.at(i + 3));
        const QByteArray whole = bytes.mid(i, 8 + length);
        if (uint8_t(whole.at(0)) != 0xFF || uint8_t(whole.at(1)) != 0x03 || whole.at(2) != 0 || whole.mid(4, 2) != hex("0495"))
            return {};
        out.append({uint16_t((uint8_t(whole.at(6)) << 8) | uint8_t(whole.at(7))), whole.mid(8), whole});
        i += whole.size();
    }
    return out;
}

using Key = QPair<uint16_t, QByteArray>;

struct Device {
    QMap<Key, QByteArray> state;
    std::function<void(Device &, uint16_t, const QByteArray &)> sideEffects;

    QByteArray &at(uint16_t command, const QByteArray &payload = {}) { return state[{command, payload}]; }

    QByteArray answer(uint16_t command, const QByteArray &payload)
    {
        if (command == 0x0007)
            return frame(0x0107, {});
        if (state.contains({command, payload}))
            return frame(command | 0x0100, state.value({command, payload}));
        const bool setter = state.contains({uint16_t(command + 1), {}}) || command == 0x1001 || command == 0x0600;
        if (!payload.isEmpty() && setter) {
            if (sideEffects)
                sideEffects(*this, command, payload);
            return frame(command | 0x0100, {});
        }
        return frame(command | 0x0180, hex("01"));
    }
};

void m4SideEffects(Device &d, uint16_t command, const QByteArray &p)
{
    const uint8_t v = p.isEmpty() ? 0 : uint8_t(p.at(0));
    switch (command) {
    case 0x1804:
        d.at(0x1805) = QByteArray(1, char(v));
        d.at(0x1A03) = QByteArray(1, char(v ? 0x64 : 0x00));
        break;
    case 0x1A04:
        d.at(0x1A05) = QByteArray(1, char(v));
        if (v) {
            d.at(0x1A03) = hex("00");
            d.at(0x1805) = hex("00");
        }
        break;
    case 0x1A00: {
        QByteArray &table = d.at(0x1A01);
        for (int i = 0; i + 1 < table.size(); i += 2) {
            if (table.at(i) == p.at(0))
                table[i + 1] = p.at(1);
        }
        d.at(0x1805) = hex("00");
        d.at(0x1A03) = hex("00");
        break;
    }
    case 0x1A02:
        d.at(0x1A03) = p;
        break;
    case 0x0803:
        d.at(0x0804) = p;
        break;
    case 0x0600:
        d.at(0x0601, hex("00")) = p;
        break;
    case 0x1001:
        d.at(0x1003, hex("00"))[uint8_t(p.at(0))] = p.at(1);
        break;
    default:
        d.at(command + 1) = p;
        break;
    }
}

struct Session {
    FakeLink link;
    MDRConnection conn{};
    SennheiserBackend backend;
    Device device;
    QList<Sent> log;

    explicit Session(const char *name)
        : backend(&conn, DeviceInfo{QString::fromLatin1(name), QStringLiteral("80:C3:BA:00:00:00"), {}, DeviceKind::Sennheiser})
    {
        conn.user = &link;
        conn.recv = fakeRecv;
        conn.send = fakeSend;
        conn.getLastError = fakeError;
    }

    void run()
    {
        for (int round = 0; round < 400; round++) {
            backend.tick();
            if (link.outgoing.isEmpty())
                return;
            const QList<Sent> sent = decode(link.outgoing);
            link.outgoing.clear();
            for (const Sent &s : sent) {
                log.append(s);
                link.incoming.append(device.answer(s.command, s.payload));
            }
        }
    }

    void feed(const QByteArray &bytes)
    {
        link.incoming.append(bytes);
        run();
    }

    bool sentRaw(const char *text) const
    {
        const QByteArray want = hex(text);
        return std::any_of(log.begin(), log.end(), [&](const Sent &s) { return s.raw == want; });
    }

    bool sent(uint16_t command) const
    {
        return std::any_of(log.begin(), log.end(), [&](const Sent &s) { return s.command == command; });
    }
};

QJsonObject findControl(const QJsonObject &settings, const char *group, const char *key)
{
    for (const QJsonValue &v : settings.value(QLatin1String(group)).toArray()) {
        if (v.toObject().value(QStringLiteral("key")).toString() == QLatin1String(key))
            return v.toObject();
    }
    return {};
}

QJsonValue value(const QJsonObject &settings, const char *group, const char *key)
{
    const QJsonObject c = findControl(settings, group, key);
    return c.isEmpty() ? QJsonValue(QJsonValue::Undefined) : c.value(QStringLiteral("value"));
}

QStringList options(const QJsonObject &settings, const char *key)
{
    QStringList out;
    for (const QJsonValue &v : findControl(settings, "choices", key).value(QStringLiteral("options")).toArray())
        out << v.toString();
    return out;
}

void fillM4(Device &d)
{
    d.at(0x1206) = hex("4D 34 41 45 42 54 20 42 6C 61 63 6B 00");
    d.at(0x1201) = hex("00 03 00 25 00 03");
    d.at(0x0603) = hex("64");
    d.at(0x0602) = hex("01 00");
    d.at(0x1A05) = hex("01");
    d.at(0x1A01) = hex("01 01 02 00 03 00");
    d.at(0x1A03) = hex("64");
    d.at(0x1805) = hex("01");
    d.at(0x0804) = hex("00 01");
    d.at(0x1000) = hex("05 C4 3C 00 00");
    d.at(0x1003, hex("00")) = hex("3C 3C 16 16 00");
    d.at(0x1009) = hex("01");
    d.at(0x0401) = hex("01");
    d.at(0x080B) = hex("00");
    d.at(0x080D) = hex("00");
    d.at(0x0815) = hex("01");
    d.at(0x1607) = hex("00");
    d.at(0x0601, hex("00")) = hex("00 03 84");
    d.at(0x0802) = hex("02");
    d.sideEffects = m4SideEffects;
}

void fillM5(Device &d)
{
    d.at(0x1206) = hex("4d 35 41 45 42 54 20 42 6c 61 63 6b 00");
    d.at(0x1201) = hex("00 06 00 15 00 05");
    d.at(0x0603) = hex("50");
    d.at(0x1A01) = hex("01 00 03 00 02 00");
    d.at(0x1A03) = hex("00");
    d.at(0x1A05) = hex("01");
    d.at(0x1805) = hex("00");
    d.at(0x1000) = hex("08 c4 3c 00 00");
    d.at(0x1003, hex("00")) = hex("00 00 00 00 00 00 00 00");
    d.at(0x1009) = hex("01");
    d.at(0x1607) = hex("00");
    d.at(0x0401) = hex("00");
    d.at(0x080B) = hex("01");
    d.at(0x080D) = hex("00");
    d.at(0x0815) = hex("00");
    d.at(0x0802) = hex("02");
    d.at(0x0804) = hex("00 01");
    d.at(0x3605) = hex("01");
    d.at(0x3603) = hex("00");
    d.at(0x060B) = hex("01");
    d.sideEffects = m4SideEffects;
}

void testEncoding()
{
    const struct {
        uint16_t command;
        const char *payload;
        const char *expected;
    } frames[] = {
        {0x1206, "", "FF 03 00 00 04 95 12 06"},
        {0x0603, "", "FF 03 00 00 04 95 06 03"},
        {0x0007, "03", "FF 03 00 01 04 95 00 07 03"},
        {0x0600, "00 07 08", "FF 03 00 03 04 95 06 00 00 07 08"},
        {0x1001, "04 0A", "FF 03 00 02 04 95 10 01 04 0A"},
        {0x1A00, "01 02 02 00 03 00", "FF 03 00 06 04 95 1A 00 01 02 02 00 03 00"},
    };
    for (const auto &f : frames) {
        const QByteArray got = SennheiserBackend::encodeFrame(0x0495, f.command, hex(f.payload));
        check(got == hex(f.expected), QStringLiteral("encode %1 -> %2").arg(f.command, 4, 16, QLatin1Char('0')).arg(QString::fromLatin1(got.toHex(' '))));
    }
}

void testModels()
{
    check(SennheiserBackend::modelFromId(QStringLiteral("M4AEBT Black")) == SennheiserModel::Momentum4, QStringLiteral("model id M4AEBT Black"));
    check(SennheiserBackend::modelFromId(QStringLiteral("M5AEBT Black")) == SennheiserModel::Momentum5, QStringLiteral("model id M5AEBT Black"));
    check(SennheiserBackend::modelFromId(QStringLiteral("M4AEBTX")) == SennheiserModel::Unknown, QStringLiteral("model id M4AEBTX unknown"));
    check(SennheiserBackend::modelFromName(QStringLiteral("MOMENTUM 4")) == SennheiserModel::Momentum4, QStringLiteral("name MOMENTUM 4"));
    check(SennheiserBackend::modelFromName(QStringLiteral("Momentum 5")) == SennheiserModel::Momentum5, QStringLiteral("name Momentum 5"));
    check(SennheiserBackend::modelFromName(QStringLiteral("MOMENTUM TW 4")) == SennheiserModel::Unknown, QStringLiteral("name MOMENTUM TW 4 unknown"));
}

void testMomentum4()
{
    Session s("MOMENTUM 4");
    fillM4(s.device);
    s.backend.start();
    s.backend.tick();
    check(s.link.outgoing == hex("FF 03 00 00 04 95 12 06"), QStringLiteral("M4: model id asked first and alone"));
    s.run();
    check(s.backend.ready(), QStringLiteral("M4: ready"));
    check(s.backend.model() == SennheiserModel::Momentum4, QStringLiteral("M4: model from id"));
    for (const char *reg : {"FF 03 00 01 04 95 00 07 03", "FF 03 00 01 04 95 00 07 0D", "FF 03 00 01 04 95 00 07 08", "FF 03 00 01 04 95 00 07 0B"})
        check(s.sentRaw(reg), QStringLiteral("M4: notification registration %1").arg(QLatin1String(reg)));
    check(!s.sent(0x3605) && !s.sent(0x3603), QStringLiteral("M4: spatial audio not queried"));

    Snapshot snap = s.backend.snapshot();
    check(snap.battery == 100, QStringLiteral("M4: battery 100"));
    const QJsonObject battery = snap.batteries.at(0).toObject();
    check(battery.value(QStringLiteral("part")).toString() == QStringLiteral("main") && battery.value(QStringLiteral("charging")).toBool(),
          QStringLiteral("M4: main battery charging"));
    check(snap.settings.value(QStringLiteral("model")).toString() == QStringLiteral("Sennheiser MOMENTUM 4"), QStringLiteral("M4: model name"));
    check(snap.settings.value(QStringLiteral("untested")).toBool(), QStringLiteral("M4: untested flag"));
    check(snap.settings.value(QStringLiteral("firmware")).toString() == QStringLiteral("3.37.3"), QStringLiteral("M4: firmware 3.37.3"));
    check(snap.mode == QStringLiteral("ambient"), QStringLiteral("M4: transparent hearing is ambient (got %1)").arg(snap.mode));
    check(snap.modes == QStringList{QStringLiteral("noise-cancelling"), QStringLiteral("adaptive"), QStringLiteral("ambient"), QStringLiteral("off")},
          QStringLiteral("M4: modes %1").arg(snap.modes.join(QLatin1Char(','))));
    check(snap.settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("preset")).toInt() == 1, QStringLiteral("M4: sound mode equalizer"));
    const QJsonObject bass = findControl(snap.settings, "sliders", "eqLowBass");
    check(bass.value(QStringLiteral("value")).toInt() == 6 && bass.value(QStringLiteral("min")).toInt() == -6 && bass.value(QStringLiteral("max")).toInt() == 6,
          QStringLiteral("M4: low bass +6 dB in -6..6"));
    check(value(snap.settings, "sliders", "eqMid").toInt() == 2 && value(snap.settings, "sliders", "eqUpperTreble").toInt() == 0,
          QStringLiteral("M4: mid +2.2 dB rounds to 2, upper treble 0"));
    check(value(snap.settings, "sliders", "ancStrength").isUndefined(), QStringLiteral("M4: no strength slider in ambient"));
    check(value(snap.settings, "toggles", "wearDetection") == QJsonValue(true), QStringLiteral("M4: on-head detection on"));
    check(value(snap.settings, "toggles", "touchControls") == QJsonValue(true), QStringLiteral("M4: touch not locked"));
    check(value(snap.settings, "toggles", "bassBoost") == QJsonValue(true), QStringLiteral("M4: bass boost on"));
    check(value(snap.settings, "toggles", "smartPause") == QJsonValue(false) && value(snap.settings, "toggles", "autoAnswer") == QJsonValue(false)
              && value(snap.settings, "toggles", "comfortCall") == QJsonValue(true),
          QStringLiteral("M4: smart pause off, auto answer off, comfort call on"));
    check(value(snap.settings, "toggles", "batteryProtection").isUndefined(), QStringLiteral("M4: rejected getter leaves no toggle"));
    check(value(snap.settings, "choices", "windNoiseReduction").toString() == QStringLiteral("high")
              && options(snap.settings, "windNoiseReduction") == QStringList{QStringLiteral("off"), QStringLiteral("high"), QStringLiteral("auto")},
          QStringLiteral("M4: anti-wind maximum"));
    check(value(snap.settings, "choices", "voicePrompts").toString() == QStringLiteral("voice"), QStringLiteral("M4: tones and voice"));
    check(value(snap.settings, "choices", "autoPowerOff").toString() == QStringLiteral("15-min"), QStringLiteral("M4: auto power off 15 min"));

    s.log.clear();
    s.backend.setMode(QStringLiteral("noise-cancelling"));
    s.run();
    check(s.sentRaw("FF 03 00 01 04 95 18 04 00"), QStringLiteral("M4: transparent hearing off for noise cancelling"));
    check(!s.sent(0x1A04) && !s.sent(0x1A00), QStringLiteral("M4: nothing else written for noise cancelling"));
    snap = s.backend.snapshot();
    check(snap.mode == QStringLiteral("noise-cancelling"), QStringLiteral("M4: mode noise cancelling (got %1)").arg(snap.mode));
    check(value(snap.settings, "sliders", "ancStrength").toInt() == 100, QStringLiteral("M4: level 0 is full strength"));

    s.log.clear();
    s.backend.apply(QJsonObject{{QStringLiteral("sliders"), QJsonObject{{QStringLiteral("ancStrength"), 70}}}});
    s.run();
    check(s.sentRaw("FF 03 00 01 04 95 1A 02 1E"), QStringLiteral("M4: strength 70 writes level 30"));
    check(value(s.backend.snapshot().settings, "sliders", "ancStrength").toInt() == 70, QStringLiteral("M4: strength 70 read back"));

    s.log.clear();
    s.backend.setMode(QStringLiteral("adaptive"));
    s.run();
    check(s.sentRaw("FF 03 00 02 04 95 1A 00 03 01"), QStringLiteral("M4: adaptive as a single pair"));
    check(s.backend.snapshot().mode == QStringLiteral("adaptive"), QStringLiteral("M4: mode adaptive"));

    s.log.clear();
    s.backend.setMode(QStringLiteral("off"));
    s.run();
    check(s.sentRaw("FF 03 00 01 04 95 1A 04 00"), QStringLiteral("M4: noise cancelling off"));
    check(s.backend.snapshot().mode == QStringLiteral("off"), QStringLiteral("M4: mode off"));

    s.log.clear();
    s.backend.setMode(QStringLiteral("ambient"));
    s.run();
    const qsizetype on = std::find_if(s.log.begin(), s.log.end(), [](const Sent &x) { return x.raw == hex("FF 03 00 01 04 95 1A 04 01"); }) - s.log.begin();
    const qsizetype th = std::find_if(s.log.begin(), s.log.end(), [](const Sent &x) { return x.raw == hex("FF 03 00 01 04 95 18 04 01"); }) - s.log.begin();
    check(on < s.log.size() && th < s.log.size() && on < th, QStringLiteral("M4: ambient from off turns noise cancelling on, then transparent hearing"));
    check(s.backend.snapshot().mode == QStringLiteral("ambient"), QStringLiteral("M4: back to ambient"));

    s.log.clear();
    s.backend.apply(QJsonObject{{QStringLiteral("choices"), QJsonObject{{QStringLiteral("windNoiseReduction"), QStringLiteral("auto")}}}});
    s.run();
    check(s.sentRaw("FF 03 00 02 04 95 1A 00 01 02"), QStringLiteral("M4: anti-wind automatic as a single pair"));
    check(s.sentRaw("FF 03 00 01 04 95 18 04 01"), QStringLiteral("M4: transparent hearing restored after the anti-wind write"));
    snap = s.backend.snapshot();
    check(snap.mode == QStringLiteral("ambient") && value(snap.settings, "choices", "windNoiseReduction").toString() == QStringLiteral("auto"),
          QStringLiteral("M4: still ambient with automatic anti-wind"));

    s.log.clear();
    s.backend.apply(QJsonObject{
        {QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 2}}},
        {QStringLiteral("sliders"), QJsonObject{{QStringLiteral("eqUpperTreble"), 1}}},
        {QStringLiteral("toggles"), QJsonObject{{QStringLiteral("touchControls"), false}, {QStringLiteral("wearDetection"), false}}},
        {QStringLiteral("choices"), QJsonObject{{QStringLiteral("autoPowerOff"), QStringLiteral("30-min")}, {QStringLiteral("voicePrompts"), QStringLiteral("tones")}}},
    });
    s.run();
    check(s.sentRaw("FF 03 00 02 04 95 08 03 00 02"), QStringLiteral("M4: podcast sound mode"));
    check(s.sentRaw("FF 03 00 02 04 95 10 01 04 0A"), QStringLiteral("M4: upper treble +1 dB"));
    check(s.sentRaw("FF 03 00 01 04 95 16 06 01"), QStringLiteral("M4: touch lock on"));
    check(s.sentRaw("FF 03 00 01 04 95 04 00 00"), QStringLiteral("M4: on-head detection off"));
    check(s.sentRaw("FF 03 00 03 04 95 06 00 00 07 08"), QStringLiteral("M4: auto power off 30 min"));
    check(s.sentRaw("FF 03 00 01 04 95 08 01 01"), QStringLiteral("M4: tones only"));
    snap = s.backend.snapshot();
    check(snap.settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("preset")).toInt() == 2
              && value(snap.settings, "sliders", "eqUpperTreble").toInt() == 1 && value(snap.settings, "toggles", "touchControls") == QJsonValue(false)
              && value(snap.settings, "toggles", "wearDetection") == QJsonValue(false)
              && value(snap.settings, "choices", "autoPowerOff").toString() == QStringLiteral("30-min")
              && value(snap.settings, "choices", "voicePrompts").toString() == QStringLiteral("tones"),
          QStringLiteral("M4: changes read back"));

    check(s.backend.apply(QJsonObject{{QStringLiteral("equalizer"), QJsonObject{{QStringLiteral("preset"), 3}}}}).isValid(),
          QStringLiteral("M4: sound personalization preset rejected"));
    check(s.backend.apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("spatialAudio"), true}}}}).isValid(),
          QStringLiteral("M4: spatial audio unsupported"));
    check(s.backend.setAmbientLevel(5).isValid(), QStringLiteral("M4: ambient level unsupported"));

    s.feed(hex("FF 03 00 01 04 95 06 82 00") + frame(0x0683, hex("2A")) + hex("FF 03 00 05 04 95 10 82 3C 3C 16 16 00"));
    snap = s.backend.snapshot();
    check(snap.battery == 42 && !snap.batteries.at(0).toObject().value(QStringLiteral("charging")).toBool(), QStringLiteral("M4: battery and charging notifications"));
    check(value(snap.settings, "sliders", "eqUpperTreble").toInt() == 0, QStringLiteral("M4: equalizer notification"));
    s.feed(hex("FF 03 00 01 04 95 1A 85 00"));
    check(s.backend.snapshot().mode == QStringLiteral("off"), QStringLiteral("M4: noise cancelling off notification"));
}

void testMomentum5()
{
    Session s("MOMENTUM 5");
    fillM5(s.device);
    s.backend.start();
    s.run();
    check(s.backend.ready() && s.backend.model() == SennheiserModel::Momentum5, QStringLiteral("M5: ready"));
    Snapshot snap = s.backend.snapshot();
    check(snap.settings.value(QStringLiteral("firmware")).toString() == QStringLiteral("6.21.5"), QStringLiteral("M5: firmware 6.21.5"));
    check(snap.battery == 80, QStringLiteral("M5: battery 80"));
    check(snap.mode == QStringLiteral("noise-cancelling"), QStringLiteral("M5: noise cancelling (got %1)").arg(snap.mode));
    check(value(snap.settings, "sliders", "ancStrength").toInt() == 100, QStringLiteral("M5: full strength"));
    check(!value(snap.settings, "sliders", "eqBand8").isUndefined() && value(snap.settings, "sliders", "eqBand9").isUndefined()
              && value(snap.settings, "sliders", "eqLowBass").isUndefined(),
          QStringLiteral("M5: eight equalizer bands"));
    check(value(snap.settings, "choices", "windNoiseReduction").toString() == QStringLiteral("off")
              && options(snap.settings, "windNoiseReduction") == QStringList{QStringLiteral("off"), QStringLiteral("on")},
          QStringLiteral("M5: anti-wind off/on"));
    check(value(snap.settings, "toggles", "spatialAudio") == QJsonValue(true) && value(snap.settings, "toggles", "headTracking") == QJsonValue(false),
          QStringLiteral("M5: immersive audio on, head tracking off"));
    check(value(snap.settings, "toggles", "batteryProtection") == QJsonValue(true), QStringLiteral("M5: battery protection on"));
    check(value(snap.settings, "choices", "autoPowerOff").isUndefined(), QStringLiteral("M5: no auto power off without an answer"));

    s.log.clear();
    s.backend.apply(QJsonObject{
        {QStringLiteral("choices"), QJsonObject{{QStringLiteral("windNoiseReduction"), QStringLiteral("on")}}},
        {QStringLiteral("toggles"), QJsonObject{{QStringLiteral("headTracking"), true}}},
    });
    s.run();
    check(s.sentRaw("ff 03 00 02 04 95 1a 00 01 01"), QStringLiteral("M5: anti-wind on as a single pair"));
    check(s.sentRaw("ff 03 00 01 04 95 36 02 01"), QStringLiteral("M5: head tracking on"));
    check(!s.sent(0x1804), QStringLiteral("M5: no transparent hearing write outside ambient"));

    s.log.clear();
    s.backend.apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("spatialAudio"), false}}}});
    s.run();
    check(s.sentRaw("ff 03 00 01 04 95 36 04 00"), QStringLiteral("M5: immersive audio off"));
    check(s.backend.apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("headTracking"), true}}}}).isValid(),
          QStringLiteral("M5: head tracking needs immersive audio"));
}

void testFraming()
{
    Session s("Sennheiser");
    s.device.at(0x1206) = hex("58 59 5A 00");
    s.backend.start();
    s.backend.tick();
    s.link.outgoing.clear();
    const QByteArray model = frame(0x1306, hex("58 59 5A 00"));
    s.link.incoming.append(hex("00 12 FF 07") + model.left(5));
    s.backend.tick();
    s.link.incoming.append(model.mid(5));
    s.backend.tick();
    check(decode(s.link.outgoing).size() == 1 && decode(s.link.outgoing).at(0).command == 0x1201, QStringLiteral("framing: split frame after garbage advances the queue"));
    s.link.outgoing.clear();

    QByteArray checked = hex("FF 03 01 06 04 95 13 01 00 01 00 02 00 03");
    uint8_t x = 0;
    for (const char c : checked)
        x ^= uint8_t(c);
    QByteArray broken = checked;
    broken.append(char(x ^ 0x55));
    checked.append(char(x));
    s.link.incoming.append(broken + checked);
    s.backend.tick();
    s.link.incoming.append(hex("FF 03 02 00 01 04 95 07 03 37"));
    s.run();
    check(s.backend.ready() && s.backend.snapshot().battery == 0x37, QStringLiteral("framing: two-byte length frame"));
    check(s.backend.snapshot().settings.value(QStringLiteral("firmware")).toString() == QStringLiteral("1.2.3"), QStringLiteral("framing: checksummed frame accepted, broken one dropped"));
    check(s.backend.model() == SennheiserModel::Unknown && s.backend.snapshot().settings.value(QStringLiteral("model")).toString() == QStringLiteral("Sennheiser"),
          QStringLiteral("framing: unknown model keeps the generic name"));
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testEncoding();
    testModels();
    testMomentum4();
    testMomentum5();
    testFraming();
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
