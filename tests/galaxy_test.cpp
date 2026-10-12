#include <QCoreApplication>
#include <QJsonDocument>

#include <cstdio>
#include <cstring>

#include "GalaxyBackend.h"

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

struct Case {
    const char *name;
    DeviceKind kind;
    const char *frame;
    int left;
    int right;
    int caseLevel;
    const char *mode;
    int ambientLevel;
    int equalizer;
    bool touchLock;
    int seamless;
};

const Case Cases[] = {
    {"Galaxy Buds (A1B2)", DeviceKind::GalaxyLegacy, "fe0112610300465f0100000000020107002201a575ee", 70, 95, -1, "off", 3, 3, false, 0},
    {"Galaxy Buds+ (A1B2)", DeviceKind::GalaxyClassic, "fd1b10610d00575401001165000000030022010401040100000003001e55dd", 87, 84, -1, "off", 1, 3, false, 1},
    {"Galaxy Buds Live (A1B2)", DeviceKind::GalaxyClassic, "fd1c106109010f610100116400000022010017011701000003001000000281dd", 15, 97, 100, "noise-cancelling", -1, 0, false, 1},
    {"Galaxy Buds Pro (A1B2)", DeviceKind::GalaxyClassic, "fd290c610a02595e010113640001002200002a012a0101000355000201000002001000010000220200009d24dd", 89, 94, 100, "off", 3, 1, false, 1},
    {"Galaxy Buds2 (A1B2)", DeviceKind::Galaxy, "fd2810610a0364640101113300033f2201003c013c0102000366000100100000000011020101000000bea3dd", 100, 100, 51, "noise-cancelling", 2, 3, true, 1},
    {"Galaxy Buds2 Pro (A1B2)", DeviceKind::Galaxy, "fd3100610d046464010033580001bf220001460146010000033500030010010101013203010101000cae0100030001000101b637dd", 100, 100, 88, "off", 3, 1, false, 1},
    {"Galaxy Buds FE (A1B2)", DeviceKind::Galaxy, "fd31106102063a590100110000008f3300004a014a01030004650001001000000101110300000000e80300009d0e1d005000c2cfdd", 58, 89, -1, "off", 2, 0, false, 1},
};

int level(const QJsonArray &batteries, const char *part)
{
    for (const QJsonValue &b : batteries) {
        if (b.toObject().value(QStringLiteral("part")).toString() == QLatin1String(part))
            return b.toObject().value(QStringLiteral("level")).toInt();
    }
    return -1;
}

void runCase(const Case &c)
{
    FakeLink link;
    link.incoming = QByteArray::fromHex(c.frame);
    MDRConnection conn{};
    conn.user = &link;
    conn.recv = fakeRecv;
    conn.send = fakeSend;
    conn.getLastError = fakeError;

    GalaxyBackend backend(&conn, DeviceInfo{QString::fromLatin1(c.name), QStringLiteral("00:11:22:33:44:55"), {}, c.kind});
    backend.start();
    backend.tick();
    const QString n = QString::fromLatin1(c.name);
    check(backend.ready(), n + QStringLiteral(": status parsed"));
    if (!backend.ready())
        return;

    const Snapshot s = backend.snapshot();
    check(level(s.batteries, "left") == c.left, n + QStringLiteral(": left battery %1").arg(c.left));
    check(level(s.batteries, "right") == c.right, n + QStringLiteral(": right battery %1").arg(c.right));
    check(level(s.batteries, "case") == c.caseLevel, n + QStringLiteral(": case battery %1").arg(c.caseLevel));
    check(s.mode == QLatin1String(c.mode), n + QStringLiteral(": mode %1 (got %2)").arg(QLatin1String(c.mode), s.mode));
    const bool hasAmbient = s.settings.contains(QStringLiteral("ambient"));
    if (c.ambientLevel < 0)
        check(!hasAmbient, n + QStringLiteral(": no ambient sound"));
    else
        check(hasAmbient && s.ambientLevel == c.ambientLevel, n + QStringLiteral(": ambient level %1 (got %2)").arg(c.ambientLevel).arg(s.ambientLevel));
    check(s.settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("preset")).toInt() == c.equalizer,
          n + QStringLiteral(": equalizer %1").arg(c.equalizer));
    check(s.settings.value(QStringLiteral("touchLock")).toBool() == c.touchLock, n + QStringLiteral(": touch lock %1").arg(c.touchLock));
    check(s.settings.value(QStringLiteral("seamless")).toBool() == bool(c.seamless), n + QStringLiteral(": seamless %1").arg(c.seamless));

    link.outgoing.clear();
    const QString target = s.modes.contains(QStringLiteral("ambient")) ? QStringLiteral("ambient") : QStringLiteral("off");
    backend.setMode(target);
    backend.tick();
    check(!link.outgoing.isEmpty(), n + QStringLiteral(": mode command sent (%1)").arg(QString::fromLatin1(link.outgoing.toHex(' '))));

    link.incoming = link.outgoing;
    link.outgoing.clear();
    QByteArray sentFrames = link.incoming;
    GalaxyBackend echo(&conn, DeviceInfo{QString::fromLatin1(c.name), {}, {}, c.kind});
    echo.start();
    echo.tick();
    check(link.incoming.isEmpty(), n + QStringLiteral(": own frames are consumed by the parser"));
    Q_UNUSED(sentFrames);
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    for (const Case &c : Cases)
        runCase(c);
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
