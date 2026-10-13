#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <functional>

class QTimer;

// 2026-07-17: マウスホイールの1コリごとにm_cameraZoomを即座に書き換えると
// 「トントン」と段階的にジャンプして見える。この見た目を、ズーム値そのものを
// 毎フレーム目標値へなめらかに追従させる（時間ベースの指数イージング）方式に
// 変えるためのヘルパー。CameraPanInertia（ドラッグパンの慣性）と同じ
// 「QTimer駆動でtickCallbackを呼ぶ」構成に揃えてある。
//
// 使い方:
//   - ホイールが来るたびに: retarget(現在のズーム値, 新しい目標ズーム値)
//     を呼ぶ。イージング中に連続してホイールが来た場合はイージング途中の
//     値からそのまま新しい目標へなめらかに繋がる（毎回起点をリセットしない）。
//   - 他のカメラ操作（ドラッグパン開始、フォーカスイン/アウトアニメ開始等）
//     が割り込んできたら stop() で即座に中断する。
class CameraZoomEase : public QObject
{
    Q_OBJECT
public:
    // 毎フレーム呼ばれる。イージング後の新しいズーム値を渡す。呼び出し側は
    // ここでm_cameraZoomを更新し、カーソル位置を基準にした再センタリングや
    // クランプ処理を行って再描画する。続行できない場合（対象が消えた等）は
    // falseを返すとイージングはその場で停止する。
    using TickCallback = std::function<bool(double zoom)>;

    explicit CameraZoomEase(QObject *parent = nullptr);

    void setTickCallback(TickCallback cb);

    // 現在のズーム値currentを起点（イージング中で無い場合のみ採用）とし、
    // 目標ズーム値targetへ向けてイージングを開始/更新する。
    void retarget(double current, double target);

    // 即座に停止する（次にretarget()するまで再開しない）。
    void stop();

    bool isActive() const;

    // 2026-09-23: 画面の表示（vsync）に合わせて1歩進める。CanvasWidget の
    // frameSwapped から呼ぶ。QTimer(16ms) だけで回すと、Windowsのタイマー
    // 分解能（15.6ms）や他の再描画要求とのずれで、表示が1フレームおきに
    // 抜けて30fps前後に見えることがあった。表示が終わるたびに次の値を
    // 出せば、描画はvsyncと同じ間隔で連続する。QTimerは表示が止まって
    // いるとき（最初の1歩・終盤の間引き中）の保険として残す。
    void stepFromVsync();

private:
    void onTimeout();

    double        m_current = 1.0;
    double        m_target  = 1.0;
    QElapsedTimer m_clock;
    QTimer       *m_timer = nullptr;
    TickCallback  m_tickCallback;

    // ---- 可変レート用（2026-09-14 Phase5）----
    // CameraPanInertiaと同じ考え方。イージングの積分は毎tick行い、
    // tickCallback（実際のズーム適用と再描画）だけを間引く。判定は
    // 「前回渡した値からの相対変化率」で、これは画面端の移動量に比例する。
    double m_lastReported = 1.0;
    qint64 m_holdMs       = 0;
};
