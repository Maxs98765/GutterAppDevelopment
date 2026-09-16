#include "videosurface.h"

#include <QPainter>

#include "mjpegclient.h"

VideoSurface::VideoSurface(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    // Smooth scaling matters here: a 320x240 frame is being blown up to fill
    // a phone screen, and nearest-neighbour looks awful at that ratio.
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
    setSmooth(true);
    setOpaquePainting(false);
}

void VideoSurface::setSource(MjpegClient *source)
{
    if (m_source == source) return;

    if (m_source) disconnect(m_source, nullptr, this, nullptr);

    m_source = source;

    if (m_source) {
        connect(m_source, &MjpegClient::frameReady, this,
                &VideoSurface::onFrameReady);
    }

    emit sourceChanged();
    emit hasFrameChanged();
    update();
}

bool VideoSurface::hasFrame() const
{
    return m_source && !m_source->currentFrame().isNull();
}

int VideoSurface::frameWidth() const
{
    return m_source ? m_source->currentFrame().width() : 0;
}

int VideoSurface::frameHeight() const
{
    return m_source ? m_source->currentFrame().height() : 0;
}

void VideoSurface::onFrameReady()
{
    if (!m_hadFrame) {
        m_hadFrame = true;
        emit hasFrameChanged();
    }
    update();
}

void VideoSurface::paint(QPainter *painter)
{
    if (!m_source) return;

    const QImage &frame = m_source->currentFrame();
    if (frame.isNull()) return;

    // Fit inside the item, preserving the camera's aspect ratio.
    const QSizeF target = frame.size().scaled(QSizeF(width(), height()),
                                              Qt::KeepAspectRatio);
    const QRectF dest((width()  - target.width())  / 2.0,
                      (height() - target.height()) / 2.0,
                      target.width(), target.height());

    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(dest, frame);
}
