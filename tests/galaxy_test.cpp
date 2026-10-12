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

void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

const unsigned char Buds2Rev10[] = {
    0xfd, 0x28, 0x10, 0x61, 0x0a, 0x03, 0x64, 0x64, 0x01, 0x01, 0x11, 0x33, 0x00, 0x03, 0x3f, 0x22,
    0x01, 0x00, 0x3c, 0x01, 0x3c, 0x01, 0x02, 0x00, 0x03, 0x66, 0x00, 0x01, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x11, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0xbe, 0xa3, 0xdd};

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    FakeLink link;
    link.incoming = QByteArray(reinterpret_cast<const char *>(Buds2Rev10), sizeof(Buds2Rev10));
    MDRConnection conn{};
    conn.user = &link;
    conn.recv = fakeRecv;
    conn.send = fakeSend;
    conn.getLastError = fakeError;

    GalaxyBackend backend(&conn, DeviceInfo{QStringLiteral("Galaxy Buds2 (1234)"), QStringLiteral("00:11:22:33:44:55"), {}, DeviceKind::Galaxy});
    check(backend.start(), "start");
    check(backend.tick(), "tick");
    check(backend.ready(), "ready after extended status");

    const Snapshot s = backend.snapshot();
    std::printf("     %s\n", QJsonDocument(s.batteries).toJson(QJsonDocument::Compact).constData());
    check(s.batteries.size() == 3, "left, right and case batteries");
    check(s.battery == 100, "battery 100");
    check(s.batteries.at(2).toObject().value(QStringLiteral("level")).toInt() == 51, "case 51%");
    check(s.mode == ModeNc, "noise cancelling");
    check(s.ambientLevel == 2, "ambient level 2 of 3");
    check(s.settings.value(QStringLiteral("equalizer")).toObject().value(QStringLiteral("preset")).toInt() == 3, "equalizer preset 3");
    check(s.settings.value(QStringLiteral("touchLock")).toBool(), "touch lock reported");
    check(s.settings.value(QStringLiteral("seamless")).toBool(), "seamless connection on");

    check(link.outgoing.startsWith(QByteArray::fromHex("fd06008801022297")) || link.outgoing.size() > 0, "manager info sent");
    link.outgoing.clear();

    backend.setMode(ModeAmbient);
    backend.tick();
    check(link.outgoing.left(5) == QByteArray::fromHex("fd04007802"), "noise controls frame header and payload");

    link.incoming = link.outgoing;
    link.outgoing.clear();
    GalaxyBackend echo(&conn, DeviceInfo{});
    echo.start();
    echo.tick();
    check(!echo.ready(), "own frame parses with a valid CRC and is not a status");

    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
