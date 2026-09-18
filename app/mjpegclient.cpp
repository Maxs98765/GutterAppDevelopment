#include "mjpegclient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include "config.h"

namespace {

// JPEG start- and end-of-image markers.
const QByteArray kSoi = QByteArray::fromHex("ffd8");
const QByteArray kEoi = QByteArray::fromHex("ffd9");

// Refuse to grow the buffer without bound if we lose sync with the stream.
constexpr int kMaxBufferBytes = 2 * 1024 * 1024;

} // namespace

MjpegClient::MjpegClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
    , m_snapshotMode(cfg::kUseSnapshotFallback)
{
    m_snapshotTimer.setInterval(1000 / qMax(1, cfg::kSnapshotFps));
    connect(&m_snapshotTimer, &QTimer::timeout, this,
            &MjpegClient::requestSnapshot);

    m_stallTimer.setInterval(1000);
    connect(&m_stallTimer, &QTimer::timeout, this, &MjpegClient::checkStall);

    m_sinceFrame.start();
    m_fpsWindow.start();
}

MjpegClient::~MjpegClient()
{
    abortReply();
}

// --- properties ------------------------------------------------------------

void MjpegClient::setUrl(const QString &url)
{
    if (url == m_url) return;
    m_url = url;
    emit urlChanged();

    if (m_running) restart();
}

void MjpegClient::setSnapshotMode(bool on)
{
    if (on == m_snapshotMode) return;
    m_snapshotMode = on;
    emit snapshotModeChanged();

    if (m_running) restart();
}

// --- lifecycle -------------------------------------------------------------

void MjpegClient::start()
{
    if (m_url.isEmpty()) {
        setMessage(tr("No video address set"));
        return;
    }

    m_running = true;
    emit runningChanged();

    m_buf.clear();
    m_sinceFrame.restart();
    m_stallTimer.start();

    if (m_snapshotMode) {
        setMessage(tr("Polling still images"));
        requestSnapshot();
        m_snapshotTimer.start();
    } else {
        setMessage(tr("Connecting to the camera"));
        openStream();
    }
}

void MjpegClient::stop()
{
    m_running = false;
    m_snapshotTimer.stop();
    m_stallTimer.stop();
    abortReply();
    m_buf.clear();
    setReceiving(false);
    emit runningChanged();
}

void MjpegClient::restart()
{
    const bool wasRunning = m_running;
    stop();
    if (wasRunning) start();
}

void MjpegClient::abortReply()
{
    if (!m_reply) return;

    m_reply->disconnect(this);
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
    m_snapshotPending = false;
}

// --- stream mode -----------------------------------------------------------

void MjpegClient::openStream()
{
    abortReply();

    QNetworkRequest req(QUrl(m_url));
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    req.setRawHeader("Accept", "multipart/x-mixed-replace, image/jpeg");
    // No transfer timeout: the stream is meant to stay open indefinitely.
    // Stalls are caught by checkStall() instead.

    m_reply = m_net->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &MjpegClient::onStreamData);
    connect(m_reply, &QNetworkReply::finished, this, &MjpegClient::onStreamFinished);
}

void MjpegClient::onStreamData()
{
    if (!m_reply) return;

    m_buf.append(m_reply->readAll());

    if (m_buf.size() > kMaxBufferBytes) {
        // Lost sync. Throw away everything before the last start marker.
        const int last = m_buf.lastIndexOf(kSoi);
        m_buf = (last > 0) ? m_buf.mid(last) : QByteArray();
        setMessage(tr("Resynchronising the stream"));
    }

    parseBuffer();
}

void MjpegClient::onStreamFinished()
{
    if (!m_reply) return;

    const bool aborted = m_reply->error() == QNetworkReply::OperationCanceledError;
    const QString err  = m_reply->errorString();
    const bool failed  = m_reply->error() != QNetworkReply::NoError;

    m_reply->deleteLater();
    m_reply = nullptr;
    setReceiving(false);

    if (!m_running || aborted) return;

    setMessage(failed ? tr("Camera link dropped: %1").arg(err)
                      : tr("Camera closed the stream"));

    // Back off a little, then try again, so a rebooting rig recovers on
    // its own without the user touching anything.
    m_retryDelayMs = qMin(m_retryDelayMs * 2, 4000);
    QTimer::singleShot(m_retryDelayMs, this, [this]() {
        if (m_running && !m_snapshotMode) openStream();
    });
}

// --- snapshot mode ---------------------------------------------------------

void MjpegClient::requestSnapshot()
{
    if (m_snapshotPending || m_url.isEmpty()) return;
    m_snapshotPending = true;

    QNetworkRequest req(QUrl(m_url));
    req.setTransferTimeout(cfg::kRequestTimeoutMs);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onSnapshotFinished(reply);
    });
}

void MjpegClient::onSnapshotFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    m_snapshotPending = false;

    if (!m_running) return;

    if (reply->error() != QNetworkReply::NoError) {
        setReceiving(false);
        setMessage(tr("Cannot fetch an image: %1").arg(reply->errorString()));
        return;
    }

    publish(reply->readAll());
}

// --- framing ---------------------------------------------------------------

void MjpegClient::parseBuffer()
{
    // Content-Length is the reliable path and the hub always sends it.
    // Marker scanning is the safety net for cameras or proxies that do not.
    while (takeByContentLength() || takeByMarkers()) { }
}

bool MjpegClient::takeByContentLength()
{
    const int headerEnd = m_buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) return false;

    const QByteArray headers = m_buf.left(headerEnd).toLower();
    const int lenAt = headers.indexOf("content-length:");
    if (lenAt < 0) return false;

    int lineEnd = headers.indexOf("\r\n", lenAt);
    if (lineEnd < 0) lineEnd = headers.size();

    bool ok = false;
    const int length = headers.mid(lenAt + 15, lineEnd - lenAt - 15)
                           .trimmed().toInt(&ok);
    if (!ok || length <= 0 || length > kMaxBufferBytes) return false;

    const int bodyAt = headerEnd + 4;
    if (m_buf.size() < bodyAt + length) return false;   // wait for the rest

    publish(m_buf.mid(bodyAt, length));
    m_buf.remove(0, bodyAt + length);
    return true;
}

bool MjpegClient::takeByMarkers()
{
    const int soi = m_buf.indexOf(kSoi);
    if (soi < 0) return false;

    const int eoi = m_buf.indexOf(kEoi, soi + 2);
    if (eoi < 0) return false;

    publish(m_buf.mid(soi, eoi + 2 - soi));
    m_buf.remove(0, eoi + 2);
    return true;
}

void MjpegClient::publish(const QByteArray &jpeg)
{
    QImage image;
    if (!image.loadFromData(jpeg, "JPEG") || image.isNull()) {
        setMessage(tr("Dropped a corrupt frame"));
        return;
    }

    m_frame = image;
    m_frameCount++;
    m_fpsFrames++;
    m_sinceFrame.restart();
    m_retryDelayMs = 500;

    setReceiving(true);
    if (!m_message.isEmpty()) setMessage(QString());

    // Recompute the rate about once a second.
    if (m_fpsWindow.elapsed() >= 1000) {
        m_fps = m_fpsFrames * 1000.0 / m_fpsWindow.elapsed();
        m_fpsFrames = 0;
        m_fpsWindow.restart();
        emit statsChanged();
    }

    emit frameReady();
}

void MjpegClient::checkStall()
{
    if (!m_running) return;
    if (m_sinceFrame.elapsed() < cfg::kVideoStallSeconds * 1000) return;

    setReceiving(false);
    m_fps = 0.0;
    emit statsChanged();
    setMessage(tr("No frames coming through"));

    if (!m_snapshotMode) openStream();   // reopen and hope
}

// --- bookkeeping -----------------------------------------------------------

void MjpegClient::setReceiving(bool receiving)
{
    if (m_receiving == receiving) return;
    m_receiving = receiving;
    emit receivingChanged();
}

void MjpegClient::setMessage(const QString &message)
{
    if (m_message == message) return;
    m_message = message;
    emit messageChanged();
}
