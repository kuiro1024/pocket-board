#include "VideoRenderResolutionPolicy.h"

#include <QtGlobal>
#include <cmath>

namespace {
constexpr int kResolutionSteps[] = {
    240, 360, 480, 720, 1080, 1440, 2160
};

constexpr qint64 kUpscaleDelayMs = 180;
constexpr qint64 kDownscaleDelayMs = 700;
constexpr double kUpscaleHysteresis = 1.08;
constexpr double kDownscaleHysteresis = 0.85;
}

QSize VideoRenderResolutionPolicy::quantizedTarget(
    const QSize &nativeSize, double requiredHeight)
{
    if (!nativeSize.isValid())
        return {};

    int chosenHeight = nativeSize.height();
    for (const int step : kResolutionSteps) {
        if (step >= requiredHeight) {
            chosenHeight = qMin(step, nativeSize.height());
            break;
        }
    }
    chosenHeight = qBound(1, chosenHeight, nativeSize.height());

    int chosenWidth = qRound(
        static_cast<double>(nativeSize.width()) * chosenHeight
        / qMax(1, nativeSize.height()));
    chosenWidth = qBound(1, chosenWidth, nativeSize.width());
    return QSize(chosenWidth, chosenHeight);
}

QSize VideoRenderResolutionPolicy::update(
    const QSize &nativeSize,
    const QRectF &logicalScreenRect,
    qreal devicePixelRatio,
    const QSize &currentTarget)
{
    if (!nativeSize.isValid() || logicalScreenRect.isEmpty())
        return currentTarget.isValid() ? currentTarget : nativeSize;

    if (!m_clock.isValid())
        m_clock.start();

    const double dpr = qMax(0.1, static_cast<double>(devicePixelRatio));
    const double physicalWidth = std::abs(logicalScreenRect.width()) * dpr;
    const double physicalHeight = std::abs(logicalScreenRect.height()) * dpr;
    const double nativeAspectHeight =
        physicalWidth * nativeSize.height() / qMax(1, nativeSize.width());
    const double requiredHeight = qMax(2.0, qMax(physicalHeight, nativeAspectHeight));

    QSize desired = quantizedTarget(nativeSize, requiredHeight);
    const QSize current = currentTarget.isValid() ? currentTarget : desired;

    // An externally supplied target (new source, restore, audio/video switch)
    // invalidates a pending decision from the previous target.
    if (current != m_lastCurrentTarget) {
        m_lastCurrentTarget = current;
        m_pendingTarget = {};
        m_pendingSinceMs = -1;
    }

    // If the source aspect changed, do not preserve an incompatible FBO while
    // waiting for the normal debounce window.
    const double currentAspect =
        static_cast<double>(current.width()) / qMax(1, current.height());
    const double nativeAspect =
        static_cast<double>(nativeSize.width()) / qMax(1, nativeSize.height());
    if (std::abs(currentAspect - nativeAspect) > 0.02)
        return desired;

    // Never preserve an oversized initial/previous target for a low-resolution
    // source. Hysteresis only applies between valid targets at or below native.
    if (current.width() > nativeSize.width()
        || current.height() > nativeSize.height()) {
        return desired;
    }

    if (desired.height() > current.height()
        && requiredHeight <= current.height() * kUpscaleHysteresis) {
        desired = current;
    } else if (desired.height() < current.height()
               && requiredHeight >= desired.height() * kDownscaleHysteresis) {
        desired = current;
    }

    if (desired == current) {
        m_pendingTarget = {};
        m_pendingSinceMs = -1;
        return current;
    }

    const qint64 nowMs = m_clock.elapsed();
    if (desired != m_pendingTarget) {
        m_pendingTarget = desired;
        m_pendingSinceMs = nowMs;
        return current;
    }

    const qint64 delayMs = desired.height() > current.height()
        ? kUpscaleDelayMs : kDownscaleDelayMs;
    if (nowMs - m_pendingSinceMs < delayMs)
        return current;

    m_pendingTarget = {};
    m_pendingSinceMs = -1;
    m_lastCurrentTarget = desired;
    return desired;
}

void VideoRenderResolutionPolicy::reset()
{
    m_clock.invalidate();
    m_lastCurrentTarget = {};
    m_pendingTarget = {};
    m_pendingSinceMs = -1;
}
