#pragma once

#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QHash>
#include <QObject>
#include <QString>

class AvrcpPlayer : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(bool CanPlay READ canControl)
    Q_PROPERTY(bool CanPause READ canControl)
    Q_PROPERTY(bool CanGoNext READ canControl)
    Q_PROPERTY(bool CanGoPrevious READ canControl)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    static constexpr auto Path = "/io/github/rodiada/Headphones/Player";

    explicit AvrcpPlayer(QObject *parent = nullptr);

    bool start();
    QString playbackStatus() const { return _status; }
    bool canControl() const { return true; }

    static bool isLocalPlayer(const QString &name);

public Q_SLOTS:
    void Play();
    void Pause();
    void PlayPause();
    void Stop();
    void Next();
    void Previous();

private Q_SLOTS:
    void onNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);
    void onPlayerProperties(const QDBusMessage &message);
    void onInterfacesAdded(const QDBusMessage &message);

private:
    struct Player {
        QString owner;
        QString status;
        qint64 lastPlaying = 0;
    };

    void refreshPlayers();
    void queryPlayer(const QString &name);
    void setPlayerStatus(const QString &name, const QString &status);
    void updateStatus();
    void announceStatus();
    void registerWithBluez();
    void registerOn(const QString &adapterPath);
    void callPlayer(const QString &name, const QString &method);
    QString resumeTarget() const;

    QHash<QString, Player> _players;
    QString _status = QStringLiteral("Stopped");
    QDBusServiceWatcher _bluez;
};
