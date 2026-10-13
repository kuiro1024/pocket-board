#pragma once
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QMatrix4x4>
#include <QVector2D>
#include <QVector3D>
#include <QRectF>
#include <QPointF>
#include <QHash>
#include <QVector>
#include <QElapsedTimer>
#include "GlassContentSource.h"
#include "UniformLocationCache.h"

// ---- GlassHighlightEffect ---------------------------------------------
// バブル（角丸矩形）の縁に「ガラスらしい」疑似スペキュラハイライトを
// 重ねて描くための専用エフェクトクラス。Rendererとはあえて別クラスに
// 分離している（chordbrushupの単機能クラス分離方針に合わせた新規追加。
// Rendererにこれ以上シェーダー種を増やさないための切り出し）。
//
// 前提: このアプリのバブルは透過ウィンドウ上へのOpenGL直接描画であり、
// デスクトップや背後の他ウィンドウをキャプチャする手段がないため、
// LiquidGlassのような「本物の背景屈折」は原理的に実現できない
// （md/PocketBoard_project_overview_2.md 8章のD3D11レイヤー分離挫折の
// 経緯も参照）。そのため本クラスは背景を屈折させるのではなく、
//   1. 角丸矩形SDFから求めた「縁の外向き法線」と光方向の内積で照明を
//      決める（光源側の辺は辺全体が光り、角で光がなめらかに回り込む。
//      反対側の縁には弱いカウンターリフレクション、側面は暗く沈む）
//   2. 深さ方向は3層: 外周の鋭いリムライン＋ベゼル内部の厚みグロー＋
//      スクリーン境界の内側ライン（ガラス板の「2本の光の帯」を再現）
//   3. 臨界減衰よりわずかに緩めたバネでライト角度を遅延追従させる
//      （「きゅるり」とした動き）
//   4. 品質=Quality時は自FBOキャプチャによる疑似屈折を追加。プロファイルは
//      「レンズ状に湾曲した成形ガラス板」(平坦な中央＋縁の湾曲)の解析形
//      （かつて存在したノイズ法線マップは2026-07-19に全廃）
// を組み合わせて、「ガラスの縁を光が滑っていく」質感を疑似的に再現する。
//
// 2026-07-19 設計変更の経緯: 当初は法線マップの微細凹凸そのものを
// Blinn-Phongスペキュラの主役にしていたが、周波数が高すぎて「砂嵐ノイズ」
// のような見た目になってしまった（実機スクリーンショットで確認済み）。
// 目指す見た目（縁をなぞる1本の滑らかな光の帯＋対角のかすかな反射）に
// 合わせ、法線マップは主役から脇役（帯の中のうっすらしたむら）に格下げし、
// 帯の形そのものは角度パラメータによるマクロな制御に置き換えた。
//
// 使い方（CanvasWidget::paintGL()内、Renderer::begin()〜end()と対で）:
//   1. GLコンテキスト有効時に initialize() を1回呼ぶ
//   2. 毎フレーム: begin(logicalW, logicalH)
//              → バブルごとに draw(itemKey, screenRect, radius, lightDirectionScreen, dt)
//              → end()
//   3. バブル削除時は forget(itemKey) でバネ状態を破棄する（任意。呼ばなくても
//      QHashに残るだけでクラッシュ等はしないが、掃除しておくと綺麗）
// -------------------------------------------------------------------------
class GlassHighlightEffect : protected QOpenGLFunctions_3_3_Core
{
public:
    enum class SurfaceProfile {
        Media,
        Launcher
    };

    GlassHighlightEffect() = default;
    ~GlassHighlightEffect();

    bool initialize();

    void begin(int logicalW, int logicalH);

    // バブル1個ぶんのガラスハイライトを描く。
    //   itemKey           : バネ状態を識別するキー（CanvasItemのthisポインタ等。
    //                        描画対象ごとに毎フレーム安定していれば何でもよい）
    //   screenRect        : バブルの角丸矩形（screen座標。drawGlowRect等と同じ渡し方）
    //   radius            : 角丸半径（px）
    //   bezelPx           : 枠（board縁〜コンテンツ縁）の厚み(px)。内側境界の
    //                        光の線とベゼル内グローの位置決めに使う
    //   lightDirectionScreen : 呼び出し側でズーム補正済みのライト方向
    //   dt                : 前回描画からの経過秒（バネの追従計算に使用）
    //   intensity         : ハイライト全体の強さ（0〜1）
    //   innerMix          : 内側レイヤー（ベゼル内グロー＋内側境界ライン）の
    //                        強さ（0〜1）。動画/画像は1.0、ランチャー等の小さな
    //                        バブルでは内側の線が「四角の中の四角」に見えて
    //                        不自然なため0.0を渡す（外周リムだけ残る）
    //   refractMix        : キャプチャ疑似屈折の強さ（0〜1）。品質=Qualityの
    //                        ときのみ>0を渡す。>0の場合は事前に同フレームで
    //                        captureRegion()を呼んでおくこと。
    //                        設計: md/glass_refraction_design_2026-07-19.md
    void draw(const void *itemKey,
              const QRectF &screenRect, float radius,
              float bezelPx, float contentRadius,
              const QVector2D &lightDirectionScreen,
              double dt, float intensity, float innerMix,
              float refractMix, bool refractionPrepass = false,
              GlassContentSource contentSource = {},
              SurfaceProfile surfaceProfile = SurfaceProfile::Media);

    // ---- キャプチャ疑似屈折（品質=Quality時のみ使用） ----
    // 現在バインドされているdraw FBO（QOpenGLWidgetの内部FBO）から、
    // screenRect（論理px）周辺を内部キャプチャテクスチャへコピーする。
    // アクティブ中または終了フェード中のバブルのdraw()直前に呼ぶ。
    void captureRegion(const QRectF &screenRect, qreal dpr, int physW, int physH);
    // 固定屈折パラメーターで最遠サンプルまで含めるために必要な余白。
    static double requiredCapturePadding(
        double normalScalePx,
        SurfaceProfile profile = SurfaceProfile::Media);
    // 比例ベゼル自体を法線の深さ基準にする。
    static double normalScaleForBezel(double bezelPx);
    // キャプチャテクスチャを解放する（終了フェード完了時、またはQuality以外へ
    // 切り替えた時にメモリを返す。未確保なら何もしない）。
    void releaseCaptureTexture();

    // バブル削除時にバネ状態を破棄する。
    void forget(const void *itemKey);

    // ---- 2026-10-12: まとめ描き（インスタンシング）----
    // draw() はまとめられるとき、その場では描かずに値だけためる（lastDrawWasBatched）。
    // flushBatch() で、ためた全バブルのガラスを1回の命令で描く。呼び出し側は、
    // ためたものと重なる描画をする前と、フレームの最後に必ず flushBatch() を呼ぶこと。
    void flushBatch();
    bool hasPendingBatch() const { return !m_batch.isEmpty(); }
    bool batchIntersects(const QRectF &rect) const;
    bool lastDrawWasBatched() const { return m_lastDrawBatched; }
    void setBatchingEnabled(bool on) { m_batchingEnabled = on; }
    bool batchingEnabled() const { return m_batchingEnabled; }
    // 計測表示用（フレームごとに resetBatchStats で0へ）
    void resetBatchStats() { m_statBatched = m_statSingles = m_statFlushes = 0; }
    int statBatched() const { return m_statBatched; }
    int statSingles() const { return m_statSingles; }
    int statFlushes() const { return m_statFlushes; }

    // デバッグ用(CanvasWidgetのShift+Dでサイクル):
    //   0: 通常 / 1: 実効サーフェス法線の表示 / 2: 屈折レイヤー単体の表示
    //   (2はライティングを完全に外した屈折の生出力。白浮き等の切り分け用)
    void setDebugMode(int mode) { m_debugMode = mode; }
    void setRefractionActivation(float activation);

    // ---- 2026-09-22: バブル融合（BubbleFusion.h / Renderer::ShapeFusion）----
    // 次の draw() から、板の形を「相手の板と smin で合成した形」として扱う。
    // ライティングは2つのうち片方（drawsLighting=true）がまとめて1回描く。
    // 描く側は、両方の動画より後に描かれる方（ドラッグ中は掴んでいる側、
    // 吸い込み中は受け側）。こうするとガラスの光が必ず両方の動画の上に乗り、
    // 境目が出ない。ライト方向は首の部分で相手のバネの値と補間する
    // （otherKey は相手の draw() に渡している itemKey）。
    // 融合を終えたら必ず clearFusion() で戻すこと。
    struct Fusion {
        const void *otherKey = nullptr;
        QRectF otherRect;          // 相手の板（screen座標）
        float  otherRadius = 0.0f;
        float  otherBezelPx = 0.0f;
        float  otherContentRadius = 0.0f;
        float  k = 0.0f;           // smin の幅（px）
        bool   drawsLighting = true;  // false: ライティングは相手が描く
        // 2026-09-23: 屈折プリパスの「穴」。中身が溶けて薄くなっている間、
        // 自分の屈折（とくにベゼル帯の強い歪み・色収差）が相手の中身の上に
        // 透けて境目の線に見えるので、相手の中身の上では屈折を引っ込める。
        QRectF prepassHole;           // 相手の中身の矩形（空なら無効）
        float  prepassHoleAmount = 0.0f;  // 0..1
        bool isValid() const {
            return k > 0.01f && otherRect.width() > 1.0 && otherRect.height() > 1.0;
        }
    };
    void setFusion(const Fusion &fusion) { m_fusion = fusion; }
    void clearFusion() { m_fusion = Fusion(); }
    // 2026-09-16: ガラススタイル。
    //   Standard: これまでの見た目
    //   Carved:   削り出し（面取りの明暗）
    //   Gummy:    2026-10-11 グミ（乳白の厚いゲル。Claude outputs/gummy-glass-lab.html で調整）。
    //             中身・背景の色は毎画素ぼかさず、64×64へ縮めた小テクスチャから読む
    enum class Style { Standard, Carved, Gummy };
    void setStyle(Style style) { m_style = style; }
    bool isGummyStyle() const { return m_style == Style::Gummy; }

private:
    // 2026-07-19の実機調整結果。ランタイム調整UI/JSONは本実装化に伴い削除。
    // 2026-09-11: アクティブ時(掴んでいる間)のみキャプチャする＝常時負荷ではない
    // ため、キャプチャ解像度をネイティブへ戻した。あわせて色収差(赤/緑への
    // 分離)が過剰との指摘を受け、0.3→0.15へ弱めた。さらに縁の屈折が強すぎる
    // との指摘でkEdgeBoostを2.0→1.0へ下げた（外周成分の追加増幅を半減）。
    static constexpr float kRefraction = 1.0f;
    static constexpr float kEdgeBoost = 1.0f;
    static constexpr float kLauncherNormalInfluence = 2.2f;
    static constexpr float kChromaticAberration = 0.15f;
    static constexpr float kCaptureScale = 1.0f;

    bool initShader();
    void initGeometry();

    // バブルごとのライト方向バネ状態（screen空間の正規化2次元ベクトル）。
    // 角度そのものをバネで追従させると±πの境界でラップして暴れるため、
    // 必ず2次元ベクトルの状態で保持し、シェーダーへ渡す直前にatan2で
    // 角度へ変換する。臨界減衰よりわずかに緩めたバネでtargetへ遅延追従
    // させることで、ウィンドウ移動時に光が「きゅるり」と遅れてついてくる
    // 質感を作る。
    struct SpringState {
        QVector2D dir    {0.0f, -1.0f};
        QVector2D vel    {0.0f, 0.0f};
        bool      seeded = false; // 初回は補間せず即座にワープさせる
    };
    QHash<const void*, SpringState> m_springs;

    void ensureCaptureTextures(int sourceW, int sourceH);

    QOpenGLShaderProgram *m_shader = nullptr;
    // 2026-09-16: ガラススタイル「削り出し」用（同じソースを CARVED_GLASS 付きで
    // コンパイルしたもの）。作れなかった場合は nullptr で、標準のまま描く。
    QOpenGLShaderProgram *m_shaderCarved = nullptr;
    // 2026-10-11: ガラススタイル「グミ」用（GUMMY_GLASS 付きでコンパイル）。
    QOpenGLShaderProgram *m_shaderGummy = nullptr;
    // 2026-10-11: グミの縮小パス（中身・背景キャプチャを小テクスチャへ）。
    QOpenGLShaderProgram *m_gummySmallShader = nullptr;
    GLuint m_emptyVao = 0;
    struct GummySmall {
        GLuint tex = 0, fbo = 0;
        GLuint srcTex = 0;     // 最後に縮めた元テクスチャ
        qint64 lastMs = -1;
    };
    static constexpr int kGummySmallSize = 64;         // ミップは 64→1（7段）
    static constexpr int kGummySmallIntervalMs = 100;  // 中身の小テクスチャの更新間隔
    // 2026-10-12: アイテムごとの小テクスチャは、1枚のテクスチャ配列の layer 枚目に持つ
    struct ItemSmall {
        int layer = -1;
        GLuint srcTex = 0;
        qint64 lastMs = -1;
        bool valid = false;
    };
    QHash<const void*, ItemSmall> m_itemSmall;          // forget で配列の枠を返す
    GLuint m_smallArray = 0, m_smallArrayFbo = 0;
    int m_smallArrayCap = 0;
    int m_smallMaxLevel = 6;
    QVector<int> m_smallFreeLayers;
    QOpenGLShaderProgram *m_gummyMipShader = nullptr;
    bool ensureSmallArray(int minLayers);
    void renderItemSmall(ItemSmall &small, GLuint srcTex, const QRectF &srcUv);
    // 2026-10-12: まとめ描き
    static constexpr int kInstanceFloats = 24;
    QOpenGLShaderProgram *m_instShader[3] = {nullptr, nullptr, nullptr};
    GLuint m_instVao = 0, m_instVbo = 0;
    int m_instVboBytes = 64 * kInstanceFloats * int(sizeof(float));
    QVector<float> m_batch;
    QVector<QRectF> m_batchRects;
    int m_batchStyle = 0;
    bool m_batchingEnabled = true;
    bool m_lastDrawBatched = false;
    int m_statBatched = 0, m_statSingles = 0, m_statFlushes = 0;
    GummySmall m_gummyCaptureSmall;                    // 背景キャプチャ（掴んでいる1個ぶん）
    QElapsedTimer m_clock;
    void releaseGummySmall(GummySmall &small);
    void renderGummySmall(GummySmall &small, GLuint srcTex, const QRectF &srcUv,
                          bool premultipliedSource, float blend);
    // 背景キャプチャの縮小を前フレームへ混ぜる割合（小さいほどなめらか・遅れる）
    static constexpr float kGummyCaptureBlend = 0.3f;
    Style m_style = Style::Standard;
    // 2026-10-11: 直近で屈折プリパスを描いたアイテム（背景キャプチャの対象）。
    const void *m_captureItemKey = nullptr;
    UniformLocationCache m_uniforms;   // 2026-09-16: 引数の番号を覚えておく
    GLuint m_vao = 0, m_vbo = 0;

    // キャプチャ疑似屈折用（アクティブ時だけ遅延確保）。
    // MSAA元FBOは解像度を変えながら直接resolveできないため、必要な小領域を
    // m_resolveTexへ等倍resolveし、そこからm_captureTexへ1/2縮小する。
    GLuint m_resolveTex = 0;
    GLuint m_resolveFbo = 0;
    GLuint m_captureTex = 0;
    GLuint m_captureFbo = 0;
    int m_resolveW = 0, m_resolveH = 0; // 確保容量（物理px）
    int m_capW = 0, m_capH = 0;         // 半解像度テクスチャの確保容量
    int m_captureOriginX = 0, m_captureOriginY = 0;
    int m_captureSourceW = 0, m_captureSourceH = 0;
    int m_captureDataW = 0, m_captureDataH = 0;
    float m_captureDpr = 1.0f;

    QMatrix4x4 m_projection;
    int m_vpW = 0, m_vpH = 0;
    bool m_initialized = false;
    int m_debugMode = 0; // Shift+D: 0=通常 1=法線表示 2=屈折レイヤー単体
    float m_refractionActivation = 0.0f;
    Fusion m_fusion;   // 2026-09-22: setFusion()
};
