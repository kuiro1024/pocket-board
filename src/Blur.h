#pragma once
#include <QColor>
#include <array>
#include <QPointer>
#include <QRectF>
#include <QVector>
#include <functional>

class QWidget;
class BackdropBlurGL;
class BackdropBlurWatcher;

// ---- BackdropBlur -----------------------------------------------------------
// 「UI(QWidget)の背後にあるものをぼかす」ための共通機構。
//
// 前提と設計の理由:
//   島やボタンはQPainter(ラスタエンジン)で描いているためGLSLを挟めない。
//   一方、それらの背後にあるのはCanvasWidget = QOpenGLWidgetの描画結果なので、
//   「UIの中で」ではなく「UIの下のレイヤーで」ならシェーダーブラーが使える。
//   そこで、CanvasWidget::paintGL()の最後に、UIが乗る矩形ぶんだけ自FBOを
//   ぼかして書き戻しておき、UI側はその上へ半透明で描く、という分担にする。
//   UI側がぼけた背景を自前で用意する必要はない。
//
//   なお、SetWindowCompositionAttribute等のWindowsネイティブなブラーは
//   HWND単位かつ「ウィンドウの背後(デスクトップ)」が対象なので、自アプリの
//   キャンバス内容をぼかす用途には使えない(MainWindow側で適用済みのアクリルは
//   デスクトップ向け)。
//
// 使い方:
//   1. ブラーを掛けたいウィジェットがSurfaceをメンバとして持ち、attach(this)。
//   2. 形が決まった時点(paintEvent等)でsetShape()を呼ぶ。値が変わったときだけ
//      キャンバスの再描画が要求されるので、毎回呼んで構わない。
//   3. CanvasWidget::paintGL()の末尾でrenderAll()を1回呼ぶ。
//
//   class MyWidget {
//       BackdropBlur::Surface m_blur;      // デストラクタで自動的に登録解除
//   };
//   MyWidget::MyWidget() { m_blur.attach(this); }
//   void MyWidget::paintEvent(...) {
//       m_blur.setShape(QRectF(...), cornerRadius, /*blurRadius=*/18.0);
//       ... 半透明で描画 ...
//   }
//
// 制約:
//   ぼかす材料はキャンバスのFBOにあるものだけ。ウィンドウの透過部分(キャンバスが
//   何も描いていない場所)にはぼかす対象が無いため、そこでは何も起きない
//   (その領域は元々DWMのブラー越しにデスクトップが見えている)。
//
//   このヘッダはUI側のヘッダからincludeされるため、OpenGL関連の型は一切
//   公開しない。GLリソースとシェーダーはBlur.cpp内のBackdropBlurGLへ隠してある。
// -----------------------------------------------------------------------------
// ---- ガラスの板（2026-09-16追加）--------------------------------------
// ぼかしの直後に、同じ角丸矩形へ「ガラスの板」（本体の塗り・フロスト・
// 加算の縁光・乗算の翳り・輪郭線）をGLシェーダーで描く。
//
// 【なぜGLか】以前は島のpaintEvent()でQPainterが縁の減衰を太さ違いの
// ストローク約57本の重ね塗りで作っていた。形が変わらない間はQPixmapの
// キャッシュで済むが、展開アニメーション中は毎フレーム作り直しになり、
// CPUラスタのコストが画素数に比例するため高解像度ほどフレームが落ちた。
// 「太さ違いのストロークN本の重なり」は「縁からの距離dに比例して弱まる
// 光」の階段近似なので、角丸矩形SDFで距離を求めれば1パスで同じ絵になる。
//
// 【見た目の同一性】QPainter版は透明な画像へ Plus / Multiply で描いてから
// SourceOverで重ねていた。シェーダーも「透明な下地に同じ合成を順に
// 掛けた結果」をpremultipliedで計算し、最後に通常のover合成で書き戻す
// （背景へ直接Multiplyを掛けると暗くなり、見た目が変わってしまうため）。
struct BackdropGlassPlateStyle
{
    // 縁の帯の作り方。旧QPainter版は島と動画UIでわずかに違っていたので、
    // 見た目を変えないためにそれぞれを再現する。
    //   Island  … DynamicIsland::paintEvent()（帯12本・span統一・baseH固定）
    //   VideoUi … CanvasVideo.cpp の paintGlassPlate()
    //             （光ごとにspanが違う・板が高いと帯6本・baseH=min(高さ,42)）
    enum class Profile { Island, VideoUi };

    // 2026-09-16: 板以外の形。
    //   RoundedRect … 角丸矩形の板（島・ラベル・時刻の台）
    //   Disc        … 島の丸ボタン（AnimatedButton::paintEvent の円のガラス）
    //   Goo         … 設定タブの液体アニメ（IslandTabGoo::paintSurface）
    // Disc / Goo はぼかしを伴わない（blurRadius=0で登録してよい）。
    // 形ごとの数値は params に入れる（並びは各 Shape の登録側コメント参照）。
    enum class Shape { RoundedRect, Disc, Goo };

    bool    enabled = false;
    Shape   shape = Shape::RoundedRect;
    std::array<float, 20> params {};
    Profile profile = Profile::Island;
    QColor  body;          // 本体の塗り（アルファ込み）
    QColor  shade;         // 翳り・輪郭線の暗色（アルファは無視）
    qreal   baseH = 42.0;  // 光の距離の基準高さ（Islandのみ使用）
    // 板の不透明度。負ならぼかしと同じ値を使う。Regionsは
    // setOpacityScale()の係数をぼかしにだけ掛けるため、板には呼び出し側が
    // 渡した値をそのまま使いたい（add()が自動で設定する）。
    qreal   opacity = -1.0;

    bool operator==(const BackdropGlassPlateStyle &o) const
    {
        if (enabled != o.enabled)
            return false;
        if (!enabled)
            return true;
        return profile == o.profile && shape == o.shape && params == o.params
            && body.rgba() == o.body.rgba() && shade.rgba() == o.shade.rgba()
            && qFuzzyCompare(baseH + 1.0, o.baseH + 1.0)
            && qFuzzyCompare(opacity + 2.0, o.opacity + 2.0);
    }
    bool operator!=(const BackdropGlassPlateStyle &o) const { return !(*this == o); }
};

class BackdropBlur
{
public:
    // ガラスの板の設定（実体はクラスの外。入れ子クラスの既定引数に使うため）。
    using GlassPlateStyle = BackdropGlassPlateStyle;

    // ブラー対象の登録ハンドル。生存期間がそのまま登録期間になる。
    class Surface
    {
    public:
        Surface() = default;
        ~Surface();
        Surface(const Surface &) = delete;
        Surface &operator=(const Surface &) = delete;

        // このSurfaceの持ち主を登録する。ownerが不可視の間は自動的にスキップされる。
        void attach(QWidget *owner);

        // localRect     : owner内のローカル座標での矩形(論理px)
        // cornerRadius  : 角丸半径(論理px)
        // blurRadius    : ぼかし半径(論理px)。0以下でブラー無効。
        // opacity       : ぼかしの効き具合(0〜1)。元の絵とのクロスフェードに
        //                 なるので、UI側のフェード値をそのまま渡せば、UIが
        //                 薄くなるのに合わせてブラーも薄くなる。
        //                 半径ではなくこちらでフェードさせること(半径を0へ
        //                 縮めても「くっきりした置き換え」は残ってしまう)。
        void setShape(const QRectF &localRect, qreal cornerRadius, qreal blurRadius,
                      qreal opacity = 1.0);

        // ガラスの板を一緒に描くかどうかと、その見た目。setShape()より前に
        // 呼ぶこと（比較と再描画の要求はsetShape()でまとめて行う）。
        void setGlassStyle(const GlassPlateStyle &style);

        void detach();

    private:
        friend class BackdropBlur;
        int m_id = 0;
    };

    // 1枚のウィジェットの中で、描画しながら複数の領域を登録したいとき用。
    // (動画の再生バーのように、要素の数も表示/非表示も毎フレーム変わる場合)
    // paintEvent中に begin() → add() を必要な回数 → end() の順で呼ぶ。
    // 今回addされなかった領域は自動的に無効化されるので、要素が消えたときの
    // 後始末を呼び出し側が書く必要はない。
    //
    //   m_blurRegions.begin();
    //   m_blurRegions.add(buttonRect, buttonRect.height()/2, 14.0);
    //   m_blurRegions.end();
    class Regions
    {
    public:
        Regions() = default;
        ~Regions();
        Regions(const Regions &) = delete;
        Regions &operator=(const Regions &) = delete;

        void attach(QWidget *owner);
        // 2026-09-15: 以降の add() の opacity に一律で掛ける係数(0〜1)。
        // ウィジェット全体を丸ごとフェードさせるとき用（描画コード側の
        // 個々の opacity 計算に手を入れずに済む）。既定1.0。
        void setOpacityScale(qreal scale) { m_opacityScale = scale; }
        void begin();
        // blurRadius<=0 や空の矩形は無視される(消えている要素をそのまま
        // 渡してよい)。
        // glass を渡すと、その領域にガラスの板も描く（GlassPlateStyle参照）。
        void add(const QRectF &localRect, qreal cornerRadius, qreal blurRadius,
                 qreal opacity = 1.0,
                 const GlassPlateStyle &glass = GlassPlateStyle());
        void end();

    private:
        QWidget           *m_owner = nullptr;
        QVector<Surface *> m_pool;
        int                m_used = 0;
        qreal              m_opacityScale = 1.0;
    };

    // UI部品の既定のぼかし半径(論理px)。島のような大きな面はもっと大きい値を
    // 使うが、ボタンやバッジ程度の小さな部品はこのくらいが目安。
    static constexpr qreal kDefaultUiBlurRadius = 14.0;

    static BackdropBlur &instance();

    // 形が変わったときにキャンバスの再描画を要求するためのコールバック。
    // CanvasWidget側で一度だけ設定する。
    //
    // 2026-09-15 D-5: 以前は引数なしで、CanvasWidget側は requestFullRender()
    // ＝ウィンドウ全面の再描画を要求していた。実測では [RenderSchedule] が
    // full=265 / ui=266 / frames=280（＝ほぼ全フレームが全面）になっており、
    // 再生バーやイコライザの矩形が1pxでも動くたびに
    //   ・キャンバス全面の再描画
    //   ・静的レイヤーキャッシュの破棄（retained=12/280）
    //   ・半透明の兄弟ウィジェット CanvasOverlayWidget の全面 paintEvent
    //     （[CanvasOverlay][stat] area/frame が 2.0〜2.4Mpx に張り付く）
    // が起きていた。「全画面にすると何もかも重い」の主因がこれ。
    //
    // owner     : setShape() を呼んだウィジェット（座標変換の基準）
    // ownerRect : owner ローカル座標での「変わった範囲」（ブラー半径ぶんの
    //             余白込み）。空なら「範囲不明＝全面」を意味する。
    void setRepaintRequester(
        std::function<void(QWidget *owner, const QRectF &ownerRect)> requester);

    // canvas  : 描画中のQOpenGLWidget(座標変換の基準)
    // dpr     : devicePixelRatioF()
    // physW/H : 現在のFBOの物理サイズ
    // GLコンテキストがカレントな状態(=paintGL内)から呼ぶこと。
    // sceneTexture: 2026-09-16。キャンバスのシーンレイヤー（UIレイヤーを描く前の
    //               絵、physW×physHの単一サンプルのテクスチャ）。0でなければ、
    //               ぼかしの材料を画面のFBOから切り出す代わりにここから直接読む
    //               （領域ごとのMSAA resolve のblitが無くなる）。
    void renderAll(QWidget *canvas, qreal dpr, int physW, int physH,
                   unsigned int sceneTexture = 0);

    // GLパスの中で描くUI用の即時版。呼んだ時点で指定矩形をその場でぼかす。
    // UIを描いた後に呼ぶとUI自身がぼけてしまうので、必ず描く直前に呼ぶこと。
    // canvasRect はキャンバスの論理座標。renderAll()とは独立に使える。
    void blurNow(qreal dpr, int physW, int physH, const QRectF &canvasRect,
                 qreal cornerRadius, qreal blurRadius, qreal opacity = 1.0);

    // GLコンテキスト破棄時に呼ぶ(コンテキストがカレントな状態で)。
    void releaseGL();

private:
    friend class BackdropBlurGL;
    friend class BackdropBlurWatcher;

    BackdropBlur() = default;
    ~BackdropBlur() = default;

    struct Entry {
        int               id = 0;
        QPointer<QWidget> owner;
        QRectF            localRect;
        qreal             cornerRadius = 0.0;
        qreal             blurRadius   = 0.0;
        qreal             opacity      = 1.0;
        GlassPlateStyle   glass;

        // 直近に「実際に描いた」状態。setShape()は要求値をこちらと比べて
        // 再描画を要求する。形が同じでも、ウィジェットが一旦隠れて
        // (renderedValid=false)再表示された場合にブラーを掛け直せるようにする
        // ため、「前回setShapeされた値」ではなくこちらと比較する必要がある。
        bool   renderedValid  = false;
        QRectF renderedRect;
        qreal  renderedCorner = 0.0;
        qreal  renderedBlur   = 0.0;
        qreal  renderedOpacity = 1.0;
        GlassPlateStyle renderedGlass;
        // このフレームでぼかしを描いた位置（キャンバス論理座標）。
        // ガラスの板はぼかしを全部描き終えてから、この矩形へ重ねる。
        QRectF renderedCanvasRect;
    };

    int    registerSurface();
    void   unregisterSurface(int id);
    Entry *findEntry(int id);
    // ownerRect が空／owner が null のときは「全面」を意味する。
    void   requestRepaint(QWidget *owner = nullptr,
                          const QRectF &ownerRect = QRectF());

    QVector<Entry>        m_entries;
    int                   m_nextId = 1;
    std::function<void(QWidget *, const QRectF &)> m_repaintRequester;
    BackdropBlurGL       *m_gl = nullptr;
};
