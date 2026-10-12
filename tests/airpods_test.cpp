#include <QCoreApplication>
#include <QJsonArray>

#include <cstdio>

#include "AirPodsBackend.h"

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
    return QByteArray::fromHex(QByteArray(text).replace(' ', ""));
}

QString hexOf(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex(' '));
}

const char *const InfoAirPodsPro2 =
    "040004001d0002d5000400416972506f64732050726f004133303438004170706c6520496e632e0051584e524848595850360036312e3138"
    "36383034303030323030303030302e323731330036312e313836383034303030323030303030302e3237313300312e302e3000636f6d2e61"
    "70706c652e6163636573736f72792e757064617465722e6170702e3731004859394c5432454632364a59004833504c5748444a32364b3000"
    "363335373533360089312a6567a5400f84a3ca234947efd40b90d78436ae5946748d70273e66066a2589300035333935303630363400";

const char *const BatteryAirPodsPro2 = "04 00 04 00 04 00 03 02 01 64 02 01 04 01 63 01 01 08 01 11 02 01";

QByteArray infoPacket(const char *name, const char *modelNumber)
{
    QByteArray p = hex("04 00 04 00 1d 00 02 00 00 04 00");
    p.append(name).append('\0').append(modelNumber).append('\0').append("Apple Inc.").append('\0');
    p.append("SERIAL").append('\0').append("1.2.3").append('\0');
    return p;
}

AirPodsBackend makeBackend(const char *name)
{
    return AirPodsBackend(nullptr, DeviceInfo{QString::fromLatin1(name), QStringLiteral("00:11:22:33:44:55"), {}, DeviceKind::AirPods, {}, true});
}

QJsonObject findBattery(const Snapshot &s, const char *part)
{
    for (const QJsonValue &b : s.batteries) {
        if (b.toObject().value(QStringLiteral("part")).toString() == QLatin1String(part))
            return b.toObject();
    }
    return {};
}

QJsonObject findControl(const Snapshot &s, const char *list, const char *key)
{
    for (const QJsonValue &v : s.settings.value(QLatin1String(list)).toArray()) {
        if (v.toObject().value(QStringLiteral("key")).toString() == QLatin1String(key))
            return v.toObject();
    }
    return {};
}

void testPackets()
{
    check(hexOf(AirPodsBackend::handshakePacket()) == QLatin1String("00 00 04 00 01 00 02 00 00 00 00 00 00 00 00 00"), QStringLiteral("handshake packet"));
    check(hexOf(AirPodsBackend::featuresPacket()) == QLatin1String("04 00 04 00 4d 00 d7 00 00 00 00 00 00 00"), QStringLiteral("feature flags packet"));
    check(hexOf(AirPodsBackend::notificationsPacket()) == QLatin1String("04 00 04 00 0f 00 ff ff ff ff"), QStringLiteral("notification request packet"));
    check(hexOf(AirPodsBackend::controlPacket(0x0D, 0x02)) == QLatin1String("04 00 04 00 09 00 0d 02 00 00 00"), QStringLiteral("listening mode packet"));
}

void testModels()
{
    const struct {
        const char *number;
        AirPodsModel model;
    } numbers[] = {
        {"A2032", AirPodsModel::AirPods2},  {"A2565", AirPodsModel::AirPods3},    {"A3054", AirPodsModel::AirPods4},
        {"A3056", AirPodsModel::AirPods4Anc}, {"A2084", AirPodsModel::AirPodsPro}, {"A2699", AirPodsModel::AirPodsPro2},
        {"A3048", AirPodsModel::AirPodsPro2}, {"A3064", AirPodsModel::AirPodsPro3}, {"A2096", AirPodsModel::AirPodsMax},
        {"A9999", AirPodsModel::Unknown},
    };
    for (const auto &n : numbers)
        check(AirPodsBackend::modelFromNumber(QLatin1String(n.number)) == n.model, QStringLiteral("model number %1").arg(QLatin1String(n.number)));
    check(AirPodsBackend::modelFromName(QStringLiteral("Rodion's AirPods Max")) == AirPodsModel::AirPodsMax, QStringLiteral("model from name: Max"));
    check(AirPodsBackend::modelFromName(QStringLiteral("AirPods Pro")) == AirPodsModel::Unknown, QStringLiteral("ambiguous name stays unknown"));

    const auto info = AirPodsBackend::parseInfo(hex(InfoAirPodsPro2));
    check(info && info->name == QLatin1String("AirPods Pro"), QStringLiteral("information: name"));
    check(info && info->modelNumber == QLatin1String("A3048"), QStringLiteral("information: model number"));
    check(info && info->manufacturer == QLatin1String("Apple Inc."), QStringLiteral("information: manufacturer"));
    check(info && info->firmware == QLatin1String("61.1868040002000000.2713"), QStringLiteral("information: firmware"));
    check(!AirPodsBackend::parseInfo(hex(BatteryAirPodsPro2)), QStringLiteral("information: rejects other messages"));
}

void testAirPodsPro2()
{
    AirPodsBackend b = makeBackend("AirPods Pro");
    b.handlePacket(hex("01 00 04 00 00 00 01 00 02 00 00 00 00 00 00 00"));
    check(!b.ready(), QStringLiteral("Pro 2: not ready before battery"));
    b.handlePacket(hex(InfoAirPodsPro2));
    check(b.model() == AirPodsModel::AirPodsPro2, QStringLiteral("Pro 2: model from information packet"));
    b.handlePacket(hex(BatteryAirPodsPro2));
    check(b.ready(), QStringLiteral("Pro 2: ready after battery"));

    b.handlePacket(hex("04 00 04 00 09 00 0d 03 00 00 00"));
    b.handlePacket(hex("04 00 04 00 09 00 28 01 00 00 00"));
    b.handlePacket(hex("04 00 04 00 09 00 0a 01 00 00 00"));
    b.handlePacket(hex("04 00 04 00 09 00 26 02 00 00 00"));
    b.handlePacket(hex("04 00 04 00 09 00 2e 32 00 00 00"));
    b.handlePacket(hex("04 00 04 00 06 00 00 01"));

    Snapshot s = b.snapshot();
    check(findBattery(s, "right").value(QStringLiteral("level")).toInt() == 100, QStringLiteral("Pro 2: right battery 100"));
    check(findBattery(s, "left").value(QStringLiteral("level")).toInt() == 99, QStringLiteral("Pro 2: left battery 99"));
    check(findBattery(s, "left").value(QStringLiteral("charging")).toBool(), QStringLiteral("Pro 2: left charging"));
    check(!findBattery(s, "right").value(QStringLiteral("charging")).toBool(), QStringLiteral("Pro 2: right discharging"));
    check(findBattery(s, "case").value(QStringLiteral("level")).toInt() == 17, QStringLiteral("Pro 2: case battery 17"));
    check(findBattery(s, "main").isEmpty(), QStringLiteral("Pro 2: no main battery"));
    check(s.battery == 99, QStringLiteral("Pro 2: overall battery is the lower bud"));
    check(s.modes == QStringList({ModeNc, ModeAmbient, ModeAdaptive, ModeOff}), QStringLiteral("Pro 2: modes %1").arg(s.modes.join(QLatin1Char(','))));
    check(s.mode == ModeAmbient, QStringLiteral("Pro 2: transparency reported as ambient"));
    check(s.settings.value(QStringLiteral("model")).toString() == QLatin1String("AirPods Pro 2"), QStringLiteral("Pro 2: model name"));
    check(s.settings.value(QStringLiteral("untested")).toBool(), QStringLiteral("Pro 2: marked untested"));
    check(s.settings.value(QStringLiteral("firmware")).toString() == QLatin1String("61.1868040002000000.2713"), QStringLiteral("Pro 2: firmware"));
    check(findControl(s, "toggles", "conversationAwareness").value(QStringLiteral("value")) == QJsonValue(true), QStringLiteral("Pro 2: conversation awareness on"));
    check(findControl(s, "toggles", "earDetection").value(QStringLiteral("value")) == QJsonValue(true), QStringLiteral("Pro 2: ear detection on"));
    check(findControl(s, "toggles", "personalizedVolume").value(QStringLiteral("value")) == QJsonValue(false), QStringLiteral("Pro 2: personalized volume off"));
    check(findControl(s, "toggles", "offListeningMode").isEmpty(), QStringLiteral("Pro 2: no off option toggle before it is reported"));
    const QJsonObject slider = findControl(s, "sliders", "adaptiveAudio");
    check(slider.value(QStringLiteral("value")).toInt() == 50 && slider.value(QStringLiteral("max")).toInt() == 100, QStringLiteral("Pro 2: adaptive audio strength 50 of 100"));
    const QJsonObject wearing = s.settings.value(QStringLiteral("wearing")).toObject();
    check(wearing.value(QStringLiteral("right")).toBool() && !wearing.value(QStringLiteral("left")).toBool(),
          QStringLiteral("Pro 2: primary (right, first in battery report) in ear, left out"));

    b.takeOutgoing();
    check(!b.setMode(ModeNc).isValid(), QStringLiteral("Pro 2: set noise cancelling"));
    check(!b.apply(QJsonObject{{QStringLiteral("toggles"), QJsonObject{{QStringLiteral("conversationAwareness"), false}}},
                               {QStringLiteral("sliders"), QJsonObject{{QStringLiteral("adaptiveAudio"), 130}}}})
               .isValid(),
          QStringLiteral("Pro 2: apply"));
    const QList<QByteArray> out = b.takeOutgoing();
    check(out.size() == 3, QStringLiteral("Pro 2: three commands queued"));
    if (out.size() == 3) {
        check(hexOf(out.at(0)) == QLatin1String("04 00 04 00 09 00 0d 02 00 00 00"), QStringLiteral("Pro 2: listening mode command %1").arg(hexOf(out.at(0))));
        check(hexOf(out.at(1)) == QLatin1String("04 00 04 00 09 00 28 02 00 00 00"), QStringLiteral("Pro 2: conversation awareness command %1").arg(hexOf(out.at(1))));
        check(hexOf(out.at(2)) == QLatin1String("04 00 04 00 09 00 2e 64 00 00 00"), QStringLiteral("Pro 2: adaptive strength clamped %1").arg(hexOf(out.at(2))));
    }
    s = b.snapshot();
    check(s.mode == ModeNc, QStringLiteral("Pro 2: mode updated"));
    check(findControl(s, "toggles", "conversationAwareness").value(QStringLiteral("value")) == QJsonValue(false), QStringLiteral("Pro 2: toggle updated"));
    check(b.setAmbientLevel(2).isValid(), QStringLiteral("Pro 2: ambient level unsupported"));

    b.handlePacket(hex("04 00 04 00 09 00 34 02 00 00 00"));
    s = b.snapshot();
    check(!s.modes.contains(ModeOff), QStringLiteral("Pro 2: off hidden when the off option is disabled"));
    check(findControl(s, "toggles", "offListeningMode").value(QStringLiteral("value")) == QJsonValue(false), QStringLiteral("Pro 2: off option toggle"));
    check(b.setMode(ModeOff).isValid(), QStringLiteral("Pro 2: off rejected while disabled"));

    b.handlePacket(hex("04 00 04 00 04 00 01 04 01 00 04 01"));
    s = b.snapshot();
    check(findBattery(s, "left").isEmpty() && !findBattery(s, "right").isEmpty(), QStringLiteral("Pro 2: disconnected bud dropped, others kept"));
}

void testAirPods2()
{
    AirPodsBackend b = makeBackend("Headphones");
    b.handlePacket(infoPacket("AirPods", "A2032"));
    b.handlePacket(hex("04 00 04 00 04 00 02 04 01 50 02 01 02 01 46 02 01"));
    b.handlePacket(hex("04 00 04 00 09 00 0a 02 00 00 00"));
    b.handlePacket(hex("04 00 04 00 09 00 28 01 00 00 00"));
    const Snapshot s = b.snapshot();
    check(b.ready(), QStringLiteral("AirPods 2: ready"));
    check(s.modes.isEmpty() && s.mode.isEmpty(), QStringLiteral("AirPods 2: no noise control"));
    check(s.battery == 70, QStringLiteral("AirPods 2: battery 70"));
    check(s.settings.value(QStringLiteral("model")).toString() == QLatin1String("AirPods 2"), QStringLiteral("AirPods 2: model name"));
    check(findControl(s, "toggles", "earDetection").value(QStringLiteral("value")) == QJsonValue(false), QStringLiteral("AirPods 2: ear detection off"));
    check(findControl(s, "toggles", "conversationAwareness").isEmpty(), QStringLiteral("AirPods 2: no conversation awareness"));
    check(!s.settings.contains(QStringLiteral("sliders")), QStringLiteral("AirPods 2: no sliders"));
    check(b.setMode(ModeNc).isValid(), QStringLiteral("AirPods 2: noise cancelling rejected"));
}

void testAirPodsMax()
{
    AirPodsBackend b = makeBackend("AirPods Max");
    b.handlePacket(hex("04 00 04 00 04 00 01 01 01 50 01 01"));
    b.handlePacket(hex("04 00 04 00 09 00 0d 01 00 00 00"));
    const Snapshot s = b.snapshot();
    check(findBattery(s, "main").value(QStringLiteral("level")).toInt() == 80, QStringLiteral("Max: headset battery 80"));
    check(findBattery(s, "main").value(QStringLiteral("charging")).toBool(), QStringLiteral("Max: charging"));
    check(s.battery == 80, QStringLiteral("Max: overall battery"));
    check(s.modes == QStringList({ModeNc, ModeAmbient, ModeOff}), QStringLiteral("Max: no adaptive"));
    check(s.mode == ModeOff, QStringLiteral("Max: off"));
}

void testUnknown()
{
    AirPodsBackend b = makeBackend("Kitchen");
    b.handlePacket(hex("04 00 04 00 04 00 01 04 01 30 02 01"));
    Snapshot s = b.snapshot();
    check(b.model() == AirPodsModel::Unknown && s.modes.isEmpty(), QStringLiteral("unknown: no modes before the listening mode is reported"));
    b.handlePacket(hex("04 00 04 00 09 00 0d 04 00 00 00"));
    s = b.snapshot();
    check(s.modes == QStringList({ModeNc, ModeAmbient, ModeAdaptive, ModeOff}), QStringLiteral("unknown: modes from reported state"));
    check(s.mode == ModeAdaptive, QStringLiteral("unknown: adaptive"));
}

void testMalformed()
{
    AirPodsBackend b = makeBackend("AirPods Pro");
    b.handlePacket(hex("04 00 04 00 04 00 03 02 01 64 02 01"));
    b.handlePacket(hex("04 00 04 00 04 00 00"));
    b.handlePacket(hex("04 00 04"));
    b.handlePacket(hex("04 00 05 00 04 00 01 04 01 30 02 01"));
    check(!b.ready(), QStringLiteral("malformed battery reports are ignored"));
    check(b.setMode(ModeNc).isValid(), QStringLiteral("commands rejected before ready"));
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testPackets();
    testModels();
    testAirPodsPro2();
    testAirPods2();
    testAirPodsMax();
    testUnknown();
    testMalformed();
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
