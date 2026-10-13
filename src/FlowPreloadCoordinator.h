#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include "BubbleState.h"

class QObject;
class MpvPlayer;

// Flow直前先読み（Phase 8）。
//
// 経緯: 元は CanvasVideo の個別メンバ(m_flowPreloadState /
// m_flowPreloadPlayer / m_flowPreloadTimer / m_flowPreloadFrameCount)と
// CanvasVideo::beginFlowPreload() / tickFlowPreloadWarmup() /
// promoteFlowPreload() / cancelFlowPreload() の4関数だった。
// Step 1で状態データをこのファイルの構造体へ、Step 2でログ用の
// toString ヘルパーを追加し、Step 3（このクラス化）で状態遷移ロジック
// 本体もここへ移した。
//
// 呼び出し元（CanvasVideo）との役割分担:
//   - Idle/Sleep/streamUrl空 等、CanvasVideo固有の状態を見るガード判定は
//     引き続き CanvasVideo::beginFlowPreload() / promoteFlowPreload() 側に残る。
//   - Warmup中の裏側MpvPlayer生成・renderToFbo・準備完了/タイムアウト判定・
//     Ready後のタイムアウト監視・player破棄は、このクラスへ移した。
//   - GLコンテキストがcurrentである前提の tick() は、従来通り
//     CanvasVideo::paintGL() 内からのみ呼ばれる（呼び出しタイミングは
//     一切変更していない）。
//   - owner には呼び出し元の CanvasVideo(QObject*) をそのまま渡す。
//     元のコードが `new MpvPlayer(this)` として CanvasVideo を
//     QObjectの親に設定していたのと同じ関係を維持するため
//     （＝万一明示的な delete 漏れがあっても、CanvasVideo破棄時に
//     Qtの親子関係で道連れに解放される安全網を保つ）。ログの
//     "video=%p" 部分にも同じポインタをそのまま使う。
enum class FlowPreloadPhase {
    Idle,       // 先読みしていない
    WarmingUp,  // 裏側MpvPlayerを生成・デコード進行中
    Ready,      // 準備完了、takeReadyPlayer()待ち
};

// ---- デバッグ・ログ用 ----
// BubbleState.h の bubbleStateToString() と同じスタイル。
inline QString flowPreloadPhaseToString(FlowPreloadPhase p)
{
    switch (p) {
    case FlowPreloadPhase::Idle:      return QStringLiteral("Idle");
    case FlowPreloadPhase::WarmingUp: return QStringLiteral("WarmingUp");
    case FlowPreloadPhase::Ready:     return QStringLiteral("Ready");
    }
    return QStringLiteral("Unknown");
}

class FlowPreloadCoordinator
{
public:
    bool isIdle() const { return m_phase == FlowPreloadPhase::Idle; }
    FlowPreloadPhase phase() const { return m_phase; }
    MpvPlayer *player() const { return m_player; }
    int frameCount() const { return m_frameCount; }

    // 先読み開始（旧 CanvasVideo::beginFlowPreload() の後半）。
    // 呼び出し元が Idle / bubbleState()==Sleep / streamUrl非空、の
    // ガード判定を済ませたあとに呼ぶこと。WarmingUpへ遷移するだけ。
    void begin(QObject *owner);

    struct WarmupInput
    {
        QString streamUrl;
        QStringList httpHeaders;
        QString audioUrl;
        int renderW {0};
        int renderH {0};
        // 呼び出し元が確認した「現在も bubbleState()==Sleep か」。
        // player がまだ無い時のみ参照する（元の tickFlowPreloadWarmup()
        // の該当ガードと同じ意味）。
        bool stillSleeping {false};
    };
    // 旧 CanvasVideo::tickFlowPreloadWarmup() 本体。
    // GLコンテキストがcurrentであることが前提（CanvasVideo::paintGL()内
    // からのみ呼ぶこと。呼び出しタイミングは変更していない）。
    void tick(QObject *owner, const WarmupInput &input);

    // 旧 CanvasVideo::promoteFlowPreload() のうち、Ready判定〜player
    // 譲渡までの部分（bubbleState()==Sleepチェック含む）。
    // 条件を満たさなければnullptrを返す。呼び出し元はGLコンテキスト外
    // から呼ばれることがあるため、ここでは破棄せず、CanvasVideo側の
    // cancelFlowPreload()でGL破棄待ちキューへ移すこと。
    // 戻り値が非nullptrの場合、以後のplayerの所有権は呼び出し元に移る
    // （このクラス内の状態はIdleへ戻る）。
    MpvPlayer *takeReadyPlayer(QObject *owner, BubbleState currentState);

    // 旧 CanvasVideo::cancelFlowPreload() 本体
    // （release + delete + phase=Idle + frameCount=0）。
    void cancel(QObject *owner);

    // ---- 破棄経路（~CanvasVideo() / CanvasVideo::releaseGlResources()）専用 ----
    // 通常の cancel() と違い、releaseVideoResources()/delete の実行や
    // phase/frameCountのリセットは呼び出し元が個別に行う場合に使う
    // （元のコードがこの2箇所で微妙に異なる後始末をしていたため、
    // 挙動を完全に保つには通常cancel()を流用せずこの形が必要）。
    // 内部ポインタをnullptrに戻しつつ、元のポインタを返す（所有権譲渡）。
    MpvPlayer *releasePlayerOwnership() { MpvPlayer *p = m_player; m_player = nullptr; return p; }
    // phaseだけをIdleに戻す（frameCountはリセットしない）。
    void markIdleAfterExternalRelease() { m_phase = FlowPreloadPhase::Idle; }

private:
    FlowPreloadPhase m_phase {FlowPreloadPhase::Idle};
    MpvPlayer *m_player {nullptr};
    QElapsedTimer m_timer;
    int m_frameCount {0};
};
