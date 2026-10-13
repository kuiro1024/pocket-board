#pragma once
#include "CanvasItem.h"
#include "Renderer.h"
#include "MediaSource.h"
#include <QPixmap>
#include <QImage>
#include <QString>

class CanvasImage : public CanvasItem
{
public:
    explicit CanvasImage(const QPixmap &pixmap, const QPointF &pos, double scale);
    ~CanvasImage() override;

    QRectF itemRect() const override;
    void   paintGL(Renderer &r, const QRectF &screenRect,
                   bool isActive, double pulseAlpha,
                   double screenScale,
                   double contentCornerFactor = 1.0) override;
    GlassContentSource glassContentSource() const override;
    bool supportsChromeLayer() const override { return true; }
    QColor contentAverageColor() const override;
    void paintChromeGL(Renderer &r, const QRectF &screenRect, bool isActive,
                       double pulseAlpha, double screenScale,
                       double contentCornerFactor = 1.0) override;
    void   releaseGlResources(Renderer &r);

    const QPixmap &pixmap() const { return m_pixmap; }
    QString sourceUrl() const { return m_source.originalPathOrUrl; }
    QString sourceFilePath() const;
    QString title() const { return m_source.title; }
    MediaKind mediaKind() const { return m_source.kind; }
    const MediaSource &mediaSource() const { return m_source; }
    void setMediaSource(const MediaSource &source) { m_source = source; }
    void setSourceUrl(const QString &url);
    void setSourceFilePath(const QString &path);
    void setLoadFailed(bool failed) { m_loadFailed = failed; }
    bool loadFailed() const { return m_loadFailed; }

private:
    QPixmap m_pixmap;
    // 2026-10-12: 平均色のキャッシュ（QPixmap::cacheKey が変わったら作り直す）
    mutable QColor m_avgColor;
    mutable qint64 m_avgColorKey = -1;
    MediaSource m_source;
    bool m_loadFailed {false};
    GLuint  m_texId {0};
};
