#include "AsyncRenderProbe.h"

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#include <array>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

// 詳細は AsyncRenderProbe.h 参照。使い捨ての実験コード。
//
// 【何を比べるか】
// ワーカーが「いま何をしているか」を phase（atomic）で公開し、GUIスレッド側の
// ハートビート（5ms周期）が phase ごとに
//   - gap    : 前回tickからの間隔（イベントループが止まったか）
//   - finish : ウィジェットのコンテキストで glFinish() にかかった時間
//              （ドライバ内部のロック待ち・GPUキュー待ちで止まるか）
// の最大値を集計する。Idle（ワーカーが寝ている区間）が基準値。
// RenderFirst / Free / Destroy の最大値が Idle と同程度なら「逃げた」。
//
// ワーカー側からは実験中に一切ログを出さない（メッセージハンドラの
// ファイル書き込みロックでGUIスレッドを巻き込み、測定を汚さないため）。
// 結果は溜めておき、終了後にGUIスレッドでまとめて出す。

namespace {

enum Phase : int {
    Idle = 0,
    MpvInit,
    RctxCreate,
    WaitFirstFrame,
    RenderFirst,
    RenderSteady,
    Free,
    Destroy,
    PhaseCount
};

const char *phaseName(int p)
{
    switch (p) {
    case Idle:           return "Idle";
    case MpvInit:        return "MpvInit";
    case RctxCreate:     return "RctxCreate";
    case WaitFirstFrame: return "WaitFirstFrame";
    case RenderFirst:    return "RenderFirst";
    case RenderSteady:   return "RenderSteady";
    case Free:           return "Free";
    case Destroy:        return "Destroy";
    default:             return "?";
    }
}

constexpr int kCycles        = 3;
constexpr int kFramesPerCycle = 60;
constexpr int kFboW = 427;
constexpr int kFboH = 240;

struct CycleResult {
    int     cycle = 0;
    qint64  mpvInitMs = -1;
    qint64  rctxCreateMs = -1;
    qint64  firstRenderMs = -1;
    qint64  maxSteadyRenderMs = -1;
    int     frames = 0;
    qint64  freeMs = -1;
    qint64  destroyMs = -1;
    QString note;
};

struct Shared {
    std::atomic<int>  phase {Idle};
    std::atomic<bool> finished {false};

    // mpv update callback → ワーカーループへの通知
    // 2026-09-15: std::condition_variable は MSYS2 MinGW の winpthreads と
    // リンクが合わず pthread_cond_timedwait64 未定義になったため Qt の同期
    // プリミティブを使う。
    QMutex         mtx;
    QWaitCondition cv;
    bool           updatePending = false;

    // ワーカーの結果（終了後にGUIが読む）
    std::vector<CycleResult> results;
    QString                  fatal;
};

struct HeartbeatStats {
    std::array<int,    PhaseCount> ticks {};
    std::array<qint64, PhaseCount> maxGapMs {};
    std::array<qint64, PhaseCount> maxFinishMs {};
};

void *probeGetProcAddress(void *, const char *name)
{
    QOpenGLContext *c = QOpenGLContext::currentContext();
    return c ? reinterpret_cast<void *>(c->getProcAddress(name)) : nullptr;
}

void probeUpdateCallback(void *ctx)
{
    auto *s = static_cast<Shared *>(ctx);
    {
        QMutexLocker lk(&s->mtx);
        s->updatePending = true;
    }
    s->cv.wakeOne();
}

void drainEvents(mpv_handle *mpv)
{
    while (true) {
        mpv_event *ev = mpv_wait_event(mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE)
            break;
    }
}

// ---- ワーカースレッド本体 ----
void workerMain(Shared *s, QOpenGLContext *ctx, QOffscreenSurface *surface, const QByteArray &url)
{
    if (!ctx->makeCurrent(surface)) {
        s->fatal = QStringLiteral("worker makeCurrent failed");
        ctx->moveToThread(QCoreApplication::instance()->thread());
        s->finished = true;
        return;
    }
    QOpenGLFunctions *gl = ctx->functions();

    for (int cycle = 1; cycle <= kCycles; ++cycle) {
        CycleResult r;
        r.cycle = cycle;
        QElapsedTimer t;

        // --- mpv_create + mpv_initialize（GL不要。参考値） ---
        s->phase = MpvInit;
        t.start();
        mpv_handle *mpv = mpv_create();
        if (!mpv) { r.note = QStringLiteral("mpv_create failed"); s->results.push_back(r); break; }
        // 本線 MpvPlayer と描画に関わる設定を揃える。音は出さない。
        mpv_set_option_string(mpv, "terminal", "no");
        mpv_set_option_string(mpv, "msg-level", "all=error");
        mpv_set_option_string(mpv, "hwdec", "auto-safe");
        mpv_set_option_string(mpv, "hwdec-extra-frames", "3");
        mpv_set_option_string(mpv, "gpu-api", "opengl");
        mpv_set_option_string(mpv, "fbo-format", "rgba8");
        mpv_set_option_string(mpv, "vo", "libmpv");
        mpv_set_option_string(mpv, "ao", "null");
        mpv_set_option_string(mpv, "vd-lavc-dr", "yes");
        mpv_set_option_string(mpv, "opengl-pbo", "no");
        mpv_set_option_string(mpv, "ytdl", "no");
        mpv_set_option_string(mpv, "loop-file", "inf");
        mpv_set_option_string(mpv, "pause", "no");
        if (mpv_initialize(mpv) < 0) {
            r.note = QStringLiteral("mpv_initialize failed");
            mpv_destroy(mpv);
            s->results.push_back(r);
            break;
        }
        r.mpvInitMs = t.elapsed();

        // --- mpv_render_context_create（ワーカーのGLコンテキストがcurrent） ---
        s->phase = RctxCreate;
        mpv_opengl_init_params glInit{};
        glInit.get_proc_address = probeGetProcAddress;
        glInit.get_proc_address_ctx = nullptr;
        int advancedControl = 1;
        mpv_render_param createParams[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advancedControl},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };
        mpv_render_context *rctx = nullptr;
        t.restart();
        const int rc = mpv_render_context_create(&rctx, mpv, createParams);
        r.rctxCreateMs = t.elapsed();
        if (rc < 0) {
            r.note = QStringLiteral("mpv_render_context_create failed rc=%1").arg(rc);
            mpv_terminate_destroy(mpv);
            s->results.push_back(r);
            break;
        }
        {
            QMutexLocker lk(&s->mtx);
            s->updatePending = false;
        }
        mpv_render_context_set_update_callback(rctx, probeUpdateCallback, s);

        // --- ワーカー側のFBO（本線の既定サイズに合わせる） ---
        GLuint tex = 0, fbo = 0;
        gl->glGenTextures(1, &tex);
        gl->glBindTexture(GL_TEXTURE_2D, tex);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kFboW, kFboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glGenFramebuffers(1, &fbo);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);

        const char *cmd[] = {"loadfile", url.constData(), nullptr};
        mpv_command(mpv, cmd);

        // --- 描画ループ ---
        s->phase = WaitFirstFrame;
        QElapsedTimer loopClock;
        loopClock.start();
        while (r.frames < kFramesPerCycle && loopClock.elapsed() < 15000) {
            {
                QMutexLocker lk(&s->mtx);
                if (!s->updatePending)
                    s->cv.wait(&s->mtx, 50);
                s->updatePending = false;
            }
            drainEvents(mpv);
            const uint64_t flags = mpv_render_context_update(rctx);
            if (!(flags & MPV_RENDER_UPDATE_FRAME))
                continue;

            mpv_opengl_fbo fboParam{};
            fboParam.fbo = static_cast<int>(fbo);
            fboParam.w = kFboW;
            fboParam.h = kFboH;
            fboParam.internal_format = GL_RGBA8;
            int flipY = 0;
            int block = 0;
            mpv_render_param rp[] = {
                {MPV_RENDER_PARAM_OPENGL_FBO, &fboParam},
                {MPV_RENDER_PARAM_FLIP_Y, &flipY},
                {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                {MPV_RENDER_PARAM_INVALID, nullptr}
            };

            const bool first = (r.frames == 0);
            s->phase = first ? RenderFirst : RenderSteady;
            t.restart();
            mpv_render_context_render(rctx, rp);
            const qint64 ms = t.elapsed();
            mpv_render_context_report_swap(rctx);
            if (first)
                r.firstRenderMs = ms;
            else if (ms > r.maxSteadyRenderMs)
                r.maxSteadyRenderMs = ms;
            ++r.frames;
            s->phase = RenderSteady;
        }
        if (r.frames == 0)
            r.note = QStringLiteral("no frame within 15s (URL unplayable?)");

        // --- 破棄（本線と同じ順序: free → terminate_destroy） ---
        gl->glDeleteFramebuffers(1, &fbo);
        gl->glDeleteTextures(1, &tex);

        s->phase = Free;
        t.restart();
        mpv_render_context_free(rctx);
        r.freeMs = t.elapsed();

        s->phase = Destroy;
        t.restart();
        mpv_terminate_destroy(mpv);
        r.destroyMs = t.elapsed();

        s->results.push_back(r);

        s->phase = Idle;
        QThread::msleep(1500);
        if (r.frames == 0)
            break; // 再生できないURLなら繰り返しても意味がない
    }

    s->phase = Idle;
    ctx->doneCurrent();
    ctx->moveToThread(QCoreApplication::instance()->thread());
    s->finished = true;
}

// 2026-09-15: QThread::create() は Qt6 の実装が内部で std::future（=std::call_once）
// を使い、実行時に「__emutls_v._ZSt11__once_call のエントリポイントが
// 見つからない」で起動できなかった（exeの横にある libstdc++-6.dll が
// ビルド時のものと食い違っている）。run() を直接オーバーライドする
// 素朴な QThread 派生で回避する（シグナル追加なしなので Q_OBJECT 不要）。
class ProbeThread : public QThread
{
public:
    ProbeThread(std::shared_ptr<Shared> s, QOpenGLContext *ctx, QOffscreenSurface *surface, QByteArray url)
        : m_s(std::move(s)), m_ctx(ctx), m_surface(surface), m_url(std::move(url)) {}
protected:
    void run() override { workerMain(m_s.get(), m_ctx, m_surface, m_url); }
private:
    std::shared_ptr<Shared> m_s;
    QOpenGLContext         *m_ctx;
    QOffscreenSurface      *m_surface;
    QByteArray              m_url;
};

void runProbe(QOpenGLWidget *canvas)
{
    QOpenGLContext *shareCtx = canvas->context();
    if (!shareCtx) {
        qWarning("[AsyncProbe] canvas has no GL context; abort");
        return;
    }

    QByteArray url = qgetenv("PB_ASYNC_PROBE_URL");
    if (url.isEmpty())
        url = QByteArrayLiteral("av://lavfi:testsrc2=size=1920x1080:rate=30");

    // QOffscreenSurface はGUIスレッドで生成する必要がある（Qtの制約）。
    auto *surface = new QOffscreenSurface();
    surface->setFormat(shareCtx->format());
    surface->create();

    auto *ctx = new QOpenGLContext();
    ctx->setFormat(shareCtx->format());
    ctx->setShareContext(shareCtx);
    const bool created = ctx->create();
    const bool sharing = created && QOpenGLContext::areSharing(ctx, shareCtx);
    qInfo("[AsyncProbe] start url=%s created=%d sharing=%d surfaceValid=%d",
          url.constData(), created, sharing, surface->isValid());
    if (!created || !surface->isValid()) {
        qWarning("[AsyncProbe] context/surface creation failed; abort");
        delete ctx;
        delete surface;
        return;
    }

    auto shared = std::make_shared<Shared>();
    auto stats  = std::make_shared<HeartbeatStats>();

    // ---- GUIスレッドのハートビート ----
    auto *hb = new QTimer(canvas);
    hb->setTimerType(Qt::PreciseTimer);
    hb->setInterval(5);
    auto lastTick = std::make_shared<QElapsedTimer>();
    lastTick->start();
    QObject::connect(hb, &QTimer::timeout, canvas, [canvas, shared, stats, lastTick]() {
        const qint64 gap = lastTick->restart();
        const int p = shared->phase.load();

        QElapsedTimer f;
        canvas->makeCurrent();
        f.start();
        if (QOpenGLContext *c = QOpenGLContext::currentContext())
            c->functions()->glFinish();
        const qint64 finishMs = f.elapsed();
        canvas->doneCurrent();
        canvas->update(); // 実際のpaintGLも回し続ける（[PerfDiag]で並行して見る）

        if (p >= 0 && p < PhaseCount) {
            stats->ticks[p]++;
            stats->maxGapMs[p]    = qMax(stats->maxGapMs[p], gap);
            stats->maxFinishMs[p] = qMax(stats->maxFinishMs[p], finishMs);
        }
        if (gap >= 20 || finishMs >= 10) {
            qInfo("[AsyncProbe][stall] phase=%s gap=%lldms glFinish=%lldms",
                  phaseName(p), (long long)gap, (long long)finishMs);
        }
    });

    auto *thread = new ProbeThread(shared, ctx, surface, url);
    ctx->moveToThread(thread);

    QObject::connect(thread, &QThread::finished, canvas,
                     [thread, hb, ctx, surface, shared, stats]() {
        hb->stop();
        hb->deleteLater();

        if (!shared->fatal.isEmpty())
            qWarning("[AsyncProbe] fatal: %s", qPrintable(shared->fatal));

        for (const CycleResult &r : shared->results) {
            qInfo("[AsyncProbe][worker] cycle=%d mpvInit=%lldms rctxCreate=%lldms firstRender=%lldms "
                  "maxSteadyRender=%lldms frames=%d free=%lldms terminateDestroy=%lldms %s",
                  r.cycle, (long long)r.mpvInitMs, (long long)r.rctxCreateMs,
                  (long long)r.firstRenderMs, (long long)r.maxSteadyRenderMs, r.frames,
                  (long long)r.freeMs, (long long)r.destroyMs, qPrintable(r.note));
        }
        for (int p = 0; p < PhaseCount; ++p) {
            if (stats->ticks[p] == 0)
                continue;
            qInfo("[AsyncProbe][gui] phase=%-14s ticks=%5d maxGap=%3lldms maxGlFinish=%3lldms",
                  phaseName(p), stats->ticks[p],
                  (long long)stats->maxGapMs[p], (long long)stats->maxFinishMs[p]);
        }
        qInfo("[AsyncProbe] done");

        delete ctx;     // ワーカー終了時にGUIスレッドへ戻してある
        delete surface;
        thread->deleteLater();
    });

    hb->start();
    thread->start();
}

} // namespace

namespace AsyncRenderProbe {

void startIfRequested(QOpenGLWidget *canvas)
{
    // 2026-09-15: 1回目の実機で [AsyncProbe] が1行も出なかったため、
    // 「コードは通っているが環境変数が届いていない」のか「そもそも
    // 呼ばれていない（再ビルドされていない）」のかを区別する診断ログ。
    const QByteArray envValue = qgetenv("PB_ASYNC_PROBE");
    qInfo("[AsyncProbe] startIfRequested reached: PB_ASYNC_PROBE='%s' canvas=%p",
          envValue.constData(), static_cast<void *>(canvas));
    // 2026-09-15: ステップB完了（結果は md/async_video_render_design_2026-09-15.md）。
    // 毎回の起動で走らないよう自動実行を止めた。再検証したいときは
    // kAutoRun を true にしてビルドする。本実装が入ったらファイルごと削除する。
    constexpr bool kAutoRun = false;
    if (!canvas || !(kAutoRun || envValue.trimmed() == "1"))
        return;
    static bool started = false;
    if (started)
        return;
    started = true;
    qInfo("[AsyncProbe] armed; starts in 8s");
    QTimer::singleShot(8000, canvas, [canvas]() { runProbe(canvas); });
}

} // namespace AsyncRenderProbe
