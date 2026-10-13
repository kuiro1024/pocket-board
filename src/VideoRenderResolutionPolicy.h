#pragma once

#include <QElapsedTimer>
#include <QRectF>
#include <QSize>

// Chooses the per-video render FBO size from the decoded source size and the
// bubble's physical on-screen size. The decoded stream is not changed; only
// mpv's GPU render target is resized.
//
// Resolution is quantized to stable tiers. A short debounce and asymmetric
// hysteresis prevent repeated FBO allocation while zooming around a boundary.
class VideoRenderResolutionPolicy
{
public:
    QSize update(const QSize &nativeSize,
                 const QRectF &logicalScreenRect,
                 qreal devicePixelRatio,
                 const QSize &currentTarget);

    void reset();

    // 2026-09-14: 再生開始時の初期FBOサイズを決めるために公開した。
    // これまで CanvasVideo は初期値にネイティブ解像度（例: 1920x1080）を
    // そのまま入れていたため、mpvの一番最初のレンダーだけが9倍の画素数で
    // 走り、その1フレームだけ paintGL が20ms超になっていた（実測値は
    // CanvasVideo::onUrlFetched() のコメント参照）。縮小側のデバウンスは
    // 700msあるので、放っておくと最初の0.7秒はずっとその大きさで焼き続ける。
    // 初期値をここで量子化しておけば、その無駄が最初から発生しない。
    static QSize quantizedTarget(const QSize &nativeSize, double requiredHeight);

private:

    QElapsedTimer m_clock;
    QSize m_lastCurrentTarget;
    QSize m_pendingTarget;
    qint64 m_pendingSinceMs {-1};
};
