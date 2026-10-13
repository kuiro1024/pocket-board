#include "FlowMode.h"
#include "CanvasVideo.h"
#include "BubbleState.h"
#include <QDebug>
#include <QSet>
#include <algorithm>
#include <cmath>

static double pointToSegmentDist(const QPointF &p, const QPointF &a, const QPointF &b)
{
    QPointF ab = b - a, ap = p - a;
    double len2 = ab.x()*ab.x() + ab.y()*ab.y();
    if (len2 < 1e-10) { double dx=p.x()-a.x(), dy=p.y()-a.y(); return std::sqrt(dx*dx+dy*dy); }
    double t = qBound(0.0, (ap.x()*ab.x()+ap.y()*ab.y())/len2, 1.0);
    QPointF c = a + QPointF(ab.x()*t, ab.y()*t);
    double dx=p.x()-c.x(), dy=p.y()-c.y();
    return std::sqrt(dx*dx+dy*dy);
}

static double segmentToSegmentDist(const QPointF &p1, const QPointF &p2,
                                    const QPointF &q1, const QPointF &q2)
{
    double d1=pointToSegmentDist(p1,q1,q2), d2=pointToSegmentDist(p2,q1,q2);
    double d3=pointToSegmentDist(q1,p1,p2), d4=pointToSegmentDist(q2,p1,p2);
    return std::min(std::min(d1,d2), std::min(d3,d4));
}

FlowMode::FlowMode(QObject *parent) : QObject(parent)
{
    m_enterAnim = new QVariantAnimation(this);
    m_enterAnim->setDuration(300);
    m_enterAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_enterAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        m_overlayAlpha = v.toFloat(); emit changed();
    });

    // グロー: SineCurve で 0→1→0 を1周期で滑らか往復（継ぎ目なし）
    m_glowAnim = new QVariantAnimation(this);
    m_glowAnim->setDuration(2000);
    m_glowAnim->setStartValue(0.0f);
    m_glowAnim->setEndValue(1.0f);
    m_glowAnim->setLoopCount(-1);
    m_glowAnim->setEasingCurve(QEasingCurve::SineCurve);
    connect(m_glowAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        m_glowAlpha = v.toFloat(); emit changed();
    });
}

void FlowMode::toggle()
{
    m_active = !m_active;
    m_stroking = false; m_strokePath.clear(); m_lastHit = nullptr; m_strokeStart = nullptr;

    m_enterAnim->stop();
    if (m_active) {
        m_enterAnim->setStartValue(m_overlayAlpha); m_enterAnim->setEndValue(1.0f); m_enterAnim->start();
        m_glowAnim->start();
    } else {
        m_enterAnim->setStartValue(m_overlayAlpha); m_enterAnim->setEndValue(0.0f); m_enterAnim->start();
        m_glowAnim->stop(); m_glowAlpha = 0.0f;
        startChain();
    }
    emit changed();
}

void FlowMode::beginStroke(const QPointF &canvasPos, bool isErase)
{
    m_stroking = true; m_isErase = isErase;
    m_lastHit = nullptr; m_strokeStart = nullptr;
    m_strokePath.clear(); m_strokePath.append(canvasPos);
}

void FlowMode::updateStroke(const QPointF &canvasPos, CanvasVideo *hitVideo,
                             std::function<QPointF(CanvasVideo*)> centerOf)
{
    if (!m_stroking) return;
    m_strokePath.append(canvasPos);

    if (m_isErase) {
        if (m_strokePath.size() >= 2 && !m_links.isEmpty()) {
            const QPointF &p1 = m_strokePath[m_strokePath.size()-2];
            const double THRESH = 30.0 / qMax(m_canvasZoom, 0.01);
            bool removed = false;
            for (auto it = m_links.begin(); it != m_links.end(); ) {
                if (segmentToSegmentDist(p1, canvasPos, centerOf(it->from), centerOf(it->to)) < THRESH) {
                    qDebug("[Flow] 線切断: %p -> %p", (void*)it->from, (void*)it->to);
                    it = m_links.erase(it); removed = true;
                } else { ++it; }
            }
            if (removed) emit changed();
        }
        return;
    }

    if (!hitVideo || hitVideo == m_lastHit) return;
    if (m_lastHit && m_lastHit != hitVideo) addLink(m_lastHit, hitVideo);
    if (!m_strokeStart) m_strokeStart = hitVideo;
    m_lastHit = hitVideo;
}

void FlowMode::endStroke()
{
    m_stroking = false; m_strokePath.clear(); m_lastHit = nullptr; m_strokeStart = nullptr;
    emit changed();
}

void FlowMode::addLink(CanvasVideo *from, CanvasVideo *to)
{
    if (!from || !to || from == to) return;
    for (const auto &lk : m_links) if (lk.from==from && lk.to==to) return;

    // 2026-07-04: サイクル防止。既存リンクでA→Bがある状態でB→Aを引くと
    // 「お互いに矢印を引ける」状態になり、tickPlayback()がEnded検知の
    // たびに行ったり来たりを繰り返してしまう（ユーザー報告により発覚）。
    // to側から既存リンクを辿っていき、fromに戻ってきてしまう場合は
    // 追加を拒否する（直接の相互リンクだけでなく、A→B→C→Aのような
    // 間接的なサイクルも同様に弾く）。
    {
        QSet<CanvasVideo*> visited;
        CanvasVideo *cur = to;
        while (cur) {
            if (cur == from) {
                qDebug("[Flow] リンク追加を拒否（サイクルになるため）: %p -> %p",
                       (void*)from, (void*)to);
                return;
            }
            if (visited.contains(cur)) break;
            visited.insert(cur);
            CanvasVideo *next = nullptr;
            for (const auto &lk2 : m_links) {
                if (lk2.from == cur) { next = lk2.to; break; }
            }
            cur = next;
        }
    }

    for (auto &lk : m_links) {
        if (lk.from == from) {
            lk.to = to; lk.triggered = false;
            qDebug("[Flow] リンク更新: %p -> %p", (void*)from, (void*)to);
            emit changed(); return;
        }
    }
    FlowLink lk; lk.from = from; lk.to = to;
    m_links.append(lk);
    qDebug("[Flow] リンク追加: %p -> %p", (void*)from, (void*)to);
    emit changed();
}

void FlowMode::removeLinksThrough(CanvasVideo *vid)
{
    int before = m_links.size();
    // 2026-07-03: 削除されるリンクのto側で先読みが進行中だった場合、
    // 宙に浮いた裏側MpvPlayerが残り続けないようキャンセルしておく
    // （vid自身が破棄される場合はCanvasVideo::releaseGlResources()側で
    // 別途後始末されるため、ここではvid以外のto側のみが対象）。
    for (const auto &lk : m_links) {
        if ((lk.from == vid || lk.to == vid) && lk.to && lk.to != vid)
            lk.to->cancelFlowPreload();
    }
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(),
        [vid](const FlowLink &lk){ return lk.from==vid || lk.to==vid; }), m_links.end());
    if (m_links.size() != before) { qDebug("[Flow] リンク削除 (via %p)", (void*)vid); emit changed(); }
}

void FlowMode::removeItem(CanvasVideo *vid) { removeLinksThrough(vid); }

void FlowMode::clearLinks()
{
    if (m_links.isEmpty()) return;
    m_links.clear();
    qDebug("[Flow] 全リンク破棄");
    emit changed();
}

bool FlowMode::isStandby(CanvasVideo *vid) const
{
    // toであるか確認（リンク発火後は既にリンク削除済みなのでfalse）
    for (const auto &lk : m_links)
        if (lk.to == vid) return true; // toである = 待機中 → ブロック
    return false;
    // ※ 再生が開始した瞬間にリンクが削除されるため、
    //    再生中のtoがisStandbyでブロックされる問題は発生しない
}

void FlowMode::applyStandby(const QList<CanvasVideo*> &allVideos)
{
    for (auto &lk : m_links) { lk.triggered = false; lk.preloadTriggered = false; }
    for (CanvasVideo *vid : allVideos) {
        if (!vid) continue;
        // Phase 1: mediaPlayer() 直接呼び出しを廃止し BC 経由に変更
        vid->setFlowStandby(true);
        qDebug("[Flow] 全停止(standby): %p", (void*)vid);
    }
}

void FlowMode::startChain()
{
    for (auto &lk : m_links) {
        lk.triggered = false;
        lk.preloadTriggered = false;
        // 【2026-07-10】fromに「前回の再生で最後まで到達してSleepした」
        // マーク（SleepReason::PlaybackEnded）が残っていると、開始直後の
        // tickPlayback()がそれを今回のチェーンの完了と誤認し、リンクを
        // 即発火してしまう（A→B→Cで開始した瞬間にB→Cが発火してCがAと
        // 同時再生され、以降のチェーンが繋がらなくなる実バグ）。
        // ここで古いマークを無効化する。今回のチェーンでfromが実際に
        // Endedへ達した時には改めてマークが付くため、正規の
        // 「Ended直後にSleepへ落ちた場合の発火」には影響しない。
        if (lk.from)
            lk.from->clearFlowChainEndedMark();
    }
    if (m_links.isEmpty()) return;

    QSet<CanvasVideo*> toSet;
    for (const auto &lk : m_links) toSet.insert(lk.to);

    QSet<CanvasVideo*> started;
    for (const auto &lk : m_links) {
        if (!toSet.contains(lk.from) && !started.contains(lk.from)) {
            if (lk.from) {
                // Phase 1: BC 経由（standby を解除して再生）
                lk.from->setFlowStandby(false);
                lk.from->bcPlay();
                started.insert(lk.from);
                qDebug("[Flow] チェーン先頭再生: %p", (void*)lk.from);
            }
        }
    }
}

void FlowMode::tickPlayback()
{
    if (m_active) return;
    if (m_links.isEmpty()) return;

    // 2026-07-03: EOF数秒前を検知したらto側の先読みを開始する猶予（ミリ秒）。
    // 短すぎると先読みが間に合わずフォールバックになりがちで、長すぎると
    // まだ再生時間が長く残っているのに早々とネットワーク接続・デコードを
    // 始めてしまう（帯域・CPUの無駄）。既存の画質切り替えウォームアップが
    // 実測150ms〜数秒で完了することを踏まえた仮値。
    constexpr qint64 kPreloadLeadMs = 4000;

    for (auto it = m_links.begin(); it != m_links.end(); ) {
        auto &lk = *it;
        if (!lk.from || !lk.to) { ++it; continue; }

        // BC の BubbleState で判定する（QMediaPlayer は使わない）
        BubbleState fromState = lk.from->bubbleState();

        // From が普通に再生中 → triggered リセット
        if (fromState == BubbleState::Playing
         || fromState == BubbleState::Buffering) {
            lk.triggered = false;

            // ピース2: まもなくEOFに達しそうなら、まだなら先読みを開始する。
            if (!lk.preloadTriggered) {
                const qint64 dur = lk.from->duration();
                const qint64 pos = lk.from->position();
                if (dur > 0 && (dur - pos) < kPreloadLeadMs) {
                    lk.preloadTriggered = true;
                    lk.to->beginFlowPreload();
                    qDebug("[Flow] EOF間近を検知、先読み開始: %p -> %p (残り%.1fs)",
                           (void*)lk.from, (void*)lk.to, (dur - pos) / 1000.0);
                }
            }

            ++it;
            continue;
        }

        if (lk.triggered) { ++it; continue; }

        const bool fromPlaybackEnded =
            fromState == BubbleState::Ended
         || (fromState == BubbleState::Sleep && lk.from->isPlaybackEndedRelease());

        // From が Ended になった（またはEnded直後に解放済みSleepになった）→ 次を起動
        if (fromPlaybackEnded) {
            lk.triggered = true;
            // erase()でlkが無効になる前に、後始末用にfromを退避しておく。
            CanvasVideo *finishedFrom = lk.from;
            // ピース3: 先読みが間に合っていれば無瞬断で昇格。
            // 間に合っていなければ従来の再オープン経路にフォールバックする
            // （promoteFlowPreload()がfalseを返した場合、内部で中途半端な
            // 先読み状態は片付け済み）。
            const bool promoted = lk.to->promoteFlowPreload();
            if (!promoted) {
                lk.to->setFlowStandby(false);
                // 【2026-07-10】fromが全画面（フォーカス）中にチェーンが
                // 発火した場合、toは旧フォーカスセッションの「保留中
                // FocusSleep」（m_focusSleepPending）を持ったままのことが
                // ある。そのままbcPlay()するとstartPlayback()がフラグを
                // 消費してロード直後にFocusSleepへ差し戻し、直後の
                // 全画面切り替え（videoStarted）でフォーカス対象になっても
                // 誰も起こさず「カメラは次へ行くのに再生されない」状態に
                // なる（ログで確認済みの実バグ）。Flowによる再生開始は
                // 明示的な再生意図なので、先に保留を解除しておく。
                lk.to->clearPendingFocusSleep();
                lk.to->bcSeek(0);
                lk.to->bcPlay();
            }
            qDebug("[Flow] 次を再生しリンク削除: %p -> %p (先読み昇格=%d)",
                   (void*)lk.from, (void*)lk.to, promoted);
            // 2026-07-04: チェーンが通り過ぎたfrom側はもう二度と自動再生され
            // ない（リンクを削除済み）ため、Ended状態のままmpv/FBOを保持し
            // 続けるとメモリを無駄に浮かせたままになる。Sleepへ落として
            // デコード資源を解放する（手動クリックでの再生は通常のIdleと
            // 同様に最初から再オープンする形で引き続き可能）。
            if (finishedFrom)
                finishedFrom->requestSleepAfterFlowEnded();
            // videoStarted は CanvasWidget 側で focusItem(to) を同期実行する。
            // 先に from を Sleep へ落としておかないと、フォーカス切替の
            // 再生ポリシーが「直前まで動いていた動画」として from を扱い、
            // 遅延wakeで再開させてしまうことがある。
            emit videoStarted(lk.to);
            it = m_links.erase(it);
            emit changed();
        } else {
            ++it;
        }
    }
}
