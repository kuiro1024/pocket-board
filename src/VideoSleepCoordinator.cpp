#include "VideoSleepCoordinator.h"
#include "engine/MpvPlayer.h"
#include "MpvTeardown.h"
#include <QDebug>

// 以下、CanvasVideo::paintGL() 冒頭にあった3つの if ブロックから
// そのまま移設した本体。条件・順序・ログ文言は元のまま
// （"video=%p"／"CanvasVideo %p" は owner ポインタをそのまま使う）。
//
// 2026-07-15: MpvPlayerの破棄そのものと、破棄に伴うOLEドロップターゲット
// 修復（🚫バグ対策）は destroyMpvPlayer()（MpvTeardown.h）に一元化した。
// 経緯はそちらのコメント参照。

void VideoSleepCoordinator::consumeGlRelease(QObject *owner, MpvPlayer *mpvPlayer)
{
    if (!m_glRelease)
        return;
    m_glRelease = false;
    if (mpvPlayer) {
        mpvPlayer->releaseVideoResources();
        qDebug("[CanvasVideo %p] [Sleep] mpv FBO/texture released", static_cast<void*>(owner));
    }
}

void VideoSleepCoordinator::consumeMpvPlayerTeardown(QObject *owner, MpvPlayer *&mpvPlayer, BubbleController &controller)
{
    if (!m_mpvPlayerTeardown)
        return;
    m_mpvPlayerTeardown = false;
    // 消費直前に改めてSleep状態か確認する: フラグが立った後・このpaintGL()
    // より前のフレームで既に復帰（wakeUp）していた場合に、生きているべき
    // プレイヤーを誤って壊さないためのガード。
    if (mpvPlayer && controller.state() == BubbleState::Sleep) {
        controller.setPlayer(nullptr);
        qDebug("[CanvasVideo %p] [Sleep] MpvPlayer 丸ごと解放（メモリ節約）", static_cast<void*>(owner));
        destroyMpvPlayer(owner, mpvPlayer);
    }
}

void VideoSleepCoordinator::consumeMpvRecreateForWake(QObject *owner, MpvPlayer *&mpvPlayer, BubbleController &controller,
                                                        const std::function<void()> &recreatePlayer)
{
    if (!m_mpvRecreateForWake)
        return;
    m_mpvRecreateForWake = false;
    if (!mpvPlayer) {
        recreatePlayer();
        if (mpvPlayer && controller.state() == BubbleState::Sleep) {
            qDebug("[CanvasVideo %p] [Sleep] MpvPlayer 再生成完了、wakeUp()再試行", static_cast<void*>(owner));
            const bool resumePlayback = m_wakeResumePlayback;
            m_wakeResumePlayback = true;
            controller.wakeUp(resumePlayback);
        } else if (!mpvPlayer) {
            qWarning("[CanvasVideo %p] [Sleep] MpvPlayer 再生成に失敗", static_cast<void*>(owner));
        }
    }
}
