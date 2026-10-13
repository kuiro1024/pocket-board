#pragma once

#include <QElapsedTimer>
#include <QPointF>

class CanvasFrameShakeDetector
{
public:
    void reset();
    void begin(const QPointF &screenPos);
    bool update(const QPointF &screenPos);

private:
    int m_reversals = 0;
    QPointF m_segmentStart;
    QPointF m_segmentDirection;
    QElapsedTimer m_timer;
};
