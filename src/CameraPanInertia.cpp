#include "CameraPanInertia.h"

#include <QTimer>
#include <cmath>

namespace {
// 移設前（CanvasWidget.cpp）と同じ定数・同じ値。挙動を変えない純粋な
// リファクタなので、チューニング値もそのまま持ってきている。
constexpr double kVelocitySmoothing     = 0.35;  // EMA平滑化係数（大きいほど直近重視）
constexpr qint64 kMaxSampleGapMs        = 200;   // これを超える間隔のサンプルは信頼しない
constexpr qint64 kMaxIdleBeforeReleaseMs = 30;   // 離す前にこれ以上静止していたら速度ゼロ扱い
constexpr double kMinFlickSpeed         = 40.0;  // canvas単位/秒。これ未満は慣性を始めない
constexpr double kFrictionPerSecond     = 0.06;  // 1秒あたりこの割合まで減速する指数減衰
constexpr double kStopSpeed             = 5.0;   // canvas単位/秒。これ未満になったら停止扱い
constexpr int    kTimerIntervalMs       = 16;    // 約60fps（速度を積分するtick）
constexpr qint64 kMaxTickDtMs           = 64;    // フレーム落ち時の暴走防止

// ---- 速度連動の可変レート（2026-09-14 Phase5）----
// カクつきの見えやすさは「1フレームあたり画面上で何px動いたか」で決まる。
// 速いときは小刻みに出さないと飛んで見えるが、減速しきった尻尾では
// 1フレームの移動量が1px未満になり、同じレートで出しても無駄なだけ。
// 積み残しがこのpx数を超えるまでtickCallbackを呼ばないことで
// 「速いとき60Hz／遅いとき最低15Hz」の可変レートになる。
// 摩擦減衰はdtベースなので、間引いても減速カーブと停止位置は変わらない。
// 2026-09-14: 3.0px / 66msで実機テストしたところ、減速の尻尾で明確に
// カクついた。溜めて一気に出す方式は「出すたびに閾値ぶんジャンプする」
// ので、3pxのジャンプはゆっくり動いている場面ではハッキリ見える
// （速く動いている時は隣のフレームとの差が大きいので紛れるが、
// 尻尾ではそれが無い＝一番バレやすい所に一番効いてしまっていた）。
// 1px未満ならそもそも画面上で別の位置に描けないので、ここまで下げる。
// 2026-09-16 F-4: さらに下げる。1.0px / 33ms でもなお、止まる寸前でレート
// 不足のカクつきが残っていた。理由は上と同じで、尻尾では隣のフレームとの
// 差が小さいぶん「溜めて一気に出す」ジャンプが一番目立つため。
//  ・kMinStepPx 1.0 → 0.5: キャンバスはアンチエイリアス付きで描いており、
//    0.5pxの移動でも実際に見た目が変わる（整数画素に丸めていない）。
//  ・kMaxHoldMs 33 → 16: tick自体が16ms(60Hz)なので、これで尻尾の実効
//    レートが上限の60Hzまで上がる（＝止まる寸前が一番なめらかになる）。
// 摩擦減衰はdtベースなので、レートを上げても減速カーブと停止位置は同じ。
// 代償は「パンの最後の0.3秒ほどのあいだ全面再描画が30→60回/秒になる」こと。
constexpr double kMinStepPx             = 0.5;   // これ未満しか動かないなら溜める
constexpr qint64 kMaxHoldMs             = 16;    // 溜めっぱなしの上限（≒60Hz）
}

CameraPanInertia::CameraPanInertia(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(kTimerIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &CameraPanInertia::onTimeout);
}

void CameraPanInertia::setTickCallback(TickCallback cb)
{
    m_tickCallback = std::move(cb);
}

void CameraPanInertia::beginDrag()
{
    stop();
    m_sampleClock.restart();
}

void CameraPanInertia::sampleDelta(const QPointF &actualDelta)
{
    const qint64 dtMs = m_sampleClock.restart();
    if (dtMs > 0 && dtMs < kMaxSampleGapMs) {
        const QPointF instVelocity = actualDelta * (1000.0 / double(dtMs));
        m_velocity = m_velocity * (1.0 - kVelocitySmoothing) + instVelocity * kVelocitySmoothing;
    }
}

void CameraPanInertia::endDrag()
{
    // ボタンを押したまま動きを止めて、少し間を置いてから離すと、動きが
    // 止まる直前のEMA速度が古い値のまま残っていて「もう止まっているはず
    // なのに離した瞬間に慣性がつく」という違和感があった。m_sampleClockは
    // sampleDelta()のたびにrestart()しているので、restart()せずelapsed()
    // だけ見れば「最後に実際に動いてからどれくらい経ったか」が分かる。
    if (m_sampleClock.elapsed() > kMaxIdleBeforeReleaseMs)
        m_velocity = QPointF(0, 0);

    const double speed = std::hypot(m_velocity.x(), m_velocity.y());
    if (speed > kMinFlickSpeed) {
        m_inertiaClock.restart();
        m_timer->start();
    } else {
        m_velocity = QPointF(0, 0);
    }
}

void CameraPanInertia::stop()
{
    m_timer->stop();
    m_velocity = QPointF(0, 0);
    m_pendingDelta = QPointF(0, 0);
    m_pendingHoldMs = 0;
}

void CameraPanInertia::setViewScale(double scale)
{
    // ズーム0や負値は来ない想定だが、来ても可変レートの判定が壊れない
    // ようにだけしておく（下限を割ったら「常に出す」側へ倒れる）。
    m_viewScale = scale > 0.0 ? scale : 1.0;
}

bool CameraPanInertia::isActive() const
{
    return m_timer->isActive();
}

void CameraPanInertia::onTimeout()
{
    const qint64 dtMs = m_inertiaClock.restart();
    if (dtMs <= 0)
        return;
    // 極端なフレーム落ち（タブ切り替え直後等）で1フレームぶんの移動量が
    // 暴走しないよう、dtの上限を設けておく。
    const double dt = static_cast<double>(qMin<qint64>(dtMs, kMaxTickDtMs)) / 1000.0;

    // 2026-09-14 Phase5: 速度の積分は毎tick行うが、実際にカメラを動かして
    // 再描画する（＝tickCallback）のは「画面上でkMinStepPx以上動く分が
    // 溜まったとき」か「kMaxHoldMs経ったとき」だけにする。
    // 積み残しはm_pendingDeltaへ足していき、出すときにまとめて渡すので、
    // 移動量の総和＝最終的な着地位置は間引かない場合と完全に一致する。
    m_pendingDelta += m_velocity * dt;
    m_pendingHoldMs += dtMs;

    // 摩擦による減速。フレームレートに依存しないよう「1秒あたりこの割合
    // まで減速する」という経過秒数ベースの指数減衰にしてある（QTimerの
    // 間隔はOS都合で多少ぶれるため、tickごとに固定倍率を掛けるより安定する）。
    const double decay = std::pow(kFrictionPerSecond, dt);
    m_velocity *= decay;

    const bool willStop =
        std::hypot(m_velocity.x(), m_velocity.y()) < kStopSpeed;

    // 停止フレームだけは必ず出す（溜めたまま止まると、その分だけ
    // 手前で止まってしまうため）。
    const double pendingPx =
        std::hypot(m_pendingDelta.x(), m_pendingDelta.y()) * m_viewScale;
    const bool due = willStop
                  || pendingPx >= kMinStepPx
                  || m_pendingHoldMs >= kMaxHoldMs;
    if (!due)
        return;

    const QPointF delta = m_pendingDelta;
    m_pendingDelta = QPointF(0, 0);
    m_pendingHoldMs = 0;

    const bool shouldContinue = m_tickCallback ? m_tickCallback(delta) : false;
    if (!shouldContinue || willStop)
        stop();
}
