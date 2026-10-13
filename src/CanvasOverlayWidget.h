#pragma once

#include "Blur.h"

#include <QElapsedTimer>
#include <QVariantAnimation>
#include <QRect>
#include <QWidget>

class CanvasWidget;

// Window-space UI lives outside CanvasWidget's OpenGL surface so lightweight
// progress, feedback, border and tooltip updates do not present the canvas FBO.
class CanvasOverlayWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit CanvasOverlayWidget(CanvasWidget *canvas, QWidget *parent = nullptr);

public slots:
    void refreshPlaybackUi();
    // 2026-09-12: 再生中の行のイコライザだけが動いているフレーム用。
    // 帯（上96px＋下144px、いずれもウィンドウ幅いっぱい）を毎フレーム
    // 無効化せず、渡された矩形だけを描き直す。
    void refreshPlaybackEqualizer(const QRect &damage);
    void refreshFocusUi();
    void refreshBorderUi();
    void refreshLauncherTooltipUi();

    // 2026-09-15: 島の設定パネル展開中に、キャンバス上のUI（ラベル・再生バー・
    // フォーカス表示・ランチャーのツールチップ）をフェードで隠す。
    // 島のブラーはキャンバスのFBOにしか効かず、このウィジェットが描くUIは
    // 島の半透明面越しにくっきり透けてしまうため。
    // ウィンドウ外周の境界線（paintBorderOverlay）はウィンドウ枠の表現なので
    // 対象外（隠さない）。
    void setUiSuppressed(bool suppressed);

    // 2026-09-17: 長押し2倍速の間、動画UI（ラベル・リスト・再生バー）と
    // ランチャーのツールチップをフェードで隠す（一般的な動画プレイヤーと同じ）。
    // 2倍速バッジ・シーク表示（paintFocusOverlay）は隠さない。
    // 設定パネルの非表示（m_uiAlpha）とは独立した係数で、描画時に掛け算する。
    void setSpeedHoldSuppressed(bool suppressed);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    CanvasWidget *m_canvas = nullptr;
    QRect m_lastLauncherTooltipRect;
    // 直近にラベルのコントローラ（リスト）が占めていた領域。畳んで消えた
    // フレームでも、そこを消すために前回分を巻き込んで無効化する。
    QRect m_lastLabelPanelRect;
    // 再生バーやバッジなど、このウィジェットが描くUI部品の背後をぼかすための
    // 領域リスト。paintEvent中に各描画コードがadd()していく(Blur.h参照)。
    BackdropBlur::Regions m_blurRegions;

    // ---- 設定パネル展開中の非表示（2026-09-15）----
    // UI全体に掛ける不透明度。paintEvent() 冒頭で painter.setOpacity() に
    // 入れる（描画コード側はいずれも painter.opacity() に掛け算する流儀
    // なので、そのまま全要素に効く）。ブラー領域には
    // Regions::setOpacityScale() で同じ値を掛ける。
    // 型はdouble固定（QVariantAnimationはfloatを補間しないため。
    // DynamicIsland::m_masterVolumeGestureAlpha のコメント参照）。
    double m_uiAlpha = 1.0;
    bool   m_uiSuppressed = false;
    QVariantAnimation *m_uiAlphaAnim = nullptr;
    // 完全に隠れていて、フェードも走っていない。この間は再生バー等の
    // refresh*() を空振りさせる（描かない絵のために無効化しない）。
    bool uiFullyHidden() const;

    // ---- 長押し2倍速中の非表示（2026-09-17）----
    double m_speedHoldAlpha = 1.0;
    bool   m_speedHoldSuppressed = false;
    QVariantAnimation *m_speedHoldAlphaAnim = nullptr;
    // 動画UI（再生バー・ラベル・ツールチップ）が完全に隠れていてフェードも
    // 走っていない。設定パネル側・2倍速側どちらの理由でも真。
    bool playbackUiFullyHidden() const;
    // フェード中の1フレーム分の無効化。各 refresh*() と同じ細い矩形を使う
    // （全面 update() にしない）。
    void invalidateSuppressibleUi();

    // 2026-09-14: このウィジェットのQPainter描画にかかっている時間の計測。
    //
    // 【なぜ要るか】[RenderSchedule] の avgPaint はGLパス（CanvasWidget::
    // paintGL）だけの時間で、実測1〜2ms。それなのに実fpsは38〜50しか出て
    // いない＝残りの十数msはGLの外にある。このウィジェットは半透明の
    // QWidgetで、CPUのバックストアへ描いてから合成されるため、
    // ウィンドウが大きいほど重くなる位置にいる。まずここの実測値を取る。
    // 出力は2秒ごとの [CanvasOverlay][stat]。
    int    m_statFrames = 0;
    double m_statMsSum = 0.0;
    double m_statMsMax = 0.0;
    qint64 m_statAreaSum = 0;   // 塗ったピクセル数（イベント領域の面積）
    qint64 m_statRectsSum = 0;  // イベント領域の矩形数（帯なのか全面なのか）
    qint64 m_statBboxAreaSum = 0; // 同・外接矩形の面積
    int    m_statFullFrames = 0;  // 外接矩形がウィンドウ全体だったフレーム数
    // 内訳（paintEventが呼ぶ4つの描画それぞれの所要時間の合計）。
    double m_statPlaybackMsSum = 0.0;
    double m_statFocusMsSum    = 0.0;
    double m_statBorderMsSum   = 0.0;
    double m_statLauncherMsSum = 0.0;
    QElapsedTimer m_statWindow;

};
