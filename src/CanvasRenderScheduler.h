#pragma once

#include <QElapsedTimer>
#include <QFlags>
#include <QRectF>
#include <QRegion>
#include <QSize>
#include <QString>

enum class CanvasRenderReason : quint32
{
    None        = 0,
    External    = 1u << 0,
    Expose      = 1u << 1,
    Camera      = 1u << 2,
    VideoFrame  = 1u << 3,
    UiAnimation = 1u << 4,
    Scene       = 1u << 5,
    Lighting    = 1u << 6,
    // 2026-09-16: UIレイヤー（BackdropBlur のぼかしとガラス）の形だけが
    // 変わった。キャンバスの中身は変わっていないので、CanvasWidget は
    // 保持しているシーンレイヤーを貼ってUIレイヤーだけを描き直せる。
    // このreasonのダメージは静的レイヤー判定用の damage には混ぜない
    // （混ぜると動画再生中の静的レイヤー再利用まで無効になるため）。
    Backdrop    = 1u << 7
};
Q_DECLARE_FLAGS(CanvasRenderReasons, CanvasRenderReason)
Q_DECLARE_OPERATORS_FOR_FLAGS(CanvasRenderReasons)

struct CanvasRenderRequest
{
    CanvasRenderReasons reasons;
    QRegion damage;
    bool full {true};
    // Backdrop だけのダメージ（統計用）
    QRegion backdropDamage;
};

// Central invalidation state for CanvasWidget. Producers describe what changed;
// the compositor consumes one coalesced request at the next paint. The current
// full renderer remains the visual fallback while retained passes are added.
class CanvasRenderScheduler
{
public:
    void setViewportSize(const QSize &logicalSize);
    void invalidateFull(CanvasRenderReason reason);
    void invalidateRect(const QRectF &logicalRect,
                        CanvasRenderReason reason,
                        qreal padding = 0.0);

    CanvasRenderRequest takeRequest();

    // Returns a compact diagnostic line approximately every five seconds.
    // Empty means that no report is due yet.
    QString recordPresented(const CanvasRenderRequest &request,
                            qint64 elapsedMilliseconds,
                            bool reusedStaticLayer = false,
                            bool reusedSceneLayer = false);

private:
    static qint64 regionArea(const QRegion &region);

    QSize m_viewportSize;
    CanvasRenderReasons m_pendingReasons;
    QRegion m_pendingDamage;
    QRegion m_pendingBackdropDamage;
    bool m_pendingFull {true};

    QElapsedTimer m_reportClock;
    quint64 m_reportFrames {0};
    quint64 m_reportFullFrames {0};
    quint64 m_reportVideoFrames {0};
    quint64 m_reportCameraFrames {0};
    quint64 m_reportUiFrames {0};
    quint64 m_reportImplicitFrames {0};
    quint64 m_reportRetainedFrames {0};
    quint64 m_reportSceneReuseFrames {0};
    qint64 m_reportDamageArea {0};
    qint64 m_reportPaintMs {0};
};
