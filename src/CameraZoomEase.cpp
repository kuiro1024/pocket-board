#include "CameraZoomEase.h"

#include <QTimer>
#include <cmath>
#include <algorithm>

namespace {
constexpr int    kTimerIntervalMs   = 16;   // 約60fps
constexpr qint64 kMaxTickDtMs       = 64;   // フレーム落ち時の暴走防止
// 大きいほど目標値へ速く追従する（秒^-1）。CameraPanInertiaの摩擦減衰と
// 同じく経過秒数ベースの指数イージングにしてあるので、OS都合でQTimerの
// 間隔が多少ぶれてもフレームレートに依存せず同じ体感速度になる。
constexpr double kEaseSpeedPerSecond = 12.0;
// これ未満まで目標値に近づいたら、そこでピタッと止めて打ち切る
// （指数イージングは理論上いつまでも目標に完全一致しないため）。
constexpr double kSnapEpsilon = 0.0005;

// ---- 変化量連動の可変レート（2026-09-14 Phase5）----
// 指数イージングは最初が速く終わりが極端に遅い。終盤は1tickあたりの
// ズーム変化が画面上で1px未満になり、同じレートで出しても無駄なだけ。
// 「前回渡した値からの相対変化率」がこれを超えるまでtickCallbackを
// 呼ばないことで、速いとき60Hz／遅いとき最低15Hzの可変レートになる。
// 相対変化率は画面端の移動量にほぼ比例する（0.003なら幅700pxで約2px）。
// イージングはdtベースなので、間引いても到達時間とカーブは変わらない。
constexpr double kMinRelStep  = 0.003;
constexpr qint64 kMaxHoldMs   = 66;   // 溜めっぱなしの上限（≒15Hz）
}

CameraZoomEase::CameraZoomEase(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(kTimerIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &CameraZoomEase::onTimeout);
}

void CameraZoomEase::setTickCallback(TickCallback cb)
{
    m_tickCallback = std::move(cb);
}

void CameraZoomEase::retarget(double current, double target)
{
    if (!m_timer->isActive()) {
        // 新規開始時のみ現在値を起点にする。イージング中に来た場合は
        // m_currentがすでに（tickCallback経由で）実際のズーム値と同期して
        // いるはずなので、そのまま引き継いで目標だけ更新する。
        m_current = current;
        // 2026-09-14 Phase5: 可変レートの基準も起点に揃える。
        m_lastReported = current;
        m_holdMs = 0;
        m_clock.restart();
        m_timer->start();
    }
    m_target = target;
}

void CameraZoomEase::stop()
{
    m_timer->stop();
}

bool CameraZoomEase::isActive() const
{
    return m_timer->isActive();
}

void CameraZoomEase::stepFromVsync()
{
    if (!m_timer->isActive())
        return;
    onTimeout();
    // QTimer の次の発火をこの時点から数え直す（vsync直後にもう1回来ると
    // 同じフレーム内で2歩進むだけで無駄になる）。
    if (m_timer->isActive())
        m_timer->start();
}

void CameraZoomEase::onTimeout()
{
    const qint64 dtMs = m_clock.restart();
    if (dtMs <= 0)
        return;
    const double dt = static_cast<double>(std::min<qint64>(dtMs, kMaxTickDtMs)) / 1000.0;

    const double factor = 1.0 - std::exp(-kEaseSpeedPerSecond * dt);
    m_current += (m_target - m_current) * factor;

    const bool atTarget = std::abs(m_target - m_current) < kSnapEpsilon;
    if (atTarget)
        m_current = m_target;

    // 2026-09-14 Phase5: 積分は毎tick進めるが、実際にズームを適用して
    // 再描画するのは変化が見える分だけ溜まったときに限る。
    // 到達フレームは必ず出す（溜めたまま止まると最終ズーム値がズレる）。
    m_holdMs += dtMs;
    const double relStep = m_lastReported > 0.0
        ? std::abs(m_current - m_lastReported) / m_lastReported
        : 1.0;
    const bool due = atTarget
                  || relStep >= kMinRelStep
                  || m_holdMs >= kMaxHoldMs;
    if (!due)
        return;

    m_lastReported = m_current;
    m_holdMs = 0;

    const bool shouldContinue = m_tickCallback ? m_tickCallback(m_current) : false;
    if (!shouldContinue || atTarget) {
        stop();
        return;
    }
}
