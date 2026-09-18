#pragma once

#include <QQmlEngine>
#include <QQuickPaintedItem>

class MjpegClient;
class QPainter;

// ---------------------------------------------------------------------------
//  Paints MjpegClient's current frame into the QML scene graph, scaled to
//  fit while preserving the camera's aspect ratio.
//
//  Reconstructed header -- videosurface.cpp survived, this declaration
//  did not.
// ---------------------------------------------------------------------------

class VideoSurface : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(MjpegClient *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged)
    Q_PROPERTY(int frameWidth READ frameWidth NOTIFY hasFrameChanged)
    Q_PROPERTY(int frameHeight READ frameHeight NOTIFY hasFrameChanged)

public:
    explicit VideoSurface(QQuickItem *parent = nullptr);

    MjpegClient *source() const { return m_source; }
    void         setSource(MjpegClient *source);

    bool hasFrame() const;
    int  frameWidth() const;
    int  frameHeight() const;

    void paint(QPainter *painter) override;

signals:
    void sourceChanged();
    void hasFrameChanged();

private slots:
    void onFrameReady();

private:
    MjpegClient *m_source   = nullptr;
    bool         m_hadFrame = false;
};
