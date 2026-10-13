#pragma once

#include <QRect>
#include <QSize>
#include <QSet>
#include <QHash>
#include <QtGlobal>

#include "BubbleState.h"

class CanvasVideo;

// フォーカス開始時、他バブルを FocusSleep へ送るかどうかの判定結果。
// CanvasWidget::beginFocusPlaybackPolicy() が保持する
// m_focusSleepBookkeeping.videosAsleep / resumeAfterFocus の元になる値そのもの。
struct FocusSleepDecision
{
    bool shouldEnterFocusSleep = false;
    bool resumePlaybackAfterFocus = false;
};

// CanvasWidget::focusItem() / unfocusItem() だけが読み書きする、
// 「フォーカス中のウィンドウジオメトリ復元」に関する一時状態をまとめたもの。
// 元は CanvasWidget の個別メンバ変数(m_preWindowGeometry 等)だったものを
// 1箇所にまとめただけで、フィールド名・意味・初期値は変更していない。
// ロジック自体はまだ CanvasWidget::focusItem()/unfocusItem() に残っている
// （このstructはただのデータ置き場で、副作用は持たない）。
struct FocusWindowGeometryState
{
    // フォーカス開始時点のウィンドウジオメトリ（ウィンドウサイズロック用の復元先）。
    QRect preWindowGeometry;
    // フォーカス確定時点（非最大化パスはリサイズアニメ完了後）のウィンドウジオメトリ。
    QRect focusEntryGeometry;
    // 2026-07-18追加: フォーカス中の「対比フィットの基準サイズ」。
    // focusItem()（非最大化パス）/ notifyWindowMaximizedChanged()で
    // animateToAspect()を開始する直前の（＝対比フィット前の）ウィンドウ
    // サイズを記録する。連続再生（nextUrl）で対比の違う動画へ切り替わる
    // たびに refitFocusedWindowForAspectChange() が「現在のウィンドウ
    // サイズの内側」へフィットし直すと、16:9→9:16→16:9のように対比が
    // 変わるたび幅→高さ→幅と削られてウィンドウが単調に縮んでいくため、
    // 毎回この基準サイズからフィットし直すことでサイズを安定させる。
    // 無効（QSize()）の間は従来通り現在サイズ基準で計算される
    // （MainWindow::computeAspectTargetGeometry()のbaseSize引数参照）。
    QSize aspectFitBaseSize;
    // フォーカス開始時、ウィンドウが全画面（最大化）状態だったか。
    bool preWindowMaximized {false};
    // 「バブル全画面解除時にフォーカス開始前のウィンドウサイズへ戻す」設定が
    // 今回のフォーカスセッションで有効か。
    bool windowSizeLockFocusActive {false};
    // 2026-10-11: 今回のフォーカスセッションが「ウィンドウ固定」モードか。
    // trueの間はウィンドウのリサイズ/復元を一切行わず、動画は余白(黒)つきで収める。
    bool windowFixedFocusActive {false};
};

// CanvasWidget::beginFocusPlaybackPolicy() / endFocusPlaybackPolicy() /
// forgetFocusPausedVideo() だけが読み書きする、「フォーカス中に他の動画を
// FocusSleepさせておくための一時状態」をまとめたもの。元は個別メンバ変数
// だったものを1箇所にまとめただけで、意味・初期値は変更していない。
// ロジック自体（enterFocusSleep()呼び出し、遅延ウェイクのスケジュール等）は
// まだ CanvasWidget 側に残っている（このstructはただのデータ置き場）。
struct FocusSleepBookkeeping
{
    // 現在のフォーカスセッションでFocusSleepにした動画の集合。
    QSet<CanvasVideo*> videosAsleep;
    // 上記各動画について、フォーカス解除時に再生を再開すべきか。
    QHash<CanvasVideo*, bool> resumeAfterFocus;
    // 現在フォーカスポリシーが有効か（フォーカス対象がある間true）。
    bool active {false};
    // endFocusPlaybackPolicy()呼び出しごとに進む世代番号。詳細は
    // CanvasWidget::endFocusPlaybackPolicy() のコメント参照。
    quint64 generation {0};
};

// Phase 6 (CanvasWidget/MainWindow のフォーカス境界整理) の最初のステップ。
//
// ここには「入力に対して出力が決まるだけの純粋な計算」のみを置く。
// フォーカス状態遷移、Sleep/FocusSleep、ウィンドウ全画面判定、タイマーなどの
// 副作用を持つロジックはまだ CanvasWidget 側に残し、後続ステップで移す。
class FocusSessionController
{
public:
    // フォーカス対象アイテムの矩形サイズ (itemWidth x itemHeight) を
    // 目標表示サイズ (targetWidth x targetHeight) に収めるためのカメラズーム値を返す。
    // CanvasWidget::focusItem() の既存計算と同一の式・同一の
    // クランプ範囲 (0.1〜10.0) を維持している。
    static double computeFocusZoomLevel(double itemWidth, double itemHeight,
                                        double targetWidth, double targetHeight);

    // フォーカス解除時、ウィンドウを「フォーカス開始前のジオメトリ」へ戻す際、
    // 途中でウィンドウが大きく縮んでいた場合に少し広げ直すための復元ジオメトリを返す。
    // CanvasWidget::unfocusItem() の既存計算と同一のしきい値 (面積比 0.75) と
    // 拡大率 (1.2倍) を維持している。
    static QRect computeUnfocusRestoreGeometry(const QRect &currentGeometry,
                                               const QRect &focusEntryGeometry);

    // 他バブル(フォーカス対象以外)の現在の BubbleState から、
    // FocusSleep へ送るべきか・送った場合に解除後再生を再開すべきかを判定する。
    // CanvasWidget::beginFocusPlaybackPolicy() の既存条件式と同一。
    // 副作用（enterFocusSleep() 呼び出し、GL context 切り替え、集合への追加等）は
    // 一切持たない。呼び出し側がこの結果を見て副作用を実行する。
    static FocusSleepDecision decideFocusSleepForState(BubbleState state);
};
