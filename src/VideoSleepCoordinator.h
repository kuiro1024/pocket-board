#pragma once

#include <functional>
#include <QObject>
#include "BubbleController.h"

class MpvPlayer;

// Sleep（完全休止）中のmpvリソース解放/再生成（Phase 8）。
//
// 経緯: 元は CanvasVideo の個別メンバ4つ(m_pendingSleepGlRelease /
// m_pendingMpvPlayerTeardown / m_pendingMpvRecreateForWake /
// m_pendingSleepWakeResumePlayback)と、CanvasVideo::paintGL() 冒頭に
// あった3つの if ブロックだった。Step 1で状態データをこのファイルへ、
// Step 4（このクラス化）で消費処理本体もここへ移した。
//
// 重要な前提（計画書の「後回しにすべき危険領域」に「Sleep中のMpvPlayer
// 解放/再生成」として名指しされている箇所）:
//   - consumeGlRelease() / consumeMpvPlayerTeardown() /
//     consumeMpvRecreateForWake() は、GLコンテキストがcurrentである
//     保証があるタイミング（CanvasVideo::paintGL()内）でのみ呼ぶこと。
//     呼び出し元・呼び出し順序・呼び出しタイミングは一切変更していない
//     （paintGL()内の3ブロックをそのまま関数化しただけ）。
//   - m_mpvPlayer（CanvasVideoが所有する再生中のプレイヤー本体）の
//     所有権はこのクラスへは移さない。参照(MpvPlayer*&)で受け取り、
//     delete / nullptr代入 / 新規生成の結果格納を行うだけで、
//     「誰が持つか」は従来通りCanvasVideo側のまま。
//   - MpvPlayerの実際の再生成（initializeMpv()相当）はGLコンテキストの
//     扱いも含めCanvasVideo側の責務のまま。このクラスからは
//     std::function コールバック経由で1回呼ぶだけ（deleteLaterには
//     置き換えない、同期delete/同期生成という既存の前提を維持）。
class VideoSleepCoordinator
{
public:
    // ---- 状態参照（診断ログ dump 用）----
    bool glReleasePending() const          { return m_glRelease; }
    bool mpvPlayerTeardownPending() const  { return m_mpvPlayerTeardown; }
    bool mpvRecreateForWakePending() const { return m_mpvRecreateForWake; }
    bool wakeResumePlayback() const        { return m_wakeResumePlayback; }

    // ---- 状態セット（既存のconnect先から呼ぶ。副作用なし、フラグを立てるだけ）----
    void markGlReleasePending()          { m_glRelease = true; }
    void markMpvPlayerTeardownPending()  { m_mpvPlayerTeardown = true; }
    void markMpvRecreateForWakePending() { m_mpvRecreateForWake = true; }
    void setWakeResumePlayback(bool resume) { m_wakeResumePlayback = resume; }

    // ---- 消費（CanvasVideo::paintGL()内からのみ呼ぶこと）----

    // 旧 paintGL() 内 if (m_pendingSleepGlRelease) ブロック。
    // owner: ログのvideo=%p用。mpvPlayer: 解放対象（nullptrなら何もしない）。
    void consumeGlRelease(QObject *owner, MpvPlayer *mpvPlayer);

    // 旧 paintGL() 内 if (m_pendingMpvPlayerTeardown) ブロック。
    // mpvPlayer: CanvasVideo::m_mpvPlayer への参照。Sleep状態かつ非null
    // であれば controller.setPlayer(nullptr) → delete → nullptr代入する
    // （所有権はCanvasVideo側のまま、このクラスはCanvasVideoに代わって
    // 破棄処理を実行するだけ）。
    void consumeMpvPlayerTeardown(QObject *owner, MpvPlayer *&mpvPlayer, BubbleController &controller);

    // 旧 paintGL() 内 if (m_pendingMpvRecreateForWake) ブロック。
    // mpvPlayer が nullptr の場合のみ recreatePlayer()（= 旧
    // initializeMpv()）を呼び、生成結果を mpvPlayer 経由で確認する。
    void consumeMpvRecreateForWake(QObject *owner, MpvPlayer *&mpvPlayer, BubbleController &controller,
                                    const std::function<void()> &recreatePlayer);

private:
    bool m_glRelease {false};
    bool m_mpvPlayerTeardown {false};
    bool m_mpvRecreateForWake {false};
    bool m_wakeResumePlayback {true};
};
