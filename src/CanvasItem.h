#pragma once
#include <QPointF>
#include <QRectF>
#include <QtGlobal>
#include "GlassContentSource.h"
#include <QColor>

class Renderer;
class CanvasVideo;

class CanvasItem
{
public:
    static constexpr double PADDING     = 25.0;
    static constexpr double HANDLE_SIZE = 35.0;
    static constexpr double MIN_CONTENT_SHORT_SIDE = 96.0;
    static constexpr double MEDIA_REFERENCE_SHORT_SIDE = 270.0 * 0.6;
    static constexpr double MEDIA_BEZEL_RATIO =
        PADDING / MEDIA_REFERENCE_SHORT_SIDE;
    static constexpr double LAUNCHER_BEZEL_RATIO =
        PADDING / 96.0;                     // 25 / ランチャー短辺96

    static double mediaBezelWidth(const QRectF &contentScreenRect) {
        return qMax(
            0.0,
            qMin(contentScreenRect.width(), contentScreenRect.height())
                * MEDIA_BEZEL_RATIO);
    }

    static double launcherBezelWidth(const QRectF &contentScreenRect) {
        return qMax(
            0.0,
            qMin(contentScreenRect.width(), contentScreenRect.height())
                * LAUNCHER_BEZEL_RATIO);
    }

    // 比例ベゼルを共通スケールとして使う。内周=0.25×bezel、
    // 外周=1.25×bezelなので、外周−内周は常にbezelとなり同心を保つ。
    static double mediaBoardCornerRadius(
        const QRectF &contentScreenRect) {
        return mediaBezelWidth(contentScreenRect) * 1.25;
    }

    static double contentCornerRadius(
        const QRectF &contentScreenRect,
        double contentCornerFactor = 1.0) {
        return mediaBezelWidth(contentScreenRect) * 0.25
            * qBound(0.0, contentCornerFactor, 1.0);
    }

    static double launcherBoardCornerRadius(
        const QRectF &contentScreenRect) {
        return launcherBezelWidth(contentScreenRect) * 0.8;
    }

    static double resizeMarkerSize(double bezelWidth) {
        return qMax(0.0, bezelWidth) * (10.0 / PADDING);
    }

    static double resizeMarkerLineWidth(double bezelWidth) {
        return qMax(0.0, bezelWidth) * (1.5 / PADDING);
    }

    virtual ~CanvasItem() = default;

    // canvas座標でのアイテム矩形
    virtual QRectF itemRect() const = 0;

    // dynamic_cast<CanvasVideo*>(item) の代替。
    // CanvasVideoの場合のみoverrideでthisを返す（既定はnullptr）。
    // RTTIの階層探索をせず通常の仮想関数呼び出し1回で済むぶん軽量。
    virtual CanvasVideo*       asVideo()       { return nullptr; }
    virtual const CanvasVideo* asVideo() const { return nullptr; }

    // Rendererで描画（screen座標に変換済みのrectを受け取る）
    // 2026-09-14: サラウンドモード廃止に伴い surroundRing 引数を削除。
    virtual void paintGL(Renderer &r, const QRectF &screenRect,
                         bool isActive, double pulseAlpha,
                         double screenScale,
                         double contentCornerFactor = 1.0) = 0;

    // 2026-10-12: 枠（板・縁の線・リサイズの印）だけを描く。BubbleLayerCache が
    // 影と一緒にテクスチャへ焼き付けるのに使う。対応する型は supportsChromeLayer() を
    // true にし、paintGL 側では Renderer::drawsChrome() が false のとき枠を描かない。
    virtual bool supportsChromeLayer() const { return false; }

    // 2026-10-12: 中身の平均色（グミの影の色に使う）。分からなければ無効な QColor。
    virtual QColor contentAverageColor() const { return QColor(); }
    virtual void paintChromeGL(Renderer & /*r*/, const QRectF & /*screenRect*/,
                               bool /*isActive*/, double /*pulseAlpha*/,
                               double /*screenScale*/,
                               double /*contentCornerFactor*/ = 1.0) {}

    // Qualityガラスがコンテンツ由来の色反射を作るため、直前のpaintGL()で
    // 実際に表示したGLテクスチャとUVを返す。テクスチャを持たない合成UIや
    // ランチャーは既定の無効値を返し、従来の白いライティングだけに戻る。
    virtual GlassContentSource glassContentSource() const { return {}; }

    virtual double boardBezelRatio() const {
        return MEDIA_BEZEL_RATIO;
    }

    double boardBezelWidth() const {
        const QRectF content = itemRect();
        return qMax(
            0.0,
            qMin(content.width(), content.height()) * boardBezelRatio());
    }

    QRectF boardRect() const {
        const double bezel = boardBezelWidth();
        return itemRect().adjusted(-bezel, -bezel, bezel, bezel);
    }

    QRectF handleRect() const {
        const QRectF br = boardRect();
        const double bezel = boardBezelWidth();
        const double handleSize =
            bezel * (HANDLE_SIZE / PADDING);
        const QPointF center(
            br.right() - bezel * 0.5,
            br.bottom() - bezel * 0.5);
        return QRectF(center.x() - handleSize * 0.5,
                      center.y() - handleSize * 0.5,
                      handleSize, handleSize);
    }

    QPointF pos()         const { return m_pos; }
    double  scale()       const { return m_scale; }
    double  liftScale()   const { return m_liftScale; }
    QPointF originPoint() const { return m_originPoint; }

    void setPos(const QPointF &p)        { m_pos = p; }
    void setCenter(const QPointF &center) {
        m_pos += center - itemRect().center();
    }
    void setScale(double s) {
        const double currentScale = qMax(m_scale, 0.000001);
        const QRectF currentRect = itemRect();
        const double baseShortSide =
            qMin(currentRect.width(), currentRect.height()) / currentScale;
        const double minimumScale = baseShortSide > 0.0
            ? MIN_CONTENT_SHORT_SIDE / baseShortSide
            : 1.0;
        m_scale = qMax(minimumScale, s);
    }
    void setLiftScale(double v)          { m_liftScale = v; }
    void setOriginPoint(const QPointF &p){ m_originPoint = p; }

    QPointF liftAnchor() const {
        return m_pos + m_originPoint * m_scale;
    }

protected:
    QPointF m_pos         {0.0, 0.0};
    double  m_scale       {1.0};
    double  m_liftScale   {1.0};
    QPointF m_originPoint {0.0, 0.0};
};

// dynamic_cast<CanvasVideo*>(item) の代替。item が nullptr でも安全
// （dynamic_castと同じくnullptrを返す）。
inline CanvasVideo* canvasItemAsVideo(CanvasItem *item)
{
    return item ? item->asVideo() : nullptr;
}
inline const CanvasVideo* canvasItemAsVideo(const CanvasItem *item)
{
    return item ? item->asVideo() : nullptr;
}
