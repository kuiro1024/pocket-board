#pragma once
#include <QObject>
#include <QPointF>
#include <QVector>
#include <QPair>
#include <QVariantAnimation>
#include <QColor>
#include <QList>
#include <functional>

class CanvasVideo;

struct FlowLink {
    CanvasVideo *from      = nullptr;
    CanvasVideo *to        = nullptr;
    bool        triggered  = false; // 一度Toを起動したら次のfromEnded/Stoppedで再起動しない
    // 2026-07-03: fromのEOFが近づいた時に一度だけto側の先読みを
    // 開始するためのワンショットフラグ。triggeredと同じタイミングで
    // リセットする。
    bool        preloadTriggered = false;
};

class FlowMode : public QObject
{
    Q_OBJECT

public:
    explicit FlowMode(QObject *parent = nullptr);

    bool  isActive()      const { return m_active; }
    float glowAlpha()     const { return m_glowAlpha; }
    float overlayAlpha()  const { return m_overlayAlpha; }

    void toggle();

    // canvas座標系のズームを渡す（線判定の閾値計算に使用）
    void setCanvasZoom(double zoom) { m_canvasZoom = zoom; }

    void beginStroke(const QPointF &canvasPos, bool isErase);
    void updateStroke(const QPointF &canvasPos, CanvasVideo *hitVideo,
                      std::function<QPointF(CanvasVideo*)> centerOf);
    void endStroke();

    void addLink(CanvasVideo *from, CanvasVideo *to);
    void removeLinksThrough(CanvasVideo *vid);
    void removeItem(CanvasVideo *vid);
    // セッション復元時など、既存のリンクを古いバブルのポインタごと
    // まとめて破棄したい場合に呼ぶ。
    void clearLinks();

    // FlowMode ON時に呼ぶ: 全動画を一時停止
    void applyStandby(const QList<CanvasVideo*> &allVideos);
    // FlowMode OFF時に呼ぶ: チェーン先頭から再生開始
    void startChain();
    // syncLogicから毎フレーム呼ぶ: 再生チェーンを監視して次を再生
    void tickPlayback();

    // vidがFlowのリンク先（to）で、かつ停止中（待機中）かどうか
    // → 再生中のtoはクリック操作（一時停止/再開）を許可する
    bool isStandby(CanvasVideo *vid) const;

    const QVector<FlowLink> &links()      const { return m_links; }
    const QVector<QPointF>  &strokePath() const { return m_strokePath; }
    bool         isErasing()       const { return m_isErase; }
    CanvasVideo *lastStrokeVideo() const { return m_lastHit; }

signals:
    void changed();
    // tickPlaybackでToを再生開始した時に発火（全画面切り替え用）
    void videoStarted(CanvasVideo *vid);

private:
    bool  m_active       {false};
    bool  m_stroking     {false};
    bool  m_isErase      {false};

    float m_overlayAlpha {0.0f};
    float m_glowAlpha    {0.0f};
    QVariantAnimation *m_enterAnim  = nullptr;
    QVariantAnimation *m_glowAnim   = nullptr;

    double m_canvasZoom {1.0};

    QVector<FlowLink> m_links;
    QVector<QPointF>  m_strokePath;
    CanvasVideo      *m_lastHit     = nullptr;
    CanvasVideo      *m_strokeStart = nullptr;
};
