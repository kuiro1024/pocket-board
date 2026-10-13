#include "VideoRenderThread.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

// 詳細は VideoRenderThread.h / md/async_video_render_design_2026-09-15.md 参照。

namespace {
VideoRenderThread *g_shared = nullptr;
bool g_quitHooked = false;
}

VideoRenderThread *VideoRenderThread::shared()
{
    return g_shared;
}

void VideoRenderThread::startShared(QOpenGLContext *shareContext)
{
    if (!shareContext) {
        qWarning("[VideoRender] startShared: no share context");
        return;
    }
    if (g_shared && g_shared->m_shareSrc == shareContext)
        return; // 同じ共有元で既に動いている
    if (g_shared) {
        qInfo("[VideoRender] share context changed; restarting worker");
        shutdownShared();
    }

    auto *t = new VideoRenderThread();
    if (!t->startWorker(shareContext)) {
        delete t;
        return;
    }
    g_shared = t;

    if (!g_quitHooked && QCoreApplication::instance()) {
        g_quitHooked = true;
        // ウィジェット破棄より先にアプリ終了処理が走る経路の保険。
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                         QCoreApplication::instance(), [] { shutdownShared(); });
    }
}

void VideoRenderThread::shutdownShared()
{
    VideoRenderThread *t = g_shared;
    if (!t)
        return;
    g_shared = nullptr; // 以後 shared() は nullptr（新しいジョブを積ませない）
    t->stopWorker();
    delete t;
}

bool VideoRenderThread::startWorker(QOpenGLContext *shareContext)
{
    QElapsedTimer clock;
    clock.start();

    m_guiThread = QThread::currentThread();
    m_shareSrc  = shareContext;

    // QOffscreenSurface はGUIスレッドで生成する必要がある（Qtの制約）。
    m_surface = new QOffscreenSurface();
    m_surface->setFormat(shareContext->format());
    m_surface->create();

    m_ctx = new QOpenGLContext();
    m_ctx->setFormat(shareContext->format());
    m_ctx->setShareContext(shareContext);
    const bool created = m_ctx->create();
    const bool sharing = created && QOpenGLContext::areSharing(m_ctx, shareContext);

    if (!created || !sharing || !m_surface->isValid()) {
        qWarning("[VideoRender] start failed: created=%d sharing=%d surfaceValid=%d",
                 created, sharing, m_surface->isValid());
        delete m_ctx;     m_ctx = nullptr;
        delete m_surface; m_surface = nullptr;
        return false;
    }

    m_ctx->moveToThread(this);
    setObjectName(QStringLiteral("VideoRenderThread"));
    start();

    // ワーカーが makeCurrent できたかだけ待つ（起動時1回・数ms）。
    {
        QMutexLocker lk(&m_mutex);
        while (!m_startDone)
            m_startCv.wait(&m_mutex);
    }
    if (!m_startOk) {
        qWarning("[VideoRender] worker makeCurrent failed");
        wait();
        delete m_ctx;     m_ctx = nullptr;   // run() がGUIスレッドへ戻してある
        delete m_surface; m_surface = nullptr;
        return false;
    }

    qInfo("[VideoRender] started thread=%p sharing=%d startMs=%lld",
          static_cast<void *>(this), sharing, static_cast<long long>(clock.elapsed()));

    // P1 疎通確認: ワーカーのGL current下で1回だけ実行されることを見る。
    QElapsedTimer *pingClock = new QElapsedTimer();
    pingClock->start();
    post([pingClock]() {
        const qint64 latency = pingClock->elapsed();
        delete pingClock;
        QOpenGLContext *c = QOpenGLContext::currentContext();
        const char *renderer = nullptr;
        if (c)
            renderer = reinterpret_cast<const char *>(c->functions()->glGetString(GL_RENDERER));
        qInfo("[VideoRender] ping ok current=%d renderer=%s latencyMs=%lld thread=%p",
              c != nullptr, renderer ? renderer : "(null)",
              static_cast<long long>(latency),
              static_cast<void *>(QThread::currentThread()));
    });
    return true;
}

void VideoRenderThread::stopWorker()
{
    QElapsedTimer clock;
    clock.start();
    int pending = 0;
    {
        QMutexLocker lk(&m_mutex);
        m_stopRequested = true;
        pending = m_jobs.size();
        m_jobCv.wakeAll();
    }
    wait(); // 残りジョブ（将来は Destroy 等）を全部実行し終えるまで待つ

    delete m_ctx;     m_ctx = nullptr;     // run() がGUIスレッドへ戻してある
    delete m_surface; m_surface = nullptr;

    qInfo("[VideoRender] shutdown done pendingAtStop=%d jobs=%llu maxJobMs=%lld waitMs=%lld",
          pending, static_cast<unsigned long long>(m_jobCount),
          static_cast<long long>(m_maxJobMs), static_cast<long long>(clock.elapsed()));
}

bool VideoRenderThread::post(Job job)
{
    if (!job)
        return false;
    QMutexLocker lk(&m_mutex);
    if (m_stopRequested)
        return false;
    m_jobs.append(std::move(job));
    m_jobCv.wakeOne();
    return true;
}

void VideoRenderThread::addFinalizer(quintptr key, Job job)
{
    if (!job)
        return;
    QMutexLocker lk(&m_mutex);
    m_finalizers.insert(key, std::move(job));
}

void VideoRenderThread::removeFinalizer(quintptr key)
{
    QMutexLocker lk(&m_mutex);
    m_finalizers.remove(key);
}

void VideoRenderThread::run()
{
    const bool ok = m_ctx->makeCurrent(m_surface);
    {
        QMutexLocker lk(&m_mutex);
        m_startOk   = ok;
        m_startDone = true;
        m_startCv.wakeAll();
    }
    if (!ok) {
        m_ctx->moveToThread(m_guiThread);
        return;
    }

    QElapsedTimer jobClock;
    for (;;) {
        Job job;
        {
            QMutexLocker lk(&m_mutex);
            while (m_jobs.isEmpty() && !m_stopRequested)
                m_jobCv.wait(&m_mutex);
            if (m_jobs.isEmpty())
                break; // 停止要求があり、積まれたジョブも全部終わった
            job = m_jobs.takeFirst();
        }
        jobClock.start();
        job();
        const qint64 ms = jobClock.elapsed();
        ++m_jobCount;
        if (ms > m_maxJobMs)
            m_maxJobMs = ms;
    }

    // 2026-09-15 P2: まだ Destroy されていない slot の後始末（GL current のうちに）。
    QHash<quintptr, Job> finalizers;
    {
        QMutexLocker lk(&m_mutex);
        finalizers.swap(m_finalizers);
    }
    if (!finalizers.isEmpty())
        qInfo("[VideoRender] running %d finalizer(s) at shutdown", int(finalizers.size()));
    for (auto it = finalizers.begin(); it != finalizers.end(); ++it)
        it.value()();

    m_ctx->doneCurrent();
    m_ctx->moveToThread(m_guiThread); // GUIスレッドで delete できるように戻す
}
