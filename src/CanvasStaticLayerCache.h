#pragma once

#include <QOpenGLFunctions_3_3_Core>

class Renderer;

// Retains the already-composited static prefix of the canvas in one
// premultiplied RGBA texture. It is reused only when CanvasWidget proves that
// every animated video is above that prefix; all other arrangements fall back
// to the original full draw path.
class CanvasStaticLayerCache : protected QOpenGLFunctions_3_3_Core
{
public:
    bool captureCurrentFramebuffer(int physicalWidth,
                                   int physicalHeight,
                                   int staticItemCount);
    bool canReuse(int physicalWidth,
                  int physicalHeight,
                  int staticItemCount) const;
    void draw(Renderer &renderer, int logicalWidth, int logicalHeight) const;
    void invalidate();
    void release();
    // 2026-09-16: 保持している絵のテクスチャ（GL座標＝下原点、物理px）。
    // 有効な絵が無ければ0。BackdropBlur がぼかしの材料として直接読む。
    GLuint validTexture() const { return m_valid ? m_texture : 0; }

private:
    bool ensureStorage(int physicalWidth, int physicalHeight);

    GLuint m_fbo {0};
    GLuint m_texture {0};
    int m_width {0};
    int m_height {0};
    int m_staticItemCount {-1};
    bool m_initialized {false};
    bool m_valid {false};
};
