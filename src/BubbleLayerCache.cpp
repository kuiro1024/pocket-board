#include "BubbleLayerCache.h"
#include "Renderer.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <cmath>

namespace {
QElapsedTimer &layerClock()
{
    static QElapsedTimer c;
    if (!c.isValid())
        c.start();
    return c;
}
bool sameSize(const QSizeF &a, const QSizeF &b)
{
    return std::abs(a.width() - b.width()) < 0.5 && std::abs(a.height() - b.height()) < 0.5;
}
}

qint64 BubbleLayerCache::nowMs() const
{
    return layerClock().elapsed();
}

bool BubbleLayerCache::initialize()
{
    if (m_initialized)
        return true;
    if (!initializeOpenGLFunctions())
        return false;
    m_initialized = true;
    return true;
}

void BubbleLayerCache::freeEntry(Entry &e)
{
    if (e.fbo) glDeleteFramebuffers(1, &e.fbo);
    if (e.tex) glDeleteTextures(1, &e.tex);
    m_bytes -= qint64(e.texW) * e.texH * 4;
    e = Entry();
}

void BubbleLayerCache::release()
{
    if (!m_initialized)
        return;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        freeEntry(it.value());
    m_entries.clear();
    m_bytes = 0;
    m_initialized = false;
}

void BubbleLayerCache::forget(const void *key)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    if (m_initialized)
        freeEntry(it.value());
    m_entries.erase(it);
}

void BubbleLayerCache::beginFrame()
{
    ++m_frame;
    m_bakesThisFrame = 0;
    m_hitsThisFrame = 0;
    m_stretchedThisFrame = 0;
}

void BubbleLayerCache::endFrame()
{
    if (!m_initialized)
        return;
    // しばらく使われていない下地を捨てる（画面外へ出たバブルなど）
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (m_frame - it->lastFrame > kEvictAfterFrames) {
            freeEntry(it.value());
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}

bool BubbleLayerCache::bake(Renderer &r, Entry &e, const QRectF &layerRect,
                            int texW, int texH, quint64 contentKey,
                            const std::function<void()> &bakeFn)
{
    if (e.tex == 0 || e.texW != texW || e.texH != texH) {
        const qint64 newBytes = qint64(texW) * texH * 4;
        const qint64 oldBytes = qint64(e.texW) * e.texH * 4;
        if (m_bytes - oldBytes + newBytes > kMaxBytes)
            return false;
        if (e.tex == 0) {
            glGenTextures(1, &e.tex);
            glGenFramebuffers(1, &e.fbo);
        }
        GLint prevTex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        glBindTexture(GL_TEXTURE_2D, e.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, GLuint(prevTex));
        m_bytes += newBytes - oldBytes;
        e.texW = texW;
        e.texH = texH;
    }

    // 描画先をこのテクスチャへ。状態は必ず元に戻す（描画の途中で呼ばれる）。
    GLint prevDraw = 0, prevRead = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    const GLboolean scissorOn = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat clear[4] = {0, 0, 0, 0};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);

    glBindFramebuffer(GL_FRAMEBUFFER, e.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e.tex, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    r.pushOffscreen(layerRect, texW, texH);
    bakeFn();
    r.popOffscreen();

    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    if (scissorOn) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(prevDraw));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(prevRead));

    e.bakedSize = layerRect.size();
    e.contentKey = contentKey;
    e.valid = true;
    ++m_bakesThisFrame;
    return true;
}

BubbleLayerCache::Result BubbleLayerCache::draw(Renderer &r, const void *key,
                                                const QRectF &layerRect,
                                                quint64 contentKey, qreal dpr,
                                                const std::function<void()> &bakeFn)
{
    if (!m_initialized || !m_enabled || layerRect.isEmpty())
        return Result::NotCached;
    const int texW = qMax(1, int(std::ceil(layerRect.width() * dpr)));
    const int texH = qMax(1, int(std::ceil(layerRect.height() * dpr)));
    if (qint64(texW) * texH > kMaxTexturePixels)
        return Result::NotCached;

    // 2026-10-13: 貼る位置を物理ピクセルにそろえる。小数位置に貼るとテクスチャが
    // 補間されて、パンのたびに枠の線や接触影がにじんだりくっきりしたりを繰り返し
    // ちらついて見えた。テクセル1つ＝画面1ピクセルで貼る（ずれは最大0.5物理px）。
    const QRectF snapped(QPointF(std::round(layerRect.left() * dpr) / dpr,
                                 std::round(layerRect.top() * dpr) / dpr),
                         QSizeF(texW / dpr, texH / dpr));

    Entry &e = m_entries[key];
    e.lastFrame = m_frame;
    const qint64 now = nowMs();
    const QSizeF size = snapped.size();
    if (!sameSize(e.lastSeenSize, size)) {
        e.lastSeenSize = size;
        e.sizeChangedMs = now;
    }

    const bool keyMatches = e.valid && e.contentKey == contentKey;
    if (keyMatches && sameSize(e.bakedSize, size)) {
        r.drawPremultipliedTexture(e.tex, snapped, 1.0f);
        ++m_hitsThisFrame;
        return Result::Drawn;
    }
    // 2026-10-12: 大きさが変わっている最中（ズーム・リサイズ）は、焼いた絵を
    // 拡大縮小して貼ると粗が目立ったため、その場で描く（NotCached）。止まってから
    // kSettleMs 経ったら新しい大きさで焼き直す。
    const bool settling = now - e.sizeChangedMs < kSettleMs;
    if (settling) {
        ++m_stretchedThisFrame;   // 止まった後に焼き直すための描き直し予約に使う
        return Result::NotCached;
    }
    if (m_bakesThisFrame < kMaxBakesPerFrame
        && bake(r, e, snapped, texW, texH, contentKey, bakeFn)) {
        r.drawPremultipliedTexture(e.tex, snapped, 1.0f);
        ++m_hitsThisFrame;
        return Result::Drawn;
    }
    // このフレームはもう焼けない。いつも通り描き、次のフレームで焼く
    ++m_stretchedThisFrame;
    return Result::NotCached;
}
