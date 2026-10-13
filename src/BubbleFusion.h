#pragma once

#include <QPointF>

class CanvasItem;

// ============================================================
//  BubbleFusion（2026-09-22）
//
//  バブルを掴んで別のバブルに重ね、少し待ってから離すと、掴んでいた方が
//  吸い込まれて相手のリストに入る「バブル融合」の状態機械。
//  試作ページ「GlassPip Merge Lab」（Web版）で調整した動きを移植したもの。
//
//  流れ:
//    1. 重ねる       … 2つの板が液体のように首でつながる（ブリッジ）。
//                      受け側が少し膨らむ。
//    2. kDwellMs 待つ … 「結合できる」状態になる。受け側がぷるんと反応し、
//                      掴んでいる方がわずかに吸い寄せられる。
//    3. 離す         … 吸い込みアニメ。半分まで進んだ時点で中身を移し
//                      （Event::Commit）、終わったら吸われた側を消す
//                      （Event::Finished）。
//    途中で離れる／結合できる前に離す … ブリッジがちぎれて元に戻る。
//
//  【このクラスがやること／やらないこと】
//  やること : 時間の進行と、描画に使う値（ブリッジの強さ・膨らみ等）の計算。
//  やらないこと: 重なりの判定・バブルの移動・描画・削除（CanvasWidget 側）。
//  バブルの種類（動画・画像）や中身（Web動画・ローカル）は一切知らない。
//  「結合してよいか」は CanvasWidget::bubblesCanFuse() が決める
//  （将来プラグインが拒否できるようにする場所）。
//
//  描画の仕組みは Renderer::ShapeFusion / GlassHighlightEffect::Fusion を参照。
// ============================================================
class BubbleFusion
{
public:
    // ---- 調整値（Web試作と同じ値から始めている）----
    static constexpr double kDwellMs          = 350.0;  // 重ねてから結合できるまで
    static constexpr double kAbsorbMs         = 520.0;  // 吸い込みアニメの長さ
    static constexpr double kOverlapThreshold = 0.22;   // 重なり率（小さい方の面積に対する割合）
    static constexpr double kBridgeBezelRatio = 1.5;    // ブリッジの幅 = ベゼル幅 × これ × 強さ
    static constexpr double kTargetSwell      = 0.055;  // 受け側の膨らみ（liftScale の増分）
    static constexpr double kDraggedShrink    = 0.07;   // 結合できる状態での掴んでいる側の縮み
    static constexpr double kMagnet           = 0.15;   // 結合できる状態での吸い寄せ（中心間の割合）
    // 2026-09-22: 境目で止めたときの「つなげる/つなげない」のちらつき対策。
    static constexpr double kLoseGraceMs      = 110.0;  // 重なりが外れてから相手を手放すまでの猶予
    // 2026-09-23: 「やっぱやめた」で離れるときを滑らかにするため、直線(140ms)から
    // 臨界減衰バネへ変更。ω[rad/s]。整定の目安は約 5/ω 秒（14 → 約0.35秒）。
    static constexpr double kArmReleaseOmega  = 14.0;   // 外れたとき、吸い寄せ・縮みが0へ戻る速さ
    static constexpr double kBridgeBreakOmega = 12.0;   // 外れたとき、首が細ってちぎれる速さ（膨らみも同じ）

    enum class Event {
        None,
        Commit,     // 吸い込みの途中。ここで中身を受け側へ移す
        Finished    // 吸い込み完了。吸われた側を消してよい
    };

    // 毎フレーム呼ぶ（CanvasWidget::tickBubbleFusion）。
    //   dragged      : 今ドラッグ中で融合できるバブル（無ければ nullptr）
    //   candidate    : dragged と重なっている相手（無ければ nullptr）
    //   overlapRatio : その重なり（小さい方の面積に対する割合）
    //
    // 2026-09-23: 重なりには2段ある。
    //   kOverlapThreshold 以上 … 結合できる状態へ向かって待ち時間が進む
    //   それ未満（でも重なっている） … 首はつながったまま、重なりが浅く
    //     なるほど細くなる。板が実際に離れた時点で0になる。
    //     （以前は 22% を割った瞬間に首が消え、まだ深く重なって見えるのに
    //       融合が終わっていた）
    //
    // 2026-09-23 (2): 首の太さは板（ベゼル込み）の重なり boardOverlapRatio で決める。
    // 中身の重なりだけで見ると、中身が離れてもまだ板は重なっているのに首が0に
    // なり、2枚の板が境目のある状態で重なって見えていた（離れぎわ・近づきぎわ）。
    // 負なら overlapRatio を使う（旧挙動）。
    Event tick(CanvasItem *dragged, CanvasItem *candidate, double dtMs,
               double overlapRatio = 1.0, double boardOverlapRatio = -1.0);

    // dragged を離したら吸収になるか（結合できる状態か）
    bool armedFor(const CanvasItem *dragged) const;
    // 離した瞬間に呼ぶ。吸われる側・受け側は直前の tick の組み合わせ。
    void startAbsorb();
    bool absorbing() const { return m_absorbing; }
    // 吸い込みの進み具合 0〜1
    double absorbProgress() const;

    // ---- 描画用 ----
    // 融合して描く組（ブリッジが残っている間は、離れた後も少しの間続く）
    CanvasItem *pairTarget()  const { return m_pairTarget; }
    CanvasItem *pairDragged() const { return m_pairDragged; }
    // ブリッジの強さ（0で無し。バネなので1を少し超えることがある）
    double bridgeStrength() const { return m_k > 0.0 ? m_k : 0.0; }
    bool pairActive() const {
        return m_pairTarget && m_pairDragged && bridgeStrength() > 0.01;
    }
    // 結合できる状態への近さ 0〜1（イーズ済み）
    double armedness() const { return m_arm; }
    // 今の受け側（重ねている相手）
    CanvasItem *hoverTarget() const { return m_target; }
    // armedness() が指している相手。外れた後も吸い寄せが0へ戻り切るまで残る。
    CanvasItem *armTarget() const { return m_armTarget; }
    // このバブルに掛ける liftScale（1.0 なら演出なし）
    double liftFor(const CanvasItem *item) const;

    // 再描画が必要な動きが残っているか
    bool needsFrames() const;
    // 破棄されるバブルを参照から外す
    void forget(const CanvasItem *item);
    // すべて打ち切る（セッション切り替え等）
    void reset();

private:
    // 今の組み合わせ（毎tick更新）
    CanvasItem *m_dragged = nullptr;
    CanvasItem *m_target  = nullptr;
    double m_dwellMs = 0.0;
    double m_arm = 0.0;
    double m_armVel = 0.0;              // 離れるときのバネの速度
    bool   m_armed = false;
    double m_loseMs = 0.0;              // 重なりが外れてからの時間（猶予中）
    CanvasItem *m_armTarget = nullptr;  // m_arm が指している相手

    // 描画中の組とブリッジのバネ
    CanvasItem *m_pairTarget  = nullptr;
    CanvasItem *m_pairDragged = nullptr;
    double m_k = 0.0, m_kVel = 0.0, m_kGoal = 0.0;

    // 受け側の膨らみ
    CanvasItem *m_swellItem = nullptr;
    double m_swell = 0.0, m_swellVel = 0.0;

    // 受け側のぷるん（結合できた瞬間と吸い込んだ瞬間）
    CanvasItem *m_popItem = nullptr;
    double m_pop = 0.0, m_popVel = 0.0;

    // 吸い込み
    bool   m_absorbing = false;
    bool   m_committed = false;
    double m_absorbMs = 0.0;

    void kickPop(CanvasItem *item, double velocity);
};
