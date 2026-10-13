#include "FlowPreloadCoordinator.h"
#include "engine/MpvPlayer.h"
#include "MpvTeardown.h"
#include "engine/VideoRenderThread.h"
#include <QDebug>

// 以下、CanvasVideo::beginFlowPreload() / tickFlowPreloadWarmup() /
// promoteFlowPreload() / cancelFlowPreload() から移設した本体。
// ログ文言・条件・順序は元のまま（"video=%p" は owner ポインタをそのまま使う）。

void FlowPreloadCoordinator::begin(QObject *owner)
{
    m_phase = FlowPreloadPhase::WarmingUp;
    qDebug("[FlowPreload] 開始 video=%p", static_cast<void*>(owner));
}

void FlowPreloadCoordinator::tick(QObject *owner, const WarmupInput &input)
{
    if (m_phase == FlowPreloadPhase::Ready) {
        // 準備完了後、takeReadyPlayer()が一定時間呼ばれなければ
        // （リンクが編集で消えた等）諦めて解放する。
        if (m_timer.elapsed() >= 20000) {
            qWarning("[FlowPreload] 準備完了後20秒経っても昇格されないため破棄 video=%p",
                     static_cast<void*>(owner));
            cancel(owner);
        }
        return;
    }

    if (!m_player) {
        // Sleepから外れていないか改めて確認（先読み要求後に何らかの
        // 理由で状態が変わっていた場合の保険）。
        if (!input.stillSleeping) {
            cancel(owner);
            return;
        }
        m_player = new MpvPlayer(owner);
        // 2026-09-16 非同期化 P5: 本再生と同じくワーカー経路で生成・描画する
        // （初回 render 約25ms を GUI から追い出す）。ワーカーが無ければ失敗扱い（P7で同期経路撤去）。
        VideoRenderThread *renderThread = VideoRenderThread::shared();
        const bool glOk = renderThread && m_player->initializeGlAsync(renderThread);
        if (!glOk) {
            qWarning("[FlowPreload] 裏側MpvPlayer GL初期化失敗、中止 video=%p", static_cast<void*>(owner));
            // 2026-09-15 P0: 直接deleteしていたため🚫対策（refreshDropTargets）が
            // 漏れていた。MpvTeardown.h の規約どおり destroyMpvPlayer() を通す
            // （m_player は内部で nullptr になる）。GL初期化失敗なのでFBOは無い。
            destroyMpvPlayer(owner, m_player);
            m_phase = FlowPreloadPhase::Idle;
            return;
        }
        m_player->setHttpHeaders(input.httpHeaders);
        m_player->setVolume(0.0f); // 昇格まで無音（二重音声防止）
        m_player->open(input.streamUrl, 0, input.audioUrl);

        m_timer.restart();
        m_frameCount = 0;
        qDebug("[FlowPreload] 裏側MpvPlayer生成完了 video=%p", static_cast<void*>(owner));
        return;
    }

    if (m_player->internalState() == PbInternalState::Error) {
        qWarning("[FlowPreload] 裏側プレイヤーでエラー発生 video=%p", static_cast<void*>(owner));
        cancel(owner);
        return;
    }

    // 2026-09-16 P5: 非同期プレイヤーは「描画してよい・サイズ」を渡して
    // ワーカーの仕上げた1枚を受け取る。フレーム数は「新しい1枚を受け取った
    // 回数」で数える（従来の renderToFbo 1回＝1枚と同じ意味）。
    m_player->updateAsyncRenderState(input.renderW, input.renderH, true);
    const bool newFrame = m_player->latchAsyncFrame();

    const PbVideoFrame frame = m_player->currentFrame();
    const bool fileReallyLoaded =
        m_player->internalState() == PbInternalState::Decoding
        || m_player->internalState() == PbInternalState::Paused
        || m_player->internalState() == PbInternalState::Buffering;
    if (newFrame && frame.isValid() && fileReallyLoaded)
        m_frameCount++;

    const bool longEnough = m_timer.elapsed() >= 150;
    if (frame.isValid() && fileReallyLoaded && m_frameCount >= 3 && longEnough) {
        m_phase = FlowPreloadPhase::Ready;
        m_timer.restart(); // Ready後のタイムアウト計測用に流用
        qDebug("[FlowPreload] 準備完了 video=%p", static_cast<void*>(owner));
        return;
    }

    // 先読みは急ぐ必要がないため、画質切り替えより長めのタイムアウトを
    // 許容する（ここで諦めても、from側が実際にEndedになった時点で
    // 従来の再オープン経路にフォールバックするだけなので実害は小さい）。
    if (m_timer.elapsed() >= 10000) {
        qWarning("[FlowPreload] ウォームアップがタイムアウト video=%p", static_cast<void*>(owner));
        cancel(owner);
    }
}

MpvPlayer *FlowPreloadCoordinator::takeReadyPlayer(QObject *owner, BubbleState currentState)
{
    if (m_phase != FlowPreloadPhase::Ready || !m_player) {
        // 間に合わなかった。呼び出し側は従来の再オープン経路へ
        // フォールバックすることになる。呼び出し元はGLコンテキスト外
        // （FlowMode::tickPlayback）なので、ここで直接deleteせず
        // CanvasVideo::cancelFlowPreload()へ後始末を委ねる。
        return nullptr;
    }
    if (currentState != BubbleState::Sleep) {
        // 先読み中に何らかの理由で状態が変わっていた（例: ユーザーが
        // 手動でクリックして既に再生していた等）。昇格を諦める。
        qWarning("[FlowPreload] 昇格直前にstateがSleepでなくなっていたため中止 state=%s video=%p",
                 qPrintable(bubbleStateToString(currentState)), static_cast<void*>(owner));
        return nullptr;
    }

    MpvPlayer *p = m_player;
    m_player = nullptr;
    m_phase = FlowPreloadPhase::Idle;
    m_frameCount = 0;
    return p;
}

void FlowPreloadCoordinator::cancel(QObject *owner)
{
    if (m_player) {
        // 2026-09-15 P0: 直接deleteしていたため🚫対策（refreshDropTargets）が
        // 漏れていた。MpvTeardown.h の規約どおり destroyMpvPlayer() を通す。
        // 呼び出し元は tick()（GL current前提）なので FBO 解放も同時に行う。
        destroyMpvPlayer(owner, m_player, /*releaseGlBeforeDelete=*/true);
    }
    m_phase = FlowPreloadPhase::Idle;
    m_frameCount = 0;
}
