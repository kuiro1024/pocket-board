#pragma once

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QList>
#include <QHash>
#include <functional>

class QOpenGLContext;
class QOffscreenSurface;

// ============================================================
//  engine/VideoRenderThread.h   （2026-09-15 非同期化 P1）
//
//  mpvの描画をGUIスレッドから追い出すためのワーカースレッド。
//  設計: md/async_video_render_design_2026-09-15.md
//
//  - 全動画で1本だけ（startShared()/shutdownShared() で管理）
//  - CanvasWidget のGLコンテキストとリソース共有する2個目のコンテキストを持ち、
//    ワーカー上で常に current にしておく
//  - post() で積んだジョブを積んだ順にワーカーで実行する（GL current 保証）
//
//  【P1時点】器だけ。どの動画もまだ使っていない。起動時に ping ジョブを
//  1回流して、共有コンテキストでGLが使えることだけをログで確認する。
//
//  【制約】MSYS2 MinGW の libstdc++ 食い違い（2026-09-15）を踏まないため、
//  std::thread / std::future / std::condition_variable::wait_for は使わない。
//  QThread + QMutex + QWaitCondition で書く。
//  【制約】ワーカーから GUI へ BlockingQueuedConnection で問い合わせない
//  （vd-lavc-dr の循環待ちの再発防止。設計§4）。
// ============================================================
class VideoRenderThread : public QThread
{
public:
    using Job = std::function<void()>;

    // CanvasWidget::initializeGL() から呼ぶ。既に別の共有元で動いていれば
    // 先に畳んでから作り直す。GUIスレッド専用。
    static void startShared(QOpenGLContext *shareContext);
    // CanvasWidget のコンテキスト破棄直前 / アプリ終了時に呼ぶ。
    // 積まれているジョブを全部実行し終えてからスレッドを止める（同期）。
    // 多重呼び出し安全。GUIスレッド専用。
    static void shutdownShared();
    // 動いていなければ nullptr。
    static VideoRenderThread *shared();

    // ワーカーでGL current下に実行するジョブを積む。どのスレッドからでも可。
    // 停止処理に入った後は false を返して捨てる。
    bool post(Job job);

    // 2026-09-15 P2: シャットダウン時にワーカー上（GL current）で必ず実行して
    // ほしい後始末を登録する。キーごとに1個。Destroy済みなら remove する。
    // 用途: 生きている mpv_render_context をワーカーのコンテキストが消える前に
    // 解放する（アプリ終了・コンテキスト喪失時）。
    void addFinalizer(quintptr key, Job job);
    void removeFinalizer(quintptr key);

protected:
    void run() override;

private:
    VideoRenderThread() = default;
    ~VideoRenderThread() override = default;

    bool startWorker(QOpenGLContext *shareContext);
    void stopWorker();

    QOpenGLContext    *m_ctx      = nullptr;
    QOffscreenSurface *m_surface  = nullptr;
    QOpenGLContext    *m_shareSrc = nullptr;
    QThread           *m_guiThread = nullptr;

    QMutex         m_mutex;
    QWaitCondition m_jobCv;
    QWaitCondition m_startCv;
    QList<Job>     m_jobs;
    QHash<quintptr, Job> m_finalizers;
    bool           m_stopRequested = false;
    bool           m_startDone     = false;
    bool           m_startOk       = false;

    // 統計（ワーカーだけが書き、停止後にGUIが読む）
    quint64 m_jobCount  = 0;
    qint64  m_maxJobMs  = 0;
};
