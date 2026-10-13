#include "CanvasStaticLayerCache.h"

#include "Renderer.h"

#include <QDebug>

bool CanvasStaticLayerCache::ensureStorage(int physicalWidth,
                                           int physicalHeight)
{
    if (physicalWidth <= 0 || physicalHeight <= 0)
        return false;
    if (!m_initialized) {
        if (!initializeOpenGLFunctions())
            return false;
        m_initialized = true;
    }
    if (m_fbo && m_texture
        && m_width == physicalWidth && m_height == physicalHeight) {
        return true;
    }

    release();
    m_initialized = true;

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 physicalWidth, physicalHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        qWarning("[CanvasStaticLayerCache] incomplete FBO: 0x%x", status);
        release();
        return false;
    }

    m_width = physicalWidth;
    m_height = physicalHeight;
    return true;
}

bool CanvasStaticLayerCache::captureCurrentFramebuffer(
    int physicalWidth, int physicalHeight, int staticItemCount)
{
    if (!m_initialized) {
        if (!initializeOpenGLFunctions())
            return false;
        m_initialized = true;
    }
    GLint previousReadFbo = 0;
    GLint previousDrawFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFbo);

    if (!ensureStorage(physicalWidth, physicalHeight)) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          static_cast<GLuint>(previousReadFbo));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                          static_cast<GLuint>(previousDrawFbo));
        return false;
    }

    // QOpenGLWidget may use MSAA. Blitting into this single-sample FBO also
    // resolves it, preserving the exact premultiplied pixels drawn so far.
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
    glBlitFramebuffer(0, 0, physicalWidth, physicalHeight,
                      0, 0, physicalWidth, physicalHeight,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFbo));

    m_staticItemCount = staticItemCount;
    m_valid = true;
    return true;
}

bool CanvasStaticLayerCache::canReuse(int physicalWidth,
                                      int physicalHeight,
                                      int staticItemCount) const
{
    return m_valid && m_texture
        && m_width == physicalWidth
        && m_height == physicalHeight
        && m_staticItemCount == staticItemCount;
}

void CanvasStaticLayerCache::draw(Renderer &renderer,
                                  int logicalWidth,
                                  int logicalHeight) const
{
    if (!m_valid || !m_texture)
        return;
    renderer.drawPremultipliedTexture(
        m_texture, QRectF(0, 0, logicalWidth, logicalHeight), 1.0f);
}

void CanvasStaticLayerCache::invalidate()
{
    m_valid = false;
    m_staticItemCount = -1;
}

void CanvasStaticLayerCache::release()
{
    if (m_fbo)
        glDeleteFramebuffers(1, &m_fbo);
    if (m_texture)
        glDeleteTextures(1, &m_texture);
    m_fbo = 0;
    m_texture = 0;
    m_width = 0;
    m_height = 0;
    invalidate();
}
