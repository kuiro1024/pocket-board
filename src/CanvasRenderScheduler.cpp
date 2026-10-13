#include "CanvasRenderScheduler.h"

#include <QtGlobal>

void CanvasRenderScheduler::setViewportSize(const QSize &logicalSize)
{
    const QSize bounded(qMax(0, logicalSize.width()),
                        qMax(0, logicalSize.height()));
    if (bounded == m_viewportSize)
        return;

    m_viewportSize = bounded;
    invalidateFull(CanvasRenderReason::Expose);
}

void CanvasRenderScheduler::invalidateFull(CanvasRenderReason reason)
{
    m_pendingReasons |= reason;
    m_pendingFull = true;
    if (m_viewportSize.isValid())
        m_pendingDamage = QRegion(QRect(QPoint(), m_viewportSize));
}

void CanvasRenderScheduler::invalidateRect(const QRectF &logicalRect,
                                           CanvasRenderReason reason,
                                           qreal padding)
{
    m_pendingReasons |= reason;
    if (reason == CanvasRenderReason::Backdrop) {
        QRect backdrop = logicalRect.adjusted(-padding, -padding, padding, padding)
                             .toAlignedRect();
        if (m_viewportSize.isValid())
            backdrop = backdrop.intersected(QRect(QPoint(), m_viewportSize));
        if (!backdrop.isEmpty())
            m_pendingBackdropDamage += QRegion(backdrop);
        return;
    }
    if (m_pendingFull)
        return;

    QRect damage = logicalRect.adjusted(-padding, -padding, padding, padding)
                       .toAlignedRect();
    if (m_viewportSize.isValid())
        damage = damage.intersected(QRect(QPoint(), m_viewportSize));
    if (!damage.isEmpty())
        m_pendingDamage += QRegion(damage);
}

CanvasRenderRequest CanvasRenderScheduler::takeRequest()
{
    CanvasRenderRequest request;
    const QRect viewport(QPoint(), m_viewportSize);

    const CanvasRenderReasons sceneReasons =
        m_pendingReasons & ~CanvasRenderReasons(CanvasRenderReason::Backdrop);
    if (m_pendingReasons == CanvasRenderReason::None) {
        // Expose, platform repaint, or a legacy update() call not migrated yet.
        request.reasons = CanvasRenderReason::External;
        request.damage = QRegion(viewport);
        request.full = true;
    } else if (sceneReasons == CanvasRenderReason::None) {
        // 2026-09-16: UIレイヤーだけの更新。シーンのダメージは無い。
        request.reasons = CanvasRenderReason::Backdrop;
        request.full = false;
        request.damage = m_pendingBackdropDamage;
    } else {
        request.reasons = m_pendingReasons;
        request.full = m_pendingFull;
        request.damage = request.full ? QRegion(viewport) : m_pendingDamage;
        if (request.damage.isEmpty()) {
            request.damage = QRegion(viewport);
            request.full = true;
        }
    }

    request.backdropDamage = m_pendingBackdropDamage;
    m_pendingReasons = {};
    m_pendingDamage = {};
    m_pendingBackdropDamage = {};
    m_pendingFull = false;
    return request;
}

qint64 CanvasRenderScheduler::regionArea(const QRegion &region)
{
    qint64 area = 0;
    for (const QRect &rect : region)
        area += static_cast<qint64>(rect.width()) * rect.height();
    return area;
}

QString CanvasRenderScheduler::recordPresented(
    const CanvasRenderRequest &request, qint64 elapsedMilliseconds,
    bool reusedStaticLayer, bool reusedSceneLayer)
{
    if (!m_reportClock.isValid())
        m_reportClock.start();

    ++m_reportFrames;
    if (request.full)
        ++m_reportFullFrames;
    if (request.reasons.testFlag(CanvasRenderReason::VideoFrame))
        ++m_reportVideoFrames;
    if (request.reasons.testFlag(CanvasRenderReason::Camera))
        ++m_reportCameraFrames;
    if (request.reasons.testFlag(CanvasRenderReason::UiAnimation))
        ++m_reportUiFrames;
    if (request.reasons.testFlag(CanvasRenderReason::External))
        ++m_reportImplicitFrames;
    if (reusedStaticLayer)
        ++m_reportRetainedFrames;
    if (reusedSceneLayer)
        ++m_reportSceneReuseFrames;
    m_reportDamageArea += regionArea(request.damage);
    m_reportPaintMs += qMax<qint64>(0, elapsedMilliseconds);

    const qint64 elapsed = m_reportClock.elapsed();
    if (elapsed < 5000)
        return {};

    const double seconds = qMax(0.001, elapsed / 1000.0);
    const double viewportArea = qMax<qint64>(
        1, static_cast<qint64>(m_viewportSize.width()) * m_viewportSize.height());
    const double averageDamagePercent = m_reportFrames > 0
        ? 100.0 * m_reportDamageArea / (viewportArea * m_reportFrames)
        : 0.0;
    const double averagePaintMs = m_reportFrames > 0
        ? static_cast<double>(m_reportPaintMs) / m_reportFrames
        : 0.0;

    const QString report = QStringLiteral(
        "[RenderSchedule] fps=%1 frames=%2 full=%3 retained=%4 video=%5 "
        "camera=%6 ui=%7 legacy/expose=%8 avgDamage=%9% avgPaint=%10ms uiLayerOnly=%11")
        .arg(m_reportFrames / seconds, 0, 'f', 1)
        .arg(m_reportFrames)
        .arg(m_reportFullFrames)
        .arg(m_reportRetainedFrames)
        .arg(m_reportVideoFrames)
        .arg(m_reportCameraFrames)
        .arg(m_reportUiFrames)
        .arg(m_reportImplicitFrames)
        .arg(averageDamagePercent, 0, 'f', 1)
        .arg(averagePaintMs, 0, 'f', 2)
        .arg(m_reportSceneReuseFrames);

    m_reportClock.restart();
    m_reportFrames = 0;
    m_reportFullFrames = 0;
    m_reportVideoFrames = 0;
    m_reportCameraFrames = 0;
    m_reportUiFrames = 0;
    m_reportImplicitFrames = 0;
    m_reportRetainedFrames = 0;
    m_reportSceneReuseFrames = 0;
    m_reportDamageArea = 0;
    m_reportPaintMs = 0;
    return report;
}
