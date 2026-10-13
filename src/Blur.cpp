#include "Blur.h"
#include "GpuProfiler.h"
#include "UniformLocationCache.h"

#include <QOpenGLFunctions_3_3_Core>
#include <QEvent>
#include <QTimer>
#include <QSet>
#include <QGraphicsOpacityEffect>
#include <QOpenGLShaderProgram>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <QWidget>
#include <QDebug>
#include <QElapsedTimer>
#include <cmath>

namespace {

// 描画先の矩形とサンプルするUV矩形を受け取る共通の頂点シェーダー。
// 座標系はGLのウィンドウ座標(物理px・左下原点)で統一する。
const char *kVertexSrc = R"(#version 330 core
layout(location = 0) in vec2 aPos;   // 0..1 の単位クワッド
uniform vec4 uRectPx;                // 描画先 (x, y, w, h) 物理px・下原点
uniform vec2 uViewportPx;            // 描画先ビューポートの物理サイズ
uniform vec4 uUvRect;                // サンプル範囲 (u0, v0, du, dv)
uniform vec2 uLocalOrigin;           // マスク形状の左下から見た描画先の位置(物理px)
out vec2 vUv;
out vec2 vLocalPx;
void main()
{
    // vLocalPxは「マスクとして使う角丸矩形の左下」を原点とする座標。
    // 画面外へはみ出して描画先が切り詰められても、マスクの形は元の矩形の
    // ままにするため、切り詰めたぶんをuLocalOriginで補正する。
    vLocalPx = uLocalOrigin + aPos * uRectPx.zw;
    vUv      = uUvRect.xy + aPos * uUvRect.zw;
    vec2 px  = uRectPx.xy + vLocalPx;
    vec2 ndc = (px / uViewportPx) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

// 分離ガウスの1パスぶん。縦横で2回呼ぶ。
// 1/4に縮小した後に掛けるので9タップで十分に滑らかになる。
const char *kBlurFragSrc = R"(#version 330 core
in vec2 vUv;
in vec2 vLocalPx;
uniform sampler2D uTex;
uniform vec2 uStepUv;    // 1タップぶんのUV移動量(方向込み)
uniform vec4 uUvClamp;   // 有効データ範囲 (minU, minV, maxU, maxV)
out vec4 fragColor;

vec4 tap(vec2 uv)
{
    // テクスチャは確保容量ぶんの余白を持つことがあるため、実際に書き込んだ
    // 範囲の外を読まないようにクランプする(余白のゴミが滲み出るのを防ぐ)。
    return texture(uTex, clamp(uv, uUvClamp.xy, uUvClamp.zw));
}

void main()
{
    vec4 c = tap(vUv) * 0.2270270270;
    c += (tap(vUv + uStepUv) + tap(vUv - uStepUv)) * 0.1945945946;
    c += (tap(vUv + uStepUv * 2.0) + tap(vUv - uStepUv * 2.0)) * 0.1216216216;
    c += (tap(vUv + uStepUv * 3.0) + tap(vUv - uStepUv * 3.0)) * 0.0540540541;
    c += (tap(vUv + uStepUv * 4.0) + tap(vUv - uStepUv * 4.0)) * 0.0162162162;
    fragColor = c;
}
)";

// 1/2への縮小と同時に sRGB -> リニア へ変換する。
//
// 2026-09-12: ぼかしを「暗く沈ませない」ための本命の修正。
// キャンバスのFBOはGL_RGBA8、つまり画素の値はsRGBでエンコードされている。
// これを素のまま平均すると、ガンマ空間での平均になるので、明るい画素と
// 暗い画素が混ざるほど物理的に正しい値より暗くなる。縮小blitもガウスも
// 全部この状態で走っていたため、「ぼかした背後は本来の色より少し暗い」が
// 常に起きていた(CanvasVideo.cppのvideoUiBodyColor()は、その実測ぶんを
// アクセント色側で持ち上げて相殺していた対症療法)。
//
// 変換は「平均する前に、1画素ずつ」でなければ意味がないので、
// バイリニアの縮小blitでは代用できない。ここだけシェーダーにして
// texelFetch()で2x2の4画素を個別に読み、各々をリニアへ直してから平均する。
// これ以降(1/4への縮小・ガウス・合成の直前まで)はリニア空間なので、
// 残りの縮小は今まで通りblitのバイリニアで正しい。
//
// 出力先はGL_RGBA16F。リニア値を8bitで持つと暗部の階調が足りず、
// ぼかしの平坦な部分にバンディングが出る。
const char *kDownFragSrc = R"(#version 330 core
uniform sampler2D uTex;
uniform float uLinearize;   // 1.0=sRGB->リニア変換を行う / 0.0=素通し(従来の挙動)
uniform vec2  uFetchOffset; // 2026-09-16: 入力テクスチャ上のキャプチャ領域の左下（整数値）
out vec4 fragColor;

// 【premultipliedのままpow()を掛けないこと】
// premultipliedの値は「色×アルファ」なので、そのままガンマを掛けると
// アルファの低い画素ほど余計に暗くなる。いったんストレート化してから
// 変換し、最後に掛け直す。
//
// 【clampしないこと】
// GlassHighlightEffectのリムライトはdual-source blendingで「色だけを足して
// αは上げない」ため、キャプチャ上にはRGB>Aの"発光"画素ができる。
// ストレート化すると1を超えるが、pow()は1超でも単調なのでそのまま通せる。
// ここでclampすると、kCompositeFragSrcのコメントにある
// 「光っている部分ほど暗くなる」逆転が再発する。
vec4 toLinear(vec4 p)
{
    if (uLinearize < 0.5)
        return p;
    float a = max(p.a, 1.0 / 255.0);
    vec3  c = p.rgb / a;
    return vec4(pow(max(c, 0.0), vec3(2.2)) * a, p.a);
}

void main()
{
    // 出力先のビューポートは (0,0,halfW,halfH)、入力のデータは
    // resolveテクスチャの (0,0,capW,capH) に入っていて capW==halfW*2 なので、
    // gl_FragCoordを2倍すれば対応する2x2ブロックの左下になる。
    ivec2 s = ivec2(gl_FragCoord.xy) * 2 + ivec2(uFetchOffset + 0.5);
    vec4  c = toLinear(texelFetch(uTex, s + ivec2(0, 0), 0))
            + toLinear(texelFetch(uTex, s + ivec2(1, 0), 0))
            + toLinear(texelFetch(uTex, s + ivec2(0, 1), 0))
            + toLinear(texelFetch(uTex, s + ivec2(1, 1), 0));
    fragColor = c * 0.25;
}
)";

// ぼかし結果を角丸マスク付きで元のFBOへ書き戻す。
//
// 2026-09-11: ここは当初、普通のpremultiplied合成
// (GL_ONE, GL_ONE_MINUS_SRC_ALPHA)で上書きしていたが、それだと
// 「元のdstがどれだけ退くか」がぼかし結果のアルファに引きずられてしまう。
// 動画や画像のように不透明(alpha≒1)な場所はきちんと置き換わる一方、
// バブルの枠やガラスのような半透明(alpha≒0.5)な場所では、退く量も半分に
// しかならず、元のくっきりした絵が半分残ってしまっていた
// (=「動画はボケるのにバブルはボケない」の原因)。
//
// GlassHighlightEffectが同じ問題に対して使っているdual-source blendingで、
// 「色」と「dstを退かせる量(=マスク)」を独立に出力する。
//   fragColor = ぼかした色 × mask   (加算される側)
//   fragCover = mask                 (dstを退かせる量)
// ブレンド式は GL_ONE, GL_ONE_MINUS_SRC1_ALPHA なので、結果は
//   out = blurred×mask + dst×(1-mask)
// となり、元の絵の不透明度に関係なくマスク内が完全に置き換わる。
const char *kCompositeFragSrc = R"(#version 330 core
in vec2 vUv;
in vec2 vLocalPx;
uniform sampler2D uTex;
uniform vec2  uHalfSizePx;   // 角丸矩形の半サイズ(物理px)
uniform float uRadiusPx;     // 角丸半径(物理px)
uniform float uOpacity;      // ぼかしの効き具合(0〜1)
uniform float uVibrancy;     // 彩度の持ち上げ(1.0で素通し)
uniform float uHiKnee;       // ハイライトを持ち上げ始める明るさ
uniform float uHiGain;       // ハイライトの持ち上げ量
uniform float uLinearize;    // 1.0=リニア->sRGBへ戻す / 0.0=素通し(従来の挙動)
layout(location = 0, index = 0) out vec4 fragColor;  // 加算される色
layout(location = 0, index = 1) out vec4 fragCover;  // dstを退かせる量

// kDownFragSrcのtoLinear()の逆変換。書き戻す先(キャンバスのFBO)はGL_RGBA8で
// sRGBエンコードされた値を持つので、ここでエンコードし直してから返す。
// ストレート化してから変換する理由・clampしない理由はtoLinear()と同じ。
vec4 toSrgb(vec4 p)
{
    if (uLinearize < 0.5)
        return p;
    float a = max(p.a, 1.0 / 255.0);
    vec3  c = p.rgb / a;
    return vec4(pow(max(c, 0.0), vec3(1.0 / 2.2)) * a, p.a);
}

void main()
{
    // 角丸矩形SDF。縁は1pxでアンチエイリアスする。
    vec2  p = vLocalPx - uHalfSizePx;
    vec2  q = abs(p) - uHalfSizePx + vec2(uRadiusPx);
    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - uRadiusPx;
    // マスクに不透明度を掛けることで、元の絵とのクロスフェードになる。
    // UIがフェードで薄くなるとき、ブラーも同じ割合で薄くなって消える。
    float mask = (1.0 - smoothstep(-1.0, 0.0, d)) * uOpacity;
    if (mask <= 0.0)
        discard;

    // キャプチャがalpha=0だった場所(ウィンドウの透過部分)は、ぼかし結果も
    // alpha=0なので置き換えても透明のまま。何も起きない。
    // ぼかしはリニア空間で行っているので、ここでsRGBへ戻す。
    vec4 src = toSrgb(texture(uTex, vUv));

    // 2026-09-11: UIは この上へ半透明の塗りを重ねるため、背後の明るさは
    // その不透明度ぶん必ず目減りする(島なら α=115/255 で背後の寄与は55%)。
    // 結果、バブルのガラス枠のように「明らかに光っている」ものまで中間灰に
    // 潰れて見えていた。ここで背後側を先に整えておく。
    //   ・彩度: 塗りで濁るぶんを先に持ち上げて色味を残す
    //   ・ハイライト: uHiKnee を超える明るさだけを白へ寄せる。暗部〜中間は
    //     ほぼ素通しなので、通常時の島の見た目は変わらない。
    //
    // 【premultipliedのまま扱うこと】
    // 2026-09-11: 当初ここで src.rgb/src.a とストレート化してから補正し、
    // 最後に×aで戻していたが、それは「光っている部分ほど暗くなる」という
    // 逆転を起こした。GlassHighlightEffectのリムライトはdual-source blending
    // で「色だけを足してαは上げない」描き方をするため、キャプチャ上では
    // RGB > A という状態(premultipliedとしては範囲外の"発光"画素)になる。
    // これをαで割ると1を大きく超え、clampで1へ潰れ、×aで戻す際に元より
    // 暗くなってしまう(例: RGB=0.9, A=0.3 → 0.3)。逆に不透明な部分(A=1)は
    // 無変化なので、光っていない場所のほうが明るいという見え方になる。
    // premultipliedのまま補正すれば、この発光ぶんも素直に持ち上がる。
    {
        vec3 rgb = src.rgb;
        float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
        rgb = mix(vec3(lum), rgb, uVibrancy);
        float peak = max(max(rgb.r, rgb.g), rgb.b);
        rgb *= 1.0 + uHiGain * smoothstep(uHiKnee, 1.0, peak);
        src = vec4(clamp(rgb, 0.0, 1.0), src.a);
    }

    fragColor = src * mask;
    fragCover = vec4(mask);
}
)";

// ---- ガラスの板（2026-09-16追加。Blur.h の GlassPlateStyle 参照）----------
//
// 座標はキャンバスの論理px（上原点）で扱う。QPainter版と同じ座標系で
// グラデーションの位置や光の距離を計算できるようにするため。
const char *kGlassVertexSrc = R"(#version 330 core
layout(location = 0) in vec2 aPos;   // 0..1 の単位クワッド
uniform vec4 uQuad;                  // 描く範囲 (x, y, w, h) 論理px・上原点
uniform vec2 uViewLogical;           // ビューポートの論理サイズ
uniform vec2 uRectOrigin;            // 板の矩形の左上（論理px）
out vec2 vLocal;                     // 板の左上を原点とした論理px（y下向き）
void main()
{
    vec2 pos = uQuad.xy + aPos * uQuad.zw;
    vLocal = pos - uRectOrigin;
    vec2 ndc = vec2(pos.x / uViewLogical.x * 2.0 - 1.0,
                    1.0 - pos.y / uViewLogical.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

// DynamicIsland::paintEvent() の旧「ガラス表現」ブロックを1パスへ写したもの。
// 旧コードの手順と1対1で対応させてある（番号はその順）。
//
// 【帯（太さ違いのストローク）の再現】旧コードは幅 2*span*(1-i/12) の
// ストロークを輪郭の中へクリップして重ねていた。i本目が画素を覆うのは
// 「内側への距離 din < span*(1-i/12)」のときなので、その被覆率を
// アンチエイリアス込みで i ごとに求め、同じ合成を同じ順に掛ける。
// Qtのラスタエンジンは部分被覆を「合成結果と元の値の線形補間」で扱うので、
// ここでも mix(dst, blend(dst, src), coverage) とする。
const char *kGlassFragSrc = R"(#version 330 core
in vec2 vLocal;
uniform vec2  uSize;       // 板の幅・高さ（論理px）
uniform float uRadius;     // 角丸半径（論理px）
uniform float uAa;         // 1デバイス画素あたりの論理px（=1/dpr）
uniform vec4  uBody;       // 本体色（premultiplied）
uniform vec3  uShade;      // 翳りの暗色（ストレート）
uniform float uBaseH;      // 光の距離の基準高さ
uniform vec4  uLightSpan;  // 加算の光4種それぞれの帯の届く距離（上・下・左上・右下）
uniform float uSideSpan;   // 乗算の翳りの帯の届く距離
uniform int   uLightCount; // 光1種あたりの帯の本数
uniform float uLightDiv;   // 帯i本目の太さ = span*(1 - i/uLightDiv)
uniform int   uSideCount;
uniform float uSideDiv;
uniform float uOpacity;    // 全体の不透明度（QGraphicsOpacityEffect相当）
out vec4 fragColor;

float sdRoundRect(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + vec2(r);
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

// 境界まで edge（内側が正）の位置にある画素の被覆率。
float cover(float edge)
{
    return clamp(edge / uAa + 0.5, 0.0, 1.0);
}

// 停止位置 0 / t1 / 1 の3点グラデーション（PadSpread）。値は0〜255。
float ramp3(float t, float a0, float t1, float a1, float a2)
{
    t = clamp(t, 0.0, 1.0);
    float v = (t < t1) ? mix(a0, a1, t / max(t1, 1e-5))
                       : mix(a1, a2, (t - t1) / max(1.0 - t1, 1e-5));
    return v / 255.0;
}

// 停止位置 0 / t1 / t2 / 1 の4点グラデーション。
float ramp4(float t, float a0, float t1, float a1, float t2, float a2, float a3)
{
    t = clamp(t, 0.0, 1.0);
    float v;
    if (t < t1)      v = mix(a0, a1, t / max(t1, 1e-5));
    else if (t < t2) v = mix(a1, a2, (t - t1) / max(t2 - t1, 1e-5));
    else             v = mix(a2, a3, (t - t2) / max(1.0 - t2, 1e-5));
    return v / 255.0;
}

vec4 compOver(vec4 d, vec4 s, float c)
{
    return mix(d, s + d * (1.0 - s.a), c);
}

// QPainter::CompositionMode_Plus
vec4 compPlus(vec4 d, vec4 s, float c)
{
    return mix(d, min(d + s, vec4(1.0)), c);
}

// QPainter::CompositionMode_Multiply（Qtのラスタエンジンと同じ式）
vec4 compMultiply(vec4 d, vec4 s, float c)
{
    vec3  rgb = s.rgb * d.rgb + s.rgb * (1.0 - d.a) + d.rgb * (1.0 - s.a);
    float a   = s.a + d.a - s.a * d.a;
    return mix(d, vec4(rgb, a), c);
}

void main()
{
    vec2  p   = vLocal;
    float W   = uSize.x;
    float H   = uSize.y;
    float d   = sdRoundRect(p - uSize * 0.5, uSize * 0.5, uRadius);
    float din = -d;
    float clip = cover(din);

    // 輪郭線（線幅1px・クリップなし）が外へ0.5pxはみ出す以外は何も描かない。
    if (d > 1.0 + uAa)
        discard;

    float innerH    = max(1.0, H);
    float lightSpan = min(uBaseH * 0.95, innerH * 0.36);
    float s1        = min(0.34, (uBaseH * 0.62) / innerH);
    float s2        = 1.0 - s1;

    vec4 D = vec4(0.0);

    // (1) 本体
    D = compOver(D, uBody, clip);
    // (2) フロスト（白 α10）
    D = compOver(D, vec4(10.0 / 255.0), clip);

    // (3) 加算の縁光4種（各12本）。旧コードと同じ順（上→下→左上→右下）。
    float aTop = ramp3(p.y / lightSpan, 14.0, 0.45, 4.0, 0.0);
    float aBottom = ramp3((p.y - (H - lightSpan)) / lightSpan, 0.0, 0.55, 3.0, 9.0);
    vec2  diag = vec2(W * 0.55, innerH * 0.55);
    float diagLen2 = max(dot(diag, diag), 1e-5);
    float aTopLeft = ramp3(dot(p, diag) / diagLen2, 20.0, 0.55, 3.0, 0.0);
    float aBottomRight = ramp3(dot(p - (uSize - diag), diag) / diagLen2,
                               0.0, 0.45, 2.0, 11.0);
    float lights[4] = float[4](aTop, aBottom, aTopLeft, aBottomRight);
    for (int k = 0; k < 4; ++k) {
        if (lights[k] <= 0.0)
            continue;
        vec4 src = vec4(lights[k]);
        for (int i = 0; i < uLightCount; ++i) {
            float c = cover(uLightSpan[k] * (1.0 - float(i) / uLightDiv) - din) * clip;
            if (c <= 0.0)
                break;   // 帯は内側ほど細くなるので、以降も覆わない
            D = compPlus(D, src, c);
        }
    }

    // (4) 乗算: 左右の縁の翳り（8本）
    float aShade = ramp4(p.y / innerH, 0.0, s1, 30.0, s2, 30.0, 0.0);
    if (aShade > 0.0) {
        vec4 src = vec4(uShade * aShade, aShade);
        for (int i = 0; i < uSideCount; ++i) {
            float c = cover(uSideSpan * (1.0 - float(i) / uSideDiv) - din) * clip;
            if (c <= 0.0)
                break;
            D = compMultiply(D, src, c);
        }
    }

    // (5) 乗算: 輪郭線（線幅1px、輪郭の中心に乗る。クリップなし）
    float aContour = ramp4(p.y / innerH, 120.0, s1, 230.0, s2, 230.0, 190.0);
    float cContour = cover(0.5 - abs(d));
    D = compMultiply(D, vec4(uShade * aContour, aContour), cContour);

    fragColor = D * uOpacity;
}
)";

// ---- ガラスの形（Disc / Goo）共通の関数（2026-09-16）------------------------
// どちらも「透明な下地にQPainterの合成を順に掛けた結果」をpremultipliedで
// 計算する（kGlassFragSrc と同じ考え方）。
const char *kGlassShapePreludeSrc = R"(#version 330 core
in vec2 vLocal;
uniform float uAa;       // 1デバイス画素あたりの論理px
uniform float uOpacity;
out vec4 fragColor;

// 境界まで edge（内側が正）の位置にある画素の被覆率
float cover(float edge) { return clamp(edge / uAa + 0.5, 0.0, 1.0); }

// 中心線からの距離 dist にある画素が、太さ 2*halfW の線に覆われる割合
// （1画素ぶんの箱フィルタ。細い線でも濃さが正しく出る）
float lineCover(float dist, float halfW)
{
    float h = uAa * 0.5;
    return (clamp(halfW - dist, -h, h) - clamp(-halfW - dist, -h, h)) / uAa;
}

float ramp3(float t, float a0, float t1, float a1, float a2)
{
    t = clamp(t, 0.0, 1.0);
    float v = (t < t1) ? mix(a0, a1, t / max(t1, 1e-5))
                       : mix(a1, a2, (t - t1) / max(1.0 - t1, 1e-5));
    return v / 255.0;
}

float ramp4(float t, float a0, float t1, float a1, float t2, float a2, float a3)
{
    t = clamp(t, 0.0, 1.0);
    float v;
    if (t < t1)      v = mix(a0, a1, t / max(t1, 1e-5));
    else if (t < t2) v = mix(a1, a2, (t - t1) / max(t2 - t1, 1e-5));
    else             v = mix(a2, a3, (t - t2) / max(1.0 - t2, 1e-5));
    return v / 255.0;
}

// 任意本数の停止位置（最大8）
float rampN(float t, float s[8], float a[8], int n)
{
    t = clamp(t, 0.0, 1.0);
    if (t <= s[0])
        return a[0] / 255.0;
    for (int i = 1; i < n; ++i) {
        if (t <= s[i])
            return mix(a[i - 1], a[i], (t - s[i - 1]) / max(s[i] - s[i - 1], 1e-5)) / 255.0;
    }
    return a[n - 1] / 255.0;
}

vec4 compOver(vec4 d, vec4 s, float c) { return mix(d, s + d * (1.0 - s.a), c); }
vec4 compPlus(vec4 d, vec4 s, float c) { return mix(d, min(d + s, vec4(1.0)), c); }
vec4 compMultiply(vec4 d, vec4 s, float c)
{
    vec3  rgb = s.rgb * d.rgb + s.rgb * (1.0 - d.a) + d.rgb * (1.0 - s.a);
    float a   = s.a + d.a - s.a * d.a;
    return mix(d, vec4(rgb, a), c);
}
vec4 white(float a) { return vec4(a); }
vec4 shaded(vec3 c, float a) { return vec4(c * a, a); }

// AnimatedButton の円のガラス（(2)リムグロー〜(4)左右の翳り）。
// clipは円（またはスライムの形）の内側の被覆率。rr はリムグローの外径
// （ボタン単体では r、スライムでは首へ光を延ばすため r + k）。
vec4 discBody(vec4 D, vec2 p, vec2 c, float r, float rr, vec3 shade, float clip)
{
    if (clip <= 0.0)
        return D;
    // 斜めの艶
    vec2 dd = vec2(2.0 * r);
    float ts = dot(p - (c - vec2(r)), dd) / max(dot(dd, dd), 1e-5);
    D = compPlus(D, white(ramp3(ts, 13.0, 0.55, 3.0, 0.0)), clip);
    // リムグロー
    float q = r / max(rr, 1e-5);
    float rs[8] = float[8](0.0, 0.38 * q, 0.68 * q, 0.86 * q, 0.96 * q, q, 1.0, 1.0);
    float ra[8] = float[8](0.0, 0.0, 4.0, 12.0, 22.0, 18.0, (rr > r + 1e-3) ? 0.0 : 18.0, 0.0);
    D = compPlus(D, white(rampN(length(p - c) / max(rr, 1e-5), rs, ra, 7)), clip);
    // 上端・下端
    float tt = length(p - vec2(c.x, c.y - 1.02 * r)) / max(0.95 * r, 1e-5);
    D = compPlus(D, white(ramp3(tt, 22.0, 0.45, 7.0, 0.0)), clip);
    float tb = length(p - vec2(c.x, c.y + 1.06 * r)) / max(0.80 * r, 1e-5);
    D = compPlus(D, white(ramp3(tb, 16.0, 0.45, 5.0, 0.0)), clip);
    // 左右の翳り（乗算）
    for (int k = 0; k < 2; ++k) {
        float side = (k == 0) ? -1.0 : 1.0;
        float tsd = length(p - vec2(c.x + side * 0.92 * r, c.y)) / max(0.95 * r, 1e-5);
        D = compMultiply(D, shaded(shade, ramp3(tsd, 70.0, 0.45, 26.0, 0.0)), clip);
    }
    return D;
}
)";

// 島の丸ボタン。AnimatedButton::paintEvent の (0)落ち影〜(6)縁の光。
// 座標はボタンのローカル論理px。painter.scale(totalScale) の効果は
// 半径・線の太さへ uScale として織り込む。
const char *kGlassDiscFragSrc = R"(
uniform vec2  uCenter;
uniform float uR;          // 12 × totalScale
uniform float uScale;      // totalScale
uniform float uShadowR;    // 落ち影の半径（出現スケールだけに追従）
uniform float uShadowCy;
uniform vec4  uTint;       // 本体色（premultiplied）
uniform vec3  uShade;

void main()
{
    vec2  p = vLocal;
    vec2  c = uCenter;
    float r = uR;
    float s = uScale;
    vec4  D = vec4(0.0);

    if (uShadowR > 0.01) {
        vec2  sc = vec2(c.x, uShadowCy);
        float ds = length(p - sc);
        D = compOver(D, vec4(0.0, 0.0, 0.0, ramp3(ds / uShadowR, 66.0, 0.60, 28.0, 0.0)),
                     cover(uShadowR - ds));
    }

    if (r > 0.05) {
        float dist = length(p - c);
        float clip = cover(r - dist);
        // (1) フロスト＋ティント
        D = compOver(D, white(10.0 / 255.0), clip);
        D = compOver(D, uTint, clip);
        // (2)〜(4)
        D = discBody(D, p, c, r, r, uShade, clip);
        // (5) 輪郭線（乗算）と (6) 縁の細い光（加算）
        float ty = (p.y - (c.y - r)) / max(2.0 * r, 1e-5);
        D = compMultiply(D, shaded(uShade, ramp3(ty, 120.0, 0.5, 230.0, 190.0)),
                         lineCover(dist - (r - 0.35 * s), 0.45 * s));
        D = compPlus(D, white(ramp4(ty, 30.0, 0.32, 11.0, 0.68, 10.0, 24.0)),
                     lineCover(dist - (r - 0.85 * s), 0.45 * s));
    }

    if (D.a <= 0.0)
        discard;
    fragColor = D * uOpacity;
}
)";

// 設定タブの液体アニメ。IslandTabGoo::buildPath の距離場（等高線を
// marching squaresでパスにしていたもの）をそのまま画素ごとに評価し、
// paintSurface() の各ストロークを「距離場の等高線からの距離」で描く。
// パスを平行移動して描いていたストロークは、距離場を平行移動して評価する。
const char *kGlassGooFragSrc = R"(
uniform float uL;
uniform float uRgt;
uniform float uHh;
uniform float uTaper;
uniform float uCx;         // ＋ボタンの円の中心（引っ張られた位置）
uniform float uCy;
uniform float uBr;         // ＋ボタンの円の半径
uniform float uK;          // smin の強さ
uniform float uSink;
uniform float uGx0;        // 横方向の重みの区間
uniform float uGx1;
uniform float uShadowBase;
uniform vec4  uBB;         // 形の外接矩形 (left, top, right, bottom)
uniform vec4  uTint;       // ボタンの色（premultiplied）
uniform vec4  uFillRight;  // 雫側の面の色（premultiplied）
uniform vec3  uShade;
uniform vec3  uEdgeBaseDrop; // 雫側の輪郭線の色
uniform float uSideS;
uniform float uSideRightW;

float sdCapsule(vec2 q, float L, float R, float hh, float cy)
{
    if (hh <= 0.01 || R - L <= 0.01)
        return 1e3;
    float r = min(hh, (R - L) / 2.0);
    float ax = L + r, bx = R - r;
    float qx = clamp(q.x, ax, bx);
    return length(vec2(q.x - qx, q.y - cy)) - r;
}

float sdTaperedCapsule(vec2 q, float L, float R, float r1, float r2, float cy)
{
    if (r1 <= 0.01 || R - L <= 0.01)
        return 1e3;
    r1 = min(r1, (R - L) / 2.0);
    r2 = min(r2, r1);
    float ax = L + r1;
    float bx = R - r2;
    float h = bx - ax;
    if (h <= (r1 - r2) + 1e-3)
        return sdCapsule(q, L, R, r1, cy);
    float v = abs(q.y - cy);
    float u = q.x - ax;
    float b = (r1 - r2) / h;
    float a = sqrt(1.0 - b * b);
    float kk = -b * v + a * u;
    if (kk < 0.0)
        return length(vec2(v, u)) - r1;
    if (kk > a * h)
        return length(vec2(v, u - h)) - r2;
    return v * a + u * b - r1;
}

float smin(float a, float b, float k)
{
    if (k <= 0.0)
        return min(a, b);
    float h = max(k - abs(a - b), 0.0) / k;
    return min(a, b) - h * h * k * 0.25;
}

float field(vec2 q)
{
    float dc = length(q - vec2(uCx, uCy)) - uBr;
    float dt = (uTaper < 0.999)
        ? sdTaperedCapsule(q, uL, uRgt, uHh, uHh * uTaper, uCy)
        : sdCapsule(q, uL, uRgt, uHh, uCy);
    return smin(dc, dt, uK);
}

void main()
{
    vec2  p = vLocal;
    float sink = clamp(uSink, 0.0, 1.0);
    float raised = 1.0 - sink;
    float tx = clamp((p.x - uGx0) / max(uGx1 - uGx0, 1e-5), 0.0, 1.0);
    float wDrop = raised * tx;          // hBrush(c, 0, raised)
    float wSunk = sink * tx;            // hBrush(c, 0, sink)
    float d = field(p);
    float clip = cover(-d);
    vec4  D = vec4(0.0);

    // ---- 落ち影（＋ボタンの円） ----
    {
        vec2  sc = vec2(uCx, uCy + uShadowBase * 0.16);
        float sr = uShadowBase * 1.28;
        if (sr > 0.01) {
            float ds = length(p - sc);
            D = compOver(D, vec4(0.0, 0.0, 0.0, ramp3(ds / sr, 66.0, 0.60, 28.0, 0.0)),
                         cover(sr - ds));
        }
    }
    if (raised > 0.001) {
        float dsh = field(p - vec2(0.0, 1.9));
        D = compOver(D, vec4(0.0, 0.0, 0.0, 34.0 / 255.0 * wDrop), cover(-dsh));
        D = compOver(D, vec4(0.0, 0.0, 0.0, 16.0 / 255.0 * wDrop), lineCover(dsh, 1.5));
    }

    // ---- (1) 面：フロスト＋ボタンの色 → 雫の色 ----
    D = compOver(D, white(10.0 / 255.0 * mix(1.0, raised, tx)), clip);
    D = compOver(D, mix(uTint, uFillRight, tx), clip);

    // ---- ＋ボタンの円（形の内側 × 円のまわりの矩形） ----
    {
        float inArea = step(uCx - uBr - 2.0, p.x) * step(p.x, uCx + uBr + uK + 2.0)
                     * step(uCy - uBr - 2.0, p.y) * step(p.y, uCy + uBr + 2.0);
        D = discBody(D, p, vec2(uCx, uCy), uBr, uBr + uK, uShade, clip * inArea);
    }

    // ---- 浮き出た雫 ----
    if (raised > 0.001) {
        D = compPlus(D, white(4.0 / 255.0 * wDrop),  lineCover(d, 4.0) * clip);
        D = compPlus(D, white(9.0 / 255.0 * wDrop),  lineCover(d, 2.0) * clip);
        D = compPlus(D, white(14.0 / 255.0 * wDrop), lineCover(d, 0.9) * clip);
        D = compPlus(D, white(22.0 / 255.0 * wDrop),
                     lineCover(field(p - vec2(0.0, 1.4)), 1.3) * clip);
        D = compPlus(D, white(14.0 / 255.0 * wDrop),
                     lineCover(field(p + vec2(0.0, 1.3)), 1.1) * clip);
        float bbW = max(1.0, uBB.z - uBB.x);
        float t = (p.x - uBB.x) / bbW;
        float a = (t <= 1.0 - uSideS) ? 0.0
            : 70.0 / 255.0 * uSideRightW * clamp((t - (1.0 - uSideS)) / max(uSideS, 1e-5), 0.0, 1.0);
        D = compMultiply(D, shaded(uShade, a), lineCover(d, 3.5) * clip);
    }

    // ---- 沈み込んだ面 ----
    if (sink > 0.001) {
        D = compOver(D, vec4(0.0, 0.0, 0.0, 70.0 / 255.0 * wSunk),
                     lineCover(field(p - vec2(0.0, 2.3)), 2.3) * clip);
        D = compOver(D, vec4(0.0, 0.0, 0.0, 36.0 / 255.0 * wSunk),
                     lineCover(field(p - vec2(0.0, 1.0)), 0.9) * clip);
        D = compPlus(D, white(16.0 / 255.0 * wSunk),
                     lineCover(field(p + vec2(0.0, 1.1)), 0.75) * clip);
        // 穴の下側の外に細い光（形の外側だけ）
        float inBB = step(uBB.x - 6.0, p.x) * step(p.x, uBB.z + 6.0)
                   * step(uBB.y - 6.0, p.y) * step(p.y, uBB.w + 6.0);
        D = compPlus(D, white(26.0 / 255.0 * wSunk),
                     lineCover(field(p - vec2(0.0, 0.9)), 0.6) * cover(d) * inBB);
    }

    // ---- (5) 輪郭線と (6) 縁の光。ボタン側と雫側で描き分ける ----
    {
        float midX = (uGx0 + uGx1) * 0.5;
        bool  discSide = p.x < midX;
        float sk = discSide ? 0.0 : sink;
        float rs = 1.0 - sk;
        float yT = discSide ? uCy - uBr : uBB.y;
        float yB = discSide ? uCy + uBr : uBB.w;
        vec3  base = discSide ? uShade : uEdgeBaseDrop;
        float t = (p.y - yT) / max(yB - yT, 1e-5);
        float lc = lineCover(d, 0.45);
        D = compMultiply(D, shaded(base, ramp3(t, floor(120.0 * rs + 150.0 * sk + 0.5), 0.5,
                                                  floor(230.0 * rs + 90.0 * sk + 0.5),
                                                  floor(190.0 * rs + 40.0 * sk + 0.5))), lc);
        D = compPlus(D, white(ramp4(t, floor(30.0 * rs + 6.0 * sk + 0.5), 0.32,
                                       floor(11.0 * rs + 4.0 * sk + 0.5), 0.68,
                                       floor(10.0 * rs + 8.0 * sk + 0.5),
                                       floor(24.0 * rs + 22.0 * sk + 0.5))), lc);
    }

    if (D.a <= 0.0)
        discard;
    fragColor = D * uOpacity;
}
)";

// 縮小率。縮めてからぼかすことで、少ないタップ数で大きな半径を稼げる。
//
// 2026-09-11: 1/4・ぼかし1回だと、下の内容が動いたときに出力がチラチラ揺れた。
// 数値実験(フレーム間の理想移動からのズレを測定)の結果、揺れの主因は縮小時の
// エイリアスではなく「低解像度側にテクセル単位の細部が残ったまま拡大され、
// バイリニア補間が暴れること」だと分かった。同じ見た目の半径のまま縮小段数や
// タップ数を増やしてもほとんど改善せず、効いたのは"低解像度側を十分にぼかす"
// ことだった(揺れの相対値: 1/4×1回=1.53 / 1/4×2回=0.95 / 1/8×2回=0.78)。
// そこで1/8まで縮小し、ガウスを2巡させる。
constexpr int kDownsample = 8;
// ぼかしの反復回数。1回増やすごとに実効半径はおよそ√2倍になる。
constexpr int kBlurPasses = 2;

// 合成時のトーン調整(kCompositeFragSrc参照)。UIが上へ重ねる半透明の塗りで
// 失われるぶんを、背後側で先に取り戻すための値。
// kHighlightKnee 未満の明るさはほぼ無変化なので、暗い背景の上では従来と
// 同じ見た目になる。
// 2026-09-12: 「明るいところを強調する」補正は撤廃し、素通し（自然な
// ぼかし）へ戻した。彩度もハイライトも触らないので、背後の絵がそのままの
// 明るさ・色でぼけるだけになる。シェーダー側の式は残してあるが、
// uVibrancy=1 / uHiGain=0 は恒等変換（何もしないのと同じ）。
constexpr float kVibrancy      = 1.0f;   // 1.0 = 彩度を触らない
constexpr float kHighlightKnee = 1.0f;   // 使われない（gain=0のため）
constexpr float kHighlightGain = 0.0f;   // 0.0 = ハイライトを持ち上げない

// 2026-09-12: ぼかしをリニア空間で行うかどうか（kDownFragSrc参照）。
// 見比べたいときのために PB_BLUR_LINEAR=0 で従来のガンマ空間へ戻せる。
// 0にしても中間バッファがRGBA16Fになる以外は以前と同じ計算になる。
float linearizeFlag()
{
    static const float s_flag =
        (qEnvironmentVariable("PB_BLUR_LINEAR", QStringLiteral("1")) == QLatin1String("0"))
            ? 0.0f : 1.0f;
    return s_flag;
}

int roundUpTo(int value, int quantum)
{
    return ((qMax(1, value) + quantum - 1) / quantum) * quantum;
}

// 2026-09-12: 確保サイズの丸め単位を、小さいうちは細かく・大きくなるほど
// 粗くする。一律64pxだと、ラベルのコントローラのように「小さな帯から
// 縦に長いカードへ伸びる」UIで、伸びていく途中に何十回も作り直しが走って
// いた（実測で64→1792pxまで毎フレーム再確保＝初回展開のカクつきの一因）。
// 粗くしたぶんテクスチャは少し大きめに取るが、作り直しの回数は数回で済む。
int allocQuantumFor(int value)
{
    if (value <= 256)  return 64;
    if (value <= 1024) return 256;
    return 512;
}

} // namespace

// ---- BackdropBlurGL --------------------------------------------------------
// GLリソースとシェーダーの実体。Blur.hにOpenGLの型を出さないため、
// クラスごとこの翻訳単位に閉じ込めてある。
class BackdropBlurGL : protected QOpenGLFunctions_3_3_Core
{
public:
    void renderAll(BackdropBlur &owner, QWidget *canvas, qreal dpr,
                   int physW, int physH, GLuint sceneTexture);
    void release();

private:
    bool ensureInitialized();
    bool initPrograms();
    void initGeometry();
    bool makeTarget(GLuint *tex, GLuint *fbo, int w, int h, GLenum internalFormat);
    bool ensureTargets(int resolveW, int resolveH, int halfW, int halfH,
                       int quarterW, int quarterH, int blurW, int blurH);
    void renderOne(BackdropBlur::Entry &entry, QWidget *canvas, qreal dpr,
                   int physW, int physH, GLuint sceneTexture);
    // キャンバス論理座標の矩形を1つぼかす。renderOne()とblurNow()の共通部分。
    bool renderRect(const QRectF &canvasRect, qreal cornerRadius, qreal blurRadius,
                    qreal opacity, qreal dpr, int physW, int physH,
                    GLuint sceneTexture = 0);
    // ぼかしを描き終えたエントリの上へガラスの板を描く（2026-09-16）。
    void renderGlassPlate(const BackdropBlur::Entry &entry, qreal dpr,
                          int physW, int physH);
    // 丸ボタン／液体アニメ（2026-09-16）。共通の頂点と不透明度などを設定して描く。
    void renderGlassShape(const BackdropBlur::Entry &entry, qreal dpr,
                          int physW, int physH);
public:
    void blurNowImpl(qreal dpr, int physW, int physH, const QRectF &canvasRect,
                     qreal cornerRadius, qreal blurRadius, qreal opacity);
private:

    bool m_initialized = false;
    bool m_initFailed  = false;

    QOpenGLShaderProgram *m_blurProgram      = nullptr;
    QOpenGLShaderProgram *m_downProgram      = nullptr;
    QOpenGLShaderProgram *m_compositeProgram = nullptr;
    QOpenGLShaderProgram *m_glassProgram     = nullptr;
    QOpenGLShaderProgram *m_discProgram      = nullptr;
    QOpenGLShaderProgram *m_gooProgram       = nullptr;
    // 2026-09-16: シェーダー引数の番号を覚えておく（UniformLocationCache.h）
    UniformLocationCache m_uniforms;
    GLuint m_vao = 0, m_vbo = 0;

    // MSAA元FBOからは異なる寸法へ直接resolveできないため、まず等倍で受ける。
    // ここだけは元と同じGL_RGBA8(=sRGBエンコードされた生の値)。
    GLuint m_resolveTex = 0, m_resolveFbo = 0;
    int    m_resolveW = 0, m_resolveH = 0;
    // 縮小の中間段。一気に縮めると参照されない画素が出るため、2倍ずつ落とす。
    // half以降はリニア空間のGL_RGBA16F(kDownFragSrc参照)。
    GLuint m_halfTex = 0, m_halfFbo = 0;        // 1/2
    int    m_halfW = 0, m_halfH = 0;
    GLuint m_quarterTex = 0, m_quarterFbo = 0;  // 1/4
    int    m_quarterW = 0, m_quarterH = 0;
    // 縮小してからぼかすためのping-pong。
    GLuint m_pingTex = 0, m_pingFbo = 0;
    GLuint m_pongTex = 0, m_pongFbo = 0;
    int    m_blurW = 0, m_blurH = 0;

    // ---- 計測（2026-09-12追加） --------------------------------------
    // ぼかしが1フレームでどれだけ時間と面積を使っているかを2秒ごとに出す。
    // 既定ではCPU側（レンダースレッドが実際に費やした時間）だけを測る。
    // 環境変数 PB_BLUR_PROFILE=1 を付けて起動すると、各フレームの最後で
    // glFinish()して「GPUが描き終わるまで」を含めた時間も測る（GPU側の
    // 負荷を切り分けるため。常時やるとパイプラインを止めるので既定はoff）。
    QElapsedTimer m_statWindow;       // ログを出す間隔の計測
    QElapsedTimer m_statFrame;        // 1フレームぶんの計測
    double m_statCpuMsSum = 0.0;      // 同期なしの所要時間の合計
    double m_statSyncMsSum = 0.0;     // glFinish込みの所要時間の合計
    double m_statCpuMsMax = 0.0;
    int    m_statFrames = 0;          // 計測したフレーム数
    int    m_statRegions = 0;         // 実際にぼかした領域の延べ数
    qint64 m_statAreaPx = 0;          // 等倍resolveした面積の合計(物理px)
    int    m_statMaxRegionPx = 0;     // 1領域あたりの最大面積
    int    m_statAllocs = 0;          // テクスチャの作り直し回数
};

// ---- BackdropBlurWatcher ---------------------------------------------------
// 2026-09-11: 島の退場アニメーションの最後にブラー領域だけが残る問題への対処。
// setShape()はウィジェットのpaintEvent経由でしか呼ばれないため、ウィジェットが
// 非表示になった瞬間は誰も「もう描かなくていい」ことをキャンバスへ伝えられず、
// 最後に描かれたフレームのブラーがそのまま残っていた。
// 表示/非表示・移動・リサイズ・不透明度の変化を監視して再描画を要求する。
class BackdropBlurWatcher : public QObject
{
public:
    static BackdropBlurWatcher *instance()
    {
        static BackdropBlurWatcher s_watcher;
        return &s_watcher;
    }

    // 表示/非表示の切り替えは、イベントが届く順序とキャンバスの再描画の
    // タイミングが噛み合わないことがある(再表示してもブラーが戻らない、
    // 動き始めの1コマだけ古いブラーが残る、など)。その場で要求しつつ、
    // 次のイベントループでもう一度だけ要求して取りこぼしを防ぐ。
    void scheduleRepaint()
    {
        BackdropBlur::instance().requestRepaint();
        if (m_deferredPending)
            return;
        m_deferredPending = true;
        QTimer::singleShot(0, this, [this] {
            m_deferredPending = false;
            BackdropBlur::instance().requestRepaint();
        });
    }

    void watch(QWidget *owner)
    {
        if (!owner)
            return;
        owner->installEventFilter(this);
        // QGraphicsOpacityEffectでフェードアウトする場合、消えきってからは
        // paintEventが呼ばれなくなるため、不透明度の変化も拾っておく。
        // Qt::UniqueConnectionはラムダ(ファンクタ)には効かないため、
        // 接続済みのエフェクトを自前で覚えて二重接続を防ぐ。
        auto *fx = qobject_cast<QGraphicsOpacityEffect *>(owner->graphicsEffect());
        if (fx && !m_watchedEffects.contains(fx)) {
            m_watchedEffects.insert(fx);
            QObject::connect(fx, &QGraphicsOpacityEffect::opacityChanged, this,
                             [this] { scheduleRepaint(); });
            QObject::connect(fx, &QObject::destroyed, this,
                             [this](QObject *o) { m_watchedEffects.remove(o); });
        }
    }

protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Hide:
        case QEvent::Show:
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::ShowToParent:
        case QEvent::HideToParent:
            scheduleRepaint();
            break;
        default:
            break;
        }
        return QObject::eventFilter(obj, event);
    }

private:
    QSet<QObject *> m_watchedEffects;
    bool            m_deferredPending = false;
};

// ---- Surface ---------------------------------------------------------------

BackdropBlur::Surface::~Surface()
{
    detach();
}

void BackdropBlur::Surface::attach(QWidget *owner)
{
    BackdropBlur &self = BackdropBlur::instance();
    if (m_id == 0)
        m_id = self.registerSurface();
    if (Entry *e = self.findEntry(m_id))
        e->owner = owner;
    BackdropBlurWatcher::instance()->watch(owner);
}

void BackdropBlur::Surface::setShape(const QRectF &localRect, qreal cornerRadius,
                                     qreal blurRadius, qreal opacity)
{
    BackdropBlur &self = BackdropBlur::instance();
    Entry *e = self.findEntry(m_id);
    if (!e)
        return;

    e->localRect    = localRect;
    e->cornerRadius = cornerRadius;
    e->blurRadius   = blurRadius;
    e->opacity      = qBound(0.0, opacity, 1.0);

    // 「前回setShapeされた値」ではなく「実際に描かれた状態」と比較する。
    // ウィジェットがsetVisible(false)で一旦消えている間、renderOne()は
    // 描画をスキップしてrenderedValid=falseにする。再表示されたとき形は
    // 前と同じなので、setShape同士の比較では変化なしと判定されてしまい、
    // 再描画が要求されず「ブラーが掛かっていない」状態が残っていた。
    const bool needsRender =
        !e->renderedValid
        || e->renderedRect != localRect
        || !qFuzzyCompare(e->renderedCorner + 1.0, cornerRadius + 1.0)
        || !qFuzzyCompare(e->renderedBlur + 1.0, blurRadius + 1.0)
        || !qFuzzyCompare(e->renderedOpacity + 1.0, e->opacity + 1.0)
        || e->renderedGlass != e->glass;
    if (!needsRender)
        return;

    // 2026-09-15 D-5: 「変わった範囲」だけを伝える（Blur.h の
    // setRepaintRequester のコメント参照）。新旧どちらの矩形も消し直す必要が
    // あるので和を取り、ブラーが滲む幅ぶんの余白を足す。
    QRectF damage;
    if (localRect.width() > 0.0 && localRect.height() > 0.0
        && (blurRadius > 0.0 || e->glass.enabled))
        damage = localRect;
    if (e->renderedValid
        && e->renderedRect.width() > 0.0 && e->renderedRect.height() > 0.0) {
        damage = damage.isNull() ? e->renderedRect : damage.united(e->renderedRect);
    }
    if (!damage.isNull()) {
        const qreal pad = qMax(blurRadius, e->renderedBlur) + 2.0;
        damage = damage.adjusted(-pad, -pad, pad, pad);
    }
    // damage が空のまま＝新旧どちらも実体が無い、という異常系だけが全面になる。
    self.requestRepaint(e->owner.data(), damage);
}

void BackdropBlur::Surface::setGlassStyle(const GlassPlateStyle &style)
{
    // 比較と再描画の要求は setShape() がまとめて行う（renderedGlass と比べる）。
    if (Entry *e = BackdropBlur::instance().findEntry(m_id))
        e->glass = style;
}

void BackdropBlur::Surface::detach()
{
    if (m_id == 0)
        return;
    BackdropBlur::instance().unregisterSurface(m_id);
    m_id = 0;
}

// ---- Regions ---------------------------------------------------------------
// Surfaceのプール。add()された順に使い回し、使われなかったぶんは無効化する。

BackdropBlur::Regions::~Regions()
{
    qDeleteAll(m_pool);
    m_pool.clear();
}

void BackdropBlur::Regions::attach(QWidget *owner)
{
    m_owner = owner;
}

void BackdropBlur::Regions::begin()
{
    m_used = 0;
}

void BackdropBlur::Regions::add(const QRectF &localRect, qreal cornerRadius,
                                qreal blurRadius, qreal opacity,
                                const GlassPlateStyle &glass)
{
    // 板は呼び出し側の不透明度をそのまま使う（GlassPlateStyle::opacity参照）。
    GlassPlateStyle plate = glass;
    if (plate.enabled && plate.opacity < 0.0)
        plate.opacity = qBound(0.0, opacity, 1.0);
    opacity *= m_opacityScale;
    if (!m_owner || blurRadius <= 0.0 || opacity <= 0.004
        || localRect.width() <= 0.0 || localRect.height() <= 0.0) {
        return;
    }
    while (m_pool.size() <= m_used) {
        auto *surface = new Surface();
        surface->attach(m_owner);
        m_pool.append(surface);
    }
    // プールのSurfaceは毎フレーム別の要素に使い回されるので、板の有無も
    // 必ず毎回設定し直す。
    m_pool[m_used]->setGlassStyle(plate);
    m_pool[m_used]->setShape(localRect, cornerRadius, blurRadius, opacity);
    ++m_used;
}

void BackdropBlur::Regions::end()
{
    // 今回使われなかったSurfaceは半径0にして無効化する。これで、要素が
    // 消えたフレームでブラーだけが残ることがない。
    for (int i = m_used; i < m_pool.size(); ++i) {
        m_pool[i]->setGlassStyle(GlassPlateStyle());
        m_pool[i]->setShape(QRectF(), 0.0, 0.0, 0.0);
    }
}

// ---- 登録管理 ---------------------------------------------------------------

BackdropBlur &BackdropBlur::instance()
{
    static BackdropBlur s_instance;
    return s_instance;
}

int BackdropBlur::registerSurface()
{
    Entry e;
    e.id = m_nextId++;
    m_entries.append(e);
    return e.id;
}

void BackdropBlur::unregisterSurface(int id)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).id == id) {
            m_entries.removeAt(i);
            requestRepaint();
            return;
        }
    }
}

BackdropBlur::Entry *BackdropBlur::findEntry(int id)
{
    for (Entry &e : m_entries) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

void BackdropBlur::setRepaintRequester(
    std::function<void(QWidget *, const QRectF &)> requester)
{
    m_repaintRequester = std::move(requester);
}

void BackdropBlur::requestRepaint(QWidget *owner, const QRectF &ownerRect)
{
    if (m_repaintRequester)
        m_repaintRequester(owner, ownerRect);
}

// ---- GLリソース -------------------------------------------------------------

bool BackdropBlurGL::ensureInitialized()
{
    if (m_initialized)
        return true;
    if (m_initFailed)
        return false;

    if (!initializeOpenGLFunctions()) {
        qWarning("[BackdropBlur] OpenGL 3.3 Core init failed");
        m_initFailed = true;
        return false;
    }
    if (!initPrograms()) {
        m_initFailed = true;
        return false;
    }
    initGeometry();
    m_initialized = true;
    qDebug("[BackdropBlur] initialized");
    return true;
}

bool BackdropBlurGL::initPrograms()
{
    m_blurProgram = new QOpenGLShaderProgram();
    m_downProgram = new QOpenGLShaderProgram();
    m_compositeProgram = new QOpenGLShaderProgram();
    m_glassProgram = new QOpenGLShaderProgram();
    m_discProgram = new QOpenGLShaderProgram();
    m_gooProgram = new QOpenGLShaderProgram();
    const QByteArray discFrag = QByteArray(kGlassShapePreludeSrc) + kGlassDiscFragSrc;
    const QByteArray gooFrag  = QByteArray(kGlassShapePreludeSrc) + kGlassGooFragSrc;

    const bool ok =
        m_blurProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexSrc)
        && m_blurProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kBlurFragSrc)
        && m_blurProgram->link()
        && m_downProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexSrc)
        && m_downProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kDownFragSrc)
        && m_downProgram->link()
        && m_compositeProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexSrc)
        && m_compositeProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kCompositeFragSrc)
        && m_compositeProgram->link()
        && m_glassProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kGlassVertexSrc)
        && m_glassProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kGlassFragSrc)
        && m_glassProgram->link()
        && m_discProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kGlassVertexSrc)
        && m_discProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, discFrag)
        && m_discProgram->link()
        && m_gooProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kGlassVertexSrc)
        && m_gooProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, gooFrag)
        && m_gooProgram->link();

    if (!ok) {
        qWarning("[BackdropBlur] shader build failed: %s / %s / %s / %s",
                 qPrintable(m_blurProgram->log()),
                 qPrintable(m_downProgram->log()),
                 qPrintable(m_compositeProgram->log()),
                 qPrintable(m_glassProgram->log()));
        qWarning("[BackdropBlur] glass shape shader: %s / %s",
                 qPrintable(m_discProgram->log()), qPrintable(m_gooProgram->log()));
        delete m_blurProgram;      m_blurProgram = nullptr;
        delete m_downProgram;      m_downProgram = nullptr;
        delete m_compositeProgram; m_compositeProgram = nullptr;
        delete m_glassProgram;     m_glassProgram = nullptr;
        delete m_discProgram;      m_discProgram = nullptr;
        delete m_gooProgram;       m_gooProgram = nullptr;
        return false;
    }
    qDebug("[BackdropBlur] linear-light blur: %s (PB_BLUR_LINEAR)",
           linearizeFlag() > 0.5f ? "on" : "off");
    return true;
}

void BackdropBlurGL::initGeometry()
{
    static const float kQuad[8] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
}

bool BackdropBlurGL::makeTarget(GLuint *tex, GLuint *fbo, int w, int h,
                                GLenum internalFormat)
{
    if (*tex) { glDeleteTextures(1, tex); *tex = 0; }
    if (*fbo) { glDeleteFramebuffers(1, fbo); *fbo = 0; }
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // GL_RGBA16FでもtypeはGL_FLOATでなくてよい(データはnullptr)。
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), w, h, 0,
                 GL_RGBA, internalFormat == GL_RGBA16F ? GL_FLOAT : GL_UNSIGNED_BYTE,
                 nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, *tex, 0);
    return glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

bool BackdropBlurGL::ensureTargets(int resolveW, int resolveH, int halfW, int halfH,
                                   int quarterW, int quarterH, int blurW, int blurH)
{
    // 島の幅アニメ中に毎フレーム再確保しないよう、64px単位へ切り上げて
    // 余裕を持たせる(GlassHighlightEffectと同じ方針)。
    // 丸め単位はサイズに応じて粗くする（allocQuantumFor参照）。
    const auto quantize = [](int v) { return roundUpTo(v, allocQuantumFor(v)); };
    const int needResolveW = quantize(resolveW);
    const int needResolveH = quantize(resolveH);
    const int needHalfW    = quantize(halfW);
    const int needHalfH    = quantize(halfH);
    const int needQuarterW = quantize(quarterW);
    const int needQuarterH = quantize(quarterH);
    const int needBlurW    = quantize(blurW);
    const int needBlurH    = quantize(blurH);

    const bool resolveOk =
        m_resolveTex && m_resolveFbo
        && m_resolveW >= needResolveW && m_resolveH >= needResolveH;
    const bool halfOk =
        m_halfTex && m_halfFbo
        && m_halfW >= needHalfW && m_halfH >= needHalfH;
    const bool quarterOk =
        m_quarterTex && m_quarterFbo
        && m_quarterW >= needQuarterW && m_quarterH >= needQuarterH;
    const bool blurOk =
        m_pingTex && m_pingFbo && m_pongTex && m_pongFbo
        && m_blurW >= needBlurW && m_blurH >= needBlurH;
    if (resolveOk && halfOk && quarterOk && blurOk)
        return true;

    ++m_statAllocs; // 計測用（renderAll参照）。

    GLint prevDrawFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);

    // 2026-09-12: 1/2以降はリニア空間の値を持つためGL_RGBA16Fにする。
    // リニア値を8bitで持つと暗部の階調が足りず、ぼかしの平坦な部分に
    // バンディングが出る。等倍resolveだけは元の生の値(sRGB)なのでRGBA8のまま。
    constexpr GLenum kRawFormat = GL_RGBA8;
    constexpr GLenum kLinFormat = GL_RGBA16F;

    bool ok = true;
    if (!resolveOk) {
        ok = makeTarget(&m_resolveTex, &m_resolveFbo, needResolveW, needResolveH,
                        kRawFormat) && ok;
        m_resolveW = needResolveW;
        m_resolveH = needResolveH;
    }
    if (!halfOk) {
        ok = makeTarget(&m_halfTex, &m_halfFbo, needHalfW, needHalfH,
                        kLinFormat) && ok;
        m_halfW = needHalfW;
        m_halfH = needHalfH;
    }
    if (!quarterOk) {
        ok = makeTarget(&m_quarterTex, &m_quarterFbo, needQuarterW, needQuarterH,
                        kLinFormat) && ok;
        m_quarterW = needQuarterW;
        m_quarterH = needQuarterH;
    }
    if (!blurOk) {
        ok = makeTarget(&m_pingTex, &m_pingFbo, needBlurW, needBlurH,
                        kLinFormat) && ok;
        ok = makeTarget(&m_pongTex, &m_pongFbo, needBlurW, needBlurH,
                        kLinFormat) && ok;
        m_blurW = needBlurW;
        m_blurH = needBlurH;
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDrawFbo));

    if (!ok) {
        qWarning("[BackdropBlur] FBO incomplete");
        release();
        m_initFailed = true;
        return false;
    }
    qDebug("[BackdropBlur] targets resolve=%dx%d half=%dx%d quarter=%dx%d blur=%dx%d",
           m_resolveW, m_resolveH, m_halfW, m_halfH,
           m_quarterW, m_quarterH, m_blurW, m_blurH);
    return true;
}

void BackdropBlurGL::release()
{
    if (m_resolveFbo) { glDeleteFramebuffers(1, &m_resolveFbo); m_resolveFbo = 0; }
    if (m_halfFbo)    { glDeleteFramebuffers(1, &m_halfFbo);    m_halfFbo = 0; }
    if (m_quarterFbo) { glDeleteFramebuffers(1, &m_quarterFbo); m_quarterFbo = 0; }
    if (m_pingFbo)    { glDeleteFramebuffers(1, &m_pingFbo);    m_pingFbo = 0; }
    if (m_pongFbo)    { glDeleteFramebuffers(1, &m_pongFbo);    m_pongFbo = 0; }
    if (m_resolveTex) { glDeleteTextures(1, &m_resolveTex);     m_resolveTex = 0; }
    if (m_halfTex)    { glDeleteTextures(1, &m_halfTex);        m_halfTex = 0; }
    if (m_quarterTex) { glDeleteTextures(1, &m_quarterTex);     m_quarterTex = 0; }
    if (m_pingTex)    { glDeleteTextures(1, &m_pingTex);        m_pingTex = 0; }
    if (m_pongTex)    { glDeleteTextures(1, &m_pongTex);        m_pongTex = 0; }
    if (m_vbo)        { glDeleteBuffers(1, &m_vbo);             m_vbo = 0; }
    if (m_vao)        { glDeleteVertexArrays(1, &m_vao);        m_vao = 0; }
    delete m_blurProgram;      m_blurProgram = nullptr;
    delete m_downProgram;      m_downProgram = nullptr;
    delete m_compositeProgram; m_compositeProgram = nullptr;
    delete m_glassProgram;     m_glassProgram = nullptr;
    delete m_discProgram;      m_discProgram = nullptr;
    delete m_gooProgram;       m_gooProgram = nullptr;
    m_uniforms.clear();
    m_resolveW = m_resolveH = 0;
    m_halfW = m_halfH = 0;
    m_quarterW = m_quarterH = 0;
    m_blurW = m_blurH = 0;
    m_initialized = false;
}

// ---- 描画 -------------------------------------------------------------------

void BackdropBlur::renderAll(QWidget *canvas, qreal dpr, int physW, int physH,
                             unsigned int sceneTexture)
{
    if (m_entries.isEmpty() || !canvas || physW <= 0 || physH <= 0)
        return;
    if (!m_gl)
        m_gl = new BackdropBlurGL();
    m_gl->renderAll(*this, canvas, dpr, physW, physH,
                    static_cast<GLuint>(sceneTexture));
}

void BackdropBlur::blurNow(qreal dpr, int physW, int physH, const QRectF &canvasRect,
                           qreal cornerRadius, qreal blurRadius, qreal opacity)
{
    if (physW <= 0 || physH <= 0 || blurRadius <= 0.0 || opacity <= 0.004)
        return;
    if (canvasRect.width() <= 0.0 || canvasRect.height() <= 0.0)
        return;
    if (!m_gl)
        m_gl = new BackdropBlurGL();
    m_gl->blurNowImpl(dpr, physW, physH, canvasRect, cornerRadius, blurRadius, opacity);
}

void BackdropBlur::releaseGL()
{
    if (!m_gl)
        return;
    m_gl->release();
    delete m_gl;
    m_gl = nullptr;
}

void BackdropBlurGL::renderAll(BackdropBlur &owner, QWidget *canvas, qreal dpr,
                               int physW, int physH, GLuint sceneTexture)
{
    if (!ensureInitialized())
        return;

    GLint prevReadFbo = 0, prevDrawFbo = 0;
    GLint prevViewport[4] = {0, 0, 0, 0};
    GLint prevScissor[4]  = {0, 0, 0, 0};
    const GLboolean prevScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glGetIntegerv(GL_SCISSOR_BOX, prevScissor);

    // 2026-09-12: 計測（メンバのコメント参照）。
    static const bool kProfileWithSync =
        !qEnvironmentVariableIsEmpty("PB_BLUR_PROFILE");
    const int regionsBefore = m_statRegions;
    m_statFrame.restart();

    for (BackdropBlur::Entry &e : owner.m_entries)
        renderOne(e, canvas, dpr, physW, physH, sceneTexture);

    // 2026-09-16: ガラスの板は、ぼかしを全部描き終えてから重ねる。
    // 1領域ずつ「ぼかし→板」とすると、後から処理する領域のぼかしが、
    // 先に描いた板を材料として拾ってしまうため（領域が近いとき）。
    for (const BackdropBlur::Entry &e : owner.m_entries) {
        if (!e.renderedValid || !e.renderedGlass.enabled)
            continue;
        if (e.renderedGlass.shape == BackdropBlur::GlassPlateStyle::Shape::RoundedRect)
            renderGlassPlate(e, dpr, physW, physH);
        else
            renderGlassShape(e, dpr, physW, physH);
    }

    const double cpuMs = m_statFrame.nsecsElapsed() / 1e6;
    double syncMs = cpuMs;
    if (kProfileWithSync) {
        glFinish();
        syncMs = m_statFrame.nsecsElapsed() / 1e6;
    }
    if (m_statRegions > regionsBefore) {
        ++m_statFrames;
        m_statCpuMsSum  += cpuMs;
        m_statSyncMsSum += syncMs;
        m_statCpuMsMax = qMax(m_statCpuMsMax, cpuMs);
    }
    if (!m_statWindow.isValid())
        m_statWindow.start();
    if (m_statWindow.elapsed() >= 2000) {
        if (m_statFrames > 0) {
            qDebug("[BackdropBlur][stat] frames=%d regions/frame=%.1f "
                   "cpu=%.2fms(max %.2f) %s "
                   "area/frame=%.2fMpx maxRegion=%.2fMpx allocs=%d",
                   m_statFrames,
                   static_cast<double>(m_statRegions) / m_statFrames,
                   m_statCpuMsSum / m_statFrames, m_statCpuMsMax,
                   kProfileWithSync
                       ? qPrintable(QStringLiteral("gpu-wait込み=%1ms")
                             .arg(m_statSyncMsSum / m_statFrames, 0, 'f', 2))
                       : "(gpu-wait未計測: PB_BLUR_PROFILE=1で有効)",
                   static_cast<double>(m_statAreaPx) / m_statFrames / 1e6,
                   static_cast<double>(m_statMaxRegionPx) / 1e6,
                   m_statAllocs);
        }
        m_statWindow.restart();
        m_statCpuMsSum = m_statSyncMsSum = m_statCpuMsMax = 0.0;
        m_statFrames = m_statRegions = 0;
        m_statAreaPx = 0;
        m_statMaxRegionPx = 0;
        m_statAllocs = 0;
    }

    // Renderer側の既定状態へ戻す。
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDrawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevReadFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevScissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(prevScissor[0], prevScissor[1], prevScissor[2], prevScissor[3]);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void BackdropBlurGL::renderOne(BackdropBlur::Entry &entry, QWidget *canvas, qreal dpr,
                             int physW, int physH, GLuint sceneTexture)
{
    // このフレームで実際に描けなかった場合は、記録を無効化しておく。
    // 次にsetShape()が呼ばれたとき(=ウィジェットが再表示されて描き直された
    // とき)に、形が同じでも再描画が要求されるようにするため。
    const auto skip = [&entry] { entry.renderedValid = false; };

    QWidget *owner = entry.owner.data();
    // 2026-09-16: ぼかし無しでガラスの形だけを描くエントリ（丸ボタン・液体）も通す。
    const bool glassOnly = entry.blurRadius <= 0.0 && entry.glass.enabled;
    if (!owner || !owner->isVisible() || (entry.blurRadius <= 0.0 && !glassOnly))
        return skip();
    if (glassOnly && entry.opacity <= 0.004)
        return skip();
    if (entry.localRect.width() <= 0.0 || entry.localRect.height() <= 0.0)
        return skip();
    if (owner->window() != canvas->window())
        return skip();
    // QGraphicsOpacityEffectで消えている間は、島本体が見えていないのに
    // 背景だけがぼけた状態になってしまうためスキップする。
    if (auto *fx = qobject_cast<QGraphicsOpacityEffect *>(owner->graphicsEffect())) {
        if (!fx->isEnabled() || fx->opacity() <= 0.02)
            return skip();
    }

    // ownerローカル座標 → キャンバスローカル座標(論理px)
    const QPoint originInWindow = owner->mapTo(owner->window(), QPoint(0, 0));
    const QPoint originInCanvas = canvas->mapFrom(canvas->window(), originInWindow);
    const QRectF rectLocal = entry.localRect.translated(originInCanvas);

    // 不透明度は呼び出し側がsetShape()で渡した値をそのまま使う。
    // QGraphicsOpacityEffectを使うウィジェットも、そのopacity()を渡して
    // もらう前提(ここで自動的に掛けると二重掛けになる)。
    if (!glassOnly
        && !renderRect(rectLocal, entry.cornerRadius, entry.blurRadius,
                       entry.opacity, dpr, physW, physH, sceneTexture)) {
        return skip();
    }

    // このフレームで実際に描いた状態を記録する(setShape()の比較対象)。
    entry.renderedValid   = true;
    entry.renderedRect    = entry.localRect;
    entry.renderedCorner  = entry.cornerRadius;
    entry.renderedBlur    = entry.blurRadius;
    entry.renderedOpacity = entry.opacity;
    entry.renderedGlass   = entry.glass;
    entry.renderedCanvasRect = rectLocal;
}

void BackdropBlurGL::renderGlassShape(const BackdropBlur::Entry &entry, qreal dpr,
                                      int physW, int physH)
{
    using Shape = BackdropBlur::GlassPlateStyle::Shape;
    const BackdropBlur::GlassPlateStyle &style = entry.renderedGlass;
    QOpenGLShaderProgram *program =
        (style.shape == Shape::Disc) ? m_discProgram : m_gooProgram;
    if (!program || dpr <= 0.0)
        return;
    const QRectF rect = entry.renderedCanvasRect;
    if (rect.width() <= 0.0 || rect.height() <= 0.0)
        return;
    const qreal opacity = qBound(0.0, style.opacity >= 0.0 ? style.opacity
                                                           : entry.renderedOpacity, 1.0);
    if (opacity <= 0.004)
        return;

    const auto premul = [](const QColor &c) {
        const float a = static_cast<float>(c.alphaF());
        return QVector4D(static_cast<float>(c.redF()) * a,
                         static_cast<float>(c.greenF()) * a,
                         static_cast<float>(c.blueF()) * a, a);
    };
    const auto rgb = [](const QColor &c) {
        return QVector3D(static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
                         static_cast<float>(c.blueF()));
    };
    const std::array<float, 20> &pr = style.params;

    glViewport(0, 0, physW, physH);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(m_vao);

    program->bind();
    // 描く範囲はウィジェットの矩形そのもの（QWidgetのときも、その外へは
    // 描けなかったので同じ）。
    program->setUniformValue(m_uniforms.get(program, "uQuad"), QVector4D(static_cast<float>(rect.x()), static_cast<float>(rect.y()),
                           static_cast<float>(rect.width()),
                           static_cast<float>(rect.height())));
    program->setUniformValue(m_uniforms.get(program, "uViewLogical"), QVector2D(static_cast<float>(physW / dpr),
                                  static_cast<float>(physH / dpr)));
    program->setUniformValue(m_uniforms.get(program, "uRectOrigin"), QVector2D(static_cast<float>(rect.x()),
                                 static_cast<float>(rect.y())));
    program->setUniformValue(m_uniforms.get(program, "uAa"), static_cast<float>(1.0 / dpr));
    program->setUniformValue(m_uniforms.get(program, "uOpacity"), static_cast<float>(opacity));

    if (style.shape == Shape::Disc) {
        // params: 0 cx / 1 cy / 2 r / 3 scale / 4 shadowR / 5 shadowCy
        program->setUniformValue(m_uniforms.get(program, "uCenter"), QVector2D(pr[0], pr[1]));
        program->setUniformValue(m_uniforms.get(program, "uR"), pr[2]);
        program->setUniformValue(m_uniforms.get(program, "uScale"), pr[3]);
        program->setUniformValue(m_uniforms.get(program, "uShadowR"), pr[4]);
        program->setUniformValue(m_uniforms.get(program, "uShadowCy"), pr[5]);
        program->setUniformValue(m_uniforms.get(program, "uTint"), premul(style.body));
        program->setUniformValue(m_uniforms.get(program, "uShade"), rgb(style.shade));
    } else {
        // params: 0 L / 1 R / 2 hh / 3 taper / 4 cx / 5 cy / 6 r / 7 k / 8 sink
        //         9 gx0 / 10 gx1 / 11 shadowBase / 12-15 bb(l,t,r,b)
        //         16 sideS / 17 sideRightW
        // 色: body=ボタンの色 / shade=翳り。雫側の面の色と輪郭線の色は
        //     sink から導く（IslandTabGoo::paintSurface と同じ式）。
        const float sink = qBound(0.0f, pr[8], 1.0f);
        const auto mixColor = [](const QColor &a, const QColor &b, double t) {
            t = qBound(0.0, t, 1.0);
            const auto m = [t](int x, int y) { return qRound(x + (y - x) * t); };
            return QColor(m(a.red(), b.red()), m(a.green(), b.green()),
                          m(a.blue(), b.blue()), m(a.alpha(), b.alpha()));
        };
        const QColor fillRight = mixColor(style.body, QColor(0, 0, 0, 58), sink);
        const QColor edgeDrop  = mixColor(style.shade, QColor(20, 20, 24), sink);
        program->setUniformValue(m_uniforms.get(program, "uL"), pr[0]);
        program->setUniformValue(m_uniforms.get(program, "uRgt"), pr[1]);
        program->setUniformValue(m_uniforms.get(program, "uHh"), pr[2]);
        program->setUniformValue(m_uniforms.get(program, "uTaper"), pr[3]);
        program->setUniformValue(m_uniforms.get(program, "uCx"), pr[4]);
        program->setUniformValue(m_uniforms.get(program, "uCy"), pr[5]);
        program->setUniformValue(m_uniforms.get(program, "uBr"), pr[6]);
        program->setUniformValue(m_uniforms.get(program, "uK"), pr[7]);
        program->setUniformValue(m_uniforms.get(program, "uSink"), sink);
        program->setUniformValue(m_uniforms.get(program, "uGx0"), pr[9]);
        program->setUniformValue(m_uniforms.get(program, "uGx1"), pr[10]);
        program->setUniformValue(m_uniforms.get(program, "uShadowBase"), pr[11]);
        program->setUniformValue(m_uniforms.get(program, "uBB"), QVector4D(pr[12], pr[13], pr[14], pr[15]));
        program->setUniformValue(m_uniforms.get(program, "uSideS"), pr[16]);
        program->setUniformValue(m_uniforms.get(program, "uSideRightW"), pr[17]);
        program->setUniformValue(m_uniforms.get(program, "uTint"), premul(style.body));
        program->setUniformValue(m_uniforms.get(program, "uFillRight"), premul(fillRight));
        program->setUniformValue(m_uniforms.get(program, "uShade"), rgb(style.shade));
        program->setUniformValue(m_uniforms.get(program, "uEdgeBaseDrop"), rgb(edgeDrop));
    }
    GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    program->release();
    glBindVertexArray(0);
}

void BackdropBlurGL::renderGlassPlate(const BackdropBlur::Entry &entry, qreal dpr,
                                      int physW, int physH)
{
    if (!m_glassProgram || dpr <= 0.0)
        return;
    const QRectF rect = entry.renderedCanvasRect;
    if (rect.width() <= 0.0 || rect.height() <= 0.0)
        return;
    const BackdropBlur::GlassPlateStyle &style = entry.renderedGlass;
    const qreal opacity = qBound(0.0, style.opacity >= 0.0 ? style.opacity
                                                           : entry.renderedOpacity, 1.0);
    if (opacity <= 0.004)
        return;

    // 帯の作り方（GlassPlateStyle::Profile参照）。
    const double innerH = qMax(1.0, rect.height());
    double baseH = style.baseH;
    QVector4D lightSpan;
    float sideSpan = 0.0f;
    int lightCount = 12, sideCount = 8;
    float lightDiv = 12.0f, sideDiv = 12.0f;
    if (style.profile == BackdropBlur::GlassPlateStyle::Profile::Island) {
        // 島: 2026-09-14に全ての帯を span=baseH*0.045 へ統一し、太さの系列
        // (1 - i/12) を共有している。翳りはその先頭8本。
        const float span = static_cast<float>(baseH * 0.045);
        lightSpan = QVector4D(span, span, span, span);
        sideSpan = span;
    } else {
        // 動画UI（paintGlassPlate）: 基準高さは板の高さ（上限42）。帯は
        // 板が高い(>72px)と6本。本数を減らしたときは太さの系列もその本数で
        // 割る（strokeBands の count = min(要求, bandCount)）。
        baseH = qMin(innerH, 42.0);
        const int bandCount = (innerH > 72.0) ? 6 : 12;
        lightCount = bandCount;
        lightDiv = static_cast<float>(bandCount);
        sideCount = qMin(8, bandCount);
        sideDiv = static_cast<float>(sideCount);
        lightSpan = QVector4D(static_cast<float>(baseH * 0.045),
                              static_cast<float>(baseH * 0.035),
                              static_cast<float>(baseH * 0.05),
                              static_cast<float>(baseH * 0.04));
        sideSpan = static_cast<float>(baseH * 0.05);
    }
    // 輪郭線がはみ出す0.5px＋アンチエイリアスぶんの余白。
    const QRectF quad = rect.adjusted(-2.0, -2.0, 2.0, 2.0);
    const float bodyA = static_cast<float>(style.body.alphaF());

    // renderRect() は終了時に元のdraw FBOへ戻しているので、そのまま描く。
    glViewport(0, 0, physW, physH);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(m_vao);

    m_glassProgram->bind();
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uQuad"), QVector4D(static_cast<float>(quad.x()), static_cast<float>(quad.y()),
                           static_cast<float>(quad.width()),
                           static_cast<float>(quad.height())));
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uViewLogical"), QVector2D(static_cast<float>(physW / dpr),
                                  static_cast<float>(physH / dpr)));
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uRectOrigin"), QVector2D(static_cast<float>(rect.x()),
                                 static_cast<float>(rect.y())));
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uSize"), QVector2D(static_cast<float>(rect.width()),
                           static_cast<float>(rect.height())));
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uRadius"), static_cast<float>(qMin(entry.renderedCorner,
                                           qMin(rect.width(), rect.height()) * 0.5)));
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uAa"), static_cast<float>(1.0 / dpr));
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uBody"), QVector4D(static_cast<float>(style.body.redF()) * bodyA,
                           static_cast<float>(style.body.greenF()) * bodyA,
                           static_cast<float>(style.body.blueF()) * bodyA,
                           bodyA));
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uShade"), QVector3D(static_cast<float>(style.shade.redF()),
                            static_cast<float>(style.shade.greenF()),
                            static_cast<float>(style.shade.blueF())));
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uBaseH"), static_cast<float>(baseH));
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uLightSpan"), lightSpan);
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uSideSpan"), sideSpan);
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uLightCount"), lightCount);
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uLightDiv"), lightDiv);
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uSideCount"), sideCount);
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uSideDiv"), sideDiv);
    m_glassProgram->setUniformValue(m_uniforms.get(m_glassProgram, "uOpacity"), static_cast<float>(opacity));
    GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_glassProgram->release();
    glBindVertexArray(0);
}

void BackdropBlurGL::blurNowImpl(qreal dpr, int physW, int physH,
                                 const QRectF &canvasRect, qreal cornerRadius,
                                 qreal blurRadius, qreal opacity)
{
    if (!ensureInitialized())
        return;

    GLint prevReadFbo = 0, prevDrawFbo = 0;
    GLint prevViewport[4] = {0, 0, 0, 0};
    GLint prevScissor[4]  = {0, 0, 0, 0};
    GLint prevProgram = 0;
    const GLboolean prevScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glGetIntegerv(GL_SCISSOR_BOX, prevScissor);
    // 描画途中(Rendererが自分のプログラムを使っている最中)から呼ばれるため、
    // 使用中のシェーダープログラムも保存して戻す。
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);

    renderRect(canvasRect, cornerRadius, blurRadius, opacity, dpr, physW, physH);

    // Renderer側の既定状態へ戻す(描画途中から呼ばれるので特に重要)。
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDrawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevReadFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevScissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(prevScissor[0], prevScissor[1], prevScissor[2], prevScissor[3]);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(static_cast<GLuint>(prevProgram));
}

bool BackdropBlurGL::renderRect(const QRectF &rectLocal, qreal cornerRadius,
                                qreal blurRadius, qreal opacity, qreal dpr,
                                int physW, int physH, GLuint sceneTexture)
{
    if (opacity <= 0.004 || blurRadius <= 0.0)
        return false;

    // 対象矩形(物理px, Qt座標=上原点)
    const int x    = static_cast<int>(std::floor(rectLocal.left() * dpr));
    const int yTop = static_cast<int>(std::floor(rectLocal.top() * dpr));
    const int w    = static_cast<int>(std::ceil(rectLocal.width() * dpr));
    const int h    = static_cast<int>(std::ceil(rectLocal.height() * dpr));
    if (w <= 0 || h <= 0)
        return false;
    if (x >= physW || yTop >= physH || x + w <= 0 || yTop + h <= 0)
        return false;

    // ぼかしは矩形の外側も材料にするため、半径ぶん広げてキャプチャする。
    const int pad = static_cast<int>(std::ceil(blurRadius * dpr)) + 2;
    int capX    = x - pad;
    int capYTop = yTop - pad;
    int capW    = w + pad * 2;
    int capH    = h + pad * 2;
    if (capX < 0)    { capW += capX;    capX = 0; }
    if (capYTop < 0) { capH += capYTop; capYTop = 0; }
    capW = qMin(capW, physW - capX);
    capH = qMin(capH, physH - capYTop);
    if (capW <= 0 || capH <= 0)
        return false;

    // ---- 縮小グリッドを画面に固定し、各段をきっかり1/2にする ----
    //
    // (1) 原点をkDownsampleの倍数へ落とす。
    //     原点が1pxでも動くと、1/8に縮小したときの各テクセルが拾う8x8画素が
    //     まるごと入れ替わるため、対象が動くたびに結果が飛んでガタガタする。
    //     原点を倍数へ丸めれば、対象が動いてもグリッドは画面に固定される。
    // (2) 幅と高さもkDownsampleの倍数へ切り下げる。
    //     端数があるとhalf/quarter/downの各サイズが整数除算で切り捨てられ、
    //     blitの縮小比がきっかり2にならない。するとUV計算との間に誤差が生じ、
    //     領域が大きいほど像がずれていく(カメラで寄るほどずれが目立つ)。
    //     切り下げで領域はわずかに縮むが、ぼかし半径ぶんのpadが吸収する。
    int capYGL = physH - (capYTop + capH);
    {
        const int alignX = capX   % kDownsample;
        capX   -= alignX; capW += alignX;
        const int alignY = capYGL % kDownsample;
        capYGL -= alignY; capH += alignY;

        capW -= capW % kDownsample;
        capH -= capH % kDownsample;
        if (capW < kDownsample || capH < kDownsample)
            return false;
        // 高さを切り下げた分は上端が下がる(下端=capYGLは固定)。
        capYTop = physH - capYGL - capH;
    }

    // 対象矩形自体もビューポート内へ収める(はみ出し分はぼかさない)。
    int dstX = qMax(x, capX);
    int dstY = qMax(yTop, capYTop);
    int dstW = qMin(x + w, capX + capW) - dstX;
    int dstH = qMin(yTop + h, capYTop + capH) - dstY;
    if (dstW <= 0 || dstH <= 0)
        return false;

    const int halfW = qMax(1, capW / 2);
    const int halfH = qMax(1, capH / 2);
    const int quarterW = qMax(1, capW / 4);
    const int quarterH = qMax(1, capH / 4);
    const int downW = qMax(1, capW / kDownsample);
    const int downH = qMax(1, capH / kDownsample);
    if (!ensureTargets(capW, capH, halfW, halfH, quarterW, quarterH, downW, downH))
        return false;

    // 計測用（renderAll参照）。等倍resolveする面積がこの段の主なコスト。
    ++m_statRegions;
    m_statAreaPx += static_cast<qint64>(capW) * capH;
    m_statMaxRegionPx = qMax(m_statMaxRegionPx, capW * capH);

    const int dstYGL = physH - (dstY + dstH);
    // マスクに使う「切り詰める前の」矩形(画面外へはみ出していてもそのまま)。
    const int fullYGL = physH - (yTop + h);

    GLint currentDrawFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &currentDrawFbo);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    // 1段目: MSAAの元FBOから等倍でresolve。MSAA resolveは同寸法かつNEAREST限定。
    // 2026-09-16: シーンレイヤーのテクスチャを渡されたときは、それが既に
    // 単一サンプルの「UIより下の完成品」なので、ここを飛ばして直接読む。
    GLuint downSource = m_resolveTex;
    QPoint fetchOffset(0, 0);
    if (sceneTexture) {
        downSource = sceneTexture;
        fetchOffset = QPoint(capX, capYGL);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(currentDrawFbo));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFbo);
        glBlitFramebuffer(capX, capYGL, capX + capW, capYGL + capH,
                          0, 0, capW, capH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    // 2段目: 1/4への縮小。ただし一気に1/4へ落とすと、線形補間が参照するのは
    // 4x4ブロックのうち中央2x2だけになり、残り12画素が捨てられる。その結果、
    // 下の内容が動くたびに拾う画素が入れ替わり、出力がチラチラ揺れる
    // (=「下のものが動かされるとぶるぶるする」の原因)。
    // 1/2の縮小を2回重ねれば各段が正しく2x2平均になり、合計で4x4平均に
    // なるので、下が動いても値が安定する。
    //
    // 2026-09-12: 最初の1/2だけはblitではなくシェーダーで行う。
    // 平均する前に1画素ずつsRGB->リニアへ直す必要があり、blitのバイリニアでは
    // それができないため(kDownFragSrc参照)。2x2平均であることは変わらない。
    glBindVertexArray(m_vao);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_halfFbo);
    glViewport(0, 0, halfW, halfH);
    m_downProgram->bind();
    m_downProgram->setUniformValue(m_uniforms.get(m_downProgram, "uTex"), 0);
    m_downProgram->setUniformValue(m_uniforms.get(m_downProgram, "uLinearize"), linearizeFlag());
    m_downProgram->setUniformValue(m_uniforms.get(m_downProgram, "uViewportPx"),
                                   QVector2D(static_cast<float>(halfW),
                                             static_cast<float>(halfH)));
    m_downProgram->setUniformValue(m_uniforms.get(m_downProgram, "uRectPx"),
                                   QVector4D(0.0f, 0.0f,
                                             static_cast<float>(halfW),
                                             static_cast<float>(halfH)));
    m_downProgram->setUniformValue(m_uniforms.get(m_downProgram, "uUvRect"), QVector4D(0.0f, 0.0f, 1.0f, 1.0f));
    m_downProgram->setUniformValue(m_uniforms.get(m_downProgram, "uLocalOrigin"), QVector2D(0.0f, 0.0f));
    m_downProgram->setUniformValue(m_uniforms.get(m_downProgram, "uFetchOffset"), QVector2D(static_cast<float>(fetchOffset.x()),
                                  static_cast<float>(fetchOffset.y())));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, downSource);
    GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_downProgram->release();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_halfFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_quarterFbo);
    glBlitFramebuffer(0, 0, halfW, halfH,
                      0, 0, quarterW, quarterH,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_quarterFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_pingFbo);
    glBlitFramebuffer(0, 0, quarterW, quarterH,
                      0, 0, downW, downH,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // 3段目: 分離ガウスを縦横で1回ずつ。
    const float texW = static_cast<float>(m_blurW);
    const float texH = static_cast<float>(m_blurH);
    const float dataU = downW / texW;
    const float dataV = downH / texH;
    // 9タップ(±4)で半径ぶんをカバーするようタップ間隔を決める。
    const float tapTexels =
        static_cast<float>(blurRadius * dpr / kDownsample / 4.0);

    m_blurProgram->bind();
    m_blurProgram->setUniformValue(m_uniforms.get(m_blurProgram, "uTex"), 0);
    m_blurProgram->setUniformValue(m_uniforms.get(m_blurProgram, "uViewportPx"),
                                   QVector2D(static_cast<float>(downW),
                                             static_cast<float>(downH)));
    m_blurProgram->setUniformValue(m_uniforms.get(m_blurProgram, "uRectPx"),
                                   QVector4D(0.0f, 0.0f,
                                             static_cast<float>(downW),
                                             static_cast<float>(downH)));
    m_blurProgram->setUniformValue(m_uniforms.get(m_blurProgram, "uUvRect"), QVector4D(0.0f, 0.0f, dataU, dataV));
    m_blurProgram->setUniformValue(m_uniforms.get(m_blurProgram, "uLocalOrigin"), QVector2D(0.0f, 0.0f));
    // 半texel内側までをサンプル可能範囲とする。
    m_blurProgram->setUniformValue(m_uniforms.get(m_blurProgram, "uUvClamp"), QVector4D(0.5f / texW, 0.5f / texH,
                              dataU - 0.5f / texW, dataV - 0.5f / texH));
    glActiveTexture(GL_TEXTURE0);
    glViewport(0, 0, downW, downH);

    // 縦横1巡を kBlurPasses 回。低解像度側を十分に平滑化しておかないと、
    // 拡大時のバイリニア補間が暴れて「下が動くとぶるぶるする」原因になる。
    for (int pass = 0; pass < kBlurPasses; ++pass) {
        // 横方向: ping -> pong
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_pongFbo);
        glBindTexture(GL_TEXTURE_2D, m_pingTex);
        m_blurProgram->setUniformValue(m_uniforms.get(m_blurProgram, "uStepUv"), QVector2D(tapTexels / texW, 0.0f));
        GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // 縦方向: pong -> ping
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_pingFbo);
        glBindTexture(GL_TEXTURE_2D, m_pongTex);
        m_blurProgram->setUniformValue(m_uniforms.get(m_blurProgram, "uStepUv"), QVector2D(0.0f, tapTexels / texH));
        GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    m_blurProgram->release();

    // 4段目: 角丸マスクを掛けて元のFBOへ書き戻す。
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(currentDrawFbo));
    glViewport(0, 0, physW, physH);
    glEnable(GL_BLEND);
    // dual-source blending。index=1の出力(fragCover)でdstを退かせる。
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC1_ALPHA);

    m_compositeProgram->bind();
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uTex"), 0);
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uViewportPx"),
                                        QVector2D(static_cast<float>(physW),
                                                  static_cast<float>(physH)));
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uRectPx"),
                                        QVector4D(static_cast<float>(dstX),
                                                  static_cast<float>(dstYGL),
                                                  static_cast<float>(dstW),
                                                  static_cast<float>(dstH)));
    // キャプチャ領域内での対象矩形の位置をUVへ変換する。
    const float u0 = static_cast<float>(dstX - capX) / kDownsample / texW;
    const float v0 = static_cast<float>(dstYGL - capYGL) / kDownsample / texH;
    const float du = static_cast<float>(dstW) / kDownsample / texW;
    const float dv = static_cast<float>(dstH) / kDownsample / texH;
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uUvRect"), QVector4D(u0, v0, du, dv));
    // 角丸マスクは「切り詰める前の矩形」で計算する。ここで切り詰めた矩形を
    // 使うと、画面端やカメラで寄ったときに、切り口そのものが角丸として
    // 描かれてしまい境界が崩れる(カメラを寄せるほど円が大きくなり、
    // はみ出す量が増えて症状が目立っていた)。
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uLocalOrigin"), QVector2D(static_cast<float>(dstX - x),
                                  static_cast<float>(dstYGL - fullYGL)));
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uHalfSizePx"), QVector2D(static_cast<float>(w) * 0.5f,
                                 static_cast<float>(h) * 0.5f));
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uRadiusPx"), static_cast<float>(cornerRadius * dpr));
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uOpacity"), static_cast<float>(qBound(0.0, opacity, 1.0)));
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uVibrancy"), kVibrancy);
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uHiKnee"),   kHighlightKnee);
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uHiGain"),   kHighlightGain);
    m_compositeProgram->setUniformValue(m_uniforms.get(m_compositeProgram, "uLinearize"), linearizeFlag());
    glBindTexture(GL_TEXTURE_2D, m_pingTex);
    GpuProfiler::countDraw(), glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_compositeProgram->release();
    glBindVertexArray(0);
    return true;
}
