#pragma once
#include "UniformLocationCache.h"
#include <functional>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QMatrix4x4>
#include <QColor>
#include <QRectF>
#include <QPointF>
#include <QImage>
#include <QFont>
#include <QHash>

// ---- Renderer --------------------------------------------------------
// 全描画をOpenGL 3.3 Coreで統一管理するクラス。
// paintGL()の先頭でbegin()、末尾でend()を呼ぶ。
// 全座標はscreen座標（左上原点、ピクセル単位）で指定する。
// カメラ変換はCanvasWidget側で行い、screen座標に変換してから渡す。
// ----------------------------------------------------------------------
class Renderer : protected QOpenGLFunctions_3_3_Core
{
public:
    Renderer() = default;
    ~Renderer();

    // OpenGLコンテキスト有効時に1回呼ぶ
    bool initialize();

    // フレーム開始/終了
    void begin(int physViewportW, int physViewportH);
    void setLogicalSize(int logicalW, int logicalH); // projection用
    void end();

    // ---- 描画API（全てscreen座標・左上原点・ピクセル単位）----

    // テクスチャ矩形描画
    void drawTexture(GLuint texId, const QRectF &screenRect,
                     float alpha = 1.0f);
    void drawPremultipliedTexture(GLuint texId, const QRectF &screenRect,
                                  float alpha = 1.0f);
    // srcUv版: テクスチャのどの範囲を使うかをUV座標(0..1)の矩形で指定する
    // （サムネイルのアスペクト比保持クロップ描画用。Renderer.cpp参照）。
    void drawTexture(GLuint texId, const QRectF &screenRect,
                     const QRectF &srcUv, float alpha = 1.0f);

    // Texture drawing clipped to an anti-aliased rounded rectangle.
    void drawRoundedTexture(GLuint texId, const QRectF &screenRect,
                            float radius, float alpha = 1.0f);
    void drawRoundedTexture(GLuint texId, const QRectF &screenRect,
                            const QRectF &srcUv, float radius,
                            float alpha = 1.0f);

    // 矩形（塗り・枠線・角丸対応）
    void drawRect(const QRectF &screenRect,
                  const QColor &fill,
                  const QColor &stroke = Qt::transparent,
                  float strokeW = 0.0f,
                  float radius = 0.0f);

    // 垂直グラデーション矩形（上から下へcolorTop→colorBottom）
    void drawGradientRect(const QRectF &screenRect,
                         const QColor &colorTop,
                         const QColor &colorBottom);

    // 2026-07-13: 角丸矩形の外周グロー（本物のグラデーション）。
    // screenRectの縁（角丸半径radius）で最大の濃さになり、外側へ
    // glowWidthピクセルかけて滑らかに減衰して0になる。フラグメント
    // シェーダーで角丸矩形のSDF（符号付き距離）から毎ピクセル減衰を
    // 計算するため、同心ストロークを重ねる方式と違い縞（バンディング）が
    // 一切出ない。アクティブバブルのハイライト用。
    void drawGlowRect(const QRectF &screenRect,
                      const QColor &color,
                      float radius,
                      float glowWidth);

    // 2026-07-16: 角丸矩形の縁に沿った塗りつぶしストローク（SDF方式）。
    // drawRect()のstroke（GL_LINE_LOOP+glLineWidth）と違い、太さをジオメトリ
    // ではなくフラグメントシェーダーの符号付き距離で作るため、glLineWidthの
    // GPU依存クランプ（コアプロファイルで実質1pxになる環境が多い）を受けず、
    // 太さを自由に変えられる。角丸矩形の縁1本を単一ドローコールで描くため、
    // 矩形4辺を個別に塗る方式と違って角で二重に重なって濃く見える問題も出ない。
    // screenRectの外側の縁がストロークの外周になり、そこから内側へthickness
    // 分だけ塗られる（外側の縁の位置は変わらず、太さは内側にだけ伸びる）。
    // featherは内側の縁（本体側）をぼかす／グラデーションにする幅（px）。
    // 0または省略時は1px程度のクッキリしたAAのみ（外側の縁は常にクッキリ）。
    void drawRoundedRectStroke(const QRectF &screenRect,
                               const QColor &color,
                               float radius,
                               float thickness,
                               float feather = 0.0f);

    // 線分
    void drawLine(const QPointF &a, const QPointF &b,
                  const QColor &color, float width = 1.0f);

    // 円（塗りつぶし）
    void drawCircle(const QPointF &center, float radius,
                    const QColor &fill);

    // 2026-07-17: ドットグリッド（フラグメントシェーダー方式）。
    // drawCircle()を交点の数だけ呼ぶ方式（=交点数に比例してVBO再アップロード
    // +ドローコールが増える）だと、カメラを引いて画面内の交点が増えるほど
    // 重くなってしまう。この方式は画面全体を覆う四角形1枚だけを描き、
    // 各ピクセルの色をフラグメントシェーダー内で「そのピクセルに対応する
    // キャンバス座標を最寄りの格子点までの距離」から計算する。そのため
    // ズーム量によらずドローコールは常に1回で済み、Apple Freeform等と
    // 同様に交点数がいくら増えても負荷が変わらない。
    // また、画面上での格子間隔が数px未満に潰れる領域はモアレ（ちらつき）
    // 防止のためなめらかにフェードアウトする（間隔を離散的に間引く方式だと
    // 見た目がガタつくため、連続的なフェードにしている）。
    // screenRectは描画範囲（通常はウィジェット全体）、cameraPos/zoomは
    // CanvasWidgetのカメラ変換パラメータ、spacingCanvasはキャンバス座標系
    // でのグリッド間隔、radiusPxは画面上でのドット半径（px、ズームによらず一定）。
    void drawDotGrid(const QRectF &screenRect,
                     const QPointF &cameraPos, float zoom,
                     float spacingCanvas, float radiusPx,
                     const QColor &color);

    // 三角形（塗りつぶし）。矢印先端等、任意方向を向いた図形に使う
    // （drawTextureと違いテクスチャの回転を実装しなくて済む）。
    void drawTriangle(const QPointF &a, const QPointF &b, const QPointF &c,
                      const QColor &fill);

    // テキスト（QFontでQImageに描いてテクスチャキャッシュ）。
    // scale: ラスタライズ済みテクスチャを等倍以外のサイズで描きたい場合の
    // 拡大率（既定1.0=そのまま）。2026-07-17追加: ズーム倍率に応じて
    // fontのpxサイズ自体をqRound()で丸めて毎フレーム作り直す呼び出し方
    // （フレーム名タイトル等）だと、ズーム中に整数pxが変わるたびにCPUで
    // 再ラスタライズ＋GPUテクスチャ再生成が走り、文字が段階的にジャンプ
    // して見えるカクつきの原因になっていた。固定サイズのfontで一度だけ
    // ラスタライズし、以降はこのscaleだけをズームに追従させて変えれば、
    // 再ラスタライズなしでGPU側の拡大縮小だけで滑らかに追従する
    // （ドットグリッドをシェーダー1発で拡縮する方式と同じ考え方）。
    void drawText(const QString &text, const QPointF &pos,
                  const QFont &font, const QColor &color, float scale = 1.0f);

    // ---- テクスチャ管理 ----
    GLuint createTexture(const QImage &image);
    void   updateTexture(GLuint texId, const QImage &image);
    void   deleteTexture(GLuint texId);

    // ---- クリッピング ----
    // screen座標（左上原点）の矩形でそれ以降の描画を切り抜く。
    // clearClip()を呼ぶまで有効。ネスト非対応。
    void setClipRect(const QRectF &screenRect);
    void clearClip();

    // 2026-07-16: 一時的な「反転（ネガ）」合成モード。
    // 通常はpremultiplied alphaの通常合成(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)だが、
    // その間はGL_ONE_MINUS_DST_COLOR, GL_ZEROに切り替える。これにより
    // dst' = src.rgb × (1 − dst.rgb) となり、白色を描くとその範囲の背景色が
    // 反転して見える（フラグメントのアルファ相当はsrc.rgbの明るさに
    // premultiply済みなので、そのままフェードの強さとして使える）。
    // beginInvertBlend()〜endInvertBlend()の間だけ描画したい図形を挟んで使う。
    // ネスト非対応（setClipRect()と同様、呼び出し側で対にすること）。
    void beginInvertBlend();
    void endInvertBlend();
    // 2026-10-12: 乗算合成（dst' = dst × mix(1, src色, src.a)）。グミの板の上の
    // ランチャー名など、下の色を残したまま暗くしたい文字に使う。end で通常へ戻す。
    void beginMultiplyBlend();
    void endMultiplyBlend();

    // ---- 2026-09-22: バブル融合（BubbleFusion.h）------------------------
    // バブルを別のバブルに重ねると、2つの板（ベゼル）が液体のように
    // つながって見える演出のための「2つ目の形」。
    //
    // 仕組み: 板を描く drawRect / drawGlowRect が、渡された矩形が
    // selfRect と一致するときだけ、角丸矩形の距離を smin(自分, 相手, k) に
    // 置き換える。つながった首の部分にも同じ塗り・縁・グローが乗る。
    //
    // 融合した板（塗り・枠・グロー・影）は、先に描かれる受け側が1回で
    // まとめて描き、上に来る側（drawsShape=false）は板を描かない。
    // 【なぜ半分ずつ受け持たないか】2026-09-22 の初版は「自分の方が近い画素」
    // だけを描く分担にしていたが、上に来る側の板の半透明の白が、先に描かれた
    // 受け側の動画の上へ重なり、分担の境目が動画の上にくっきり出た。
    // 板はすべての動画より下にあるべきなので、最初に1回で描く。
    //
    // 融合していない間（k=0）はシェーダーの結果が従来と完全に同じ。
    struct ShapeFusion {
        QRectF selfRect;       // この矩形で描かれる図形だけに効く（板の矩形）
        QRectF otherRect;      // 相手の板の矩形（screen座標）
        float  otherRadius = 0.0f;
        float  k = 0.0f;       // smin の幅（px）。0で無効
        bool   drawsShape = true;  // false: この板は描かない（相手が描く）
        bool isValid() const {
            return k > 0.01f && selfRect.width() > 1.0 && selfRect.height() > 1.0
                && otherRect.width() > 1.0 && otherRect.height() > 1.0;
        }
    };
    void setShapeFusion(const ShapeFusion &fusion) { m_fusion = fusion; }
    void clearShapeFusion() { m_fusion = ShapeFusion(); }

    // 2026-09-23: 融合中、中身（動画）を溶かす。
    // 【どこが溶けるか】2枚の中身が重なっているところだけ。重なりの外は
    // まったく手を付けない。重なりの中では「どちらの中身により深く入っているか」の
    // 比で上下の中身をクロスフェードし、溶け目をぼかす（Renderer.cpp TEX_FRAG 参照）。
    // 上（後から描く側）だけが薄くなり、下は不透明のまま＝濁らない。
    // drawRoundedTexture / drawTexture の描画にだけ効く。
    struct ContentMelt {
        // どちらも「中身（動画）の矩形」。板ではない（板で測ると、中身の縁の
        // 直線が相手越しに透けて見えてしまう）。
        QRectF  selfRect;         // 自分の中身（screen座標）
        float   selfRadius = 0.0f;
        QRectF  otherRect;        // 相手の中身
        float   otherRadius = 0.0f;
        float   amount = 0.0f;    // 0=無効, 1=最大
        float   blurPx = 14.0f;   // 溶け目でのぼかし半径（画面px）
        bool    selfIsTop = false;// true: 自分が相手の上に描かれる
        bool isValid() const {
            return amount > 0.002f
                && selfRect.width() > 1.0 && selfRect.height() > 1.0
                && otherRect.width() > 1.0 && otherRect.height() > 1.0;
        }
    };
    void setContentMelt(const ContentMelt &melt) { m_melt = melt; }
    // 2026-09-23: 中身（テクスチャ描画）の色をこの割合だけ暗くする（0=そのまま）。
    // 停止中のアクティブ動画に使う。上から黒を塗るのと見た目は同じで、
    // 追加の描画もテクスチャ読み取りも無い（シェーダーで掛け算1回）。
    void setContentDim(float dim) { m_contentDim = dim; }
    void clearContentMelt() { m_melt = ContentMelt(); }

    bool isInitialized() const { return m_initialized; }

    // Ratio between the physical OpenGL viewport and the logical coordinate
    // system used by CanvasItem. Video FBO sizing uses this so a bubble on a
    // high-DPI monitor receives enough physical pixels without involving
    // CanvasWidget in the per-video resolution policy.
    qreal devicePixelRatio() const {
        return m_vpW > 0
            ? static_cast<qreal>(m_physW) / static_cast<qreal>(m_vpW)
            : 1.0;
    }

    // グローバルなアルファ乗数（0〜1）。以降の全ての描画呼び出しの色/アルファに
    // 乗算される。ウィンドウ全画面アニメーション中にキャンバス内容全体を
    // フェードアウト/インさせる用途などに使う。begin()では自動リセットしない
    // ため、呼び出し側（CanvasWidget::paintGL()）が明示的に管理すること。
    void setGlobalAlpha(float alpha) { m_globalAlpha = qBound(0.0f, alpha, 1.0f); }
    float globalAlpha() const { return m_globalAlpha; }

    // 2026-10-12: バブルの枠（板・縁の線・リサイズの印）だけを描く／描かないの切り替え。
    // 枠を焼き付けたテクスチャ（BubbleLayerCache）を使うときに、CanvasItem::paintGL が
    // 枠を二重に描かないよう NoChrome にする。焼き付け時は ChromeOnly で枠だけ描く。
    enum class ChromeMode { All, ChromeOnly, NoChrome };
    void setChromeMode(ChromeMode mode) { m_chromeMode = mode; }
    ChromeMode chromeMode() const { return m_chromeMode; }
    bool drawsChrome() const { return m_chromeMode != ChromeMode::NoChrome; }
    bool drawsContent() const { return m_chromeMode != ChromeMode::ChromeOnly; }

    // 2026-10-12: 画面の一部（logicalRect）を、別のFBO（物理 physW×physH）へ描くための
    // 投影の差し替え。FBO のバインドは呼び出し側。pop で元の投影・ビューポートへ戻す。
    // 入れ子は1段だけ。
    void pushOffscreen(const QRectF &logicalRect, int physW, int physH);
    void popOffscreen();

    // 現在のビューポートの物理サイズ。GLパスの途中でUIの背後をぼかす
    // (BackdropBlur::blurNow)際に必要。
    int physWidth() const  { return m_physW; }
    int physHeight() const { return m_physH; }

private:
    void initShaders();
    void initBuffers();
    void updateProjection(int w, int h);
    void drawTextureInternal(GLuint texId, const QRectF &screenRect,
                             const QRectF &srcUv, float alpha, float radius,
                             bool premultipliedInput = false);

    // テクスチャシェーダー（画像・動画）
    QOpenGLShaderProgram *m_texShader = nullptr;
    GLuint m_texVAO = 0, m_texVBO = 0;

    // カラーシェーダー（図形・線・円）
    QOpenGLShaderProgram *m_colShader = nullptr;
    GLuint m_colVAO = 0, m_colVBO = 0;

    // 頂点カラーシェーダー（グラデーション）
    QOpenGLShaderProgram *m_vcolShader = nullptr;
    GLuint m_vcolVAO = 0, m_vcolVBO = 0;

    // 角丸矩形SDFグローシェーダー（drawGlowRect用。VAO/VBOはm_colVAOを共用）
    QOpenGLShaderProgram *m_glowShader = nullptr;

    // 角丸矩形SDFストロークシェーダー（drawRoundedRectStroke用。VAO/VBOはm_colVAOを共用）
    QOpenGLShaderProgram *m_strokeShader = nullptr;

    // ドットグリッドシェーダー（drawDotGrid用。VAO/VBOはm_colVAOを共用）
    QOpenGLShaderProgram *m_dotGridShader = nullptr;
    // 2026-09-16: 塗り・線・円・三角形を距離関数で描くシェーダー（SHAPE_FRAG）。
    // 縁のアンチエイリアスをMSAAに頼らないため。
    QOpenGLShaderProgram *m_shapeShader = nullptr;
    // 2026-09-16: シェーダー引数の番号を覚えておく（UniformLocationCache.h）
    UniformLocationCache m_uniforms;
    // mode: 0=角丸矩形の塗り 1=角丸矩形の内側の帯 2=線分 3=三角形 4=円
    float physicalLineHalfWidth(float widthPx) const;
    void drawShapeQuad(int mode, const QRectF &bounds, const QColor &color,
                       const std::function<void(QOpenGLShaderProgram *)> &setup);

    // 2026-09-22: 融合（setShapeFusion）。
    ShapeFusion m_fusion;
    ContentMelt m_melt;
    float m_contentDim = 0.0f;
    // rect が融合対象の板なら融合情報を返す（違えば nullptr）。
    const ShapeFusion *fusionFor(const QRectF &rect) const;
    // 融合の uniform を設定する（fusion が nullptr なら無効化）。
    void applyFusionUniforms(QOpenGLShaderProgram *p, const ShapeFusion *fusion);

    // テキストテクスチャキャッシュ（テキスト+フォント+色 → テクスチャ）
    //
    // 再生時間表示（"0:01 / 3:45" 等）のように毎秒中身が変わる文字列がキーになるため、
    // 何も刈り込まずに放置すると使われなくなった過去のキー・テクスチャが際限なく
    // 蓄積し続けてしまう（動画再生を続けるほどメモリが増える主要因だった）。
    // そのため「直近 kTextCacheEvictAfterFrames フレームの間、一度も描画に
    // 使われなかったエントリ」を end() の呼び出しごとに間引く。
    struct TextEntry { GLuint texId = 0; int w = 0, h = 0; quint64 lastUsedFrame = 0; };
    QHash<QString, TextEntry> m_textCache;
    quint64 m_frameCounter = 0;
    static constexpr quint64 kTextCacheEvictAfterFrames = 120; // 目安: 数秒分

    void pruneTextCache();

    QMatrix4x4 m_projection;  // orthographic（screen座標→NDC）
    int  m_vpW = 0, m_vpH = 0;     // 論理ピクセル（projection用）
    int  m_physW = 0, m_physH = 0; // 物理ピクセル（glViewport/glScissor用）
    bool m_initialized = false;
    float m_globalAlpha = 1.0f;
    ChromeMode m_chromeMode = ChromeMode::All;
    struct OffscreenSave {
        QMatrix4x4 projection;
        int vpW = 0, vpH = 0, physW = 0, physH = 0;
        float globalAlpha = 1.0f;
        bool active = false;
    } m_offscreenSave;
};
