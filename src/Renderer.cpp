#include "Renderer.h"
#include "GpuProfiler.h"
#include <QPainter>
#include <QFontMetrics>
#include <QDebug>
#include <cmath>

static const char *TEX_VERT = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uProjection;
out vec2 vUV;
out vec2 vPos;
void main() {
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
    vUV = aUV;
    vPos = aPos;
}
)";

static const char *TEX_FRAG = R"(
#version 330 core
in vec2 vUV;
in vec2 vPos;
out vec4 fragColor;
uniform sampler2D uTex;
uniform float uAlpha;
uniform vec2 uCenter;
uniform vec2 uHalfSize;
uniform float uRadius;
uniform bool uPremultipliedInput;
// 2026-09-23: 融合中の「溶け」（改訂版）。
// 【旧版がきたなかった理由】重なりの中で上下どちらの中身も最大55%薄くしていた
// ため、2枚とも半透明になって下の暗い板が透け、濁った灰色になっていた。
// ぼかしも「動画テクスチャのtexel」単位の疎な13点だったので、ぼけずに
// 二重像（ゴースト）になっていた。
// 【新版】
//  - 重なりの各点で「上の中身に何px入っているか」「下の中身に何px入っているか」を
//    比べ、深く入っている側の中身が勝つ。中身の縁では自分の深さ=0なので、
//    その中身は必ず0になる＝どちらの縁（直線）も見えない。2つの深さの比でなめらかに切り替える
//    ので、境界線が消えて一方からもう一方へグラデーションで溶ける。
//  - 薄くするのは上（後から描く側）だけ。下は不透明のまま＝合計の濃さは常に1で、
//    板が透けて濁ることがない（上のアルファ＝上の重み、の単純なクロスフェード）。
//  - ぼかしは溶け目（両者が半々の所）で最大、画面px単位の16点ディスク
//    （点ごとに回転をずらすので輪状のゴーストが出ない）。上下とも同じ量だけぼかす。
// 融合していないときは uMeltAmount=0 で、追加の読み取りは一切無い。
uniform vec2  uMeltSelfCenter;
uniform vec2  uMeltSelfHalf;
uniform vec2  uMeltOtherCenter;
uniform vec2  uMeltOtherHalf;
uniform float uMeltAmount;
uniform float uMeltBlurPx;
uniform bool  uMeltIsTop;      // true: この中身は相手の上に描かれる（薄くなる側）
// 2026-09-23: 停止中の暗さ（setContentDim）。0=そのまま。上から黒を重ねる代わりに
// 中身の色そのものを暗くするので、溶けや角丸と必ず同じ形になる。
uniform float uDim;
// 矩形の「中へ何px入っているか」をなめらかに測る。距離関数(SDF)の内側は
// 対角線上に折れ目（稜線）があり、それで比をとると溶け目が四角い箱＋X字の
// すじになる。縦横それぞれの縁までの距離を調和平均ぎみに合わせて折れ目を消す
// （縁の近くでは短い方の距離とほぼ同じ、外では0）。
float meltDepth(vec2 p, vec2 c, vec2 h) {
    vec2 d = max(h - abs(p - c), vec2(0.0));
    return d.x * d.y / max(length(d), 1e-4);
}
vec4 meltBlur(vec2 uv, vec2 radiusUv) {
    // 16点の Vogel ディスク。画素ごとに回転をずらし、規則的な二重像を出さない。
    float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float a0 = ign * 6.2831853;
    vec4 acc = texture(uTex, uv);
    float wsum = 1.0;
    for (int i = 0; i < 16; ++i) {
        float fi = float(i) + 0.5;
        float rr = sqrt(fi / 16.0);
        float a = a0 + fi * 2.39996323;
        float w = 1.0 - 0.5 * rr;          // 外ほど少し軽く（ガウスっぽい裾）
        acc += texture(uTex, uv + vec2(cos(a), sin(a)) * rr * radiusUv) * w;
        wsum += w;
    }
    return acc / wsum;
}
void main() {
    // 画面1pxあたりのUV（ぼかし半径を画面pxで決めるため）。分岐の外で求める。
    vec2 dUVx = dFdx(vUV), dUVy = dFdy(vUV);
    vec2 dPx = dFdx(vPos), dPy = dFdy(vPos);
    vec2 uvPerPx = vec2(length(dUVx) / max(length(dPx), 1e-4),
                        length(dUVy) / max(length(dPy), 1e-4));
    float topW = 1.0;     // 上の中身の重み（=上のアルファ）
    float blurAmt = 0.0;
    if (uMeltAmount > 0.0) {
        float inSelf  = meltDepth(vPos, uMeltSelfCenter, uMeltSelfHalf);
        float inOther = meltDepth(vPos, uMeltOtherCenter, uMeltOtherHalf);
        if (inSelf > 0.0 && inOther > 0.0) {
            float depthTop = uMeltIsTop ? inSelf : inOther;
            float depthBot = uMeltIsTop ? inOther : inSelf;
            float r = depthTop / (depthTop + depthBot);   // 0=下の奥, 1=上の奥
            float w = r * r * (3.0 - 2.0 * r);
            topW = mix(1.0, w, uMeltAmount);
            // ぼかしは溶け目（半々）で最大。どちらかの縁の近く（40px以内）では
            // 弱めて、ぼけた四角い箱の輪郭が出ないようにする。
            float s = 1.0 - abs(2.0 * topW - 1.0);
            blurAmt = uMeltAmount * s * s * clamp(min(depthTop, depthBot) / 40.0, 0.0, 1.0);
        }
    }
    vec4 c = blurAmt > 0.02
        ? meltBlur(vUV, uvPerPx * (uMeltBlurPx * blurAmt))
        : texture(uTex, vUV);
    float mask = 1.0;
    if (uRadius > 0.0) {
        vec2 q = abs(vPos - uCenter) - uHalfSize + vec2(uRadius);
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius;
        float aa = max(fwidth(d), 0.0001);
        mask = 1.0 - smoothstep(-aa, aa, d);
    }
    // 上の中身だけ重みで薄くする（下は不透明のまま → 2枚のクロスフェード）
    float meltFade = uMeltIsTop ? topW : 1.0;
    float a = c.a * uAlpha * mask * meltFade;
    vec3 rgb = uPremultipliedInput
        ? c.rgb * uAlpha * mask * meltFade
        : c.rgb * a;
    rgb *= 1.0 - uDim;
    fragColor = vec4(rgb, a); // premultiplied alphaで出力（透過ウィンドウ合成向け）
}
)";

static const char *COL_VERT = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform mat4 uProjection;
void main() {
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
}
)";

static const char *COL_FRAG = R"(
#version 330 core
out vec4 fragColor;
uniform vec4 uColor;
void main() {
    fragColor = vec4(uColor.rgb * uColor.a, uColor.a); // premultiplied alpha
}
)";

// 頂点カラーシェーダー（グラデーション用）
static const char *VCOL_VERT = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uProjection;
out vec4 vColor;
void main() {
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
    vColor = aColor;
}
)";

static const char *VCOL_FRAG = R"(
#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor.rgb * vColor.a, vColor.a); // premultiplied alpha
}
)";

// 角丸矩形SDFグローシェーダー（drawGlowRect用）。
// 頂点はscreen座標のままvPosへ流し、フラグメント側で角丸矩形までの
// 符号付き距離dを計算する（d<0: 内側、d=0: 縁、d>0: 外側）。
// 縁で最大・外へuGlowWかけて二次減衰、内側へは2pxで消えるようにして
// 本体の枠線ストロークと自然に重なるようにする。
static const char *GLOW_VERT = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform mat4 uProjection;
out vec2 vPos;
void main() {
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
    vPos = aPos;
}
)";

static const char *GLOW_FRAG = R"(
#version 330 core
in vec2 vPos;
out vec4 fragColor;
uniform vec2  uCenter;    // 矩形中心（screen座標）
uniform vec2  uHalfSize;  // 矩形の半サイズ
uniform float uRadius;    // 角丸半径
uniform float uGlowW;     // グロー幅（縁から外側へ何pxで0になるか）
uniform vec4  uColor;     // グロー色（aは縁での最大アルファ）

// 2026-09-22: バブル融合（Renderer::ShapeFusion）。uFuseK=0 なら d をそのまま返す。
uniform float uFuseK;
uniform vec2  uFuseCenter;
uniform vec2  uFuseHalfSize;
uniform float uFuseRadius;
float fuseSdf(float dSelf, vec2 p)
{
    if (uFuseK <= 0.0)
        return dSelf;
    vec2 qo = abs(p - uFuseCenter) - uFuseHalfSize + vec2(uFuseRadius);
    float dOther = length(max(qo, 0.0)) + min(max(qo.x, qo.y), 0.0) - uFuseRadius;
    float h = clamp(0.5 + 0.5 * (dOther - dSelf) / uFuseK, 0.0, 1.0);
    return mix(dOther, dSelf, h) - uFuseK * h * (1.0 - h);
}
void main() {
    vec2 q = abs(vPos - uCenter) - uHalfSize + vec2(uRadius);
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius;
    d = fuseSdf(d, vPos);
    float outer = 1.0 - clamp(d / uGlowW, 0.0, 1.0); // 縁=1 → 外側glowW=0
    outer *= outer;                                   // 二次減衰で自然な光に
    float inner = smoothstep(-2.0, 0.0, d);          // 内側はすぐ消す
    float a = uColor.a * outer * inner;
    fragColor = vec4(uColor.rgb * a, a); // premultiplied alpha
}
)";

// 角丸矩形SDFストロークシェーダー（drawRoundedRectStroke用）。
// GLOW_FRAGと同じ符号付き距離dを使い、d=0（縁）から内側へthickness分だけの
// 帯にアルファ1を立てる。矩形4辺を個別ジオメトリで塗る方式と違い単一の
// シェーダー計算で角丸の縁全周を一括処理するため、コーナー部分が二重に
// 重なって濃く見えることがない。
//
// 2026-07-16: 帯の外側の縁（ウィンドウ端）はクッキリ、内側の縁（本体側）は
// uFeatherピクセルかけてグラデーションでフェードするよう分離した
// （元は両方とも1pxのAA程度で固定していた）。外側をクッキリのままにするのは
// ウィンドウ端で色がにじんで見えるのを避けるため。
static const char *STROKE_FRAG = R"(
#version 330 core
in vec2 vPos;
out vec4 fragColor;
uniform vec2  uCenter;
uniform vec2  uHalfSize;
uniform float uRadius;
uniform float uThickness;
uniform float uFeather;   // 内側の縁のぼかし幅（px）。0でクッキリ。
uniform vec4  uColor;
void main() {
    vec2 q = abs(vPos - uCenter) - uHalfSize + vec2(uRadius);
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius;
    float t = -d; // 縁の内側への距離（外側は負）
    float outerEdge = smoothstep(-1.0, 0.0, t);
    float feather = max(uFeather, 1.0);
    float innerEdge = 1.0 - smoothstep(uThickness - feather, uThickness, t);
    float a = uColor.a * outerEdge * innerEdge;
    fragColor = vec4(uColor.rgb * a, a); // premultiplied alpha
}
)";

// ドットグリッドシェーダー（drawDotGrid用）。
// GLOW_VERTと同じくvPosに screen座標をそのまま流し、フラグメント側で
// 「このピクセルに対応するキャンバス座標」を逆算してから最寄りの格子点
// までの距離を計算する。画面全体を覆う四角形1枚だけの単一ドローコールで
// 済むため、格子点の数（＝ズームアウト量）に描画コストが左右されない。
static const char *DOTGRID_FRAG = R"(
#version 330 core
in vec2 vPos;
out vec4 fragColor;
uniform vec2  uScreenCenter; // width/2, height/2
uniform vec2  uCameraPos;    // CanvasWidget::m_cameraPos（キャンバス座標）
uniform float uZoom;         // CanvasWidget::m_cameraZoom
uniform float uSpacing;      // グリッド間隔（キャンバス座標系）
uniform float uRadiusPx;     // ドット半径（画面px、ズームによらず一定）
uniform vec4  uColor;
void main() {
    vec2 canvasPos = (vPos - uScreenCenter) / uZoom + uCameraPos;
    vec2 local = mod(canvasPos, uSpacing) - uSpacing * 0.5;
    float distPx = length(local) * uZoom;

    float aa = 1.0;
    float shape = 1.0 - smoothstep(uRadiusPx - aa, uRadiusPx + aa, distPx);

    // 画面上の格子間隔が数pxまで潰れる領域はモアレ（ちらつき）が出るため、
    // 離散的に間引くのではなく連続的にフェードアウトさせて自然に消す。
    float screenSpacing = uSpacing * uZoom;
    float fade = smoothstep(3.0, 10.0, screenSpacing);

    float a = uColor.a * shape * fade;
    fragColor = vec4(uColor.rgb * a, a); // premultiplied alpha
}
)";


// 2026-09-16: 塗りの図形（角丸矩形・その内側の帯・線分・三角形・円）を、
// 頂点で形を作るのではなく「覆う四角形1枚＋フラグメントでの距離計算」で描く。
// 以前はポリゴン（角丸は1隅8分割）を描いて、縁のギザギザをハードウェアMSAA
// (main.cppのsetSamples(2))で消していた。MSAAは画面全体を2倍の量で持ち、
// 毎フレームまとめ直すため、4Kで効いてくる。ほかの描画（グロー・ストローク・
// ガラス・テクスチャの角丸）は既にシェーダー側で滑らかにしてあるので、
// これで全部がMSAA無しで成立する。
static const char *SHAPE_FRAG = R"(
#version 330 core
in vec2 vPos;
out vec4 fragColor;
uniform int   uMode;
uniform vec2  uCenter;
uniform vec2  uHalfSize;
uniform float uRadius;
uniform float uStroke;     // mode1: 帯の中心線の内側への距離
uniform float uHalfWidth;  // mode1/2: 線の太さの半分（論理px）
uniform vec2  uA;
uniform vec2  uB;
uniform vec2  uC;
uniform vec4  uColor;

float sdRoundRect(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + vec2(r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// 2026-09-22: バブル融合（Renderer::ShapeFusion）。uFuseK=0 なら d をそのまま返す。
uniform float uFuseK;
uniform vec2  uFuseCenter;
uniform vec2  uFuseHalfSize;
uniform float uFuseRadius;
float fuseSdf(float dSelf, vec2 p)
{
    if (uFuseK <= 0.0)
        return dSelf;
    vec2 qo = abs(p - uFuseCenter) - uFuseHalfSize + vec2(uFuseRadius);
    float dOther = length(max(qo, 0.0)) + min(max(qo.x, qo.y), 0.0) - uFuseRadius;
    float h = clamp(0.5 + 0.5 * (dOther - dSelf) / uFuseK, 0.0, 1.0);
    return mix(dOther, dSelf, h) - uFuseK * h * (1.0 - h);
}

// 端を切り落とした太さ2*hwの線分（GL_LINESの見た目と同じ）
float sdSegmentBox(vec2 p, vec2 a, vec2 b, float hw)
{
    vec2 d = b - a;
    float len = length(d);
    if (len < 1e-4)
        return 1e3;
    vec2 u = d / len;
    vec2 n = vec2(-u.y, u.x);
    vec2 q = p - (a + b) * 0.5;
    return sdRoundRect(vec2(dot(q, u), dot(q, n)), vec2(len * 0.5, hw), 0.0);
}

// 三角形の符号付き距離（内側が負）
float sdTriangle(vec2 p, vec2 p0, vec2 p1, vec2 p2)
{
    vec2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    vec2 v0 = p - p0,  v1 = p - p1,  v2 = p - p2;
    vec2 pq0 = v0 - e0 * clamp(dot(v0, e0) / max(dot(e0, e0), 1e-6), 0.0, 1.0);
    vec2 pq1 = v1 - e1 * clamp(dot(v1, e1) / max(dot(e1, e1), 1e-6), 0.0, 1.0);
    vec2 pq2 = v2 - e2 * clamp(dot(v2, e2) / max(dot(e2, e2), 1e-6), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    vec2 d = min(min(vec2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)),
                     vec2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
                     vec2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

void main() {
    float d;
    if (uMode == 0) {
        d = sdRoundRect(vPos - uCenter, uHalfSize, uRadius);
        d = fuseSdf(d, vPos);
    } else if (uMode == 1) {
        float o = sdRoundRect(vPos - uCenter, uHalfSize, uRadius);
        o = fuseSdf(o, vPos);
        d = abs(o + uStroke) - uHalfWidth;
    } else if (uMode == 2) {
        d = sdSegmentBox(vPos, uA, uB, uHalfWidth);
    } else if (uMode == 3) {
        d = sdTriangle(vPos, uA, uB, uC);
    } else {
        d = length(vPos - uCenter) - uRadius;
    }
    // 1画素ぶんの幅で縁をぼかす（fwidthで画面上の画素幅に追従）
    float aa = max(fwidth(d), 1e-4);
    float cov = clamp(0.5 - d / aa, 0.0, 1.0);
    if (cov <= 0.0)
        discard;
    float a = uColor.a * cov;
    fragColor = vec4(uColor.rgb * a, a); // premultiplied alpha
}
)";

Renderer::~Renderer()
{
    if (!m_initialized) return;
    if (m_texVAO)  glDeleteVertexArrays(1, &m_texVAO);
    if (m_texVBO)  glDeleteBuffers(1, &m_texVBO);
    if (m_colVAO)  glDeleteVertexArrays(1, &m_colVAO);
    if (m_colVBO)  glDeleteBuffers(1, &m_colVBO);
    if (m_vcolVAO) glDeleteVertexArrays(1, &m_vcolVAO);
    if (m_vcolVBO) glDeleteBuffers(1, &m_vcolVBO);
    for (auto &e : m_textCache) glDeleteTextures(1, &e.texId);
    delete m_texShader;
    delete m_colShader;
    delete m_vcolShader;
    delete m_glowShader;
    delete m_strokeShader;
    delete m_dotGridShader;
    delete m_shapeShader;
}

bool Renderer::initialize()
{
    if (!initializeOpenGLFunctions()) {
        qWarning("[Renderer] OpenGL 3.3 Core init failed");
        return false;
    }
    initShaders();
    initBuffers();
    glEnable(GL_BLEND);
    // 全シェーダーがpremultiplied alpha（rgb*aを出力）に統一されているため、
    // ブレンド式はRGB/アルファともGL_ONE, GL_ONE_MINUS_SRC_ALPHAで正しく合成できる。
    // straight alphaのままGL_SRC_ALPHAで混ぜると、半透明ピクセルのRGBが
    // 透過ウィンドウ合成(DWM/DirectComposition)側の premultiplied 前提とズレて
    // 明るさがちらつく／背景が薄く透けるといった不具合が出るため統一する。
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    m_initialized = true;
    qDebug("[Renderer] initialized");
    return true;
}

void Renderer::initShaders()
{
    m_texShader = new QOpenGLShaderProgram();
    m_texShader->addShaderFromSourceCode(QOpenGLShader::Vertex,   TEX_VERT);
    m_texShader->addShaderFromSourceCode(QOpenGLShader::Fragment, TEX_FRAG);
    m_texShader->link();

    m_colShader = new QOpenGLShaderProgram();
    m_colShader->addShaderFromSourceCode(QOpenGLShader::Vertex,   COL_VERT);
    m_colShader->addShaderFromSourceCode(QOpenGLShader::Fragment, COL_FRAG);
    m_colShader->link();

    m_vcolShader = new QOpenGLShaderProgram();
    m_vcolShader->addShaderFromSourceCode(QOpenGLShader::Vertex,   VCOL_VERT);
    m_vcolShader->addShaderFromSourceCode(QOpenGLShader::Fragment, VCOL_FRAG);
    m_vcolShader->link();

    m_glowShader = new QOpenGLShaderProgram();
    m_glowShader->addShaderFromSourceCode(QOpenGLShader::Vertex,   GLOW_VERT);
    m_glowShader->addShaderFromSourceCode(QOpenGLShader::Fragment, GLOW_FRAG);
    m_glowShader->link();

    m_strokeShader = new QOpenGLShaderProgram();
    m_strokeShader->addShaderFromSourceCode(QOpenGLShader::Vertex,   GLOW_VERT);
    m_strokeShader->addShaderFromSourceCode(QOpenGLShader::Fragment, STROKE_FRAG);
    m_strokeShader->link();

    m_dotGridShader = new QOpenGLShaderProgram();
    m_dotGridShader->addShaderFromSourceCode(QOpenGLShader::Vertex,   GLOW_VERT);
    m_dotGridShader->addShaderFromSourceCode(QOpenGLShader::Fragment, DOTGRID_FRAG);
    m_dotGridShader->link();

    m_shapeShader = new QOpenGLShaderProgram();
    m_shapeShader->addShaderFromSourceCode(QOpenGLShader::Vertex,   GLOW_VERT);
    m_shapeShader->addShaderFromSourceCode(QOpenGLShader::Fragment, SHAPE_FRAG);
    if (!m_shapeShader->link())
        qWarning("[Renderer] shape shader: %s", qPrintable(m_shapeShader->log()));
}

float Renderer::physicalLineHalfWidth(float widthPx) const
{
    // 2026-09-16 修正: 線は「物理1ピクセル」の太さに固定する（widthPx は無視）。
    //
    // 旧実装は glLineWidth(widthPx) で線を描いていたが、このアプリのコンテキストは
    // Core Profile（前方互換）で、そこでは1より太い glLineWidth は仕様上
    // GL_INVALID_VALUE になり、太さは1pxのまま変わらない。つまり呼び出し側が
    // 4.0 や 1.5 を渡していても、実際の見た目はずっと物理1pxだった。
    // SDF化の直後にその値をそのまま太さとして使ったため、アクティブ枠
    // （accent, 4px）などが急に太く見える不具合になった（バブル外周の青い線）。
    //
    // ついでに: このとき残っていた GL_INVALID_VALUE が、mpv のログに毎回出ていた
    // "after creating texture: OpenGL error INVALID_VALUE" の出所だったと思われる
    // （mpv はテクスチャ生成後に glGetError() を読むだけなので、こちらの残した
    //  エラーを拾っていた）。glLineWidth を呼ばなくなったので出なくなるはず。
    Q_UNUSED(widthPx);
    const float dpr = (m_vpW > 0 && m_physW > 0)
        ? static_cast<float>(m_physW) / static_cast<float>(m_vpW) : 1.0f;
    return 0.5f / qMax(0.01f, dpr);
}

// 2026-09-22: バブル融合（Renderer.h の ShapeFusion 参照）。
const Renderer::ShapeFusion *Renderer::fusionFor(const QRectF &rect) const
{
    if (!m_fusion.isValid())
        return nullptr;
    // 板の矩形は描画側で毎回同じ式から作られるので、ほぼ完全に一致する。
    // 浮動小数の丸めぶんだけ許す。
    constexpr double kTol = 0.75;
    const QRectF &a = m_fusion.selfRect;
    if (qAbs(a.left() - rect.left()) > kTol || qAbs(a.top() - rect.top()) > kTol
        || qAbs(a.right() - rect.right()) > kTol || qAbs(a.bottom() - rect.bottom()) > kTol)
        return nullptr;
    return &m_fusion;
}

void Renderer::applyFusionUniforms(QOpenGLShaderProgram *p, const ShapeFusion *fusion)
{
    if (!fusion) {
        p->setUniformValue(m_uniforms.get(p, "uFuseK"), 0.0f);
        return;
    }
    const QRectF &o = fusion->otherRect;
    const float ohw = qMax(0.0f, (float)o.width()  * 0.5f);
    const float ohh = qMax(0.0f, (float)o.height() * 0.5f);
    p->setUniformValue(m_uniforms.get(p, "uFuseK"), fusion->k);
    p->setUniformValue(m_uniforms.get(p, "uFuseCenter"),
        QVector2D((float)o.center().x(), (float)o.center().y()));
    p->setUniformValue(m_uniforms.get(p, "uFuseHalfSize"), QVector2D(ohw, ohh));
    p->setUniformValue(m_uniforms.get(p, "uFuseRadius"),
        qMin(qMax(0.0f, fusion->otherRadius), qMin(ohw, ohh)));
}

void Renderer::drawShapeQuad(int mode, const QRectF &bounds, const QColor &color,
                             const std::function<void(QOpenGLShaderProgram *)> &setup)
{
    if (color.alpha() <= 0 || bounds.width() <= 0.0 || bounds.height() <= 0.0)
        return;
    // アンチエイリアスの1画素ぶんがはみ出しても欠けないよう、少し広く覆う。
    const QRectF q = bounds.adjusted(-2.0, -2.0, 2.0, 2.0);
    const float verts[8] = {
        (float)q.left(),  (float)q.top(),
        (float)q.right(), (float)q.top(),
        (float)q.right(), (float)q.bottom(),
        (float)q.left(),  (float)q.bottom(),
    };
    glBindVertexArray(m_colVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_colVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    m_shapeShader->bind();
    m_shapeShader->setUniformValue(m_uniforms.get(m_shapeShader, "uProjection"), m_projection);
    m_shapeShader->setUniformValue(m_uniforms.get(m_shapeShader, "uMode"), mode);
    m_shapeShader->setUniformValue(m_uniforms.get(m_shapeShader, "uColor"),
        QVector4D(color.redF(), color.greenF(), color.blueF(),
                  color.alphaF() * m_globalAlpha));
    // 融合は板の塗り/枠（drawRect）だけが setup の中で有効にする。
    // それ以外の図形では必ず無効に戻す（uniform は前回の値が残るため）。
    applyFusionUniforms(m_shapeShader, nullptr);
    setup(m_shapeShader);
    GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    m_shapeShader->release();
    glBindVertexArray(0);
}

void Renderer::initBuffers()
{
    glGenVertexArrays(1, &m_texVAO);
    glGenBuffers(1, &m_texVBO);
    glBindVertexArray(m_texVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_texVBO);
    glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glBindVertexArray(0);

    glGenVertexArrays(1, &m_colVAO);
    glGenBuffers(1, &m_colVBO);
    glBindVertexArray(m_colVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_colVBO);
    glBufferData(GL_ARRAY_BUFFER, 512 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    glBindVertexArray(0);

    // 頂点カラー用 VAO/VBO（stride: xy + rgba = 6 floats）
    glGenVertexArrays(1, &m_vcolVAO);
    glGenBuffers(1, &m_vcolVBO);
    glBindVertexArray(m_vcolVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_vcolVBO);
    glBufferData(GL_ARRAY_BUFFER, 256 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); // pos
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1); // color
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(2*sizeof(float)));
    glBindVertexArray(0);
}

void Renderer::updateProjection(int w, int h)
{
    if (w == m_vpW && h == m_vpH) return;
    m_vpW = w; m_vpH = h;
    m_projection.setToIdentity();
    m_projection.ortho(0, w, h, 0, -1, 1);
}

void Renderer::begin(int physViewportW, int physViewportH)
{
    m_physW = physViewportW;
    m_physH = physViewportH;
    glViewport(0, 0, physViewportW, physViewportH);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    // depthバッファは確保していない（2D合成のみでGL_DEPTH_TESTを使わないため
    // main.cppでdepth/stencilバッファのリクエスト自体を削除済み）。
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::setLogicalSize(int logicalW, int logicalH)
{
    updateProjection(logicalW, logicalH);
}

void Renderer::pushOffscreen(const QRectF &logicalRect, int physW, int physH)
{
    m_offscreenSave.projection = m_projection;
    m_offscreenSave.vpW = m_vpW;
    m_offscreenSave.vpH = m_vpH;
    m_offscreenSave.physW = m_physW;
    m_offscreenSave.physH = m_physH;
    m_offscreenSave.globalAlpha = m_globalAlpha;
    m_offscreenSave.active = true;
    // 画面座標のまま描けるよう、logicalRect をFBO全体へ写す投影にする。
    // m_vpW / m_physW の比が線の太さ（物理1px）の基準なので、同じ比になるよう揃える。
    m_projection.setToIdentity();
    m_projection.ortho(float(logicalRect.left()), float(logicalRect.right()),
                       float(logicalRect.bottom()), float(logicalRect.top()), -1, 1);
    m_vpW = qMax(1, qRound(logicalRect.width()));
    m_vpH = qMax(1, qRound(logicalRect.height()));
    m_physW = qMax(1, qRound(double(physW) * m_vpW / qMax(1e-6, logicalRect.width())));
    m_physH = qMax(1, qRound(double(physH) * m_vpH / qMax(1e-6, logicalRect.height())));
    m_globalAlpha = 1.0f;
    glViewport(0, 0, physW, physH);
}

void Renderer::popOffscreen()
{
    if (!m_offscreenSave.active)
        return;
    m_projection = m_offscreenSave.projection;
    m_vpW = m_offscreenSave.vpW;
    m_vpH = m_offscreenSave.vpH;
    m_physW = m_offscreenSave.physW;
    m_physH = m_offscreenSave.physH;
    m_globalAlpha = m_offscreenSave.globalAlpha;
    m_offscreenSave.active = false;
    glViewport(0, 0, m_physW, m_physH);
}

void Renderer::end()
{
    glBindVertexArray(0);
    ++m_frameCounter;
    pruneTextCache();
}

void Renderer::pruneTextCache()
{
    // 直近 kTextCacheEvictAfterFrames フレームの間、一度も drawText() で
    // 参照されなかったエントリを削除する。再生時間表示などの毎秒変化する
    // 文字列がキャッシュを無制限に肥大化させるのを防ぐ。
    for (auto it = m_textCache.begin(); it != m_textCache.end(); ) {
        if (m_frameCounter - it->lastUsedFrame > kTextCacheEvictAfterFrames) {
            glDeleteTextures(1, &it->texId);
            it = m_textCache.erase(it);
        } else {
            ++it;
        }
    }
}

// screen座標（左上原点・論理ピクセル）の矩形でこれ以降の描画をクリップする。
// popClip()または次のsetClipRect()まで有効。ネストは非対応（単純なon/offのみ）。
void Renderer::setClipRect(const QRectF &screenRect)
{
    if (m_vpW <= 0 || m_vpH <= 0 || m_physW <= 0 || m_physH <= 0) return;

    QRectF clamped = screenRect.intersected(QRectF(0, 0, m_vpW, m_vpH));
    if (clamped.width() <= 0 || clamped.height() <= 0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 0, 0);
        return;
    }

    const double sx = double(m_physW) / double(m_vpW);
    const double sy = double(m_physH) / double(m_vpH);

    // glScissorは左下原点なのでYを反転する
    int x = qMax(0, (int)std::lround(clamped.left() * sx));
    int y = qMax(0, (int)std::lround((m_vpH - clamped.bottom()) * sy));
    int w = (int)std::lround(clamped.width()  * sx);
    int h = (int)std::lround(clamped.height() * sy);

    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
}

void Renderer::clearClip()
{
    glDisable(GL_SCISSOR_TEST);
}

void Renderer::beginInvertBlend()
{
    // dst' = src.rgb × (1 − dst.rgb) + dst.rgb × (1 − src.a)
    //
    // 2026-07-16 修正: 当初GL_ZEROを使っていたが、これだと描画したクワッドの
    // 範囲全部でdstの寄与が常に0倍されてしまい、シェーダー側でalpha=0に
    // マスクしていたはず（stroke帯の外側＝バブル本体部分など）まで含めて
    // 黒く消えてしまっていた（GL_ZEROはsrc.aの値を一切見ないため）。
    // dstFactorをsrc.aに連動する(1-src.a)にすることで、マスクでalpha=0の
    // ピクセルは dst' = 0 + dst×1 = dst のまま保たれ、alpha=1のピクセルだけ
    // 完全に反転されるようになる。
    glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::endInvertBlend()
{
    // initialize()で設定した通常のpremultiplied alpha合成に戻す。
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::beginMultiplyBlend()
{
    // 2026-10-12: 乗算。src は premultiplied（色×α）なので
    //   dst' = src.rgb × dst.rgb + dst.rgb × (1 − src.a) = dst.rgb × (1 − α + 色×α)
    // α=0 の画素は dst のまま、α=1 の画素は dst × 色 になる。アルファ側も
    // dst.a × src.a + dst.a × (1 − src.a) = dst.a で、透過ウィンドウの α を変えない。
    glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::endMultiplyBlend()
{
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::drawTexture(GLuint texId, const QRectF &r, float alpha)
{
    drawTextureInternal(texId, r, QRectF(0.0, 0.0, 1.0, 1.0),
                        alpha, 0.0f);
}

void Renderer::drawPremultipliedTexture(GLuint texId, const QRectF &r,
                                        float alpha)
{
    drawTextureInternal(texId, r, QRectF(0.0, 1.0, 1.0, -1.0),
                        alpha, 0.0f, true);
}

// srcUv版: テクスチャのどの範囲を使うかをUV座標(0..1)で指定する。
// 2026-07-11: 16:9でないメディアのサムネイル（動画サイトのサムネは動画本編の
// 縦横比に関係なく16:9で届く等）をバブル矩形へそのまま引き伸ばすと歪むため、
// アスペクト比を保った中央クロップ（cover）描画に使う。
void Renderer::drawTexture(GLuint texId, const QRectF &r, const QRectF &srcUv, float alpha)
{
    drawTextureInternal(texId, r, srcUv, alpha, 0.0f);
}

void Renderer::drawRoundedTexture(GLuint texId, const QRectF &r,
                                  float radius, float alpha)
{
    drawTextureInternal(texId, r, QRectF(0.0, 0.0, 1.0, 1.0),
                        alpha, radius);
}

void Renderer::drawRoundedTexture(GLuint texId, const QRectF &r,
                                  const QRectF &srcUv, float radius, float alpha)
{
    drawTextureInternal(texId, r, srcUv, alpha, radius);
}

void Renderer::drawTextureInternal(GLuint texId, const QRectF &r,
                                   const QRectF &srcUv, float alpha, float radius,
                                   bool premultipliedInput)
{
    float x0=r.left(), y0=r.top(), x1=r.right(), y1=r.bottom();
    float u0=(float)srcUv.left(),  v0=(float)srcUv.top();
    float u1=(float)srcUv.right(), v1=(float)srcUv.bottom();
    float verts[16] = { x0,y0,u0,v0, x1,y0,u1,v0, x1,y1,u1,v1, x0,y1,u0,v1 };
    glBindVertexArray(m_texVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_texVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    m_texShader->bind();
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uProjection"), m_projection);
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uTex"), 0);
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uAlpha"), alpha * m_globalAlpha);
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uPremultipliedInput"), premultipliedInput);
    const float halfW = qMax(0.0f, static_cast<float>(r.width()) * 0.5f);
    const float halfH = qMax(0.0f, static_cast<float>(r.height()) * 0.5f);
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uCenter"),
        QVector2D(static_cast<float>(r.center().x()),
                  static_cast<float>(r.center().y())));
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uHalfSize"), QVector2D(halfW, halfH));
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uRadius"), qMin(qMax(0.0f, radius), qMin(halfW, halfH)));
    // 融合中の「溶け」（setContentMelt）。無効なら amount=0 で分岐ごと通らない。
    const bool meltOn = m_melt.isValid();
    auto vec2Of = [](const QPointF &p) {
        return QVector2D(static_cast<float>(p.x()), static_cast<float>(p.y()));
    };
    auto halfOf = [](const QRectF &rc) {
        return QVector2D(static_cast<float>(rc.width() * 0.5),
                         static_cast<float>(rc.height() * 0.5));
    };
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uMeltSelfCenter"),
        vec2Of(m_melt.selfRect.center()));
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uMeltSelfHalf"),
        halfOf(m_melt.selfRect));
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uMeltOtherCenter"),
        vec2Of(m_melt.otherRect.center()));
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uMeltOtherHalf"),
        halfOf(m_melt.otherRect));
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uMeltAmount"),
        meltOn ? m_melt.amount : 0.0f);
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uMeltBlurPx"),
        m_melt.blurPx);
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uMeltIsTop"),
        meltOn && m_melt.selfIsTop);
    m_texShader->setUniformValue(m_uniforms.get(m_texShader, "uDim"),
        qBound(0.0f, m_contentDim, 1.0f));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texId);
    GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_texShader->release();
    glBindVertexArray(0);
}

// ---- drawRect with rounded corners ----
void Renderer::drawRect(const QRectF &r, const QColor &fill,
                        const QColor &stroke, float strokeW, float radius)
{
    // 2026-09-16: 距離関数で描く（SHAPE_FRAG）。形の決まり方は旧ポリゴン版と同じ:
    //   塗り … 角丸半径は短辺の半分まで
    //   枠線 … 旧版は「strokeW/2だけ内側の角丸パス」を glLineWidth(strokeW) で
    //          なぞっていた。glLineWidth は物理ピクセル指定（dprで拡大されない）
    //          なので、線の太さは strokeW/dpr 論理px。位置と太さをそのまま距離で表す。
    const float halfW = qMax(0.0f, (float)r.width()  * 0.5f);
    const float halfH = qMax(0.0f, (float)r.height() * 0.5f);
    const float rad = qMin(qMax(0.0f, radius), qMin(halfW, halfH));
    const QVector2D center((float)r.center().x(), (float)r.center().y());
    // 2026-09-22: 融合中の板なら、相手の板まで覆う四角形で描く。
    const ShapeFusion *fusion = fusionFor(r);
    if (fusion && !fusion->drawsShape)
        return;   // 融合した板は相手（受け側）がまとめて描く
    const QRectF quad = fusion
        ? r.united(fusion->otherRect).adjusted(-fusion->k, -fusion->k, fusion->k, fusion->k)
        : r;
    if (fill.alpha() > 0) {
        drawShapeQuad(0, quad, fill, [&](QOpenGLShaderProgram *p) {
            p->setUniformValue(m_uniforms.get(p, "uCenter"), center);
            p->setUniformValue(m_uniforms.get(p, "uHalfSize"), QVector2D(halfW, halfH));
            p->setUniformValue(m_uniforms.get(p, "uRadius"), rad);
            applyFusionUniforms(p, fusion);
        });
    }
    if (stroke.alpha() > 0 && strokeW > 0) {
        drawShapeQuad(1, quad, stroke, [&](QOpenGLShaderProgram *p) {
            applyFusionUniforms(p, fusion);
            p->setUniformValue(m_uniforms.get(p, "uCenter"), center);
            p->setUniformValue(m_uniforms.get(p, "uHalfSize"), QVector2D(halfW, halfH));
            p->setUniformValue(m_uniforms.get(p, "uRadius"), rad);
            p->setUniformValue(m_uniforms.get(p, "uStroke"), strokeW * 0.5f);
            p->setUniformValue(m_uniforms.get(p, "uHalfWidth"), physicalLineHalfWidth(strokeW));
        });
    }
}

void Renderer::drawGradientRect(const QRectF &r,
                               const QColor &colorTop,
                               const QColor &colorBottom)
{
    float x0=(float)r.left(),  y0=(float)r.top();
    float x1=(float)r.right(), y1=(float)r.bottom();
    auto c = [this](const QColor &col) -> std::array<float,4> {
        return {(float)col.redF(),(float)col.greenF(),(float)col.blueF(),(float)col.alphaF() * m_globalAlpha};
    };
    auto t = c(colorTop), b = c(colorBottom);
    // 頂点レイアウト: x,y, r,g,b,a  (TRIANGLE_STRIP: TL,BL,TR,BR)
    float verts[24] = {
        x0,y0, t[0],t[1],t[2],t[3],  // TL
        x0,y1, b[0],b[1],b[2],b[3],  // BL
        x1,y0, t[0],t[1],t[2],t[3],  // TR
        x1,y1, b[0],b[1],b[2],b[3],  // BR
    };
    glBindVertexArray(m_vcolVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_vcolVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    m_vcolShader->bind();
    m_vcolShader->setUniformValue(m_uniforms.get(m_vcolShader, "uProjection"), m_projection);
    GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vcolShader->release();
    glBindVertexArray(0);
}

void Renderer::drawGlowRect(const QRectF &r, const QColor &color,
                            float radius, float glowWidth)
{
    if (color.alpha() <= 0 || glowWidth <= 0.0f)
        return;

    // グローが完全に減衰する範囲までを覆うクワッドを1枚描き、
    // 減衰カーブはすべてフラグメントシェーダー側で計算する。
    const float gw = glowWidth;
    // 2026-09-22: 融合中の板なら、相手の板まで覆う（首の部分にもグローが乗る）。
    const ShapeFusion *fusion = fusionFor(r);
    if (fusion && !fusion->drawsShape)
        return;   // 融合した板のグロー・影も受け側がまとめて描く
    const QRectF cover = fusion
        ? r.united(fusion->otherRect).adjusted(-fusion->k, -fusion->k, fusion->k, fusion->k)
        : r;
    const float x0 = (float)cover.left()   - gw;
    const float y0 = (float)cover.top()    - gw;
    const float x1 = (float)cover.right()  + gw;
    const float y1 = (float)cover.bottom() + gw;
    const float verts[8] = { x0,y0, x1,y0, x1,y1, x0,y1 };

    glBindVertexArray(m_colVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_colVBO);
    // 2026-09-16: 確保し直さず中身だけ書き換える（m_colVBOは初期化時に512要素確保済み）。
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    // 角丸半径は drawRect() と同じ規則でクランプ（短辺の半分まで）。
    const float halfW = (float)r.width()  * 0.5f;
    const float halfH = (float)r.height() * 0.5f;
    const float rad = qMin(qMax(0.0f, radius), qMin(halfW, halfH));

    m_glowShader->bind();
    m_glowShader->setUniformValue(m_uniforms.get(m_glowShader, "uProjection"), m_projection);
    m_glowShader->setUniformValue(m_uniforms.get(m_glowShader, "uCenter"),
        QVector2D((float)r.center().x(), (float)r.center().y()));
    m_glowShader->setUniformValue(m_uniforms.get(m_glowShader, "uHalfSize"), QVector2D(halfW, halfH));
    m_glowShader->setUniformValue(m_uniforms.get(m_glowShader, "uRadius"), rad);
    m_glowShader->setUniformValue(m_uniforms.get(m_glowShader, "uGlowW"), gw);
    applyFusionUniforms(m_glowShader, fusion);
    m_glowShader->setUniformValue(m_uniforms.get(m_glowShader, "uColor"),
        QVector4D(color.redF(), color.greenF(), color.blueF(),
                  color.alphaF() * m_globalAlpha));
    GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    m_glowShader->release();
    glBindVertexArray(0);
}

void Renderer::drawRoundedRectStroke(const QRectF &r, const QColor &color,
                                      float radius, float thickness, float feather)
{
    if (color.alpha() <= 0 || thickness <= 0.0f)
        return;

    // ストロークが1px程度AAではみ出ても破綻しないよう、覆うクワッドを
    // 少し外側にとる（内側のフェザーはrの内部で完結するのでpadには影響しない）。
    const float pad = 2.0f;
    const float x0 = (float)r.left()   - pad;
    const float y0 = (float)r.top()    - pad;
    const float x1 = (float)r.right()  + pad;
    const float y1 = (float)r.bottom() + pad;
    const float verts[8] = { x0,y0, x1,y0, x1,y1, x0,y1 };

    glBindVertexArray(m_colVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_colVBO);
    // 2026-09-16: 確保し直さず中身だけ書き換える（m_colVBOは初期化時に512要素確保済み）。
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    const float halfW = (float)r.width()  * 0.5f;
    const float halfH = (float)r.height() * 0.5f;
    const float rad = qMin(qMax(0.0f, radius), qMin(halfW, halfH));
    const float th  = qMin(thickness, qMin(halfW, halfH));

    m_strokeShader->bind();
    m_strokeShader->setUniformValue(m_uniforms.get(m_strokeShader, "uProjection"), m_projection);
    m_strokeShader->setUniformValue(m_uniforms.get(m_strokeShader, "uCenter"),
        QVector2D((float)r.center().x(), (float)r.center().y()));
    m_strokeShader->setUniformValue(m_uniforms.get(m_strokeShader, "uHalfSize"), QVector2D(halfW, halfH));
    m_strokeShader->setUniformValue(m_uniforms.get(m_strokeShader, "uRadius"), rad);
    m_strokeShader->setUniformValue(m_uniforms.get(m_strokeShader, "uThickness"), th);
    m_strokeShader->setUniformValue(m_uniforms.get(m_strokeShader, "uFeather"), qMax(0.0f, feather));
    m_strokeShader->setUniformValue(m_uniforms.get(m_strokeShader, "uColor"),
        QVector4D(color.redF(), color.greenF(), color.blueF(),
                  color.alphaF() * m_globalAlpha));
    GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    m_strokeShader->release();
    glBindVertexArray(0);
}

void Renderer::drawLine(const QPointF &a, const QPointF &b,
                        const QColor &color, float width)
{
    // 2026-09-16: 距離関数で描く（端は切り落とし＝旧GL_LINESと同じ形）。
    // 太さは旧 glLineWidth と同じく物理ピクセル指定として扱う。
    const float hw = physicalLineHalfWidth(width);
    const QRectF bounds = QRectF(a, b).normalized().adjusted(-hw, -hw, hw, hw);
    drawShapeQuad(2, bounds, color, [&](QOpenGLShaderProgram *p) {
        p->setUniformValue(m_uniforms.get(p, "uA"), QVector2D((float)a.x(), (float)a.y()));
        p->setUniformValue(m_uniforms.get(p, "uB"), QVector2D((float)b.x(), (float)b.y()));
        p->setUniformValue(m_uniforms.get(p, "uHalfWidth"), hw);
    });
}

void Renderer::drawCircle(const QPointF &center, float radius, const QColor &fill)
{
    // 2026-09-16: 距離関数で描く。
    if (radius <= 0.0f)
        return;
    const QRectF bounds(center.x() - radius, center.y() - radius,
                        radius * 2.0, radius * 2.0);
    drawShapeQuad(4, bounds, fill, [&](QOpenGLShaderProgram *p) {
        p->setUniformValue(m_uniforms.get(p, "uCenter"), QVector2D((float)center.x(), (float)center.y()));
        p->setUniformValue(m_uniforms.get(p, "uRadius"), radius);
    });
}

void Renderer::drawDotGrid(const QRectF &screenRect,
                           const QPointF &cameraPos, float zoom,
                           float spacingCanvas, float radiusPx,
                           const QColor &color)
{
    if (color.alpha() <= 0 || spacingCanvas <= 0.0f || zoom <= 0.0f)
        return;

    const float x0 = (float)screenRect.left();
    const float y0 = (float)screenRect.top();
    const float x1 = (float)screenRect.right();
    const float y1 = (float)screenRect.bottom();
    const float verts[8] = { x0,y0, x1,y0, x1,y1, x0,y1 };

    glBindVertexArray(m_colVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_colVBO);
    // 2026-09-16: 確保し直さず中身だけ書き換える（m_colVBOは初期化時に512要素確保済み）。
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    m_dotGridShader->bind();
    m_dotGridShader->setUniformValue(m_uniforms.get(m_dotGridShader, "uProjection"), m_projection);
    m_dotGridShader->setUniformValue(m_uniforms.get(m_dotGridShader, "uScreenCenter"),
        QVector2D((float)screenRect.width()*0.5f + x0, (float)screenRect.height()*0.5f + y0));
    m_dotGridShader->setUniformValue(m_uniforms.get(m_dotGridShader, "uCameraPos"), QVector2D((float)cameraPos.x(), (float)cameraPos.y()));
    m_dotGridShader->setUniformValue(m_uniforms.get(m_dotGridShader, "uZoom"), zoom);
    m_dotGridShader->setUniformValue(m_uniforms.get(m_dotGridShader, "uSpacing"), spacingCanvas);
    m_dotGridShader->setUniformValue(m_uniforms.get(m_dotGridShader, "uRadiusPx"), radiusPx);
    m_dotGridShader->setUniformValue(m_uniforms.get(m_dotGridShader, "uColor"),
        QVector4D(color.redF(), color.greenF(), color.blueF(), color.alphaF() * m_globalAlpha));
    GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    m_dotGridShader->release();
    glBindVertexArray(0);
}

void Renderer::drawTriangle(const QPointF &a, const QPointF &b, const QPointF &c,
                            const QColor &fill)
{
    // 2026-09-16: 距離関数で描く。
    const double x0 = qMin(a.x(), qMin(b.x(), c.x()));
    const double y0 = qMin(a.y(), qMin(b.y(), c.y()));
    const double x1 = qMax(a.x(), qMax(b.x(), c.x()));
    const double y1 = qMax(a.y(), qMax(b.y(), c.y()));
    drawShapeQuad(3, QRectF(x0, y0, x1 - x0, y1 - y0).adjusted(-1, -1, 1, 1), fill,
                  [&](QOpenGLShaderProgram *p) {
        p->setUniformValue(m_uniforms.get(p, "uA"), QVector2D((float)a.x(), (float)a.y()));
        p->setUniformValue(m_uniforms.get(p, "uB"), QVector2D((float)b.x(), (float)b.y()));
        p->setUniformValue(m_uniforms.get(p, "uC"), QVector2D((float)c.x(), (float)c.y()));
    });
}

void Renderer::drawText(const QString &text, const QPointF &pos,
                        const QFont &font, const QColor &color, float scale)
{
    QString key = text + "|" + font.toString() + "|" + color.name(QColor::HexArgb);
    TextEntry &entry = m_textCache[key];
    if (entry.texId == 0) {
        QFontMetrics fm(font);
        QRect br = fm.boundingRect(text);
        // 2026-10-12: 描き始めは x=2 なのに幅を boundingRect の幅だけで取っていたため、
        // 左のベアリング（br.left()>0）のぶん右端の字が画像からはみ出して欠けていた
        // （太字の Arial で目立った）。右端は br.right() と字送り幅の大きい方まで確保する。
        const int inkRight = qMax(br.right() + 1, fm.horizontalAdvance(text));
        int w = qMax(br.width(), inkRight) + 4 + 2, h = br.height()+4;
        QImage img(w, h, QImage::Format_RGBA8888);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setFont(font); p.setPen(color);
        p.drawText(2, fm.ascent()+2, text);
        p.end();
        entry.texId = createTexture(img);
        entry.w = w; entry.h = h;
    }
    entry.lastUsedFrame = m_frameCounter;
    drawTexture(entry.texId, QRectF(pos.x(), pos.y(), entry.w*scale, entry.h*scale));
}

GLuint Renderer::createTexture(const QImage &image)
{
    QImage img = image.convertToFormat(QImage::Format_RGBA8888);
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width(), img.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    glBindTexture(GL_TEXTURE_2D, 0);
    return id;
}

void Renderer::updateTexture(GLuint texId, const QImage &image)
{
    QImage img = image.convertToFormat(QImage::Format_RGBA8888);
    glBindTexture(GL_TEXTURE_2D, texId);
    // 【2026-07-09】確保済みテクスチャと新しい画像のサイズが違う場合、
    // glTexSubImage2Dでは「小さい画像→左上にだけ部分書き込みされ周囲に
    // 古い絵が残る」「大きい画像→GLエラーで何も更新されない」となり、
    // どちらも壊れた見た目になる（旧・初回チュートリアルのサンプルバブルで
    // 実際に発生: Windows Explorerのサムネイルはキャッシュ状態によって
    // 呼ぶたびに異なるサイズで届くことがあるため）。サイズが変わっていたら
    // glTexImage2Dでストレージごと作り直す。
    GLint curW = 0, curH = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &curW);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &curH);
    if (curW != img.width() || curH != img.height()) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width(), img.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.width(), img.height(),
                        GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::deleteTexture(GLuint texId)
{
    glDeleteTextures(1, &texId);
}
