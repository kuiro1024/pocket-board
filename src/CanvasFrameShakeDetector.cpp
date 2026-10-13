#include "CanvasFrameShakeDetector.h"

#include <cmath>

void CanvasFrameShakeDetector::reset()
{
    m_reversals = 0;
    m_segmentDirection = QPointF(0.0, 0.0);
    m_timer.invalidate();
}

void CanvasFrameShakeDetector::begin(const QPointF &screenPos)
{
    reset();
    m_segmentStart = screenPos;
}

bool CanvasFrameShakeDetector::update(const QPointF &screenPos)
{
    static constexpr double kShakeMinSegmentPx = 22.0;
    static constexpr int kShakeResetMs = 700;
    static constexpr int kShakeRequiredReversals = 4;

    if (m_timer.isValid() && m_timer.elapsed() > kShakeResetMs) {
        m_reversals = 0;
        m_segmentDirection = QPointF(0.0, 0.0);
        m_segmentStart = screenPos;
    }

    const QPointF segment = screenPos - m_segmentStart;
    const double segmentLength = std::hypot(segment.x(), segment.y());
    if (segmentLength < kShakeMinSegmentPx)
        return false;

    if (!m_segmentDirection.isNull()) {
        const double dot = segment.x() * m_segmentDirection.x()
                         + segment.y() * m_segmentDirection.y();
        if (dot < 0.0) {
            ++m_reversals;
            m_timer.restart();
        }
    } else {
        m_timer.restart();
    }

    m_segmentDirection = segment;
    m_segmentStart = screenPos;
    return m_reversals >= kShakeRequiredReversals;
}
