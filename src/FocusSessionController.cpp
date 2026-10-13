#include "FocusSessionController.h"

#include <QtGlobal>

double FocusSessionController::computeFocusZoomLevel(double itemWidth, double itemHeight,
                                                      double targetWidth, double targetHeight)
{
    return qBound(0.1, qMin(targetWidth / itemWidth, targetHeight / itemHeight), 10.0);
}

QRect FocusSessionController::computeUnfocusRestoreGeometry(const QRect &currentGeometry,
                                                            const QRect &focusEntryGeometry)
{
    if (!focusEntryGeometry.isValid())
        return currentGeometry;

    const double ratio =
        static_cast<double>(currentGeometry.width()) * currentGeometry.height() /
        (static_cast<double>(focusEntryGeometry.width()) * focusEntryGeometry.height());

    if (ratio >= 0.75)
        return currentGeometry;

    const int nw = qRound(currentGeometry.width() * 1.2);
    const int nh = qRound(currentGeometry.height() * 1.2);
    QRect targetGeom(0, 0, nw, nh);
    targetGeom.moveCenter(currentGeometry.center());
    return targetGeom;
}

FocusSleepDecision FocusSessionController::decideFocusSleepForState(BubbleState state)
{
    FocusSleepDecision decision;
    decision.resumePlaybackAfterFocus =
        state == BubbleState::Playing
     || state == BubbleState::Buffering
     || state == BubbleState::AudioOnly
     || state == BubbleState::Loading
     || state == BubbleState::Idle;
    decision.shouldEnterFocusSleep =
        decision.resumePlaybackAfterFocus
     || state == BubbleState::Paused
     || state == BubbleState::Ready
     || state == BubbleState::PausedHidden
     || state == BubbleState::ReadyHidden;
    return decision;
}
