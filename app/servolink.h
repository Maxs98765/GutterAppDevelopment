#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// ---------------------------------------------------------------------------
//  Talks to the hub's HTTP API (see the project README's "HTTP API"
//  table) and exposes its state to Main.qml: the rig's address, whether
//  it's reachable, the servo's angle and hold countdown, and the
//  rotate/home/hold commands.
//
//  Reconstructed header -- servolink.cpp survived, this declaration did
//  not. Property names and signatures match every `rig.*` reference in
//  Main.qml and every member/call in servolink.cpp.
// ---------------------------------------------------------------------------

class ServoLink : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString host READ host WRITE setHost NOTIFY hostChanged)
    Q_PROPERTY(int holdMs READ holdMs WRITE setHoldMs NOTIFY holdMsChanged)
    Q_PROPERTY(bool online READ online NOTIFY onlineChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

    // Telemetry: these all change together, off the same /status poll (or
    // the once-a-tick countdown between polls).
    Q_PROPERTY(QString state READ state NOTIFY telemetryChanged)
    Q_PROPERTY(int angle READ angle NOTIFY telemetryChanged)
    Q_PROPERTY(int remainingMs READ remainingMs NOTIFY telemetryChanged)
    Q_PROPERTY(bool cameraUp READ cameraUp NOTIFY telemetryChanged)
    Q_PROPERTY(bool holding READ holding NOTIFY telemetryChanged)
    Q_PROPERTY(bool moving READ moving NOTIFY telemetryChanged)

    Q_PROPERTY(QString streamUrl READ streamUrl NOTIFY hostChanged)
    Q_PROPERTY(QString snapshotUrl READ snapshotUrl NOTIFY hostChanged)

public:
    explicit ServoLink(QObject *parent = nullptr);

    QString host() const { return m_host; }
    void    setHost(const QString &host);

    int  holdMs() const { return m_holdMs; }
    void setHoldMs(int ms);

    bool    online()  const { return m_online; }
    QString message() const { return m_message; }
    bool    busy()    const { return m_inFlight > 0; }

    QString state()      const { return m_state; }
    int     angle()       const { return m_angle; }
    int     remainingMs() const { return m_remainingMs; }
    bool    cameraUp()    const { return m_cameraUp; }
    bool    holding()     const { return m_state == QStringLiteral("holding"); }
    bool    moving()      const;

    QString streamUrl()   const;
    QString snapshotUrl() const;

public slots:
    // Begins polling GET /status on a timer. Call once, e.g. from
    // Main.qml's Component.onCompleted.
    void startPolling();
    void stopPolling();

    void rotate();   // GET /rotate (with ?hold=<holdMs> if configured)
    void goHome();   // GET /home
    void pushHold(); // GET /hold?ms=<holdMs>, making it the Uno's new default

signals:
    void hostChanged();
    void holdMsChanged();
    void onlineChanged();
    void messageChanged();
    void busyChanged();
    void telemetryChanged();

private slots:
    void requestStatus();
    void onStatusReply(QNetworkReply *reply);
    void onCommandReply(QNetworkReply *reply, const QString &label);
    void tickCountdown();

private:
    QUrl url(const QString &path, const QString &query = {}) const;
    void send(const QString &path, const QString &query, const QString &label);

    void setOnline(bool online);
    void setMessage(const QString &message);
    void beginRequest();
    void endRequest();

    QNetworkAccessManager *m_net;

    QString m_host;
    int     m_holdMs;

    QTimer m_poll;   // drives requestStatus()
    QTimer m_tick;   // drives the on-screen countdown between polls

    bool    m_online        = false;
    QString m_message;
    int     m_inFlight      = 0;
    int     m_statusMisses  = 0;
    bool    m_statusPending = false;

    QString m_state       = QStringLiteral("unknown");
    int     m_angle       = 0;
    int     m_remainingMs = 0;
    bool    m_cameraUp    = false;
};
