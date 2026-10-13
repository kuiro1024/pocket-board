#pragma once

class QOpenGLWidget;

// ============================================================
//  AsyncRenderProbe（2026-09-15 使い捨て実験コード・本線では使わない）
//
//  目的: md/async_video_render_study_2026-09-14.md のステップB。
//  「mpvの重い区間（初回render 22ms / render_context_free 31ms /
//  terminate_destroy 10ms）を、ウィジェットのGLコンテキストと共有する
//  2個目のGLコンテキスト＋ワーカースレッドで実行したとき、GUIスレッドが
//  止まらないか」だけを確かめる。
//
//  起動条件: 【実験期間中は既定で有効】initializeGL()の8秒後に1回走る。
//  止めたいときは環境変数 PB_ASYNC_PROBE=0。実験後はファイルごと削除する。
//  任意: PB_ASYNC_PROBE_URL で再生するメディアを指定（未指定ならlavfiの
//  1080pテストパターン）。
//
//  ログ: [AsyncProbe] で grep。実験が終わったらこのファイルごと削除する。
// ============================================================
namespace AsyncRenderProbe {
void startIfRequested(QOpenGLWidget *canvas);
}
