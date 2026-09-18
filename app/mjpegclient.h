#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

// ---------------------------------------------------------------------------
//  Fetches video from the hub and hands decoded frames to VideoSurface.
//  Two modes: parse the hub's multipart MJPEG stream as it arrives
//  (smooth, the default), or poll snapshot.jpg on a timer (choppier, more
//  tolerant of a weak connection) -- see config.h's kUseSnapshotFallback
//  and the app's settings-drawer toggle.
//
//  Reconstructed header -- mjpegclient.cpp survived, this declaration did
//  not.
// ---------------------------------------------------------------------------

class MjpegClient : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString url READ url WRITE setUrl NOTIFY urlChanged)
    Q_PROPERTY(bool snapshotMode READ snapshotMode WRITE setSnapshotMode NOTIFY snapshotModeChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool receiving READ receiving NOTIFY receivingChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(double fps READ fps NOTIFY statsChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY statsChanged)

public:
    explicit MjpegClient(QObject *parent = nullptr);
    ~MjpegClient() override;

    QString url() const { return m_url; }
    void    setUrl(const QString &url);

    bool snapshotMode() const { return m_snapshotMode; }
    void setSnapshotMode(bool on);

    bool    running()    const { return m_running; }
    bool    receiving()  const { return m_receiving; }
    QString message()    const { return m_message; }
    double  fps()         const { return m_fps; }
    int     frameCount()   const { return m_frameCount; }

    // The most recently decoded frame. Used by VideoSurface::paint(); may
    // be null before the first frame arrives.
    const QImage &currentFrame() const { return m_frame; }

public slots:
    void start();
    void stop();
    void restart();

signals:
    void urlChanged();
    void snapshotModeChanged();
    void runningChanged();
    void receivingChanged();
    void messageChanged();
    void statsChanged();
    void frameReady();

private slots:
    void onStreamData();
    void onStreamFinished();
    void requestSnapshot();
    void onSnapshotFinished(QNetworkReply *reply);
    void checkStall();

private:
    void abortReply();
    void openStream();
    void parseBuffer();
    bool takeByContentLength();
    bool takeByMarkers();
    void publish(const QByteArray &jpeg);

    void setReceiving(bool receiving);
    void setMessage(const QString &message);

    QNetworkAccessManager *m_net;

    QString m_url;
    bool    m_snapshotMode;
    bool    m_running = false;

    QTimer m_snapshotTimer;
    QTimer m_stallTimer;

    QByteArray     m_buf;
    QNetworkReply *m_reply           = nullptr;
    bool           m_snapshotPending = false;
    int            m_retryDelayMs    = 500;

    QImage  m_frame;
    int     m_frameCount = 0;
    int     m_fpsFrames  = 0;
    double  m_fps        = 0.0;
    bool    m_receiving  = false;
    QString m_message;

    QElapsedTimer m_sinceFrame;
    QElapsedTimer m_fpsWindow;
};
