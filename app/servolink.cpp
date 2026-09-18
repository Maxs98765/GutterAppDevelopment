#include "servolink.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "config.h"

ServoLink::ServoLink(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
    , m_host(QString::fromLatin1(cfg::kDefaultHost))
    , m_holdMs(cfg::kDefaultHoldMs)
{
    m_poll.setInterval(cfg::kStatusPollMs);
    connect(&m_poll, &QTimer::timeout, this, &ServoLink::requestStatus);

    m_tick.setInterval(cfg::kUiTickMs);
    connect(&m_tick, &QTimer::timeout, this, &ServoLink::tickCountdown);
    m_tick.start();
}

// --- properties ------------------------------------------------------------

void ServoLink::setHost(const QString &host)
{
    const QString trimmed = host.trimmed();
    if (trimmed.isEmpty() || trimmed == m_host) return;

    m_host = trimmed;
    m_statusMisses = 0;
    setOnline(false);
    emit hostChanged();
}

void ServoLink::setHoldMs(int ms)
{
    if (ms <= 0 || ms == m_holdMs) return;
    m_holdMs = ms;
    emit holdMsChanged();
}

bool ServoLink::moving() const
{
    return m_state == QStringLiteral("sweep_out")
        || m_state == QStringLiteral("sweep_back");
}

QUrl ServoLink::url(const QString &path, const QString &query) const
{
    QUrl u;
    u.setScheme(QStringLiteral("http"));

    // The host field may carry a port, so let QUrl split it out for us.
    const QString authority = m_host;
    u.setAuthority(authority);
    u.setPath(path);
    if (!query.isEmpty()) u.setQuery(query);
    return u;
}

QString ServoLink::streamUrl() const
{
    return url(QString::fromLatin1(cfg::kStreamPath)).toString();
}

QString ServoLink::snapshotUrl() const
{
    return url(QString::fromLatin1(cfg::kSnapshotPath)).toString();
}

// --- commands --------------------------------------------------------------

void ServoLink::rotate()
{
    QString query;
    if (cfg::kSendHoldWithRotate) {
        query = QStringLiteral("hold=%1").arg(m_holdMs);
    }
    send(QStringLiteral("/rotate"), query, tr("Rotating"));

    // Show the countdown immediately rather than waiting for the next poll.
    m_remainingMs = m_holdMs;
    emit telemetryChanged();
}

void ServoLink::goHome()
{
    send(QStringLiteral("/home"), {}, tr("Returning to 0"));
}

void ServoLink::pushHold()
{
    send(QStringLiteral("/hold"), QStringLiteral("ms=%1").arg(m_holdMs),
         tr("Hold set to %1 s").arg(m_holdMs / 1000.0, 0, 'f', 1));
}

void ServoLink::send(const QString &path, const QString &query,
                     const QString &label)
{
    QNetworkRequest req(url(path, query));
    req.setTransferTimeout(cfg::kRequestTimeoutMs);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);

    beginRequest();
    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, label]() {
        onCommandReply(reply, label);
    });
}

void ServoLink::onCommandReply(QNetworkReply *reply, const QString &label)
{
    reply->deleteLater();
    endRequest();

    if (reply->error() != QNetworkReply::NoError) {
        setOnline(false);
        setMessage(tr("Cannot reach the rig at %1").arg(m_host));
        return;
    }

    const QJsonObject obj =
        QJsonDocument::fromJson(reply->readAll()).object();

    if (obj.value(QStringLiteral("ok")).toBool()) {
        setOnline(true);
        setMessage(label);
    } else {
        const QString err = obj.value(QStringLiteral("error"))
                                .toString(tr("the rig refused that"));
        setMessage(tr("Failed: %1").arg(err));
    }

    requestStatus();
}

// --- status ----------------------------------------------------------------

void ServoLink::startPolling()
{
    requestStatus();
    m_poll.start();
}

void ServoLink::stopPolling()
{
    m_poll.stop();
}

void ServoLink::requestStatus()
{
    if (m_statusPending) return;   // never queue these up
    m_statusPending = true;

    QNetworkRequest req(url(QStringLiteral("/status")));
    req.setTransferTimeout(cfg::kRequestTimeoutMs);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onStatusReply(reply);
    });
}

void ServoLink::onStatusReply(QNetworkReply *reply)
{
    reply->deleteLater();
    m_statusPending = false;

    if (reply->error() != QNetworkReply::NoError) {
        if (++m_statusMisses >= 3) setOnline(false);
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();

    m_statusMisses = 0;
    setOnline(true);

    m_state       = obj.value(QStringLiteral("state")).toString(m_state);
    m_angle       = obj.value(QStringLiteral("angle")).toInt(m_angle);
    m_remainingMs = obj.value(QStringLiteral("remaining_ms")).toInt();
    m_cameraUp    = obj.value(QStringLiteral("camera")).toBool();

    emit telemetryChanged();
}

void ServoLink::tickCountdown()
{
    if (m_remainingMs <= 0) return;

    m_remainingMs -= cfg::kUiTickMs;
    if (m_remainingMs < 0) m_remainingMs = 0;
    emit telemetryChanged();
}

// --- bookkeeping -----------------------------------------------------------

void ServoLink::setOnline(bool online)
{
    if (m_online == online) return;
    m_online = online;
    if (!online) {
        m_state    = QStringLiteral("unknown");
        m_cameraUp = false;
        emit telemetryChanged();
    }
    emit onlineChanged();
}

void ServoLink::setMessage(const QString &message)
{
    if (m_message == message) return;
    m_message = message;
    emit messageChanged();
}

void ServoLink::beginRequest()
{
    if (m_inFlight++ == 0) emit busyChanged();
}

void ServoLink::endRequest()
{
    if (--m_inFlight <= 0) {
        m_inFlight = 0;
        emit busyChanged();
    }
}
