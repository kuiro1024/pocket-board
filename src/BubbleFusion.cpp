#include "BubbleFusion.h"

#include <QtGlobal>
#include <cmath>

namespace {

// バネ（半陰的オイラー）。硬いバネでもフレームが落ちたときに発散しないよう、
// 1/240秒以下に刻んで積分する（Web試作でこれが無いと実際に発散した）。
void springStep(double &x, double &v, double goal, double stiffness,
                double damping, double dtSec)
{
    const int n = qMax(1, static_cast<int>(std::ceil(dtSec * 240.0)));
    const double h = dtSec / n;
    for (int i = 0; i < n; ++i) {
        v += ((goal - x) * stiffness - v * damping) * h;
        x += v * h;
    }
}

double easeInOutCubic(double t)
{
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

// easeInOutCubic の逆関数（0〜1）
double easeInOutCubicInverse(double a)
{
    a = qBound(0.0, a, 1.0);
    return a < 0.5 ? std::cbrt(a / 4.0) : 1.0 - std::cbrt((1.0 - a) * 2.0) / 2.0;
}

} // namespace

BubbleFusion::Event BubbleFusion::tick(CanvasItem *dragged, CanvasItem *candidate,
                                       double dtMs, double overlapRatio,
                                       double boardOverlapRatio)
{
    const double dtSec = qBound(0.0, dtMs, 50.0) / 1000.0;
    Event event = Event::None;

    if (m_absorbing) {
        m_absorbMs += dtSec * 1000.0;   // 1tickは最大50ms。Commitを飛び越さない
        const double p = absorbProgress();
        m_kGoal = 1.25;   // 吸い込み中はしっかりつなげたまま
        if (!m_committed && p >= 0.5) {
            m_committed = true;
            event = Event::Commit;
            kickPop(m_pairTarget, 1.1);   // 振幅はおよそ 1.1/√270 ≈ 6.7%
        }
        if (p >= 1.0) {
            m_absorbing = false;
            m_committed = false;
            m_pairDragged = nullptr;   // 吸われた側はこの後消える
            m_pairTarget = nullptr;
            m_k = m_kVel = m_kGoal = 0.0;
            m_dragged = m_target = nullptr;
            m_armTarget = nullptr;
            m_dwellMs = 0.0;
            m_loseMs = 0.0;
            m_arm = m_armVel = 0.0;
            m_armed = false;
            event = Event::Finished;
        }
    } else if (dragged && candidate) {
        // 結合できる状態へ進めるだけの重なりがあるか（浅い重なりは「首が
        // 残っているだけ」で、待ち時間は進まない）。
        const bool deep = overlapRatio >= kOverlapThreshold * 0.75;
        if (candidate != m_target || dragged != m_dragged) {
            m_target = candidate;
            m_dragged = dragged;
            // 外れてすぐ同じ相手に戻った場合は、戻りかけの吸い寄せの位置から
            // 続ける（待ち時間を0からやり直すと吸い寄せがカクッと抜ける）。
            m_dwellMs = (m_armTarget == candidate && m_arm > 0.0)
                ? easeInOutCubicInverse(m_arm) * kDwellMs
                : 0.0;
            m_armed = false;
            if (m_pairTarget != candidate || m_pairDragged != dragged) {
                // 同じバブルを掴んだまま相手だけ変わった場合は、今のブリッジの
                // 強さを引き継がず0から伸ばし直す（形が別物になるため）。
                m_pairTarget = candidate;
                m_pairDragged = dragged;
                m_k = m_kVel = 0.0;
            }
            m_swellItem = candidate;
        }
        m_loseMs = 0.0;
        if (deep) {
            m_dwellMs += dtMs;
            const double linear = qBound(0.0, m_dwellMs / kDwellMs, 1.0);
            if (m_armTarget != candidate) {
                m_armTarget = candidate;
                m_arm = 0.0;
            }
            m_arm = easeInOutCubic(linear);
            m_armVel = 0.0;   // 離れ始めはバネを静止状態から動かす（出だしが柔らかい）
            if (linear >= 1.0 && !m_armed) {
                m_armed = true;
                kickPop(candidate, 0.6);      // 約3.5%
            }
            m_kGoal = 0.3 + 0.7 * m_arm;
        } else {
            // 浅い重なり: 首だけが残る。待ち時間は進めず、結合できる状態も
            // 解く（この深さで離しても吸い込まない）。
            // 2026-09-23 (2): 首の太さは「板」の重なりで決め、板が離れるところで
            // 0になるようにする。面積比は深さの2乗で効くので平方根で深さに
            // 戻す（そのままだと浅いところで一気に細り、角が立って見えた）。
            // 深い側との境目（t=1）で 0.3+0.7*arm と一致させ、段差を出さない。
            m_armed = false;
            m_dwellMs = 0.0;
            const double ratio = boardOverlapRatio >= 0.0 ? boardOverlapRatio : overlapRatio;
            const double t = qBound(0.0, std::sqrt(qMax(0.0, ratio)
                                                   / (kOverlapThreshold * 0.75)), 1.0);
            const double eased = t * t * (3.0 - 2.0 * t);
            m_arm = qMin(m_arm, eased);       // 吸い寄せ・縮みも同じだけ抜く
            m_armVel = 0.0;
            m_kGoal = eased * (0.3 + 0.7 * m_arm);
        }
    } else if (dragged && m_target && dragged == m_dragged
               && m_loseMs + dtMs < kLoseGraceMs) {
        // 重なりのしきい値の上を行き来しているだけかもしれないので、少しの間は
        // 相手を手放さない（待ち時間も進めず、そのまま保つ）。境目で止めたときに
        // 「つなげる/つなげない」がフレームごとに入れ替わるのを防ぐ。
        m_loseMs += dtMs;
    } else {
        m_dragged = dragged;
        m_target = nullptr;
        m_dwellMs = 0.0;
        m_loseMs = 0.0;
        m_armed = false;
        m_kGoal = 0.0;
        // 吸い寄せ・縮みは一瞬で消さず0へ戻す（カクッと飛ばない）。
        // 2026-09-23: 以前は kArmReleaseMs で直線的に減らしていたが、出だしと
        // 終わりに速度の段差があり「パッと離れる」印象だったため、臨界減衰の
        // バネに変更（行き過ぎ無し・速度0から動き出して滑らかに止まる）。
        springStep(m_arm, m_armVel, 0.0, kArmReleaseOmega * kArmReleaseOmega,
                   2.0 * kArmReleaseOmega, dtSec);
        if (m_arm < 0.002 && std::abs(m_armVel) < 0.02) {
            m_arm = m_armVel = 0.0;
            m_armTarget = nullptr;
        } else if (m_arm < 0.0) {
            m_arm = m_armVel = 0.0;
            m_armTarget = nullptr;
        }
    }

    // ブリッジ: 伸びるときはやや弱めの減衰で少し揺らす。ちぎれるとき
    // （目標0）は0を行き過ぎたところで止める。行き過ぎて跳ね返ると、
    // ちぎれた首がもう一度つながりかけて「迷っている」ように見えるため。
    // 2026-09-23: ちぎれるとき（目標0、吸い込み中以外）は臨界減衰で細らせる。
    // 伸びるときと同じ弱い減衰（減衰比≈0.4）のままだと、0を行き過ぎたところで
    // 止める処理により首が「ブツッ」と切れて見えたため。
    if (m_kGoal <= 0.0 && !m_absorbing)
        springStep(m_k, m_kVel, 0.0, kBridgeBreakOmega * kBridgeBreakOmega,
                   2.0 * kBridgeBreakOmega, dtSec);
    else
        springStep(m_k, m_kVel, m_kGoal, 230.0, 12.0, dtSec);
    if (m_kGoal <= 0.0 && m_k < 0.0) {
        m_k = 0.0;
        m_kVel = 0.0;
    }
    if (!m_absorbing && m_kGoal <= 0.0
        && std::abs(m_k) < 0.01 && std::abs(m_kVel) < 0.05) {
        m_k = m_kVel = 0.0;
        m_pairTarget = m_pairDragged = nullptr;
    }

    // 受け側の膨らみ（吸い込み中は引っ込める）
    const double swellGoal =
        (!m_absorbing && m_swellItem && m_swellItem == m_target) ? m_arm : 0.0;
    if (swellGoal <= 0.0)   // しぼむときも臨界減衰（同上の理由）
        springStep(m_swell, m_swellVel, 0.0, kBridgeBreakOmega * kBridgeBreakOmega,
                   2.0 * kBridgeBreakOmega, dtSec);
    else
        springStep(m_swell, m_swellVel, swellGoal, 330.0, 15.0, dtSec);
    if (swellGoal <= 0.0 && m_swell < 0.0) {   // 戻るときに縮みすぎない
        m_swell = 0.0;
        m_swellVel = 0.0;
    }
    if (swellGoal <= 0.0 && std::abs(m_swell) < 0.002 && std::abs(m_swellVel) < 0.01) {
        m_swell = m_swellVel = 0.0;
        if (m_swellItem != m_target)
            m_swellItem = nullptr;
    }

    // ぷるん: 減衰を弱くして数回揺らす
    springStep(m_pop, m_popVel, 0.0, 270.0, 7.0, dtSec);
    if (std::abs(m_pop) < 0.0005 && std::abs(m_popVel) < 0.005) {
        m_pop = m_popVel = 0.0;
        m_popItem = nullptr;
    }

    return event;
}

bool BubbleFusion::armedFor(const CanvasItem *dragged) const
{
    return !m_absorbing && m_armed && dragged && dragged == m_dragged && m_target;
}

void BubbleFusion::startAbsorb()
{
    if (!m_target || !m_dragged)
        return;
    m_pairTarget = m_target;
    m_pairDragged = m_dragged;
    m_absorbing = true;
    m_committed = false;
    m_absorbMs = 0.0;
}

double BubbleFusion::absorbProgress() const
{
    return m_absorbing ? qBound(0.0, m_absorbMs / kAbsorbMs, 1.0) : 0.0;
}

double BubbleFusion::liftFor(const CanvasItem *item) const
{
    if (!item)
        return 1.0;
    double lift = 1.0;
    if (item == m_swellItem)
        lift += kTargetSwell * m_swell;
    if (item == m_popItem)
        lift += m_pop;
    return lift;
}

bool BubbleFusion::needsFrames() const
{
    return m_absorbing || m_target || m_armTarget || m_pairTarget || m_swellItem
        || m_popItem;
}

void BubbleFusion::forget(const CanvasItem *item)
{
    if (!item)
        return;
    const bool involved = item == m_dragged || item == m_target
        || item == m_pairTarget || item == m_pairDragged;
    if (involved) {
        m_dragged = m_target = nullptr;
        m_pairTarget = m_pairDragged = nullptr;
        m_armTarget = nullptr;
        m_k = m_kVel = m_kGoal = 0.0;
        m_dwellMs = 0.0;
        m_loseMs = 0.0;
        m_arm = m_armVel = 0.0;
        m_armed = false;
        m_absorbing = false;
        m_committed = false;
    }
    if (item == m_armTarget) { m_armTarget = nullptr; m_arm = m_armVel = 0.0; }
    if (item == m_swellItem) { m_swellItem = nullptr; m_swell = m_swellVel = 0.0; }
    if (item == m_popItem)   { m_popItem = nullptr;   m_pop = m_popVel = 0.0; }
}

void BubbleFusion::reset()
{
    *this = BubbleFusion();
}

void BubbleFusion::kickPop(CanvasItem *item, double velocity)
{
    if (!item)
        return;
    if (m_popItem != item) {
        m_popItem = item;
        m_pop = 0.0;
        m_popVel = 0.0;
    }
    m_popVel += velocity;
}
