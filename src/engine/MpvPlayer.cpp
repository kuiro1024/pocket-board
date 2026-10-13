#include "MpvPlayer.h"
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions_3_3_Core>
#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include "VideoRenderThread.h"
#include <atomic>
#include <utility>
#include <QDateTime>
#include <QMetaObject>
#include <QDir>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QPointer>
#include <cstring>

#define MPV(x)  static_cast<mpv_handle*>(x)
#define RCTX(x) static_cast<mpv_render_context*>(x)

static constexpr bool kVerboseMpvRenderLog = false;

// 2026-09-12: mpvが延々と出す既知の警告を間引くためのフィルタ。
//
// "mpv_render_context_render() not being called or stuck." は、画面外や
// 非表示のバブルを描画していない間ずっと出続けるmpv側の警告。本体の
// 設計（見えていないものは描かない）どおりの状態で実害はないが、
// 毎秒大量に流れてログが埋まり、他の調査ができなくなる。
// 初回だけそのまま出し、以後は kNoisyLogReportIntervalMs ごとに
// 抑制した件数だけを報告する。
//
// 【ここを触るときの注意】mpvのログレベル自体（初期化時の
// mpv_request_log_messages(..., "v")）は下げていない。下げると
// DASH+seek詰まりの調査に必要な内部ログまで消えるため、
// 「レベルは維持したまま、この文言だけ間引く」方針にしてある。
static constexpr int kNoisyLogReportIntervalMs = 10000;

static bool isNoisyMpvLog(const char *text)
{
    if (!text)
        return false;
    return std::strstr(text, "not being called or stuck") != nullptr;
}

static void *mpvGetProcAddress(void *ctx, const char *name)
{
    Q_UNUSED(ctx)
    QOpenGLContext *glCtx = QOpenGLContext::currentContext();
    if (!glCtx) return nullptr;
    return reinterpret_cast<void*>(glCtx->getProcAddress(name));
}

static void mpvWakeupCallback(void *ctx)
{
    MpvPlayer *self = static_cast<MpvPlayer*>(ctx);
    emit self->_wakeup();
}

// ============================================================
//  2026-09-15 非同期化 P2: 非同期プレイヤーの共有状態とワーカー側処理
//  設計: md/async_video_render_design_2026-09-15.md（§1-2〜1-6）
//
//  スレッドごとに触ってよいもの:
//    GUI     : guiOwner / targetW,H / renderAllowed（書き）/ swapMutex下の readyIdx,frontIdx
//    ワーカー: rctx / gl / fbo[] / tex[]・w[]・h[] の確保 / renderIdx
//    mpv内部 : 更新コールバック → jobQueued/kickPending と GUIへの通知だけ
//  GUIが使うのは「front」の1枚だけ。ワーカーは「render」に描いて「ready」と
//  入れ替える。GUIは paintGL 冒頭で「ready」と「front」を入れ替える（トリプル
//  バッファ）。GPU完了待ち（glClientWaitSync）はワーカーだけが行う。
// ============================================================
struct MpvAsyncRenderSlot
{
    std::weak_ptr<MpvAsyncRenderSlot> self;
    VideoRenderThread *thread = nullptr;
    quintptr           key    = 0;

    // ---- ワーカー専用 ----
    mpv_render_context        *rctx = nullptr;
    QOpenGLFunctions_3_3_Core *gl   = nullptr;
    GLuint fbo[3] {0, 0, 0};
    quint64 renders = 0;

    // ---- swapMutex で保護（tex/w/h は確保をワーカーだけが行う） ----
    QMutex  swapMutex;
    GLuint  tex[3] {0, 0, 0};
    int     w[3]   {0, 0, 0};
    int     h[3]   {0, 0, 0};
    int     renderIdx = 0;   // ワーカーだけが読む（入れ替えは swapMutex 下）
    int     readyIdx  = 1;
    int     frontIdx  = 2;
    quint64 readySerial = 0;

    // ---- スレッド間 ----
    std::atomic<int>  targetW {0};
    std::atomic<int>  targetH {0};
    std::atomic<bool> renderAllowed {false};
    std::atomic<bool> jobQueued {false};
    std::atomic<bool> kickPending {false};
    std::atomic<bool> dead {false};
    std::atomic<bool> rctxGone {true};          // rctx が無い（未生成 or 解放済み）
    // 2026-09-23: mpv_render_context_create() が済んだ（＝ファイルを開いてよい）。
    std::atomic<bool> attached {false};
    std::atomic<bool> guiFramePending {false};
    std::atomic<bool> guiCallbackPending {false};
    // mpv が「新しいフレームがある」（MPV_RENDER_UPDATE_FRAME）と返した回数。
    // open() 時点の値より大きい通知だけを firstFrameReady の判定に使う。
    std::atomic<quint64> frameSeq {0};
    // フレームはあるが描画不可（サイズ未確定・描画禁止中）で保留している
    std::atomic<bool> pendingFrame {false};
    // 2026-09-15 P3: GUI が paintGL でこの動画を描いた（=次の1枚を受け取れる）。
    // ワーカーは1回描くごとに下ろし、GUI の paintGL が再び立てる。
    // 従来の「paintGL 1回につき renderToFbo 最大1回」と同じ粒度にするため
    // （画面外カリング中のバブルはワーカーでも描かない）。
    std::atomic<bool> presentArmed {true};

    // ---- GUI専用 ----
    MpvPlayer *guiOwner = nullptr;
};

namespace {

std::function<void()> g_afterAsyncTeardownHook;

void asyncRenderJob(const std::shared_ptr<MpvAsyncRenderSlot> &s);

void asyncSchedule(const std::shared_ptr<MpvAsyncRenderSlot> &s)
{
    if (!s || s->dead.load())
        return;
    if (s->jobQueued.exchange(true))
        return; // 既に積まれている（1本に合流）
    std::shared_ptr<MpvAsyncRenderSlot> keep = s;
    if (!s->thread->post([keep]() { asyncRenderJob(keep); }))
        s->jobQueued.store(false);
}

// mpv内部スレッドから呼ばれる。ここではプレイヤー本体に触らない
// （破棄と競合しうるため）。ワーカーへの描画依頼と、GUIへの通知だけ。
void asyncOnUpdateCallback(void *ctx)
{
    auto *raw = static_cast<MpvAsyncRenderSlot *>(ctx);
    std::shared_ptr<MpvAsyncRenderSlot> s = raw->self.lock();
    if (!s || s->dead.load())
        return;
    asyncSchedule(s);
}

// ワーカー専用。3枚のFBO/テクスチャを全部消す。
void asyncDeleteBuffers(MpvAsyncRenderSlot *s)
{
    if (!s->gl)
        return;
    GLuint texCopy[3];
    {
        QMutexLocker lk(&s->swapMutex);
        for (int i = 0; i < 3; ++i) {
            texCopy[i] = s->tex[i];
            s->tex[i] = 0;
            s->w[i] = 0;
            s->h[i] = 0;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (s->fbo[i] != 0)
            s->gl->glDeleteFramebuffers(1, &s->fbo[i]);
        if (texCopy[i] != 0)
            s->gl->glDeleteTextures(1, &texCopy[i]);
        s->fbo[i] = 0;
    }
}

// ワーカー専用。更新コールバックを外し、rctx とバッファを解放する。
void asyncReleaseRenderContext(MpvAsyncRenderSlot *s, qint64 *freeMsOut)
{
    qint64 freeMs = -1;
    if (s->rctx) {
        mpv_render_context_set_update_callback(s->rctx, nullptr, nullptr);
        QElapsedTimer t;
        t.start();
        mpv_render_context_free(s->rctx);
        freeMs = t.elapsed();
        s->rctx = nullptr;
    }
    asyncDeleteBuffers(s);
    delete s->gl;
    s->gl = nullptr;
    s->rctxGone.store(true);
    if (freeMsOut)
        *freeMsOut = freeMs;
}

void asyncRenderJob(const std::shared_ptr<MpvAsyncRenderSlot> &s)
{
    s->jobQueued.store(false);
    if (s->dead.load() || !s->rctx || !s->gl)
        return;

    const uint64_t flags = mpv_render_context_update(s->rctx);
    const bool newFrame = (flags & MPV_RENDER_UPDATE_FRAME) != 0;
    if (newFrame) {
        // firstFrameReady 判定用の通知（GUIスレッドで open() 以降のものだけ採用）。
        const quint64 seq = s->frameSeq.fetch_add(1) + 1;
        if (!s->guiCallbackPending.exchange(true)) {
            QMetaObject::invokeMethod(QCoreApplication::instance(), [s, seq]() {
                s->guiCallbackPending.store(false);
                if (s->guiOwner)
                    s->guiOwner->handleAsyncUpdateOnGui(seq);
            }, Qt::QueuedConnection);
        }
    }
    if (!newFrame && !s->pendingFrame.exchange(false))
        return;

    const int w = s->targetW.load();
    const int h = s->targetH.load();
    if (!s->renderAllowed.load() || w <= 0 || h <= 0) {
        // 描画は保留（従来の suppressRenderToFbo 相当）。フレームがあることは
        // 覚えておき、描画が許可されたらGUIが再依頼する。
        s->pendingFrame.store(true);
        s->kickPending.store(true);
        return;
    }
    if (!s->presentArmed.exchange(false)) {
        // 前の1枚を GUI がまだ描いていない（GUI が遅い / 画面外カリング）。
        // 従来どおり「描かれた分だけ描く」。再描画要求だけ出して保留する
        // （従来も mpv の更新ごとに frameUpdated を出していた）。
        s->pendingFrame.store(true);
        s->kickPending.store(true);
        if (!s->guiFramePending.exchange(true)) {
            QMetaObject::invokeMethod(QCoreApplication::instance(), [s]() {
                s->guiFramePending.store(false);
                if (s->guiOwner)
                    emit s->guiOwner->frameUpdated();
            }, Qt::QueuedConnection);
        }
        return;
    }

    QOpenGLFunctions_3_3_Core *gl = s->gl;
    const int i = s->renderIdx;
    if (s->fbo[i] == 0 || s->w[i] != w || s->h[i] != h) {
        if (s->fbo[i] != 0) {
            gl->glDeleteFramebuffers(1, &s->fbo[i]);
            s->fbo[i] = 0;
        }
        GLuint oldTex = 0;
        GLuint newTex = 0;
        gl->glGenTextures(1, &newTex);
        gl->glBindTexture(GL_TEXTURE_2D, newTex);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        gl->glGenFramebuffers(1, &s->fbo[i]);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[i]);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, newTex, 0);
        const GLenum status = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        {
            QMutexLocker lk(&s->swapMutex);
            oldTex = s->tex[i];
            s->tex[i] = newTex;
            s->w[i] = w;
            s->h[i] = h;
        }
        if (oldTex != 0)
            gl->glDeleteTextures(1, &oldTex); // render 用の1枚なので GUI は使っていない
        if (status != GL_FRAMEBUFFER_COMPLETE)
            qWarning("[VideoRender][slot %p] FBO incomplete 0x%x", static_cast<void *>(s.get()), status);
    }

    gl->glViewport(0, 0, w, h);
    mpv_opengl_fbo fboParam{};
    fboParam.fbo = static_cast<int>(s->fbo[i]);
    fboParam.w = w;
    fboParam.h = h;
    fboParam.internal_format = GL_RGBA8;
    int flipY = 0;
    int blockForTargetTime = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO,            &fboParam},
        {MPV_RENDER_PARAM_FLIP_Y,                &flipY},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTargetTime},
        {MPV_RENDER_PARAM_INVALID,               nullptr}
    };

    QElapsedTimer clock;
    clock.start();
    mpv_render_context_render(s->rctx, params);
    const qint64 renderMs = clock.elapsed();

    // 待つのはワーカーだけ。GUIは完成したテクスチャしか受け取らない。
    GLsync fence = gl->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (fence) {
        gl->glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 200000000ULL /* 200ms */);
        gl->glDeleteSync(fence);
    }
    const qint64 totalMs = clock.elapsed();
    mpv_render_context_report_swap(s->rctx);

    {
        QMutexLocker lk(&s->swapMutex);
        std::swap(s->renderIdx, s->readyIdx);
        ++s->readySerial;
    }

    const bool first = (s->renders == 0);
    ++s->renders;
    if (first || renderMs >= 5) {
        qInfo("[VideoRender][slot %p] render %lldms (withGpuWait=%lldms) fbo=%dx%d first=%d",
              static_cast<void *>(s.get()), static_cast<long long>(renderMs),
              static_cast<long long>(totalMs), w, h, first ? 1 : 0);
    }

    if (!s->guiFramePending.exchange(true)) {
        QMetaObject::invokeMethod(QCoreApplication::instance(), [s]() {
            s->guiFramePending.store(false);
            if (s->guiOwner)
                emit s->guiOwner->frameUpdated();
        }, Qt::QueuedConnection);
    }
}

void asyncDestroyOnWorker(const std::shared_ptr<MpvAsyncRenderSlot> &s, mpv_handle *mpv)
{
    QElapsedTimer total;
    total.start();
    if (s->thread)
        s->thread->removeFinalizer(s->key);
    qint64 freeMs = -1;
    asyncReleaseRenderContext(s.get(), &freeMs);
    qint64 terminateMs = -1;
    if (mpv) {
        QElapsedTimer t;
        t.start();
        mpv_terminate_destroy(mpv);
        terminateMs = t.elapsed();
    }
    qInfo("[VideoRender][slot %p] destroyed on worker render_context_free=%lldms "
          "mpv_terminate_destroy=%lldms total=%lldms renders=%llu",
          static_cast<void *>(s.get()), static_cast<long long>(freeMs),
          static_cast<long long>(terminateMs), static_cast<long long>(total.elapsed()),
          static_cast<unsigned long long>(s->renders));
    QMetaObject::invokeMethod(QCoreApplication::instance(), []() {
        MpvPlayer::runAfterAsyncTeardownHookOnGui();
    }, Qt::QueuedConnection);
}

} // namespace

void MpvPlayer::setAfterAsyncTeardownHook(std::function<void()> hook)
{
    g_afterAsyncTeardownHook = std::move(hook);
}

void MpvPlayer::handleAsyncRenderAttachedOnGui()
{
    flushPendingOpen();
}

void MpvPlayer::flushPendingOpen()
{
    if (!m_pendingOpenValid)
        return;
    m_pendingOpenValid = false;
    const QString url        = m_pendingOpenUrl;
    const QString audioUrl   = m_pendingOpenAudioUrl;
    const qint64  startMs    = m_pendingOpenStartMs;
    const bool    disableVid = m_pendingOpenDisableVideo;
    m_pendingOpenUrl.clear();
    m_pendingOpenAudioUrl.clear();
    qDebug("[MpvPlayer %p] open: render context ready, opening now",
           static_cast<void *>(this));
    open(url, startMs, audioUrl, disableVid);
}

void MpvPlayer::runAfterAsyncTeardownHookOnGui()
{
    if (g_afterAsyncTeardownHook)
        g_afterAsyncTeardownHook();
}

bool MpvPlayer::initializeGlAsync(VideoRenderThread *thread)
{
    if (!m_mpv || m_glInitialized || !thread)
        return m_glInitialized;

    auto s = std::make_shared<MpvAsyncRenderSlot>();
    s->self = s;
    s->thread = thread;
    s->key = static_cast<quintptr>(reinterpret_cast<quintptr>(s.get()));
    s->guiOwner = this;

    mpv_handle *mpv = MPV(m_mpv);
    QElapsedTimer queued;
    queued.start();
    const qint64 queuedAtMs = queued.msecsSinceReference();

    const bool posted = thread->post([s, mpv, queuedAtMs]() {
        QElapsedTimer now;
        now.start();
        const qint64 waitedMs = now.msecsSinceReference() - queuedAtMs;
        if (s->dead.load())
            return;

        s->gl = new QOpenGLFunctions_3_3_Core();
        if (!s->gl->initializeOpenGLFunctions()) {
            delete s->gl;
            s->gl = nullptr;
            qCritical("[VideoRender][slot %p] initializeOpenGLFunctions failed", static_cast<void *>(s.get()));
            QMetaObject::invokeMethod(QCoreApplication::instance(), [s]() {
                if (s->guiOwner)
                    emit s->guiOwner->asyncGlInitFailed();
            }, Qt::QueuedConnection);
            return;
        }

        mpv_opengl_init_params glInitParams{};
        glInitParams.get_proc_address     = mpvGetProcAddress;
        glInitParams.get_proc_address_ctx = nullptr;
        int advancedControl = 1;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInitParams},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL,   &advancedControl},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };
        mpv_render_context *rctx = nullptr;
        QElapsedTimer t;
        t.start();
        const int rc = mpv_render_context_create(&rctx, mpv, params);
        const qint64 createMs = t.elapsed();
        if (rc < 0) {
            delete s->gl;
            s->gl = nullptr;
            qCritical("[VideoRender][slot %p] mpv_render_context_create failed rc=%d",
                      static_cast<void *>(s.get()), rc);
            QMetaObject::invokeMethod(QCoreApplication::instance(), [s]() {
                if (s->guiOwner)
                    emit s->guiOwner->asyncGlInitFailed();
            }, Qt::QueuedConnection);
            return;
        }
        s->rctx = rctx;
        s->rctxGone.store(false);

        // シャットダウン時（ワーカーのコンテキストが消える前）の保険。
        std::weak_ptr<MpvAsyncRenderSlot> weak = s;
        s->thread->addFinalizer(s->key, [weak]() {
            if (std::shared_ptr<MpvAsyncRenderSlot> p = weak.lock()) {
                p->dead.store(true);
                qint64 freeMs = -1;
                asyncReleaseRenderContext(p.get(), &freeMs);
                qInfo("[VideoRender][slot %p] finalized at shutdown render_context_free=%lldms",
                      static_cast<void *>(p.get()), static_cast<long long>(freeMs));
            }
        });

        s->attached.store(true);
        // 預かっている open() があればGUIスレッドで実行させる（映像が落ちるのを防ぐ）。
        QMetaObject::invokeMethod(QCoreApplication::instance(), [s]() {
            if (s->guiOwner)
                s->guiOwner->handleAsyncRenderAttachedOnGui();
        }, Qt::QueuedConnection);
        mpv_render_context_set_update_callback(rctx, asyncOnUpdateCallback, s.get());
        // コールバック設定前に既にフレームが用意されていた場合の取りこぼし防止。
        // 描画不可（まだサイズ未確定など）なら kickPending が立ち、GUIが後で再依頼する。
        asyncSchedule(s);
        qInfo("[VideoRender][slot %p] attached render_context_create=%lldms queuedMs=%lld",
              static_cast<void *>(s.get()), static_cast<long long>(createMs),
              static_cast<long long>(waitedMs));
    });

    if (!posted) {
        qWarning("[MpvPlayer %p] initializeGlAsync: worker is stopping", static_cast<void *>(this));
        return false;
    }
    m_async = s;
    m_glInitialized = true;
    qInfo("[MpvPlayer %p] GL 初期化をワーカーへ依頼 slot=%p", static_cast<void *>(this),
          static_cast<void *>(s.get()));
    return true;
}

void MpvPlayer::updateAsyncRenderState(int w, int h, bool renderAllowed)
{
    if (!m_async || VideoRenderThread::shared() != m_async->thread)
        return; // ワーカーが止まっている（終了処理中など）
    const int oldW = m_async->targetW.exchange(w);
    const int oldH = m_async->targetH.exchange(h);
    m_async->presentArmed.store(true);
    // 2026-09-15 P3: 描画サイズが変わった（ズームで解像度段が変わった）ときは、
    // 一時停止中でも新しいサイズで今のフレームを描き直す（ワーカーで行うので
    // GUIのコストはない）。まだ1枚も受け取っていなければ不要。
    if ((oldW != w || oldH != h) && m_asyncFrontSerial > 0 && w > 0 && h > 0) {
        m_async->pendingFrame.store(true);
        m_async->kickPending.store(true);
        qInfo("[VideoRender][slot %p] target %dx%d -> %dx%d (redraw)",
              static_cast<void *>(m_async.get()), oldW, oldH, w, h);
    }
    m_asyncGateAllowed = renderAllowed;
    applyAsyncRenderGate();
}

void MpvPlayer::setAsyncRenderAllowed(bool renderAllowed)
{
    if (!m_async || VideoRenderThread::shared() != m_async->thread)
        return;
    m_asyncGateAllowed = renderAllowed;
    applyAsyncRenderGate();
}

// 2026-09-15 P3: 「描いてよいか」をワーカーへ反映する唯一の場所。
// 状態遷移（stateChanged）・vid=no/auto の切り替え・paintGL のどれからでも
// ここを通すので、paintGL が来ない（画面外カリング）場合も即座に止まる。
void MpvPlayer::applyAsyncRenderGate()
{
    if (!m_async || VideoRenderThread::shared() != m_async->thread)
        return;
    const bool allowed = m_asyncGateAllowed && !m_videoSuppressed;
    const bool was = m_async->renderAllowed.exchange(allowed);
    if (was != allowed) {
        qInfo("[VideoRender][slot %p] renderAllowed %d -> %d (gate=%d videoSuppressed=%d)",
              static_cast<void *>(m_async.get()), was ? 1 : 0, allowed ? 1 : 0,
              m_asyncGateAllowed ? 1 : 0, m_videoSuppressed ? 1 : 0);
    }
    if (allowed && m_async->targetW.load() > 0 && m_async->targetH.load() > 0
        && m_async->kickPending.exchange(false))
        asyncSchedule(m_async);
}

bool MpvPlayer::latchAsyncFrame()
{
    if (!m_async)
        return false;
    QMutexLocker lk(&m_async->swapMutex);
    const bool latched = (m_async->readySerial != m_asyncFrontSerial);
    if (latched) {
        std::swap(m_async->readyIdx, m_async->frontIdx);
        m_asyncFrontSerial = m_async->readySerial;
    }
    const int i = m_async->frontIdx;
    m_fboColorTex = m_async->tex[i];
    m_fboW = m_async->w[i];
    m_fboH = m_async->h[i];
    // 非同期プレイヤーでは m_fbo はGL名として使わない（hasFbo() 判定用の目印）。
    m_fbo = m_fboColorTex;
    return latched;
}

void MpvPlayer::handleAsyncUpdateOnGui(quint64 frameSeq)
{
    // 従来 initializeGl() の更新コールバック内で行っていた firstFrameReady の
    // 判定を、GUIスレッドで行う（プレイヤー本体への参照をmpv内部スレッドに
    // 持たせないため。従来あった m_internalState の非同期読みも同時に消える）。
    //
    // 2026-09-15 P2 修正: 最初は「更新コールバックが来たら」で判定していたが、
    // GUIへの通知はキュー経由のため、open() より前のコールバック（レンダー
    // コンテキスト生成直後など）の通知が open() の後に届き、FILE_LOADED より
    // 前に Loading→Ready へ進んでしまった（実機ログで確認）。
    // ワーカーが mpv から「新しいフレームがある」と返されたときだけ通知し、
    // さらにその通知が open() より後のフレームのものかを通し番号で確かめる。
    if (!m_async || frameSeq <= m_asyncOpenSeq)
        return;
    if (!m_firstFrameEmitted
     && (m_internalState == PbInternalState::Opening
      || m_internalState == PbInternalState::Decoding)) {
        m_firstFrameEmitted = true;
        emit firstFrameReady();
    }
}

MpvPlayer::MpvPlayer(QObject *parent)
    : IPbPlayer(parent)
{
    connect(this, &MpvPlayer::_wakeup,
            this, &MpvPlayer::processMpvEvents,
            Qt::QueuedConnection);

    // 2026-09-15: [MpvLifecycle] 計測。Sleep降格41ms/再生開始21msの内訳を
    // 「GLが要る区間」と「要らない区間」に分けるためのもの。生成・破棄は
    // プレイヤー1個につき1回だけなので、閾値なしで毎回出す。
    QElapsedTimer __lifeTotal;
    __lifeTotal.start();
    QElapsedTimer __lifeClock;
    __lifeClock.start();
    m_mpv = static_cast<void*>(mpv_create());
    const qint64 __createMs = __lifeClock.elapsed();
    if (!m_mpv) { qCritical("[MpvPlayer] mpv_create() 失敗"); return; }

    mpv_set_option_string(MPV(m_mpv), "terminal",         "no");
    // 通常運用のログレベル。2026-07-01〜02にメモリ増加問題の調査のため
    // 一時的に "all=v"（verbose）にしていたが、調査完了・原因解決に伴い
    // "all=error" に戻した。調査の全経緯は
    // md/memory_leak_upstream_report_2026-07-02.md 参照。
    mpv_set_option_string(MPV(m_mpv), "msg-level",        "all=error");
    // ハードウェアデコード（NVIDIA環境では d3d11va-copy 経由になる）。
    // 2026-07-02: 長期間再生時にメモリ（PrivateUsage）が際限なく増加する問題を
    // 調査した結果、原因はこのhwdec設定ではなく、mpv本体側のバグ
    // （vo_libmpv使用時にGLsyncフェンスが回収されずリークする、upstream commit
    // f74adc4で修正）と判明した。修正を反映した自前ビルド版libmpv-2.dllへの
    // 差し替えで解決済みのため、調査中に一時的に "no"（ソフトウェアデコード）
    // へ変更していたのを "auto-safe"（ハードウェアデコード、軽量）に戻した。
    // 詳細・検証データは md/memory_leak_upstream_report_2026-07-02.md 参照。
    mpv_set_option_string(MPV(m_mpv), "hwdec",            "auto-safe");
    // hwdec-extra-frames は d3d11va 系ハードウェアデコード時のサーフェス数制御。
    mpv_set_option_string(MPV(m_mpv), "hwdec-extra-frames", "3");
    // 検証済み: gpu-api=vulkan を試したが、Render APIを
    // MPV_RENDER_API_TYPE_OPENGL で初期化している構成では
    // "This is not a libplacebo vulkan gpu api context" というエラーで
    // Vulkan版hwdecの初期化自体が失敗し、結局d3d11va-copyにフォールバックする
    // ことをログで確認した（mpv issue #15099 の回避策はネイティブ
    // vo=gpu-next+Vulkan構成の場合の話で、Qt埋め込みのOpenGL Render APIでは
    // 適用できない）。よって "opengl" のまま維持する。
    mpv_set_option_string(MPV(m_mpv), "gpu-api",          "opengl");
    // PocketBoard's QOpenGLWidget composition target is 8-bit RGBA. mpv's
    // automatic choice was rgba16f even for 8-bit SDR H.264, doubling the
    // bandwidth of its intermediate render surfaces without preserving any
    // additional precision in the final window. Match the compositor target.
    mpv_set_option_string(MPV(m_mpv), "fbo-format",       "rgba8");
    mpv_set_option_string(MPV(m_mpv), "ao",               "wasapi");
    mpv_set_option_string(MPV(m_mpv), "vo",               "libmpv");
    mpv_set_option_string(MPV(m_mpv), "ytdl",             "no");
    mpv_set_option_string(MPV(m_mpv), "pause",            "yes");
    mpv_set_option_string(MPV(m_mpv), "loop-file",        "no");
    mpv_set_option_string(MPV(m_mpv), "network-timeout",  "10");
    // vd-lavc-dr（Direct Rendering）はデフォルトの "yes" に戻した。
    // 2026-07-01に "no" にしていたのは、hwdec="no"（ソフトウェアデコード）時に
    // 限って、Direct Renderingとadvanced-control構成のRender APIが
    // デッドロックする別バグへの対策だった（デコーダがVO側バッファプールから
    // 直接バッファを確保しようとし、そのVO側はQt側のrenderToFbo呼び出しを
    // 待っている、という循環待ち）。hwdec="auto-safe"に戻した現在、この
    // 組み合わせ（auto-safe + vd-lavc-dr=yes）はこの調査以前から問題なく
    // 動いていたため "yes" に戻して問題ない。
    mpv_set_option_string(MPV(m_mpv), "vd-lavc-dr",       "yes");
    mpv_set_option_string(MPV(m_mpv), "opengl-pbo",       "no");
    mpv_set_option_string(MPV(m_mpv), "cache",            "yes");
    mpv_set_option_string(MPV(m_mpv), "cache-secs",       "12");
    mpv_set_option_string(MPV(m_mpv), "demuxer-readahead-secs", "8");
    mpv_set_option_string(MPV(m_mpv), "demuxer-max-bytes",      "16MiB");
    mpv_set_option_string(MPV(m_mpv), "demuxer-max-back-bytes", "4MiB");
    // 2026-07-03追加: HTTPストリーム（動画サイト等）が瞬断した際の自動再接続。
    // ffmpegのhttpプロトコル層のAVOptionで、mpvのstream-lavf-o経由で設定する。
    // これが無いと、接続が切れた時点までしか読めていないデータでデコードを
    // 続けることになり、参照フレーム欠落（h264: Missing reference picture）の
    // 一因になりうる（詳細は 動画サイト用プラグイン側のコーデック優先度
    // 設定、および maybeHandleHwdecDecodeError() 参照）。
    // reconnect_on_http_error（4xx,5xx等のカンマ区切り指定）はstream-lavf-o
    // 自体もカンマ区切りのkey=valueリストのため、値中のカンマとの混同を
    // 避けるためあえて含めていない。
    mpv_set_option_string(MPV(m_mpv), "stream-lavf-o",
        "reconnect_streamed=1,reconnect_on_eof=1,reconnect_on_network_error=1,reconnect_delay_max=5");

    // 2026-09-14: GLシェーダーのディスクキャッシュ。
    //
    // 【なぜ要るか】実測で、再生開始の最初の1フレームだけ paintGL が
    // 20〜22ms（Sleep復帰後の再生でも同じ）かかっていた。犯人はこちらの
    // 描画ループではなく、mpvの初回 mpv_render_context_render() で、
    // その直後にmpv自身が "Texture for plane 0" / "Testing FBO format"
    // を出しているのがログ上の根拠。つまり毎回その場でシェーダーを
    // コンパイルし直している。
    //
    // PocketBoardは「Sleepで MpvPlayer を丸ごと解放し、復帰時に作り直す」
    // 設計なので、このコンパイルがアプリ起動中に何度も発生する。
    // mpvは GL_ARB_get_program_binary があればコンパイル済みバイナリを
    // ディスクへ保存して次回以降使い回せる（起動ログでこの拡張が
    // Loaded になっていることは確認済み）が、保存先が設定されていないと
    // その機構自体が働かない。
    //
    // 【オプション名がmpvのバージョンで違う】新しめのmpvは
    // gpu-shader-cache(bool) + cache-dir、古いものは gpu-shader-cache-dir。
    // 知らないオプションは mpv_set_option_string がエラーを返すだけで
    // 他に影響しないため、両方投げて通った方を使う。
    {
        const QString shaderCacheDir =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
            + QStringLiteral("/mpv-shader-cache");
        QDir().mkpath(shaderCacheDir);
        const QByteArray dirUtf8 = QDir::toNativeSeparators(shaderCacheDir).toUtf8();
        const int rcNew = mpv_set_option_string(MPV(m_mpv), "gpu-shader-cache", "yes");
        const int rcDir = mpv_set_option_string(MPV(m_mpv), "cache-dir", dirUtf8.constData());
        const int rcOld = mpv_set_option_string(MPV(m_mpv), "gpu-shader-cache-dir", dirUtf8.constData());
        qDebug("[MpvPlayer] shader cache: dir=%s new=%d cacheDir=%d old=%d",
               dirUtf8.constData(), rcNew, rcDir, rcOld);
    }

    mpv_set_wakeup_callback(MPV(m_mpv), mpvWakeupCallback, this);

    __lifeClock.restart();
    const int __initRc = mpv_initialize(MPV(m_mpv));
    const qint64 __initializeMs = __lifeClock.elapsed();
    qInfo("[MpvLifecycle][ctor] player=%p mpv_create=%lldms mpv_initialize=%lldms thread=%p",
          static_cast<void*>(this), (long long)__createMs, (long long)__initializeMs,
          static_cast<void*>(QThread::currentThread()));
    if (__initRc < 0) {
        qCritical("[MpvPlayer] mpv_initialize() 失敗");
        mpv_destroy(MPV(m_mpv));
        m_mpv = nullptr;
        return;
    }

    mpv_observe_property(MPV(m_mpv), 0, "playback-time",   MPV_FORMAT_DOUBLE);
    mpv_observe_property(MPV(m_mpv), 0, "duration",        MPV_FORMAT_DOUBLE);
    mpv_observe_property(MPV(m_mpv), 0, "pause",           MPV_FORMAT_FLAG);
    mpv_observe_property(MPV(m_mpv), 0, "paused-for-cache",MPV_FORMAT_FLAG);
    mpv_observe_property(MPV(m_mpv), 0, "core-idle",       MPV_FORMAT_FLAG);
    mpv_observe_property(MPV(m_mpv), 0, "width",           MPV_FORMAT_INT64);
    mpv_observe_property(MPV(m_mpv), 0, "height",          MPV_FORMAT_INT64);
    // 2026-09-16: hwdec 自動フォールバックの判定を同期読み取りから置き換える。
    mpv_observe_property(MPV(m_mpv), 0, "hwdec-current",   MPV_FORMAT_STRING);

    // 2026-07-03: DASH+seekフリーズの根本原因調査用に一時的に詳細化。
    // "error"だとcache/demuxer内部で何が起きているかがログに出ず、
    // 外側の症状（demuxer-cache-durationが0のまま固まる）しか見えない
    // ため、mpv自身の内部ログでフリーズの瞬間に何をしているか確認する。
    //
    // 2026-10-12: 詳細ログは Debug ビルドだけにする。Release では warn 以上。
    // hwdec のフォールバック判定（maybeHandleHwdecDecodeError）が見る
    // "Missing reference picture" は ffmpeg のエラーレベルなので warn でも届く。
#ifdef NDEBUG
    mpv_request_log_messages(MPV(m_mpv), "warn");
#else
    mpv_request_log_messages(MPV(m_mpv), "v");
#endif

    qDebug("[MpvPlayer %p] 初期化完了", static_cast<void*>(this));
    qInfo("[MpvLifecycle][ctor] player=%p total=%lldms",
          static_cast<void*>(this), (long long)__lifeTotal.elapsed());
}

MpvPlayer::~MpvPlayer()
{
    // 2026-07-15 🚫調査用診断ログ: mpv_terminate_destroy()はWASAPI関連の
    // COM破棄処理を内包しており、これがメインスレッドのOLEアパートメント
    // （ドラッグ&ドロップ登録の基盤）を巻き添えにする不具合の主因と推定
    // されている。🚫発生時はネイティブ層でQtにイベントすら届かず後から
    // 直接の証拠を残せないため、せめて「いつ・どのスレッドで・どの
    // MpvPlayerが破棄されたか」を破棄の前後で必ずログ（qInstallMessageHandler
    // 経由でファイルへ同期書き込みされる）に残し、後からログの末尾と
    // 🚫報告時刻を突き合わせられるようにする。
    qInfo("[MpvPlayer %p] destroy begin thread=%p async=%p mpv=%p",
          static_cast<void*>(this),
          static_cast<void*>(QThread::currentThread()),
          static_cast<void*>(m_async.get()), m_mpv);

    // 2026-09-15 非同期化 P2: 非同期プレイヤーは free → terminate_destroy を
    // ワーカーで行う（GUIスレッドを止めない。GL current も不要）。
    if (m_async) {
        std::shared_ptr<MpvAsyncRenderSlot> s = std::move(m_async);
        s->guiOwner = nullptr;
        s->dead.store(true);
        mpv_handle *mpv = MPV(m_mpv);
        m_mpv = nullptr;
        if (mpv) // 以後このプレイヤーへ wakeup が飛ばないように（設定はmpv側で同期される）
            mpv_set_wakeup_callback(mpv, nullptr, nullptr);

        VideoRenderThread *t = VideoRenderThread::shared();
        const bool posted = (t && t == s->thread)
            && t->post([s, mpv]() { asyncDestroyOnWorker(s, mpv); });
        if (posted) {
            qInfo("[MpvPlayer %p] destroy queued to worker slot=%p",
                  static_cast<void*>(this), static_cast<void*>(s.get()));
        } else if (s->rctxGone.load()) {
            // ワーカーは既に停止し、rctx はシャットダウン時の finalizer で解放済み。
            if (mpv) {
                QElapsedTimer tt;
                tt.start();
                mpv_terminate_destroy(mpv);
                qInfo("[MpvPlayer %p] worker gone; mpv_terminate_destroy on GUI=%lldms",
                      static_cast<void*>(this), static_cast<long long>(tt.elapsed()));
            }
        } else {
            // 起こらないはず（rctx 解放前に terminate すると mpv 側で詰まるため触らない）。
            qCritical("[MpvPlayer %p] worker gone but render context still alive; leaking mpv handle",
                      static_cast<void*>(this));
        }
        qInfo("[MpvPlayer %p] destroy end", static_cast<void*>(this));
        return;
    }
    // 2026-09-16 P7: 同期経路は撤去。非同期化されずに終わったプレイヤー
    // （GL初期化前/失敗）はレンダーコンテキストを持たないので terminate のみ。
    if (m_mpv) {
        QElapsedTimer tt;
        tt.start();
        mpv_terminate_destroy(MPV(m_mpv));
        m_mpv = nullptr;
        qInfo("[MpvLifecycle][dtor] player=%p (no render ctx) mpv_terminate_destroy=%lldms",
              static_cast<void*>(this), static_cast<long long>(tt.elapsed()));
    }
    qInfo("[MpvPlayer %p] destroy end", static_cast<void*>(this));
}

namespace {
// mpvのファイルローカルオプション文字列（"key1=value1,key2=value2"形式）に
// 値を埋め込む際、値中に区切り文字（カンマ）やエスケープ文字自体が
// 含まれていても壊れないようにエスケープする。
QByteArray escapeMpvSubOptionValue(const QByteArray &value)
{
    QByteArray out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == ',')
            out.append('\\');
        out.append(c);
    }
    return out;
}

QByteArray escapeMpvStringListValue(const QByteArray &value)
{
    return escapeMpvSubOptionValue(value);
}

// 2026-07-11追記: DASH（映像/音声別URL）結合方式の変更に伴い追加。
//
// 【背景】従来は audio-file= をloadfileの「ファイルローカルオプション」
// 文字列（カンマ区切り）に埋め込んでいたが、値中のカンマを独自の "\,"
// エスケープで処理しようとしても、このオプション文字列パーサー側の
// 解釈と噛み合わず、値にカンマを含むURL（例: BilibiliのCDN URLにある
// "uparams=e,uipk,nbs,..." 形式のクエリ）で
// 「loadfile: argument options can't be parsed」という致命的エラーに
// なり、動画が一切ロードされない不具合が実機で確認された。
//
// mpv本体に同梱されている公式の外部解決ツール連携スクリプト（player/lua/
// ytdl_hook.lua）を調べたところ、DASHの映像/音声結合には audio-file=
// を一切使っておらず、代わりに EDL（Edit Decision List）仮想ファイル
// （edl://...;!new_stream;...）を組み立てて1本の仮想ファイルとして
// 開いていた。mpv公式ドキュメント（DOCS/edl-mpv.rst, new_stream節）に
// よれば、この方式は audio-file と機能的には同じだが「全ストリームで
// 統一されたキャッシュを使う」という利点がある、と明記されている
// （＝audio-fileが抱えていた「映像と音声で別々にキャッシュされて
// ズレる」系の不具合、mpv issue #8920/#7162/#1824 の温床そのものが
// 構造的に起きにくくなる）。
//
// EDLの値エスケープは「%<バイト数>%<生データ>」という長さプレフィックス
// 方式で、値の中身に何が入っていても（カンマだろうが何だろうが）区切り
// 文字と衝突しようがない。これがescapeMpvSubOptionValue()のカンマ
// エスケープ方式より根本的に頑丈な理由。
QByteArray edlEscape(const QByteArray &value)
{
    return "%" + QByteArray::number(value.size()) + "%" + value;
}

// 映像URL(videoUrl)と音声URL(audioUrl)を、mpvのEDL仮想ファイルとして
// 結合した edl:// URL 文字列を組み立てる（new_streamで別ストリーム扱いに
// することで、mpvから見て「1本の動画ファイルに映像・音声トラックが
// 両方入っている」のと同じ見え方にする）。
QByteArray buildDashEdlUrl(const QByteArray &videoUrl, const QByteArray &audioUrl)
{
    QByteArray edl = "edl://";
    edl += edlEscape(videoUrl);
    edl += ";!new_stream;";
    edl += edlEscape(audioUrl);
    return edl;
}
}

void MpvPlayer::open(const QString &streamUrl, qint64 startPositionMs, const QString &audioUrl,
                     bool disableVideoTrack)
{
    if (!m_mpv) return;
    if (streamUrl.isEmpty()) {
        qWarning("[MpvPlayer] open() に空 URL が渡されました！スキップ");
        return;
    }
    qDebug("[MpvPlayer] open URL: %s startPositionMs=%lld audioUrl=%s disableVideoTrack=%d",
           qPrintable(streamUrl.left(80)), static_cast<long long>(startPositionMs),
           audioUrl.isEmpty() ? "(none)" : qPrintable(audioUrl.left(80)),
           disableVideoTrack);

    // 2026-09-23: レンダーコンテキストがまだ出来ていない間は開かない。
    // mpv はファイルを開いた瞬間に映像出力を初期化するので、ここで先に
    // loadfile を送ると "No render context set" → "Video: no video" となり、
    // そのファイルは音声だけで再生されてしまう（ローカル動画がときどき
    // 音だけになる不具合の正体）。コンテキスト生成の完了通知
    // （handleAsyncRenderAttachedOnGui）で開き直す。映像を使わない
    // open（disableVideoTrack）は待つ必要がない。
    if (m_async && !disableVideoTrack && !m_async->attached.load()) {
        m_pendingOpenValid        = true;
        m_pendingOpenUrl          = streamUrl;
        m_pendingOpenAudioUrl     = audioUrl;
        m_pendingOpenStartMs      = startPositionMs;
        m_pendingOpenDisableVideo = disableVideoTrack;
        qDebug("[MpvPlayer %p] open: waiting for render context before loadfile",
               static_cast<void *>(this));
        // 通知が来ない場合の保険（ワーカーが止まっている等）。待ち続けて
        // 再生が始まらないより、映像無しでも開いたほうがまし。
        QPointer<MpvPlayer> guard(this);
        QTimer::singleShot(1500, this, [guard]() {
            if (!guard || !guard->m_pendingOpenValid)
                return;
            qWarning("[MpvPlayer %p] open: render context still not ready after 1500ms, "
                     "opening anyway", static_cast<void *>(guard.data()));
            guard->flushPendingOpen();
        });
        return;
    }
    m_pendingOpenValid = false;

    // 2026-07-08: ローカル音声ファイル対策。"cache"プロパティ（下の
    // isDash分岐）と同じ理由・同じパターンで、loadfileコマンドを投げる
    // 直前に mpv_set_property_string() で明示的に設定する。MpvPlayer
    // インスタンスは画質切り替えの裏側プレイヤー等でopen()が複数回
    // 呼ばれることがあるため、disableVideoTrack=falseの通常動画open()でも
    // 前回の音声専用open()の"vid=no"が持ち越されないよう毎回明示的に
    // リセットする。
    // 2026-09-16: 非同期化（送信順は loadfile より前のまま。失敗は返信でログ）。
    setPropertyStringAsync("vid", disableVideoTrack ? QByteArrayLiteral("no") : QByteArrayLiteral("auto"));
    m_videoTrackDisabled = disableVideoTrack;
    m_firstFrameEmitted = false;
    m_videoSuppressed   = false;
    m_internalState     = PbInternalState::Opening;
    applyAsyncRenderGate();
    // 2026-09-15 P2: この時点より前に発生した更新コールバック（レンダー
    // コンテキスト生成直後など）で firstFrameReady を出さないための基準点。
    if (m_async)
        m_asyncOpenSeq = m_async->frameSeq.load();
    // 新しいファイルを開くたびに hwdec 自動フォールバック検出をリセットする。
    m_hwdecFallbackTriggered = false;

    // "start="（再開位置）は loadfile のファイルローカルオプション
    // （カンマ区切りで併記可能）として渡す。open() 完了後に別途 seek() を
    // 送る方式だと、mpv がまだファイルを開いていないタイミングで
    // コマンドが届いて無視されてしまうことがある（"start=" 単体の頃に
    // 実際に踏んだ不具合と同じ理由）。
    //
    // 【2026-07-03 追記1】ただし DASH（audio-file= あり）に限り、start= も
    // 同時に指定すると mpv/ffmpeg 側のデマルチプレクサがフリーズし、
    // 一度もフレームを描画できなくなる不具合が実機ログで確認された
    // （ffmpeg側ログ "root atom ... partial file"、以降 renderToFbo() が
    // MPV_RENDER_UPDATE_FRAME を永久に受け取らない）。画質切り替えの
    // 裏側プレイヤー・再生位置つきセッション復元のDASH動画の両方で再現し、
    // 非DASH（audio-fileなし）の start= 単体では問題が起きないことも確認済み。
    // そのため DASH の場合だけ start= を loadfile から外し、FILE_LOADED後に
    // 改めて seek() する2段階方式に分けた（下の handleMpvEvent 参照）。
    //
    // 【2026-07-03 追記2】追記1の対策後も、FILE_LOADED後のseek()自体で
    // 今度は「1フレームだけ描画してそこから完全に停止し二度と進まない」
    // という不具合が再現した（demuxer-cache-durationが0.000000のまま
    // 一切増えない）。これはmpv本体のバグではなく、一部の動画配信
    // CDN側がシーク時のRangeリクエストの扱いに癖があり、
    // demuxerキャッシュを有効にした状態でシークするとキャッシュ
    // スレッドがデッドロック同然に詰まる既知の相性問題
    // （mpv-player/mpv issue #8920, #7162 で報告・確認済み。
    // mpv開発者もmpv側の問題ではないとコメント）。対策は下記追記3・追記4を参照。
    const bool isDash = !audioUrl.isEmpty();
    m_isDash = isDash;
    m_pendingSeekAfterLoadMs = -1;
    // 新しいファイルを開くので、前のファイルに対して監視中だった
    // DASHシーク詰まりウォッチドッグがあれば無効化する
    // （tokenを変えるだけで、既に予約済みのQTimer::singleShotは
    // 発火しても何もせず抜けるようになる）。
    m_dashSeekTargetMs = -1;
    ++m_dashSeekWatchdogToken;

    // 2026-07-11: DASH（音声URLあり）の場合、映像URLと音声URLを
    // audio-file=（loadfileのファイルローカルオプション文字列）で結合する
    // 方式は撤回した。値中にカンマを含むURL（例: Bilibili CDNの
    // "uparams=e,uipk,nbs,..."形式のクエリ）でオプション文字列パーサーが
    // 壊れ、loadfileコマンド自体が失敗して動画が一切読み込まれない不具合が
    // 実機で確認されたため（buildDashEdlUrl()直前のコメント参照）。
    // 代わりにmpv公式のytdl_hook.luaと同じ、EDL(new_stream)で1本の仮想
    // ファイルに結合したURLをloadfileへ渡す方式にする。
    const QByteArray urlBytes = isDash
        ? buildDashEdlUrl(streamUrl.toUtf8(), audioUrl.toUtf8())
        : streamUrl.toUtf8();

    QList<QByteArray> optParts;
    if (startPositionMs > 0 && !isDash) {
        const double startSec = startPositionMs / 1000.0;
        optParts << ("start=" + QByteArray::number(startSec, 'f', 3));
    } else if (startPositionMs > 0 && isDash) {
        m_pendingSeekAfterLoadMs = startPositionMs;
    }
    if (isDash) {
        // 【2026-07-03 追記4、2026-07-13に撤回】"cache" プロパティを
        // DASHの場合だけ cache=no にすることで、配信側のRangeリクエスト
        // 特性とdemuxerキャッシュスレッドの相性問題（追記2参照。当時は
        // シーク時にdemuxerキャッシュスレッドが詰まって動画が完全停止する
        // 不具合があった）を回避していた。
        //
        // 2026-07-13追記: この cache=no はStreamProxy導入前の対策であり、
        // 副作用として「demuxer-max-bytes/demuxer-readahead-secs等の
        // 先読み上限が丸ごと無効になる」ことが実機の長時間DASH再生検証で
        // 判明した。cache=noだと再生位置とは無関係にmpv/ffmpegが読める限り
        // 先読みし続け、しかもStreamProxyがCDNアクセスを高速・安定化させた
        // 結果、以前は詰まって自然に止まっていた先読みが止まらなくなり、
        // 長時間再生でメモリが際限なく増え続ける（実機で数分のDASH再生で
        // 数百MB〜3GB近くまで増加）新たな不具合を引き起こしていた。
        //
        // StreamProxyが「CDNへの単一巨大Rangeリクエストが応答不能になる」
        // 根本原因そのものを解消済みのため、cache=noでのdemuxerスレッド
        // 詰まり回避策はもう不要ではないかと考え、cache=yesに戻し、
        // 上のResourceScope::initialize()で設定済みの
        // demuxer-max-bytes(16MiB)/demuxer-readahead-secs(8s)等の
        // 上限をDASHでもそのまま効かせるようにする。
        // もしこれでシーク詰まりが再発するようなら、cache=noに戻すのではなく
        // 「cache=yesのままdemuxer-max-bytesを更に絞る」方向で対策すること
        // （cache=no自体は先読み上限を丸ごと失うトレードオフが大きすぎる）。
        setPropertyStringAsync("cache", QByteArrayLiteral("yes"));
    } else {
        // DASH以外（ローカル再生や非シーク時）はデフォルトのキャッシュ挙動に
        // 戻す。MpvPlayerインスタンスは画質切り替えの裏側プレイヤー等で
        // open()が複数回呼ばれることがあるため、前回DASHでcache=noにした
        // 状態が非DASHの再生に持ち越されないよう明示的にリセットする。
        setPropertyStringAsync("cache", QByteArrayLiteral("yes"));
    }

    if (!optParts.isEmpty()) {
        QByteArray optsStr;
        for (int i = 0; i < optParts.size(); ++i) {
            if (i > 0) optsStr += ',';
            optsStr += optParts[i];
        }
        // このmpvビルドの loadfile は
        // "loadfile <url> [<flags> [<index> [<options>]]]" の4引数形式。
        // options を渡すには index（"replace" では無視されるが位置引数として
        // 整数でパースされる）も明示的に埋める必要がある。これを省いて
        // {"loadfile", url, "replace", "start=..."} の3引数形式で呼ぶと、
        // mpv が "start=..." を index 位置の値として解釈しようとして
        // 「The loadfile option must be an integer」エラーになり、
        // loadfile コマンド自体が失敗して動画が一切再生されなくなる
        // （実際に報告された不具合）。
        const char *cmd[] = {"loadfile", urlBytes.constData(), "replace", "-1", optsStr.constData(), nullptr};
        mpv_command_async(MPV(m_mpv), 0, cmd);
    } else {
        const char *cmd[] = {"loadfile", urlBytes.constData(), nullptr};
        mpv_command_async(MPV(m_mpv), 0, cmd);
    }
}

void MpvPlayer::setHttpHeaders(const QStringList &headers)
{
    m_httpHeaders = headers;
    if (!m_mpv)
        return;

    QString userAgent;
    QString referrer;
    QByteArrayList extraHeaderFields;
    for (const QString &header : headers) {
        const int colon = header.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString name = header.left(colon).trimmed().toLower();
        const QString value = header.mid(colon + 1).trimmed();
        if (value.isEmpty())
            continue;
        if (name == QStringLiteral("user-agent"))
            userAgent = value;
        else if (name == QStringLiteral("referer") || name == QStringLiteral("referrer"))
            referrer = value;
        else
            extraHeaderFields << escapeMpvStringListValue(header.toUtf8());
    }

    // 2026-09-16: 非同期化（open() の loadfile より先に送られる順序は変わらない）。
    if (!userAgent.isEmpty())
        setPropertyStringAsync("user-agent", userAgent.toUtf8());
    setPropertyStringAsync("referrer", referrer.toUtf8());

    QByteArray joinedExtraHeaders;
    for (int i = 0; i < extraHeaderFields.size(); ++i) {
        if (i > 0)
            joinedExtraHeaders += ',';
        joinedExtraHeaders += extraHeaderFields.at(i);
    }
    setPropertyStringAsync("http-header-fields", joinedExtraHeaders);
}

void MpvPlayer::play()
{
    if (!m_mpv) return;
    qDebug("[MpvPlayer %p] play() — pause=0 送信", static_cast<void*>(this));
    int flag = 0;
    mpv_set_property_async(MPV(m_mpv), 0, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvPlayer::pause()
{
    if (!m_mpv) return;
    qDebug("[MpvPlayer %p] pause() — pause=1 送信", static_cast<void*>(this));
    int flag = 1;
    mpv_set_property_async(MPV(m_mpv), 0, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvPlayer::stop()
{
    if (!m_mpv) return;
    const char *cmd[] = {"stop", nullptr};
    mpv_command_async(MPV(m_mpv), 0, cmd);
    m_internalState     = PbInternalState::Idle;
    m_position          = 0;
    m_firstFrameEmitted = false;
    m_videoSuppressed   = false;
    applyAsyncRenderGate();
}

// ================================================================
//  AudioOnly モード実装
// ================================================================
void MpvPlayer::pauseVideo()
{
    if (!m_mpv) return;
    if (m_videoSuppressed) return;

    // 2026-07-16: vd-lavc-skipframeへの切り替えを試したが、実機ログで
    // このmpvビルドでは同プロパティの変更自体がフルのデコーダ再構築
    // （hwdec再探索・VFチェーン再構築）を伴うことが判明し、しかも
    // AudioOnly突入時にもその重い処理が走るようになってしまった
    // （従来のvid=no方式は突入時は軽く、復帰時のみ重かった）。
    // 突入・復帰の両方が重くなるのは明確な悪化のため、vid=no方式に戻す。
    // 復帰時の重さ（デコーダ再構築＋refresh seekによる音声アンダーラン）
    // 自体は残るが、これは同時復帰本数を減らす／ずらす方向
    // （BubbleController側のstagger実装）で緩和する方針とする。
    qDebug("[MpvPlayer %p] [AudioOnly] pauseVideo() — vid=no を送信  pos=%.2fs",
           static_cast<void*>(this),
           m_position / 1000.0);
    qDebug("[MpvPlayer %p] [AudioOnly]   音声は継続・映像デコードのみ停止します",
           static_cast<void*>(this));

    static const char *kVidNo = "no";
    mpv_set_property_async(MPV(m_mpv), 0, "vid", MPV_FORMAT_STRING, (void*)&kVidNo);
    m_videoSuppressed = true;
    applyAsyncRenderGate();
}

void MpvPlayer::resumeVideo()
{
    if (!m_mpv) return;
    if (!m_videoSuppressed) return;

    // 2026-07-08: このプレイヤーがdisableVideoTrack=trueで開かれている
    // 場合（ローカル音声ファイル等、埋め込みジャケット画像をmpvが動画
    // トラックとして誤ってデコードしクラッシュする不具合の対策でvid=noを
    // 強制している）、AudioOnly機能の画面内外トグル（requestAudioOnly()の
    // pauseVideo()／wakeUpVideo()のresumeVideo()）がここを素通りして
    // vid=autoへ戻してしまうと、せっかく無効化した映像デコードが復活し、
    // 元のクラッシュが別経路から再現する（実機ログで確認済み: AudioOnly
    // から画面内復帰した直後にresumeVideo()が呼ばれ、その直後にクラッシュ）。
    // open()時にdisableVideoTrackが指定されている限り、AudioOnly機能から
    // 呼ばれても実際のvidプロパティは変更せず、m_videoSuppressedの
    // bookkeepingだけ戻す（呼び出し側のAudioOnly状態機械には影響しない）。
    if (m_videoTrackDisabled) {
        qDebug("[MpvPlayer %p] [AudioOnly] resumeVideo() スキップ — disableVideoTrack中のためvid=noを維持",
               static_cast<void*>(this));
        m_videoSuppressed = false;
        applyAsyncRenderGate();
        return;
    }

    qDebug("[MpvPlayer %p] [AudioOnly] resumeVideo() — vid=auto を送信  pos=%.2fs"
           "  videoSuppressed=%d renderCtx=%p fbo=%u tex=%u",
           static_cast<void*>(this),
           m_position / 1000.0,
           m_videoSuppressed,
           static_cast<void*>(m_async.get()),
           m_fbo,
           m_fboColorTex);
    qDebug("[MpvPlayer %p] [AudioOnly]   映像デコード再開・再生位置はリセットしません",
           static_cast<void*>(this));

    // 2026-07-15: vid の set / 反映確認の同期読み取りは、mpv コアが
    // デコーダを再初期化している最中に UI スレッドをブロックしうるため、
    // pause() と同じ非同期 API に統一し、確認用の同期 mpv_get_property()
    // は削除した（復帰時のカクつき調査 #AudioOnly-resume-stutter）。
    static const char *kVidAuto = "auto";
    mpv_set_property_async(MPV(m_mpv), 0, "vid", MPV_FORMAT_STRING, (void*)&kVidAuto);
    m_videoSuppressed = false;
    applyAsyncRenderGate();
}

void MpvPlayer::setLowCacheMode(bool enabled)
{
    if (!m_mpv || m_lowCacheMode == enabled) return;
    m_lowCacheMode = enabled;

    const char *cacheSecs = enabled ? "4" : "12";
    const char *readaheadSecs = enabled ? "2" : "8";
    const char *maxBytes = enabled ? "4MiB" : "16MiB";
    const char *maxBackBytes = enabled ? "1MiB" : "4MiB";

    // 2026-09-16: 非同期化。実測で同期 mpv_set_property_string の1回目が
    // 52ms / 134ms 待たされていた（AudioOnly 突入直後。直前に非同期で送った
    // vid=no のトラック切替を mpv コアが処理し終えるまで、同期呼び出しは
    // コアのロック待ちになる）。2〜4回目は 0ms。
    // 非同期 API はキューに積むだけで GUI を待たせない。適用順は送信順のまま
    // （vid=no → cache 系4つ）なので挙動は変わらない。値は呼び出し時にコピーされる。
    mpv_set_property_async(MPV(m_mpv), 0, "cache-secs",             MPV_FORMAT_STRING, (void*)&cacheSecs);
    mpv_set_property_async(MPV(m_mpv), 0, "demuxer-readahead-secs", MPV_FORMAT_STRING, (void*)&readaheadSecs);
    mpv_set_property_async(MPV(m_mpv), 0, "demuxer-max-bytes",      MPV_FORMAT_STRING, (void*)&maxBytes);
    mpv_set_property_async(MPV(m_mpv), 0, "demuxer-max-back-bytes", MPV_FORMAT_STRING, (void*)&maxBackBytes);

    qDebug("[MpvPlayer %p] cache mode=%s cache-secs=%s demuxer-readahead-secs=%s demuxer-max-bytes=%s demuxer-max-back-bytes=%s",
           static_cast<void*>(this),
           enabled ? "low" : "normal",
           cacheSecs,
           readaheadSecs,
           maxBytes,
           maxBackBytes);
}

void MpvPlayer::releaseVideoResources()
{
    // 2026-09-15 非同期化 P2: バッファはワーカーのもの。GUIは受け取り済みの
    // 1枚を手放し、削除はワーカーへ依頼する（GL current 不要）。
    if (m_async) {
        {
            QMutexLocker lk(&m_async->swapMutex);
            m_asyncFrontSerial = m_async->readySerial; // 解放前のフレームを後で拾わない
        }
        m_fbo = 0;
        m_fboColorTex = 0;
        m_fboW = 0;
        m_fboH = 0;
        std::shared_ptr<MpvAsyncRenderSlot> s = m_async;
        if (VideoRenderThread::shared() == s->thread)
            s->thread->post([s]() { asyncDeleteBuffers(s.get()); });
        qDebug("[MpvPlayer %p] releaseVideoResources: async buffers release queued slot=%p",
               static_cast<void*>(this), static_cast<void*>(s.get()));
        return;
    }
    // 2026-09-16 P7: 同期経路撤去。非同期でないプレイヤーはバッファを持たない。
}

void MpvPlayer::seekCommand(qint64 posMs, const char *flags)
{
    // 2026-07-12: seek()/seekFast()/DASHシーク詰まり監視の再試行、
    // すべてから使われる下請け関数。トークン管理には一切触れない
    // （watchdogの内部リトライがこれを呼んでも自分自身の監視を
    // 無効化してしまわないようにするため。詳細はseek()側のコメント
    // 参照）。
    double posSec = posMs / 1000.0;
    QByteArray posStr = QByteArray::number(posSec, 'f', 3);
    const char *cmd[] = {"seek", posStr.constData(), flags, nullptr};
    mpv_command_async(MPV(m_mpv), 0, cmd);
}

void MpvPlayer::seek(qint64 posMs)
{
    if (!m_mpv) return;
    // 2026-07-12追記: FocusSleep復帰時やFILE_LOADED直後の位置復元で
    // 発行されたseekWithStallWatchdog()の監視が完了/断念する前に、
    // ユーザーが別の場所へ精密seek()（シークバー操作等）した場合、
    // 古い監視サイクルがトークンチェックだけでは無効化されず生き残り、
    // 数秒後に「詰まっている」と誤判定して古いtargetMsへ強制的に
    // 引き戻してしまう不具合が実機で確認された（「一気に別の場所へ
    // シークすると元居た場所に戻ろうとする」症状）。新しいシークは
    // 発行元に関わらず古い監視を必ず無効化するべきなので、ここで
    // トークンを進めて古いコールバックを無条件に無効化する
    // （watchdog自身の内部リトライはseekCommand()を直接呼ぶため、
    // ここには来ず自滅しない）。
    ++m_dashSeekWatchdogToken;
    m_dashSeekTargetMs = -1;

    // "absolute" 単体はmpvのデフォルトで exact（精密）シークになる。
    // 対象キーフレームより後ろまでデコードして正確な位置に合わせるため、
    // シークバーのドラッグ中のように短時間に連続して呼ぶと重くなる
    // （かくつきの原因）。ドラッグ終了時の最終確定シークなど、
    // 1回だけ呼ぶ用途で使うこと。
    seekCommand(posMs, "absolute");
}

void MpvPlayer::seekFast(qint64 posMs)
{
    if (!m_mpv) return;
    // 2026-07-12追記: seek()と同じ理由で、古いDASHシーク詰まり監視を
    // 無効化する（詳細はseek()側のコメント参照）。
    ++m_dashSeekWatchdogToken;
    m_dashSeekTargetMs = -1;

    // "keyframes" フラグで近傍キーフレームへ飛ぶだけの粗いシークにする。
    // 対象位置まで逐次デコードしないため非常に軽く、シークバーの
    // ドラッグ中など連続呼び出しに向く。
    seekCommand(posMs, "absolute+keyframes");
}

void MpvPlayer::seekWithStallWatchdog(qint64 posMs)
{
    // 2026-07-04: このデマルチプレクサ詰まり対策（キーフレーム基準の粗い
    // シーク+自動リトライ監視）が必要なのは、外部音声ストリームを結合
    // している DASH（audio-file=あり）の場合だけと判明している（open()側
    // のコメント群・MpvPlayer.hのm_isDashコメント参照）。非DASH（通常の
    // 結合フォーマット＝プログレッシブ）ではこの不具合は再現しないため、
    // 素直に精密seek()を使う。以前はDASH/非DASH問わず一律でこちらを
    // 使っていたため、矢印キーでの5秒スキップ等がキーフレーム間隔まで
    // 丸められてしまい、狙った位置にほぼ動かない＝「5秒スキップが
    // 機能しない」ように見える不具合になっていた。
    if (!m_isDash) {
        seek(posMs);
        return;
    }

    // 2026-07-12: ここではseekFast()を呼ばず、直接seekCommand()を使う。
    // seekFast()を呼ぶとその中でトークンが進んでしまい、直後に自分で
    // 設定するm_dashSeekTargetMs/tokenと矛盾してこの監視サイクル自体が
    // 即座に無効扱いになってしまう（自滅する）ため。
    seekCommand(posMs, "absolute+keyframes");
    m_dashSeekTargetMs = posMs;
    m_dashSeekLastObservedPos = -1; // 2026-07-13: 新しい監視サイクルの開始。前回値をリセット。
    const int token = ++m_dashSeekWatchdogToken;
    scheduleDashSeekWatchdog(posMs, token, 3);
}

void MpvPlayer::scheduleDashSeekWatchdog(qint64 targetMs, int token, int retriesLeft)
{
    QTimer::singleShot(2500, this, [this, targetMs, token, retriesLeft]() {
        checkDashSeekWatchdog(targetMs, token, retriesLeft);
    });
}

void MpvPlayer::checkDashSeekWatchdog(qint64 targetMs, int token, int retriesLeft)
{
    if (!m_mpv) return;
    // open()や別のシークで無効化された古い監視は何もしない。
    if (token != m_dashSeekWatchdogToken) return;
    if (m_dashSeekTargetMs != targetMs) return;

    // 2026-07-03: ログ実測に基づく許容誤差。詰まっていなければ通常
    // 数百ms～1秒程度でtargetMs付近まで進むはずなので、これより大きく
    // 離れたままなら「固まっている」とみなす。
    //
    // 2026-07-12修正: 以前はqAbs(m_position - targetMs)で判定していたが、
    // これだと「シークは成功し、そのまま順調に再生が進んで、2.5秒後の
    // このチェック時点で既にtargetMsを2.5秒以上追い越していただけ」の
    // 正常なケースまで「詰まっている」と誤判定していた。実機で「普通に
    // 再生しているだけなのに勝手に少し巻き戻る」症状として確認された
    // （誤判定→targetMsへの無駄な再シークが発生していた）。本当に
    // 詰まっているのは「targetMsに全く追いつけていない（手前で止まった
    // まま）」場合だけなので、targetMs方向への到達判定は片側だけで良い。
    constexpr qint64 kStallToleranceMs = 2500;
    if (m_position >= targetMs - kStallToleranceMs) {
        // 無事到達できた（またはそれ以上進んでいる）。監視終了。
        m_dashSeekTargetMs = -1;
        return;
    }

    // 2026-07-13修正: 上のtargetMs到達判定だけだと、今度は別の誤判定が
    // 実機で確認された。seekWithStallWatchdog()が使う"absolute+keyframes"
    // シークは、mpvの仕様上「指定位置以前の直近キーフレーム」に着地する
    // （ログの"adjust seek target from 544.8 to 540.0"がこれ）。
    // キーフレーム間隔がkStallToleranceMs(2.5秒)より広い動画では、
    // 着地点からtargetMsまでの距離が2.5秒を超えるため、実際には正常に
    // 再生が進んでいても「毎回」このチェック時点でtargetMs-2.5秒に
    // 届いておらず「詰まっている」と誤判定され、同じキーフレームへの
    // 無意味な再シーク（体感的には再生位置が一瞬巻き戻る/カクつく）を
    // 引き起こしていた。
    //
    // 本当に詰まっている場合は「再生位置が全く進んでいない（フリーズ
    // したまま）」はずなので、targetMsへの到達可否ではなく「前回チェック
    // 時点から今回までの間に再生位置が実際に進んだか」で判定する。
    // 前回と比べて明確に前進していれば、まだtargetMsに届いていなくても
    // 正常に再生が進んでいるとみなし、再シークはせず様子見を続ける
    // （再シークで着地点をリセットしてしまうと、かえって永久に
    // targetMsへ追いつけなくなる）。
    constexpr qint64 kProgressEpsilonMs = 300; // タイマーの揺らぎ吸収用の遊び
    const bool firstCheck = (m_dashSeekLastObservedPos < 0);
    const bool isProgressing = !firstCheck && (m_position > m_dashSeekLastObservedPos + kProgressEpsilonMs);
    m_dashSeekLastObservedPos = m_position;

    if (firstCheck || isProgressing) {
        // まだtargetMsには届いていないが、初回チェックか、あるいは
        // 前回から確実に前進しているので「詰まってはいない」と判断。
        // 再シークはせず、リトライ回数も消費せずに監視だけ続ける。
        scheduleDashSeekWatchdog(targetMs, token, retriesLeft);
        return;
    }

    if (retriesLeft <= 0) {
        qWarning("[MpvPlayer %p] [Quality] DASHシーク詰まり監視: %.2fs付近で固まったまま断念 (pos=%.2fs)",
                 static_cast<void*>(this), targetMs / 1000.0, m_position / 1000.0);
        m_dashSeekTargetMs = -1;
        return;
    }

    // ここに来るのは「前回チェックから位置が実質進んでいない」＝本当に
    // フリーズしている場合のみ。実機ログで、固まった状態でも「もう一度
    // シークし直す」と復帰するケースが確認されているため、再シークで
    // 復帰を試みる。
    // 2026-07-12: ここもseekFast()ではなくseekCommand()を直接呼ぶ
    // （理由はseekWithStallWatchdog()側のコメント参照。seekFast()経由だと
    // トークンが進んでこの監視サイクル自身を無効化してしまう）。
    qWarning("[MpvPlayer %p] [Quality] DASHシーク詰まり検出。%.2fsへ再シーク（残り%d回）",
             static_cast<void*>(this), targetMs / 1000.0, retriesLeft);
    seekCommand(targetMs, "absolute+keyframes");
    m_dashSeekLastObservedPos = -1; // 再シーク後の着地点を新たな基準にするためリセット
    scheduleDashSeekWatchdog(targetMs, token, retriesLeft - 1);
}

void MpvPlayer::setLooping(bool loop)
{
    if (!m_mpv) return;
    // 2026-09-16: 動作中の set_option（同期）→ プロパティの非同期書き込みへ。
    setPropertyStringAsync("loop-file", loop ? QByteArrayLiteral("inf") : QByteArrayLiteral("no"));
}

void MpvPlayer::setSpeed(double speed)
{
    if (!m_mpv) return;
    double s = qBound(0.1, speed, 4.0);
    mpv_set_property_async(MPV(m_mpv), 0, "speed", MPV_FORMAT_DOUBLE, &s);
}

void MpvPlayer::setVolume(float v)
{
    m_volume = v;
    if (!m_mpv) return;
    double vol100 = static_cast<double>(v * 100.0);
    mpv_set_property_async(MPV(m_mpv), 0, "volume", MPV_FORMAT_DOUBLE, &vol100);
}

PbInternalState MpvPlayer::internalState() const { return m_internalState; }
qint64          MpvPlayer::position()      const { return m_position; }
qint64          MpvPlayer::duration()      const { return m_duration; }
QSize           MpvPlayer::videoSize()     const { return m_videoSize; }
float           MpvPlayer::volume()        const { return m_volume; }

// 2026-09-16: 同期 mpv_get_property をやめた。控えを返しつつ、非同期で
// 取り寄せ直す（同じ名前の取り寄せが返ってくるまでは重ねて送らない）。
// 値は「前回取り寄せた時点」のもの。経過msを "(age=Nms)" で併記する。
QString MpvPlayer::mirroredProperty(const char *name, int format) const
{
    if (!m_mpv) return QStringLiteral("no-mpv");
    if (!m_propMirrorClock.isValid())
        m_propMirrorClock.start();
    const QByteArray key(name);
    PropMirror &e = m_propMirror[key];
    if (!e.pending) {
        e.pending = true;
        // reply_userdata は書式を持たせる（返信で文字列化の方法を決める）
        mpv_get_property_async(MPV(m_mpv), static_cast<uint64_t>(format), name,
                               static_cast<mpv_format>(format));
    }
    if (e.atMs < 0)
        return QStringLiteral("pending");
    return QStringLiteral("%1(age=%2ms)").arg(e.value).arg(m_propMirrorClock.elapsed() - e.atMs);
}

QString MpvPlayer::mpvPropertyString(const char *name) const
{
    return mirroredProperty(name, MPV_FORMAT_STRING);
}

QString MpvPlayer::mpvPropertyFlagString(const char *name) const
{
    return mirroredProperty(name, MPV_FORMAT_FLAG);
}

void MpvPlayer::handlePropertyReply(void *ptr)
{
    auto *ev = static_cast<mpv_event*>(ptr);
    auto *prop = static_cast<mpv_event_property*>(ev->data);
    if (!prop || !prop->name)
        return;
    if (!m_propMirrorClock.isValid())
        m_propMirrorClock.start();
    PropMirror &e = m_propMirror[QByteArray(prop->name)];
    e.pending = false;
    e.atMs = m_propMirrorClock.elapsed();
    if (ev->error < 0) {
        e.value = QStringLiteral("n/a(%1)").arg(QString::fromUtf8(mpv_error_string(ev->error)));
    } else if (prop->format == MPV_FORMAT_STRING && prop->data) {
        const char *v = *static_cast<char **>(prop->data);
        e.value = QString::fromUtf8(v ? v : "(null)");
    } else if (prop->format == MPV_FORMAT_FLAG && prop->data) {
        e.value = *static_cast<int *>(prop->data) ? QStringLiteral("yes") : QStringLiteral("no");
    } else {
        e.value = QStringLiteral("n/a(format=%1)").arg(static_cast<int>(prop->format));
    }
}

void MpvPlayer::setPropertyStringAsync(const char *name, const QByteArray &value)
{
    if (!m_mpv) return;
    const quint64 id = ++m_setReplySeq;
    m_setReplyNames.insert(id, QByteArray(name) + '=' + value.left(60));
    const char *cstr = value.constData();
    const int rc = mpv_set_property_async(MPV(m_mpv), id, name, MPV_FORMAT_STRING, (void *)&cstr);
    if (rc < 0) {
        m_setReplyNames.remove(id);
        qWarning("[MpvPlayer %p] set_property_async(%s) 送信失敗 mpv_error=%d",
                 static_cast<void*>(this), name, rc);
    }
}

void MpvPlayer::logDiagnostics(const char *tag) const
{
    qDebug("[%s][MpvPlayer %p] state=%d mpv=%d renderCtx=%d fbo=%d videoSuppressed=%d lowCacheMode=%d fboSize=%dx%d fboTex=%u fboMem=%.2fMB",
           tag,
           static_cast<const void*>(this),
           static_cast<int>(m_internalState),
           m_mpv != nullptr,
           m_async != nullptr,
           hasFbo(),
           m_videoSuppressed,
           m_lowCacheMode,
           m_fboW,
           m_fboH,
           m_fboColorTex,
           estimatedFboBytes() / 1048576.0);

    qDebug("[%s][MpvPlayer %p] playback pause=%s vid=%s aid=%s pos=%.2fs duration=%.2fs eof=%s paused-for-cache=%s",
           tag,
           static_cast<const void*>(this),
           qPrintable(mpvPropertyFlagString("pause")),
           qPrintable(mpvPropertyString("vid")),
           qPrintable(mpvPropertyString("aid")),
           m_position / 1000.0,
           m_duration / 1000.0,
           qPrintable(mpvPropertyFlagString("eof-reached")),
           qPrintable(mpvPropertyFlagString("paused-for-cache")));

    qDebug("[%s][MpvPlayer %p] cache cache=%s demuxer-cache-duration=%s demuxer-cache-time=%s demuxer-cache-idle=%s cache-used=%s cache-size=%s file-size=%s",
           tag,
           static_cast<const void*>(this),
           qPrintable(mpvPropertyString("cache")),
           qPrintable(mpvPropertyString("demuxer-cache-duration")),
           qPrintable(mpvPropertyString("demuxer-cache-time")),
           qPrintable(mpvPropertyFlagString("demuxer-cache-idle")),
           qPrintable(mpvPropertyString("cache-used")),
           qPrintable(mpvPropertyString("cache-size")),
           qPrintable(mpvPropertyString("file-size")));

    qDebug("[%s][MpvPlayer %p] media hwdec-current=%s video-codec=%s estimated-vf-fps=%s current-vo=%s current-ao=%s",
           tag,
           static_cast<const void*>(this),
           qPrintable(mpvPropertyString("hwdec-current")),
           qPrintable(mpvPropertyString("video-codec")),
           qPrintable(mpvPropertyString("estimated-vf-fps")),
           qPrintable(mpvPropertyString("current-vo")),
           qPrintable(mpvPropertyString("current-ao")));
}

PbVideoFrame MpvPlayer::currentFrame() const
{
    if (!hasValidFboTexture()) return {};
    PbVideoFrame f;
    f.textureId = m_fboColorTex;
    f.width     = m_fboW;
    f.height    = m_fboH;
    f.pts       = m_position;
    return f;
}

void MpvPlayer::processMpvEvents()
{
    if (!m_mpv) return;
    while (true) {
        mpv_event *ev = mpv_wait_event(MPV(m_mpv), 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        handleMpvEvent(static_cast<void*>(ev));
    }
}

void MpvPlayer::handleMpvEvent(void *ptr)
{
    auto *ev = static_cast<mpv_event*>(ptr);
    switch (ev->event_id) {
    case MPV_EVENT_FILE_LOADED:
        qDebug("[MpvPlayer %p] FILE_LOADED", static_cast<void*>(this));
        m_internalState = PbInternalState::Decoding;
        // 2026-07-08: vid=noで開いた場合（ローカル音声ファイル）、映像
        // フレームが一切デコードされないため、initializeGl()内のrender
        // update callbackで発火するはずのfirstFrameReady()が永久に来ない。
        // 以前はCanvasVideo::startPlayback()側の個別ワークアラウンド
        // （QTimer::singleShotでnotifyQMediaPlayerReady()+play()を強制）
        // が初回ロードだけをカバーしていたが、BubbleController::wakeUp()/
        // retry()等（一時停止アイドル解放からの復帰を含む）による再オープン
        // はそちらを経由しないため、Loading状態のまま固まる不具合が
        // 実機ログで確認された（再オープン後、mpv内部では音声が普通に
        // 再生されているのにアプリの状態機械だけがLoadingのまま進まない）。
        // ここでファイルが実際に開けたタイミングに合わせてfirstFrameReady()
        // を合成発火させることで、どの経路からの再オープンでも一貫して
        // Loading→Readyへ進めるようにする。
        if (m_videoTrackDisabled && !m_firstFrameEmitted) {
            m_firstFrameEmitted = true;
            emit firstFrameReady();
        }
        // DASH+start=同時指定によるフリーズ対策（open()側のコメント参照）。
        // loadfile時点ではstart=を送らずここまで来ているので、ファイルが
        // 実際に開けたこのタイミングで改めてseekする。
        //
        // 2026-07-03 追記4: verboseログでの実測により、ここでseek()
        // （absolute=精密シーク/hr-seek）を使うとフリーズすることが
        // 確定した。精密シークは対象位置まで正確に合わせるため、
        // video用のmkvデマクサとaudio用のlavfデマクサ（外部audio-file=の
        // 別HTTPストリーム）の両方が目標位置まで同期して読み進む必要が
        // あるが、ログを見るとvideo側のストリームレベルseekは完了する
        // 一方でaudio側のストリームレベルseekがそのまま無応答になり、
        // 二度と後続ログが出ないまま再生位置が永久に固まる
        // （mpv本体側のDASH+外部音声+精密シークの既知の弱点、
        // 参考: mpv issue #1824 "Audio freezes on backward seek with
        // external audio file"と同種の症状）。
        // ロード直後・二本の別々のHTTP接続がまだ確立したばかりの
        // タイミングでこの精密同期を要求するのが特に危険と見られるため、
        // seekFast()（absolute+keyframes=直近キーフレームへの粗いシーク）
        // に変更した。
        //
        // 2026-07-03 追記5: しかし実機再テストで、seekFast()に変えても
        // 全く同じ箇所（audio側のffmpegストリームレベルseek）で固まる
        // ことが確認された。つまり精密/粗いの違いは無関係で、DASHの
        // 外部音声ストリームに対してロード直後にシークを送ること自体に
        // mpv/ffmpeg側の相性問題がある。一方で、興味深いことに前回の
        // ログでは「別の機能（サムネ生成のカスケードseek）が偶然同じ
        // ストリームに追加でseekを送ったことで復帰する」現象が確認できた。
        // つまり「固まったらもう一度シークし直す」ことで復帰できる
        // 見込みが高いため、単発のseekFast()ではなく再試行付きの
        // seekWithStallWatchdog()を使う。
        if (m_pendingSeekAfterLoadMs >= 0) {
            const qint64 seekPos = m_pendingSeekAfterLoadMs;
            m_pendingSeekAfterLoadMs = -1;
            qDebug("[MpvPlayer %p] [Quality] DASH+start対策: FILE_LOADED後に%.2fsへseekFast(監視付き)",
                   static_cast<void*>(this), seekPos / 1000.0);
            seekWithStallWatchdog(seekPos);
        }
        break;
    case MPV_EVENT_START_FILE:
        m_firstFrameEmitted = false;
        break;
    case MPV_EVENT_END_FILE: {
        auto *ef = static_cast<mpv_event_end_file*>(ev->data);
        if (ef->reason == MPV_END_FILE_REASON_EOF) {
            m_internalState = PbInternalState::Ended;
            emit internalStateChanged(PbInternalState::Ended);
        } else if (ef->reason == MPV_END_FILE_REASON_ERROR) {
            m_internalState = PbInternalState::Error;
            emit internalStateChanged(PbInternalState::Error);
            emit errorOccurred(QString::fromUtf8(mpv_error_string(ef->error)));
        }
        break;
    }
    case MPV_EVENT_PROPERTY_CHANGE:
        handlePropertyChange(ev->data);
        break;
    case MPV_EVENT_GET_PROPERTY_REPLY:
        handlePropertyReply(ptr);
        break;
    case MPV_EVENT_SET_PROPERTY_REPLY: {
        // setPropertyStringAsync() の返信。id が登録済みのものだけ扱う
        // （pause/volume 等の既存の非同期書き込みは userdata=0 なので無視）。
        const auto it = m_setReplyNames.find(ev->reply_userdata);
        if (it != m_setReplyNames.end()) {
            if (ev->error < 0)
                qWarning("[MpvPlayer %p] set_property_async(%s) 失敗 mpv_error=%d(%s)",
                         static_cast<void*>(this), it.value().constData(), ev->error,
                         mpv_error_string(ev->error));
            m_setReplyNames.erase(it);
        }
        break;
    }
    case MPV_EVENT_LOG_MESSAGE: {
        auto *msg = static_cast<mpv_event_log_message*>(ev->data);
        // 2026-09-12: 常時垂れ流される既知の警告は間引く。詳細は
        // isNoisyMpvLog() のコメント参照。
        if (isNoisyMpvLog(msg->text)) {
            if (!m_noisyLogClock.isValid()) {
                // 初回だけはそのまま出す（存在に気付けるように）。
                m_noisyLogClock.start();
                qDebug("[mpv/%s] %s", msg->prefix, msg->text);
                qDebug("[MpvPlayer %p] ↑この警告は以後%d秒ごとに件数だけ報告します",
                       static_cast<void*>(this), kNoisyLogReportIntervalMs / 1000);
                break;
            }
            ++m_noisyLogSuppressed;
            if (m_noisyLogClock.elapsed() >= kNoisyLogReportIntervalMs) {
                qDebug("[mpv/%s] (間引き) 直近%.1f秒で同種の警告を%d回抑制",
                       msg->prefix, m_noisyLogClock.elapsed() / 1000.0,
                       m_noisyLogSuppressed);
                m_noisyLogClock.restart();
                m_noisyLogSuppressed = 0;
            }
            break;
        }
        qDebug("[mpv/%s] %s", msg->prefix, msg->text);
        maybeHandleHwdecDecodeError(msg->prefix, msg->text);
        break;
    }
    default:
        break;
    }
}

void MpvPlayer::handlePropertyChange(void *ptr)
{
    auto *prop = static_cast<mpv_event_property*>(ptr);
    const QString name = QString::fromUtf8(prop->name);

    if (name == "hwdec-current") {
        // 2026-09-16: 未確定/解除時は MPV_FORMAT_NONE で来る → 空文字扱い
        if (prop->format == MPV_FORMAT_STRING && prop->data) {
            const char *v = *static_cast<char **>(prop->data);
            m_hwdecCurrent = QString::fromUtf8(v ? v : "");
        } else {
            m_hwdecCurrent.clear();
        }
        return;
    }
    if (name == "playback-time" && prop->format == MPV_FORMAT_DOUBLE) {
        m_position = static_cast<qint64>(*static_cast<double*>(prop->data) * 1000.0);
        emit positionChanged(m_position);
    } else if (name == "duration" && prop->format == MPV_FORMAT_DOUBLE) {
        m_duration = static_cast<qint64>(*static_cast<double*>(prop->data) * 1000.0);
    } else if (name == "pause" && prop->format == MPV_FORMAT_FLAG) {
        int paused = *static_cast<int*>(prop->data);
        qDebug("[MpvPlayer %p] pause プロパティ変化: paused=%d internalState=%d",
               static_cast<void*>(this), paused, (int)m_internalState);
        if (paused) {
            if (m_internalState == PbInternalState::Decoding) {
                m_internalState = PbInternalState::Paused;
                emit internalStateChanged(PbInternalState::Paused);
            }
        } else {
            if (m_internalState == PbInternalState::Paused
             || m_internalState == PbInternalState::Opening
             || m_internalState == PbInternalState::Decoding) {
                m_internalState = PbInternalState::Decoding;
                emit internalStateChanged(PbInternalState::Decoding);
            }
        }
    } else if (name == "paused-for-cache" && prop->format == MPV_FORMAT_FLAG) {
        int buf = *static_cast<int*>(prop->data);
        if (buf) {
            m_internalState = PbInternalState::Buffering;
            emit internalStateChanged(PbInternalState::Buffering);
        } else if (m_internalState == PbInternalState::Buffering) {
            m_internalState = PbInternalState::Decoding;
            emit internalStateChanged(PbInternalState::Decoding);
        }
    } else if (name == "width" && prop->format == MPV_FORMAT_INT64) {
        m_videoSize.setWidth(static_cast<int>(*static_cast<int64_t*>(prop->data)));
    } else if (name == "height" && prop->format == MPV_FORMAT_INT64) {
        m_videoSize.setHeight(static_cast<int>(*static_cast<int64_t*>(prop->data)));
    }
}

// ================================================================
//  h264 デコードエラー時のソフトウェアデコード自動フォールバック
// ================================================================
// 2026-07-03 報告: 動画サイトの動画（DASH映像のみストリーム、720p等の
// 高解像度で選ばれやすい経路）の特定の
// 再生位置で、何度再生しても同じ箇所だけ映像が読み飛ばされる不具合が
// 報告された。ログには
//   [mpv/ffmpeg/video] h264: Missing reference picture, default is 0
// が出ており、直前の current-vo/current-ao 等から hwdec-current が
// d3d11va-copy（ハードウェアデコード経由）であることが確認できる。
//
// この警告自体はffmpegのH.264デコーダが参照ピクチャをDPBに見つけられず
// デフォルト値で代用するというもので、原因は主に2つ考えられる:
//   (a) その特定の配信ストリーム（＝プラグインが
//       resolve()で選んだURLの実体データ）に、GOP境界のエンコード上の
//       不整合が実際に存在する
//   (b) ネットワーク経由でのバイトレンジ取得時に一部NALが欠落した
// いずれにせよプラグイン自体はURLを選んで
// 返すだけで、H.264のデコード処理には一切関与していない（本ファイル・
// mpv/ffmpegの領域）。ただし、d3d11va-copy 等のハードウェアデコード経路は
// ソフトウェアデコードに比べて破損GOPからのエラーコンシールメント
// （欠落参照フレームの補完・継続復帰）が弱く、同じ破損データでも
// ソフトウェアデコードなら見た目の乱れ程度で通過できることが多い。
//
// そのため、この特定の警告を検出した場合、そのファイル（再生セッション）
// 限りで自動的にソフトウェアデコード（hwdec=no）へフォールバックする。
// "hwdec" は実行時に書き換え可能なmpvプロパティで、再ロードやシークを
// 挟まずにその場でデコーダが再初期化される。1ファイルにつき一度だけ
// 発火するようにし（m_hwdecFallbackTriggered）、頻発による無限ループや
// ちらつきを防ぐ。
void MpvPlayer::maybeHandleHwdecDecodeError(const char *prefix, const char *text)
{
    if (!m_mpv || m_hwdecFallbackTriggered) return;
    if (!prefix || !text) return;

    const QByteArray prefixBytes(prefix);
    const QByteArray textBytes(text);
    if (!prefixBytes.contains("video") || !textBytes.contains("Missing reference picture"))
        return;

    // 2026-09-16: 同期読み取り → observe で保持している値（handlePropertyChange で更新）。
    const QString hwdecCurrent = m_hwdecCurrent;

    // 既にソフトウェアデコード中（"no" もしくは未使用扱いの空文字）なら
    // これ以上できることはないので何もしない。
    if (hwdecCurrent.isEmpty() || hwdecCurrent == QStringLiteral("no"))
        return;

    m_hwdecFallbackTriggered = true;
    qWarning("[MpvPlayer %p] h264 'Missing reference picture' 検出 (hwdec-current=%s pos=%.2fs) — "
             "この再生に限りソフトウェアデコード(hwdec=no)へ自動フォールバックします",
             static_cast<void*>(this),
             qPrintable(hwdecCurrent),
             m_position / 1000.0);
    setPropertyStringAsync("hwdec", QByteArrayLiteral("no"));
}
