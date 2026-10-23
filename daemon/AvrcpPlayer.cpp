#include "AvrcpPlayer.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusVariant>
#include <QDateTime>
#include <QTimer>

#include "Log.h"

namespace {

using InterfaceMap = QMap<QString, QVariantMap>;
using ManagedObjects = QMap<QDBusObjectPath, InterfaceMap>;

const QString MprisPrefix = QStringLiteral("org.mpris.MediaPlayer2.");
const QString PlayerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");
const QString MprisPath = QStringLiteral("/org/mpris/MediaPlayer2");

}

Q_DECLARE_METATYPE(InterfaceMap)
Q_DECLARE_METATYPE(ManagedObjects)

AvrcpPlayer::AvrcpPlayer(QObject *parent)
    : QObject(parent)
    , _bluez(QStringLiteral("org.bluez"), QDBusConnection::systemBus(), QDBusServiceWatcher::WatchForRegistration)
{
    qDBusRegisterMetaType<InterfaceMap>();
    qDBusRegisterMetaType<ManagedObjects>();
}

bool AvrcpPlayer::isLocalPlayer(const QString &name)
{
    return name.startsWith(MprisPrefix) && !name.startsWith(MprisPrefix + QStringLiteral("kdeconnect"));
}

bool AvrcpPlayer::start()
{
    auto system = QDBusConnection::systemBus();
    if (!system.registerObject(QString::fromLatin1(Path), this,
                               QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties)) {
        qCWarning(lcHp) << "avrcp: cannot export player:" << system.lastError().message();
        return false;
    }
    auto session = QDBusConnection::sessionBus();
    session.connect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                    QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"), this,
                    SLOT(onNameOwnerChanged(QString, QString, QString)));
    session.connect(QString(), MprisPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                    QStringLiteral("PropertiesChanged"), this, SLOT(onPlayerProperties(QDBusMessage)));
    system.connect(QStringLiteral("org.bluez"), QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                   QStringLiteral("InterfacesAdded"), this, SLOT(onInterfacesAdded(QDBusMessage)));
    connect(&_bluez, &QDBusServiceWatcher::serviceRegistered, this, &AvrcpPlayer::registerWithBluez);
    refreshPlayers();
    registerWithBluez();
    return true;
}

void AvrcpPlayer::refreshPlayers()
{
    const QStringList names = QDBusConnection::sessionBus().interface()->registeredServiceNames().value();
    for (const QString &name : names) {
        if (isLocalPlayer(name))
            queryPlayer(name);
    }
    updateStatus();
}

void AvrcpPlayer::queryPlayer(const QString &name)
{
    auto session = QDBusConnection::sessionBus();
    Player &p = _players[name];
    p.owner = session.interface()->serviceOwner(name).value();
    auto msg = QDBusMessage::createMethodCall(name, MprisPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                              QStringLiteral("Get"));
    msg << PlayerInterface << QStringLiteral("PlaybackStatus");
    const QDBusMessage reply = session.call(msg, QDBus::Block, 500);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
        setPlayerStatus(name, reply.arguments().constFirst().value<QDBusVariant>().variant().toString());
}

void AvrcpPlayer::setPlayerStatus(const QString &name, const QString &status)
{
    Player &p = _players[name];
    p.status = status;
    if (status == QLatin1String("Playing"))
        p.lastPlaying = QDateTime::currentMSecsSinceEpoch();
}

void AvrcpPlayer::onNameOwnerChanged(const QString &name, const QString &, const QString &newOwner)
{
    if (!isLocalPlayer(name))
        return;
    if (newOwner.isEmpty())
        _players.remove(name);
    else
        queryPlayer(name);
    updateStatus();
}

void AvrcpPlayer::onPlayerProperties(const QDBusMessage &message)
{
    const QList<QVariant> args = message.arguments();
    if (args.size() < 2 || args[0].toString() != PlayerInterface)
        return;
    const QVariantMap changed = qdbus_cast<QVariantMap>(args[1]);
    const auto status = changed.constFind(QStringLiteral("PlaybackStatus"));
    if (status == changed.cend())
        return;
    for (auto it = _players.begin(); it != _players.end(); ++it) {
        if (it->owner == message.service()) {
            setPlayerStatus(it.key(), status->toString());
            updateStatus();
            return;
        }
    }
}

void AvrcpPlayer::updateStatus()
{
    QString status = QStringLiteral("Stopped");
    for (const Player &p : std::as_const(_players)) {
        if (p.status == QLatin1String("Playing")) {
            status = p.status;
            break;
        }
        if (p.status == QLatin1String("Paused"))
            status = p.status;
    }
    if (status == _status)
        return;
    _status = status;
    announceStatus();
}

void AvrcpPlayer::announceStatus()
{
    auto signal = QDBusMessage::createSignal(QString::fromLatin1(Path), QStringLiteral("org.freedesktop.DBus.Properties"),
                                             QStringLiteral("PropertiesChanged"));
    signal << PlayerInterface << QVariantMap{{QStringLiteral("PlaybackStatus"), _status}} << QStringList();
    QDBusConnection::systemBus().send(signal);
}

void AvrcpPlayer::registerWithBluez()
{
    auto call = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), QStringLiteral("/"),
                                               QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                                               QStringLiteral("GetManagedObjects"));
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 2000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return;
    ManagedObjects objects;
    reply.arguments().constFirst().value<QDBusArgument>() >> objects;
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        if (it->contains(QStringLiteral("org.bluez.Media1")))
            registerOn(it.key().path());
    }
}

void AvrcpPlayer::onInterfacesAdded(const QDBusMessage &message)
{
    const QList<QVariant> args = message.arguments();
    if (args.size() < 2)
        return;
    InterfaceMap interfaces;
    args[1].value<QDBusArgument>() >> interfaces;
    if (interfaces.contains(QStringLiteral("org.bluez.Media1")))
        registerOn(args[0].value<QDBusObjectPath>().path());
}

void AvrcpPlayer::registerOn(const QString &adapterPath)
{
    auto call = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), adapterPath, QStringLiteral("org.bluez.Media1"),
                                               QStringLiteral("RegisterPlayer"));
    const QVariantMap properties{{QStringLiteral("PlaybackStatus"), QStringLiteral("Stopped")},
                                 {QStringLiteral("Identity"), QStringLiteral("KDE Plasma")},
                                 {QStringLiteral("CanPlay"), true},
                                 {QStringLiteral("CanPause"), true},
                                 {QStringLiteral("CanGoNext"), true},
                                 {QStringLiteral("CanGoPrevious"), true},
                                 {QStringLiteral("CanControl"), true}};
    call << QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(Path))) << properties;
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 5000);
    if (reply.type() == QDBusMessage::ErrorMessage && reply.errorName() != QLatin1String("org.bluez.Error.AlreadyExists"))
        qCWarning(lcHp) << "avrcp: register on" << adapterPath << "failed:" << reply.errorMessage();
    else
        qCInfo(lcHp) << "avrcp: player registered on" << adapterPath;
    if (_status != QLatin1String("Stopped"))
        QTimer::singleShot(500, this, &AvrcpPlayer::announceStatus);
}

void AvrcpPlayer::callPlayer(const QString &name, const QString &method)
{
    if (name.isEmpty())
        return;
    QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(name, MprisPath, PlayerInterface, method));
}

QString AvrcpPlayer::resumeTarget() const
{
    QString best;
    qint64 bestTime = -1;
    for (auto it = _players.cbegin(); it != _players.cend(); ++it) {
        if (it->status == QLatin1String("Playing"))
            return it.key();
        if (it->status == QLatin1String("Paused") && it->lastPlaying > bestTime) {
            best = it.key();
            bestTime = it->lastPlaying;
        }
    }
    return best;
}

void AvrcpPlayer::Play()
{
    callPlayer(resumeTarget(), QStringLiteral("Play"));
}

void AvrcpPlayer::Pause()
{
    for (auto it = _players.cbegin(); it != _players.cend(); ++it) {
        if (it->status == QLatin1String("Playing"))
            callPlayer(it.key(), QStringLiteral("Pause"));
    }
}

void AvrcpPlayer::PlayPause()
{
    if (_status == QLatin1String("Playing"))
        Pause();
    else
        Play();
}

void AvrcpPlayer::Stop()
{
    Pause();
}

void AvrcpPlayer::Next()
{
    callPlayer(resumeTarget(), QStringLiteral("Next"));
}

void AvrcpPlayer::Previous()
{
    callPlayer(resumeTarget(), QStringLiteral("Previous"));
}
