#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <functional>

class QTimer;

// 2026-07-12: 中ボタンドラッグパンの「離した後も少し動き続けて減速して
// 止まる」慣性を汎用化したヘルパー。元々はフォーカス中パン
// （focus_image_pan）専用にCanvasWidget.cppへ直接書いていたが、通常
// キャンバスのパン（camera_pan）にも同じ挙動を足す計画があり、かつ
// CanvasWidget.cppへこれ以上ロジックを増やさない方針にしたため、
// このクラスへ切り出した（振る舞いは移設前と変更していない）。
//
// 壁クランプの有無や「対象が消えた／他のアニメが割り込んできたら
// 中断する」といった呼び出し元固有の判断はtickCallback経由で外に
// 出してあるので、このクラス自体はfocus_image_pan/camera_panどちらの
// 都合も知らない。
//
// 使い方:
//   - ドラッグ開始時:            beginDrag()
//   - ドラッグ中、実際にカメラを動かした量（壁クランプ後の実移動量など）
//     を計算するたびに:          sampleDelta(actualDelta)
//   - ドラッグ終了（ボタンを離した）時: endDrag()
//     直近の速度が閾値を超えていれば、以後tickCallbackが呼ばれ続ける
//     慣性アニメーションを自動的に開始する。
//   - いつでも即座に止めたい時:  stop()
class CameraPanInertia : public QObject
{
    Q_OBJECT
public:
    // 毎フレーム（約60fps）呼ばれる。そのフレームで動かすべきdeltaを渡す。
    // 呼び出し側はここでカメラ位置を進め、必要ならクランプ・再描画する。
    // 対象の消失や他アニメの割り込みなどで続行できない場合はfalseを返す
    // （その場でdeltaは適用されず、慣性は即座に停止する）。
    using TickCallback = std::function<bool(const QPointF &delta)>;

    explicit CameraPanInertia(QObject *parent = nullptr);

    void setTickCallback(TickCallback cb);

    // 新規ドラッグ開始時に呼ぶ。進行中の慣性を止め、サンプリング用の
    // 経過時間計測をリセットする。
    void beginDrag();

    // ドラッグ中、壁クランプ後などの「実際の」移動量をサンプリングする。
    // 前回サンプルからの経過時間をもとに瞬間速度を求め、EMA平滑化する。
    void sampleDelta(const QPointF &actualDelta);

    // ドラッグ終了（ボタンリリース）時に呼ぶ。
    // 最後にsampleDelta()してから一定時間動きが無ければ静止とみなし
    // 速度をゼロ扱いにする。残った速度が閾値を超えていれば慣性tickを
    // 開始する。
    void endDrag();

    // 慣性アニメを即座に止めて速度をゼロに戻す。
    void stop();

    bool isActive() const;

    // 2026-09-14 Phase5: 現在のカメラ倍率（キャンバス単位→画面px）。
    // 速度連動の可変レート（onTimeout()のコメント参照）で「1フレームに
    // 画面上で何px動くか」を判断するために要る。CanvasWidget側の
    // updateCanvasFromCamera()から渡す。
    void setViewScale(double scale);

private:
    void onTimeout();

    TickCallback   m_tickCallback;
    QPointF        m_velocity;
    QElapsedTimer  m_sampleClock;
    QElapsedTimer  m_inertiaClock;
    QTimer        *m_timer = nullptr;

    // ---- 可変レート用（2026-09-14 Phase5）----
    // タイマーは16msのまま回して速度の積分は毎tick行い、tickCallback
    // （＝実際のカメラ移動と再描画）だけを間引く。積み残しはここへ溜めて
    // 次に出すときまとめて渡すので、移動量の総和と着地位置は
    // 間引かない場合と完全に同じになる。
    double  m_viewScale     = 1.0;
    QPointF m_pendingDelta;
    qint64  m_pendingHoldMs = 0;
};
