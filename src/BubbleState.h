#pragma once
#include <QString>

// ============================================================
//  BubbleState.h
//
//  PocketBoard の動画バブルが取りうる全状態を定義する。
//  BubbleController がこの状態機械を管理する。
//
//  【依存】
//    Qt: QString のみ（Core のみ）
//    libmpv / QMediaPlayer: 一切依存しない
//
//  このヘッダは CanvasVideo / BubbleController / FlowMode /
//  CanvasWidget すべてから安全に include できる。
// ============================================================

enum class BubbleState
{
    // ---- 初期 ----
    Idle,

    // ---- ロード ----
    Loading,    // IPbPlayer::open() 呼び出し後

    // ---- 再生 ----
    Ready,      // 最初のフレーム到達・再生待ち
    Playing,    // デコード・レンダリングループ稼働中
    Paused,     // 明示的一時停止（ユーザー / FlowMode）
    Buffering,  // ネットワーク待ち
    PausedHidden,
    ReadyHidden,
    FocusSleep,

    // ---- 省資源 ----
    AudioOnly,  // 画面外。音声・再生位置は継続。映像デコードのみ停止。
                // vid=no で映像トラックを切り離し、最後のフレームを保持する。
                // 画面内に戻ると vid=auto で映像を再開（seek 不要）。
                //
                // ★ Sleep とは別物 ★
                //   Sleep   … 音声も停止・再生位置もリセット。stop() を呼ぶ。
                //   AudioOnly … 音声継続・再生位置継続。pauseVideo() を呼ぶのみ。

    Sleep,      // 完全待機。デコード停止・フレーム解放済み・音声も停止。
                // 画面内に戻ると Loading → Ready へ復帰（最初から再生）。
                // FlowMode のスタンバイ等、音声まで止めたい場合に使用。

    // ---- 終端 ----
    Ended,      // EOF 到達。FlowMode が次を起動する契機。
    Error,      // 取得失敗 / デコードエラー。retry() で Loading へ。
};

// ---- デバッグ・ログ用 ----
inline QString bubbleStateToString(BubbleState s)
{
    switch (s) {
    case BubbleState::Idle:       return QStringLiteral("Idle");
    case BubbleState::Loading:    return QStringLiteral("Loading");
    case BubbleState::Ready:      return QStringLiteral("Ready");
    case BubbleState::Playing:    return QStringLiteral("Playing");
    case BubbleState::Paused:     return QStringLiteral("Paused");
    case BubbleState::Buffering:  return QStringLiteral("Buffering");
    case BubbleState::PausedHidden: return QStringLiteral("PausedHidden");
    case BubbleState::ReadyHidden:  return QStringLiteral("ReadyHidden");
    case BubbleState::FocusSleep:   return QStringLiteral("FocusSleep");
    case BubbleState::AudioOnly:  return QStringLiteral("AudioOnly");
    case BubbleState::Sleep:      return QStringLiteral("Sleep");
    case BubbleState::Ended:      return QStringLiteral("Ended");
    case BubbleState::Error:      return QStringLiteral("Error");
    }
    return QStringLiteral("Unknown");
}

// ---- 状態遷移の合法性チェック（BubbleController 内で使用）----
inline bool isBubbleStateTransitionValid(BubbleState from, BubbleState to)
{
    using S = BubbleState;
    switch (from) {
    case S::Idle:
        return to == S::Loading;

    case S::Loading:
        return to == S::Ready
            || to == S::Playing      // FILE_LOADED → Decoding を直接受けた場合
            || to == S::AudioOnly    // ロード中に画面外へ移動した場合（音声優先）
            || to == S::FocusSleep   // ロード中に別動画が全画面になった場合
            || to == S::Sleep        // ロード中に完全待機が必要な場合
            || to == S::Error;

    case S::Ready:
        return to == S::Playing
            || to == S::AudioOnly
            || to == S::FocusSleep
            || to == S::ReadyHidden
            || to == S::Sleep
            || to == S::Loading;     // 品質切り替え時の再ロード

    case S::Playing:
        return to == S::Paused
            || to == S::Buffering
            || to == S::Ended
            || to == S::AudioOnly    // 画面外 → 映像だけ停止
            || to == S::FocusSleep
            || to == S::Sleep        // 完全停止が必要な場合
            || to == S::Error;

    case S::Paused:
        return to == S::Playing
            || to == S::AudioOnly
            || to == S::FocusSleep
            || to == S::PausedHidden
            || to == S::Sleep
            || to == S::Loading      // 品質切り替え
            || to == S::Error;

    case S::Buffering:
        return to == S::Playing
            || to == S::AudioOnly
            || to == S::FocusSleep
            || to == S::Sleep
            || to == S::Error;

    case S::PausedHidden:
        return to == S::Paused
            || to == S::Playing
            || to == S::FocusSleep
            || to == S::Sleep
            || to == S::Loading
            || to == S::Error;

    case S::ReadyHidden:
        return to == S::Ready
            || to == S::Playing
            || to == S::FocusSleep
            || to == S::Sleep
            || to == S::Loading
            || to == S::Error;

    case S::FocusSleep:
        return to == S::Paused
            || to == S::Playing
            || to == S::Sleep
            || to == S::Loading
            || to == S::Error;

    case S::AudioOnly:
        return to == S::Playing      // 画面内復帰 → resumeVideo()
            || to == S::Buffering
            || to == S::Paused       // ユーザーが一時停止操作した場合
            || to == S::PausedHidden
            || to == S::FocusSleep
            || to == S::Sleep        // AudioOnly 中に完全待機が必要になった場合
            || to == S::Ended        // AudioOnly 中に EOF 到達
            || to == S::Error;

    case S::Sleep:
        return to == S::Loading      // wakeUp → 再オープン
            || to == S::Error;

    case S::Ended:
        return to == S::Ready        // seek(0) で先頭に戻した
            || to == S::Playing      // seek(0)+play() 後に Decoding 経由で直接遷移
            || to == S::Loading      // 品質切り替え
            || to == S::AudioOnly
            || to == S::Sleep;

    case S::Error:
        return to == S::Loading;     // retry() のみ
    }
    return false;
}
