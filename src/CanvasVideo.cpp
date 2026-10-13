#include "CanvasVideo.h"
#include "CanvasWidget.h"
#include "Renderer.h"
#include "AppSettings.h"
#include "MpvTeardown.h"
#include "engine/VideoRenderThread.h" // 2026-09-15 非同期化 P2
#include "plugin/PluginManager.h"
#include "plugin/PbLabelActionApi.h"
#include "CanvasDropHandler.h"
#include "WebVideoCache.h"
#include <QCollator>
#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>
#include <QUrl>
#include <QFont>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QDateTime>
#include <QTimer>
#include <QThreadPool>
#include <QFontMetrics>
#include <QtGlobal>
#include <QColor>
#include <QPointer>
#include <QPainter>
#include <QPainterPath>
#include <QConicalGradient>
#include <QRadialGradient>
#include <QSvgRenderer>
#include <QEasingCurve>
// 一覧のスナップショットのセッション保存（panelSnapshotToJson）用。
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <cmath>
#include <utility> // std::as_const

// DynamicIsland.cpp で定義済みのWindowsアクセントカラー取得関数を再利用する。
// DynamicIsland.h（QPushButton/QComboBox等の重いUIヘッダー群）は不要なため、
// ここでは軽量なextern宣言のみ行う。
QColor getWindowsAccentColor();
QColor appAccentColor();   // DynamicIsland.cpp（設定のカラーを反映したアクセントカラー）

// 島（DynamicIsland）の本体アルファに合わせる。動画UIのガラスも同じ濃さ。
// 2026-09-12: 115では明るい背景の上で色が背景に引きずられすぎたため、
// 島側（kIslandBodyAlpha）と揃えて165へ。
constexpr int kPlateBodyAlpha = 165;

// UIの文字は背景（動画の絵）次第で埋もれるので、必ず影を落としてから描く。
// 影は1pxずらした暗色＋真下の1本だけの軽いもので、ぼかしは使わない
// （毎フレーム描くため）。alphaは本体のフェードに合わせて渡す。
void drawShadowedText(QPainter &painter, const QRectF &rect, int flags,
                      const QString &text, const QColor &color, double alpha)
{
    if (text.isEmpty() || alpha <= 0.01)
        return;
    const int shadowAlpha = qBound(0, qRound(170 * alpha), 255);
    painter.save();
    painter.setPen(QColor(0, 0, 0, shadowAlpha));
    painter.drawText(rect.translated(1.0, 1.0), flags, text);
    painter.drawText(rect.translated(0.0, 1.0), flags, text);
    painter.setPen(color);
    painter.drawText(rect, flags, text);
    painter.restore();
}

// 2026-09-15 非同期化 P3: 従来 paintGL 内にあった suppressRenderToFbo の判定式を
// そのまま関数化（式は変更なし）。paintGL と stateChanged の両方から使う。
// stateChanged からも使うのは、画面外カリングで paintGL が来ないバブルでも
// ワーカーの描画を状態遷移の瞬間に止める／再開させるため。
static bool videoRenderSuppressedFor(BubbleState bs, MediaKind kind)
{
    const bool isAudioOnly       = (bs == BubbleState::AudioOnly);
    const bool isLocalAudioMedia = (kind == MediaKind::LocalAudio);
    const bool isHiddenPaused    = (bs == BubbleState::PausedHidden
                                 || bs == BubbleState::ReadyHidden
                                 || bs == BubbleState::FocusSleep);
    return isLocalAudioMedia || isAudioOnly || isHiddenPaused
        || bs == BubbleState::Sleep || bs == BubbleState::Idle;
}

namespace {
std::atomic<quint64> s_nextThumbnailRequestId {1};
constexpr qint64 kThumbnailEvictionGraceMs = 10000;
constexpr qint64 kThumbnailRetryDelayMs = 30000;
constexpr qsizetype kMaxProviderThumbnailBytes = 8 * 1024 * 1024;

bool isLocalMediaUrl(const QString &url)
{
    if (url.isEmpty())
        return false;

    const QUrl qurl(url);
    if (qurl.isLocalFile())
        return true;

    // QUrl::isLocalFile() は "file:///..." というスキーム付きURLしか true にしない。
    // ところがセッション復元経路（restoreLazy）では、保存されたパスの形式次第で
    // QUrl(url) のパースが意図通りにならず isLocalFile() が false を返すことがある
    // （例: パス中の '#' 以降がURLフラグメントとして解釈される、ドライブレターの
    // 扱いでスキームなしパスがホスト名付きURLと誤認識される、等）。
    // この場合に何も救済しないと、CanvasVideo::beginDeferredLoadForPlayback() が
    // 「外部URLとして無視」した上で m_deferredLoad を false にしてしまい、
    // 以後 m_streamUrl が一切セットされないまま再生ボタンを押しても
    // 永久に無反応になってしまう（実際に報告されたバグの原因）。
    //
    // そのため、スキームなし・素のWindowsパス（"C:/..." や "C:\..."）であれば
    // ローカルファイルとして扱えるように、フォールバック判定を追加する。
    if (qurl.scheme().isEmpty() || qurl.scheme().length() == 1 /* ドライブレター誤認識 */) {
        if (url.length() >= 2 && url[1] == QLatin1Char(':'))
            return true; // "C:\..." "C:/..." 形式の素のWindowsパス
    }
    if (!qurl.toLocalFile().isEmpty())
        return true;

    return false;
}

MediaSource sourceFromUrl(const QString &url, MediaKind kind)
{
    MediaSource source;
    source.kind = kind;
    source.originalPathOrUrl = url;
    const QString localPath = QUrl(url).toLocalFile();
    source.title = QFileInfo(localPath.isEmpty() ? url : localPath).fileName();
    return source;
}

// サムネイルの縮小処理自体は ThumbnailLoader（Phase 7 Step 1で分離）へ
// 移設済み。CanvasVideo::setRestoredThumbnailImage() からも同じ上限を
// 適用するため、ThumbnailLoader::downscaleThumbnailIfNeeded() を呼ぶ。

// ---- 再生バーの音量アイコン(SVG) ----
// DynamicIsland（マスターボリューム用+ボタン）と同じ image/svg/ico_Sound_0〜3.svg
// （.qrc経由で :/icons/ にバンドル）を、白でティントして使う。個別バブルの
// 音量(0〜100%)に応じて 0=ミュート/1〜33/34〜66/67〜100 の4段階を切り替える。
QImage tintedSvgImageForPlaybackBar(const QString &resourcePath, const QSize &size, const QColor &color)
{
    static QHash<QString, QImage> s_imageCache;
    const QString cacheKey = resourcePath
        + QLatin1Char('|') + QString::number(size.width())
        + QLatin1Char('x') + QString::number(size.height())
        + QLatin1Char('|') + QString::number(color.rgba());
    const auto cached = s_imageCache.constFind(cacheKey);
    if (cached != s_imageCache.cend())
        return cached.value();

    static QHash<QString, QSvgRenderer*> s_rendererCache;
    QSvgRenderer *renderer = s_rendererCache.value(resourcePath, nullptr);
    if (!renderer) {
        renderer = new QSvgRenderer(resourcePath);
        s_rendererCache.insert(resourcePath, renderer);
    }
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    renderer->render(&p);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(image.rect(), color);
    p.end();
    s_imageCache.insert(cacheKey, image);
    return image;
}

// 音量(0.0-1.0) → アイコン段階(0-3)。DynamicIsland::updateMasterVolumeIcon()と
// 同じ区切り（0はミュート、1〜33/34〜66/67〜100の3等分）。
int soundIconLevelForVolume(float vol01)
{
    const int pct = qRound(qBound(0.0f, vol01, 1.0f) * 100.0f);
    if (pct <= 0)  return 0;
    if (pct <= 33) return 1;
    if (pct <= 66) return 2;
    return 3;
}

// ---- 再生バーの円形ボタンのガラス表現 ----
// 2026-09-11: 島のボタン(AnimatedButton::paintEvent)と同じ言語を、再生バー
// 左下の再生停止ボタン・音量ボタンにも適用する。方針は島側と同一で、
// 「平らな半透明のガラス板」に
//   ・加算(Plus)のハイライト = 光が当たって輝いている縁
//   ・乗算(Multiply)の翳りと輪郭線 = 光が当たらず沈んでいる縁
// の2種類だけを重ね、中央は素通しのままにする(中央にグラデーションを入れる
// と途端にプラスチック/ゲルの質感になる)。
//
// 島との違いは下地の色だけ。島のボタンは自分の色(黄/赤/緑/アクセント色)を
// 持つのでティントを濃く載せられるが、こちらは背後の映像を透かす無彩色の
// ガラスなので、板の白は薄いまま・暗色も無彩色にする。
//
// 引数のfadeは再生バーのフェード(m_playbarFadeAlpha)。全レイヤーのアルファに
// 掛けることで、バーの出現/消滅に合わせてガラスも一緒に濃くなる/消える。
// rにはポップスケールを掛けた後の半径を渡すこと(形がそのまま追従する)。
// Windowsのアクセント色。島（DynamicIsland）と同じ色を動画UIでも使うため
// の共有窓口。取得自体は環境によってはレジストリ参照を伴うので、毎フレーム
// 引かずに短時間キャッシュする。
//
// 2026-09-16: 設定のカラー（「Windowsのカラー設定を使用」オフ時のカラーコード）も
// 反映するため appAccentColor() から取る。設定を変えた瞬間に切り替わるよう、
// 設定値が前回と違えば2秒の期限を待たずに取り直す（設定値の読み出し自体は
// メモリ上の値なので軽い。重いのは Windows 側のレジストリ参照だけ）。
QColor cachedAccentColor()
{
    static QColor cached;
    static QElapsedTimer age;
    static bool lastUseWindows = true;
    static QString lastCustom;
    const AppSettings &cfg = AppSettings::instance();
    const bool useWindows = cfg.useWindowsAccentColor();
    const bool settingsChanged = useWindows != lastUseWindows
        || (!useWindows && cfg.customAccentColor() != lastCustom);
    if (!cached.isValid() || !age.isValid() || age.elapsed() > 2000 || settingsChanged) {
        cached = appAccentColor();
        lastUseWindows = useWindows;
        lastCustom = cfg.customAccentColor();
        age.start();
    }
    return cached;
}

// アクセント色から作る暗色（島のガラス表現と同じ作り方）。縁を沈める陰と
// 輪郭線に使う。
QColor accentShadeColor()
{
    const QColor hsv = cachedAccentColor().toHsv();
    return QColor::fromHsv(qMax(0, hsv.hue()),
                           qMin(255, static_cast<int>(hsv.saturation() * 1.10)),
                           static_cast<int>(hsv.value() * 0.42));
}

// 2026-09-12: 動画UIの下地は、その背後をBackdropBlurでぼかしている影響で
// 本来のアクセント色より少し暗く沈む（板やボタンの実測でそう見える）。
// 相殺するぶんだけ明るいアクセント色を作り、動画UIの本体色に使う。
// ラベル・島は別の見え方で成立しているので、こちらだけを持ち上げる。
QColor videoUiBodyColor()
{
    const QColor hsv = cachedAccentColor().toHsv();
    return QColor::fromHsv(qMax(0, hsv.hue()),
                           qBound(0, static_cast<int>(hsv.saturation() * 0.94), 255),
                           qBound(0, static_cast<int>(hsv.value() * 1.18 + 14), 255));
}

// アクセント色から作る明色。トラックの「進んだぶん」など、光って見せたい
// ところに使う（島・ラベルと同じ色の家族に収める）。
QColor accentLightColor()
{
    const QColor hsv = cachedAccentColor().toHsv();
    return QColor::fromHsv(qMax(0, hsv.hue()),
                           qBound(0, static_cast<int>(hsv.saturation() * 0.82), 255),
                           qBound(0, static_cast<int>(hsv.value() * 1.35 + 40), 255));
}

// 2026-09-16: ガラスの板（ラベルの台・リストの板・時刻の台）をGL側で描くか。
// 島と同じく BackdropBlur がぼかしの直後にシェーダーで描く（Blur.h の
// GlassPlateStyle）。伸び縮みの間に毎フレーム走っていたQPainterの約57本の
// ストローク描画が無くなる。PB_VIDEO_GL_GLASS=0 で従来のQPainter描画へ戻せる。
bool videoGlassOnGpu()
{
    static const bool s_on =
        qEnvironmentVariable("PB_VIDEO_GL_GLASS", QStringLiteral("1"))
        != QLatin1String("0");
    return s_on;
}

// paintGlassPlate() と同じ見た目になるGL版の設定。
BackdropBlur::GlassPlateStyle videoGlassStyle(const QColor &accent)
{
    BackdropBlur::GlassPlateStyle style;
    style.enabled = true;
    style.profile = BackdropBlur::GlassPlateStyle::Profile::VideoUi;
    QColor body = accent;
    body.setAlpha(kPlateBodyAlpha);
    style.body  = body;
    style.shade = accentShadeColor();
    return style;
}

// 2026-09-13: 板（ガラスの台）の実体はラベルのコントローラのブロック
// （roundedPathLR / paintGlassPlate）にある。無名名前空間は翻訳単位ごとに
// 1つなので、ここで前方宣言しておけば再生バー側からも同じ関数を呼べる
// （＝時刻の台もラベル・島とまったく同じライティングで描ける）。
QPainterPath roundedPathLR(const QRectF &r, double leftRadius, double rightRadius);
void paintGlassPlate(QPainter &painter, const QPainterPath &path,
                     const QRectF &r, const QColor &accent, double alpha);

// 2026-09-12: シークバーや音量スライダーの「溝」を、島・ラベルと同じ
// ガラスの素材感で塗る。細い帯なので、板のような縁の作り込み（帯状の
// ストロークの重ね合わせ）は入らない。同じ言語のまま最小限に写す:
//   本体 = アクセント色を薄く / 上辺 = 加算の細い光 / 輪郭 = 乗算の暗色。
void paintGlassTrack(QPainter &painter, const QRectF &rect, double radius, float fade)
{
    if (rect.width() <= 0.0 || rect.height() <= 0.0 || fade <= 0.001f)
        return;
    const auto a = [fade](int v) { return qBound(0, qRound(v * fade), 255); };

    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);

    painter.save();
    painter.setPen(Qt::NoPen);
    QColor body = videoUiBodyColor();
    body.setAlpha(a(qRound(kPlateBodyAlpha * 0.72)));
    painter.setBrush(body);
    painter.drawPath(path);

    painter.save();
    painter.setClipPath(path);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.fillRect(QRectF(rect.left(), rect.top(),
                            rect.width(), qMax(1.0, rect.height() * 0.34)),
                     QColor(255, 255, 255, a(16)));
    painter.restore();

    painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    QColor contour = accentShadeColor();
    contour.setAlpha(a(170));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(contour, 1.0));
    painter.drawPath(path);
    painter.restore();
}

void paintPlaybackGlassCircle(QPainter &painter, const QPointF &center,
                              double r, float fade)
{
    if (r <= 0.5 || fade <= 0.001f)
        return;

    const auto a = [fade](int v) {
        return qBound(0, qRound(v * fade), 255);
    };
    const auto shade = [&a](int alpha) {
        // 2026-09-12: 島と同じくアクセント色由来の暗色にする（以前は無彩色）。
        QColor c = accentShadeColor();
        c.setAlpha(a(alpha));
        return c;
    };
    const QRectF bodyRect(center.x() - r, center.y() - r, r * 2.0, r * 2.0);

    painter.save();
    painter.setPen(Qt::NoPen);

    {
        QPainterPath clip;
        clip.addEllipse(center, r, r);
        painter.save();
        painter.setClipPath(clip);

        // (1) ガラス板そのもの。2026-09-12: 島と色を揃えるため、白のベタ塗り
        //     からアクセント色へ変更した（濃さは島の本体アルファと同じ）。
        {
            QColor body = videoUiBodyColor();
            body.setAlpha(a(kPlateBodyAlpha));
            painter.fillRect(bodyRect, body);
        }

        // (2) 加算: 縁の内側へにじむリムグロー(全周)。
        //     2026-09-11: 当初は0.38から立ち上げていたが、それだと光が中央まで
        //     侵食して「面全体がぼんやり光る」見え方になり、縁だけが光っている
        //     ガラスらしさが出なかった(ユーザー指摘)。立ち上がりを0.82まで
        //     遅らせ、そのぶんピークを上げて細く強い縁にする。中央は完全に
        //     素通しのまま平坦に保つ。
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
        QRadialGradient ring(center, r);
        ring.setColorAt(0.00, QColor(255, 255, 255, 0));
        ring.setColorAt(0.82, QColor(255, 255, 255, 0));
        // 2026-09-12: 光の強さをラベル・島の板（paintGlassPlate）に合わせて
        // 落とした。以前は縁のリムが104、上下のグロウが96まで立ち上がって
        // いて、同じ画面に並ぶ板（上辺14／下辺9）と比べて明らかに過剰だった。
        ring.setColorAt(0.91, QColor(255, 255, 255, a(4)));
        ring.setColorAt(0.960, QColor(255, 255, 255, a(12)));
        ring.setColorAt(0.988, QColor(255, 255, 255, a(22)));
        ring.setColorAt(1.00, QColor(255, 255, 255, a(18)));
        painter.fillRect(bodyRect, ring);

        // (3) 加算: 光の入る上端と、抜ける下端をもう一段明るく。
        //     こちらも到達距離を縮め、上下の"縁の近く"だけを持ち上げる。
        //     半径をrの半分以下にすることで、面の中ほどには一切届かない。
        QRadialGradient topGlow(QPointF(center.x(), center.y() - r * 1.02),
                                r * 0.46);
        topGlow.setColorAt(0.00, QColor(255, 255, 255, a(22)));
        topGlow.setColorAt(0.45, QColor(255, 255, 255, a(7)));
        topGlow.setColorAt(1.00, QColor(255, 255, 255, 0));
        painter.fillRect(bodyRect, topGlow);

        QRadialGradient bottomGlow(QPointF(center.x(), center.y() + r * 1.06),
                                   r * 0.42);
        bottomGlow.setColorAt(0.00, QColor(255, 255, 255, a(16)));
        bottomGlow.setColorAt(0.45, QColor(255, 255, 255, a(5)));
        bottomGlow.setColorAt(1.00, QColor(255, 255, 255, 0));
        painter.fillRect(bodyRect, bottomGlow);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        // (4) 乗算: 上下と違って光の乗らない左右を沈める。
        //
        // 2026-09-11: 左右それぞれに「円の外へ中心を置いた放射グラデーション」
        // を置く方式は、到達距離を縮めた途端に左右へ独立した暗い塊が2つ残り、
        // ヘッドホンの耳当てのように見えてしまった(ユーザー指摘)。翳りは
        // 「面の左右」ではなく「縁に沿って左右が濃い帯」でなければならない。
        //
        // そこで島のガラス表現と同じく、円周に沿ったストロークで作る。
        //   ・角度方向の濃淡 … QConicalGradient(左右が濃く、上下でゼロ)
        //   ・内側への減衰   … 太さを変えたストロークの重ね掛け
        // こうすると塊にならず、縁沿いの帯として中央へ段差なく消える。
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        {
            // 幅を段階的に細くしながら重ねる。1本の太い帯だとペンの内側が
            // そのまま段差(境界線)になるため。クリップで外側は捨てられる。
            //
            // 2026-09-11: 全帯を同じ濃さで重ねると、いちばん太い帯の内側の端
            // (半径0.66r)に1/kBandsぶんの段差が残り、「内側に薄い黒い円」として
            // 見えていた(ユーザー指摘)。太い帯ほど薄くし、最も太い帯をα0に
            // することで、帯の外周から内側へ濃度が0まで連続に落ちるようにする。
            constexpr int kBands = 12;
            const qreal band = r * 0.34;
            painter.setBrush(Qt::NoBrush);
            for (int i = 0; i < kBands; ++i) {
                const qreal k = static_cast<qreal>(i) / kBands; // 0=最太/最薄
                const auto sa = [&](int v) {
                    return shade(qRound(v * k));
                };
                // Qtの角度は反時計回りで0°=右。位置0.25が上、0.5が左、0.75が下。
                QConicalGradient sideShade(center, 0.0);
                // 2026-09-12: 板の左右の陰（30）と同じ濃さへ。
                sideShade.setColorAt(0.00, sa(30));  // 右
                sideShade.setColorAt(0.13, sa(16));
                sideShade.setColorAt(0.25, sa(0));   // 上(ハイライト側)
                sideShade.setColorAt(0.37, sa(16));
                sideShade.setColorAt(0.50, sa(30));  // 左
                sideShade.setColorAt(0.63, sa(16));
                sideShade.setColorAt(0.75, sa(0));   // 下(ハイライト側)
                sideShade.setColorAt(0.87, sa(16));
                sideShade.setColorAt(1.00, sa(30));

                painter.setPen(QPen(QBrush(sideShade), band * 2.0 * (1.0 - k)));
                painter.drawEllipse(center, r, r);
            }
        }
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        painter.restore(); // クリップ解除
    }

    // (5) 乗算: 輪郭線(ガラス板の切り口)。これが無いと下地に溶けて板に見えない。
    painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    QLinearGradient contour(QPointF(center.x(), center.y() - r),
                            QPointF(center.x(), center.y() + r));
    //     明るいリング(6)のすぐ外に暗い線が1本通ることで、縁が「厚みのある
    //     切り口」として立ち、中央の平坦さと対比が付く。
    // 2026-09-12: 板の輪郭（120/230/190）と同じ濃さに揃える。
    contour.setColorAt(0.00, shade(120));
    contour.setColorAt(0.50, shade(230));
    contour.setColorAt(1.00, shade(190));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QBrush(contour), 1.0));
    painter.drawEllipse(center, r - 0.30, r - 0.30);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // (6) 加算: 縁を走る細い光の線。
    //     2026-09-11: 以前は左右でほぼ消していた(上端だけが光る)が、
    //     「縁だけ感」を出すには一周ぐるりと光の輪が通っているほうが効く。
    //     左右も完全には消さず下限を持たせ、線幅も少し太くして、輪郭線(5)の
    //     内側に明確な明るいリングが1本走る状態にする。
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    QLinearGradient edgeLight(QPointF(center.x(), center.y() - r),
                              QPointF(center.x(), center.y() + r));
    // 2026-09-12: 板と同じ控えめな光量へ（以前は122まで立ち上げていた）。
    edgeLight.setColorAt(0.00, QColor(255, 255, 255, a(30)));
    edgeLight.setColorAt(0.35, QColor(255, 255, 255, a(11)));
    edgeLight.setColorAt(0.60, QColor(255, 255, 255, a(10)));
    edgeLight.setColorAt(1.00, QColor(255, 255, 255, a(24)));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QBrush(edgeLight), 1.3));
    painter.drawEllipse(center, r - 1.05, r - 1.05);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    painter.restore();
}

// ---- 再生/停止アイコン(SVG) ----
// image/svg/now_play.svg（||、再生中に表示＝押すと一時停止できる）と
// now_stop.svg（▶、停止/一時停止中に表示＝押すと再生できる）を白でティントして使う。
// 画面中央のオーバーレイ・再生バー左下・ローカル音声カードの3箇所で共有する。
GLuint playStateIconTexture(Renderer &r, bool playing)
{
    static GLuint s_texIds[2] = {0, 0};
    static bool   s_loaded[2] = {false, false};
    static const char *kPaths[2] = {
        ":/icons/now_stop.svg", // [0] 停止中に表示する「▶（再生する）」アイコン
        ":/icons/now_play.svg", // [1] 再生中に表示する「||（一時停止する）」アイコン
    };
    const int idx = playing ? 1 : 0;
    if (!s_loaded[idx]) {
        const QImage img = tintedSvgImageForPlaybackBar(
            QString::fromLatin1(kPaths[idx]), QSize(64, 64), QColor(255, 255, 255));
        s_texIds[idx] = r.createTexture(img);
        s_loaded[idx] = true;
    }
    return s_texIds[idx];
}
}

namespace {
// 2026-09-12: 文字の下ごしらえ。コントローラを初めて開いたときだけ描画が
// 400ms近く固まっていた（[LabelPanel][stat] の max=393ms）。原因は描画量
// ではなく、行に使うフォント（サイズ違い＋日本語のフォールバック）を
// Windowsのフォントエンジンがその場で初めて用意すること。一度どこかで
// 描いてしまえば以後は効かないので、起動時（＝どうせ待ち時間がある間）に
// 1x1の画像へこっそり描いて済ませておく。
void warmUpPanelFontsOnce()
{
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;

    QImage scratch(4, 4, QImage::Format_ARGB32_Premultiplied);
    scratch.fill(Qt::transparent);
    QPainter p(&scratch);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    // paintLabelPanelRows / CanvasVideoLabelPainter と同じ組み合わせ。
    const QList<QFont> fonts{
        QFont(QStringLiteral("Arial"), 7, QFont::DemiBold),
        QFont(QStringLiteral("Arial"), 9),
        QFont(QStringLiteral("Arial"), 10, QFont::DemiBold),
    };
    // 英数字と日本語の両方を通す（日本語はフォールバックの解決が走る）。
    const QString sample = QStringLiteral("Ag0 あア亜");
    for (const QFont &f : fonts) {
        p.setFont(f);
        QFontMetrics(f).horizontalAdvance(sample); // 省略計算と同じ経路
        p.drawText(QRectF(0, 0, 4, 4), Qt::AlignLeft, sample);
    }
}
// 島（DynamicIsland、画面上部中央）と被らないよう小ぶりに保つ。
constexpr double kPanelWidth     = 244.0;
constexpr double kPanelMinWidth  = 168.0;
constexpr double kPanelRowHeight = 44.0;
// 2026-09-12: 板の角丸はラベルの台（カプセル）の丸みをそのまま使うように
// したため、この定数は未使用。数値で角丸を決め打ちしたくなったとき用に
// 残してある（使うなら labelPanelPlateGeometry と行のクリップの両方を
// 必ず同じ値に揃えること）。
[[maybe_unused]] constexpr double kPanelRadius = 14.0;
constexpr double kPanelPadding   = 7.0;
// リストの上下の端をこの高さぶんフェードさせる（スパッと切れて見えないように）。
// 2026-09-12: 26 → 34 へ。もう少し効かせたいのと、下の「端に着いたら帯ごと
// 縮める」処理を入れたぶん、最大の帯は広めでちょうどよくなった。
constexpr double kPanelEdgeFade  = 34.0;
constexpr double kThumbWidth     = 58.0;
constexpr double kThumbHeight    = 33.0;

// ラベルの引っ張り（固定操作）。しきい値を超えて離すと固定／解除が
// トグルする。kLabelPullMax は「これ以上は伸びない」上限で、指の移動量は
// tanh で丸めて渡す（引くほど重くなるゴムの手応え）。
// ---- 行の左まわりの寸法（2026-09-12）----
//
// ヘッダのロゴ（円）を縦の基準線にして、リストの行をそこへ揃える。
//
//   ロゴの左端  … アクティブ行の枠の左端
//   ロゴの中心  … 行のピンの中心
//   ロゴの右端  … サムネイルの左端
//
// ロゴの寸法は CanvasVideoLabel 側が持っているので、そこから引いてくる
// （数値を直書きすると、ラベルの余白を変えた瞬間に黙ってずれる）。
// ウィンドウ座標では
//   ロゴの左端 = 台の左端 + (kIconCenterFromPillLeft - kIconSizePx/2)
//   行の左端   = 台の左端 + kPanelPadding
// なので、その差が「行の左端から見た位置」になる。
constexpr double kRowIconLeftX =
    CanvasVideoLabelPainter::kIconCenterFromPillLeft
    - CanvasVideoLabelPainter::kIconSizePx * 0.5 - kPanelPadding;
constexpr double kRowPinCenterX =
    CanvasVideoLabelPainter::kIconCenterFromPillLeft - kPanelPadding;
// 行の左端からサムネイルの左端まで（＝ロゴの右端）。
constexpr double kRowPinWidth =
    CanvasVideoLabelPainter::kIconCenterFromPillLeft
    + CanvasVideoLabelPainter::kIconSizePx * 0.5 - kPanelPadding;
// 再生中のピンの半径（最大のとき）。
constexpr double kRowPinRadius  = 3.4;
// サムネイル⇔タイトルの間隔。ピンの右端⇔サムネイルの左端
// （kRowPinWidth - kRowPinCenterX - kRowPinRadius ≒ 6.6）とほぼ同じ値に
// して、サムネイルの左右の余白が揃って見えるようにしている。
constexpr double kRowGap        = 6.5;
// 再生中の行の右に置く簡易イコライザの幅（タイトルはこの分だけ短く切る）。
constexpr double kRowEqWidth  = 34.0;
constexpr double kLabelPullThreshold = 40.0;
constexpr double kLabelPullMax       = 74.0;

// ---- 再生中の行のイコライザ（棒4本＋音符2つ）の寸法 ----
// 2026-09-12: 描画（paintNowPlayingEqualizer）と部分更新の矩形
// （equalizerDamageRect）の両方から使うので、ここに集約してある。
// 片方だけ書き換えると「動いているのに再描画されない／逆に無駄に広い」
// ことになるので、値を変えるときは必ずこの定数を触ること。
constexpr double kEqBarWidth    = 3.0;
constexpr double kEqBarGap      = 2.0;
constexpr int    kEqBarCount    = 4;
constexpr double kEqMaxHeight   = 15.0;
// 行の右端からどれだけ内側に置くか。
constexpr double kEqRightInset  = 11.0;
// 棒の下端が行の中心からどれだけ下か。
constexpr double kEqBaselineDrop = 8.0;
// 音符が昇る距離（板の外へ出てよい）と、左右の揺れ幅。
constexpr double kEqNoteRise    = 34.0;
constexpr double kEqNoteSway    = 8.0;

// 棒が占める横幅。
constexpr double kEqBarsWidth =
    kEqBarCount * kEqBarWidth + (kEqBarCount - 1) * kEqBarGap;

} // namespace

CanvasVideo::CanvasVideo(QObject *parent)
    : QObject(parent)
{
    warmUpPanelFontsOnce();
    connect(&m_controller, &BubbleController::frameUpdated,
            this, [this](){ emit frameReady(); });
    connect(&m_controller, &BubbleController::stateChanged,
            this, [this](BubbleState newState, BubbleState oldState){
        emit playbackUiChanged();
        // 2026-09-15 非同期化 P3: 描画可否を遷移の瞬間にワーカーへ反映する。
        if (m_mpvPlayer && m_mpvPlayer->isAsyncRender()) {
            m_mpvPlayer->setAsyncRenderAllowed(
                !videoRenderSuppressedFor(newState, m_source.kind));
        }
        // 2026-09-12: ラベルのコントローラから選んで移動した場合は、
        // ロードが終わった時点で必ず再生を始める。
        // BubbleController::onPlayerFirstFrameReady() はロード完了時に
        // 直前の状態（m_sleepWakeResumePlayback）次第で一時停止のまま
        // 止まることがあり、連続再生（再生し終わってから差し替わる）
        // では問題にならないが、再生中にユーザーが明示的に選んだ移動で
        // それが起きると「クリックしたのに始まらない」になる。
        // 遷移の途中で再入しないよう、次のイベントループで再生する。
        // Readyだけを合図にする。Pausedでも拾うと、移動前にこちらから
        // 掛けた一時停止を「ロード完了」と誤認してしまう（navigateToUrl
        // のコメント参照）。
        // 【2026-09-13 追記】「移動先のロードが終わった」合図として使える
        // Readyは、mpvが実際に移動先を抱えている場合のReadyだけ。
        // BubbleController::sourceUrl() は loadUrl() のときに更新されるので、
        // これが m_source と一致していなければ、そのReadyは「移動前の動画が
        // 今ごろロードを終えた」もの（＝解決を待っている間に前の動画が
        // Loading→Readyと進んだケース）になる。以前はそれも合図として
        // 食べてしまい、
        //   ・前の動画の方が勝手に再生を始める
        //   ・使い切ったフラグのせいで移動先が1コマ目で止まる
        // という二重の食い違いを起こしていた。
        if (m_navigatePlayPending && newState == BubbleState::Ready
            && controllerHoldsCurrentSource()) {
            m_navigatePlayPending = false;
            // 予約した時点の移動世代。発火までに次の移動が始まっていたら
            // （＝mpvの中身がもう別物なら）この予約は用済みなので捨てる。
            const quint32 gen = m_navigateGen;
            QTimer::singleShot(0, this, [this, gen]{
                if (gen != m_navigateGen)
                    return;
                if (!m_controller.isPlaying())
                    bcPlay();
            });
            // 念のための保険。上のbcPlay()が「まだロードが終わっていない」
            // 等で空振りした場合に備え、少し待ってもう一度だけ確認する
            // （既に再生中なら何もしない）。
            QTimer::singleShot(700, this, [this, gen]{
                if (gen != m_navigateGen)
                    return;
                if (!m_controller.isPlaying()
                    && m_controller.state() != BubbleState::Loading)
                    bcPlay();
            });
        }
        // 移動の終わり。移動先が実際に動き出した（Playing/AudioOnly）か、
        // 失敗・休止で終わったなら、切り替え中の扱いを解除する
        // （停止マークの抑制もここで解ける）。
        if (m_navigateInFlight) {
            const bool arrived = controllerHoldsCurrentSource()
                && (newState == BubbleState::Playing
                    || newState == BubbleState::AudioOnly);
            if (arrived
                || newState == BubbleState::Error
                || newState == BubbleState::Sleep
                || newState == BubbleState::FocusSleep)
                endNavigation();
        }
        // 【2026-07-14 検討済み・不採用】ライブのAudioOnly→Playing復帰時に
        // 「音声の現在位置へ精密シーク」して映像を音声位置から再開させる案を
        // 実装・テストしたが削除した。理由:
        //  (1) AudioOnly中は映像パケットがdemuxerキャッシュに存在しないため、
        //      シークしても映像は再配置できず（lavf: "seeking stream 1 (video)
        //      to nothing" / HLSでは "Cached seek not possible"）、映像は結局
        //      プレイリスト最先端から取得される＝音ズレ解消の効果がない。
        //  (2) シーク自体が再バッファリング連発（Playing→Buffering往復・
        //      音声underrun）を誘発し、復帰時のカクつきの原因になった。
        // よって復帰後の同期はmpv標準のA/V同期（映像側の速度調整・フレーム
        // ドロップで徐々に収束）に任せる（従来通り）。
        //
        // 2026-07-14: ライブメディア（プラグインがseekBarVisible=falseを指定。
        // SRT画面共有等）の画面外AudioOnlyは、音声継続のため維持する。
        // ただし低キャッシュモードは適用しない（下記）。
        //
        // 【背景】AudioOnly移行時、BubbleControllerは省メモリのため
        // setLowCacheMode(true)（demuxer-max-bytes=4MiB等）を適用する。
        // 画面共有のような高ビットレートのライブTS（mpeg2video 1080p60等、
        // 数十Mbps級）では4MiBが1秒未満に相当し、mpvが「キャッシュが目標
        // 秒数貯まるまで再生を止める」動き（paused-for-cache）とバイト上限が
        // 噛み合って、貯め切れないまま再生停止で固まることがある。再生が
        // 止まると消費ゼロ→プッシュ型SRTの受信キューが十数秒で溢れ、届く
        // パケット全てに"No room to store incoming packet"警告がstderrへ
        // 出続ける（洪水でアプリが応答なしに見える実害が報告された）。
        //
        // そこでライブに限り、AudioOnly移行直後に低キャッシュモードを
        // 解除して通常上限（16MiB/8s）へ戻す。BubbleController側の適用と
        // 順序が前後しないよう、次のイベントループで実行する。
        // 5秒後にmpvのキャッシュ診断もログへ出す（それでも詰まる場合の
        // 原因特定用: paused-for-cache / cache-used を確認できる）。
        if (!m_seekBarVisible && m_source.kind == MediaKind::PluginMedia
            && newState == BubbleState::AudioOnly) {
            QTimer::singleShot(0, this, [this] {
                if (m_controller.state() != BubbleState::AudioOnly || !m_mpvPlayer)
                    return;
                qDebug("[CanvasVideo %p] live AudioOnly: keep normal cache limits",
                       static_cast<void*>(this));
                m_mpvPlayer->setLowCacheMode(false);
            });
            QTimer::singleShot(5000, this, [this] {
                if (m_controller.state() == BubbleState::AudioOnly && m_mpvPlayer)
                    m_mpvPlayer->logDiagnostics("LiveAudioOnly");
            });
        }
        // Sleep（完全休止・復帰は最初から再ロード）は、AudioOnly/FocusSleepと
        // 違って「フリーズフレームをそのまま見せ続ける」ことを想定していない
        // ため、mpv側のFBO/テクスチャ（見えていないGPUメモリ）を解放してよい。
        // 実際の解放はGLコンテキストがcurrentな次回paintGL()で行う。
        if (newState == BubbleState::Sleep) {
            m_sleepPending.markGlReleasePending();
            // 2026-07-03: メモリ節約 — FBO/テクスチャだけでなく
            // MpvPlayerオブジェクト自体（mpv_handle/render_context込み）を
            // 丸ごと解放する。Sleepは元々「復帰は最初から再ロード」前提の
            // 状態のため、位置保持等の複雑さなしに踏み込める
            // （FocusSleepには絶対に適用しないこと。md/bug.md参照）。
            //
            // 2026-09-14: Sleepで破棄せず保持する案を実測した結果、却下。
            // 破棄をやめるとSleep復帰の paintGL は 41ms → 15ms まで下がるが、
            // メモリの代償が大きすぎた（バブル数本で下記のとおり）。
            //   破棄あり（従来） WorkingSet=502MB PrivateUsage=611MB
            //   破棄なし（保持） WorkingSet=751MB PrivateUsage=1030MB
            // BubbleController.cpp の2026-07-03のメモにある「1個あたり
            // 数MB〜10MB」という見積もりは、そこに書かれている懸念のとおり
            // 甘かった。mpvコア・libassのフォントデータ等が stop() では
            // 返らず、1インスタンスあたり数十MB規模を抱え続ける。
            // 33個のバブルを常駐で抱えるアプリでこれは受け入れられないため、
            // 従来どおり破棄する。残る1フレームのヒッチについては
            // md/async_video_render_study_2026-09-14.md を参照
            // （mpvの描画をワーカースレッドへ逃がすのが本筋という結論）。
            m_sleepPending.markMpvPlayerTeardownPending();
        }
        // 2026-07-18追加: 再生が実際に始まった（Playingに到達した）ら、
        // それまでのエラーは解消したとみなし自動リトライ回数をリセット
        // する。長時間視聴中にたまたま複数回403に当たった場合でも、
        // その都度「まだリトライ余地あり」の状態に戻したいため。
        if (newState == BubbleState::Playing)
            m_autoRetryOnErrorCount = 0;
        // 2026-07-18追加: Error状態への自動復帰。従来はクリックでの
        // retryAfterError()呼び出しのみが復帰手段で、配信側の一過性の
        // 403（CDNチャンク取得失敗）等でErrorに落ちるとユーザーが気付いて
        // クリックし直すまでバブルが固まったままになっていた
        // （実際に報告された不具合）。ここで少し待ってから自動的に
        // retryAfterError()相当を試みる。本当に再生不可能な動画
        // （削除済み・地域制限等）まで無限リトライしないよう、
        // kMaxAutoRetryOnError回で打ち切り、それ以降は従来通り手動クリック
        // での再試行に委ねる（赤いエラー表示はretryAfterError()を呼ばない
        // 限り残るので、ユーザーには「失敗している」ことがちゃんと見える）。
        if (newState == BubbleState::Error && m_autoRetryOnErrorCount < kMaxAutoRetryOnError) {
            ++m_autoRetryOnErrorCount;
            const int attemptNo = m_autoRetryOnErrorCount;
            QPointer<CanvasVideo> guard(this);
            QTimer::singleShot(1500, this, [this, guard, attemptNo]() {
                if (!guard || m_controller.state() != BubbleState::Error)
                    return;
                qDebug("[CanvasVideo %p] auto-retry after error (attempt %d/%d)",
                       static_cast<void*>(this), attemptNo, kMaxAutoRetryOnError);
                retryAfterError();
            });
        }
        emit frameReady();
    });
    // 2026-07-03: Sleep中にMpvPlayerを解放済みの状態でwakeUp()が
    // 呼ばれた場合、BubbleController側ではプレイヤーを作り直せない
    // （OpenGLコンテキストが必要）ため、CanvasVideoへ再生成を依頼される。
    connect(&m_controller, &BubbleController::playerNeeded,
            this, [this](){
        m_sleepPending.markMpvRecreateForWakePending();
        emit frameReady();
    });
    connect(&m_controller, &BubbleController::playbackEnded,
            this, [this](){
        // 2026-09-12: 自動送りも「画面に見えているリスト」の順に従う。
        //
        // 【なぜ】▶▶ ボタン・行のクリック・表示順はすべてリスト（＝一覧＋
        // ユーザーの並べ替え）に従うのに、勝手に次へ進むときだけプラグインの
        // nextUrl を使っていたため、並べ替えたのに放っておくと並べ替え前の
        // 順で進む、という食い違いがあった。見えているものと起きることを
        // 一致させる。
        //
        // 【約束は壊していない】ここで本体がするのは「受け取った項目の列の
        // 次の行を再生する」だけで、列の意味（再生リストなのかMixなのか、
        // なぜその順なのか）には踏み込まない。順序の中身を作るのは変わらず
        // プラグイン。
        //
        // 【フォールバック】リストの終端に来た／一覧がまだ取れていない場合は
        // navigateRelativeInPanel() が false を返すので、下の従来経路
        // （nextUrl）へ落ちる。継ぎ足し待ちで無音になる事故を防ぐため。
        // 2026-09-22: 本体のリストは種類（ローカル／プラグイン）を問わず次へ。
        if (hasCoreList()) {
            if (navigateRelativeInPanel(1)) {
                qDebug().noquote() << QStringLiteral(
                    "[CanvasVideo %1] playback ended — advancing to the next item in the core list")
                        .arg(reinterpret_cast<quintptr>(this), 0, 16);
                return;
            }
            // 最後の動画だった。本体のリストの終わりなので、プラグインの
            // 「次のURL」（その動画の再生リストの続き等）へは進まず休止する。
            qDebug("[CanvasVideo %p] playback ended — end of core list",
                   static_cast<void*>(this));
            m_controller.requestSleepAfterPlaybackEnd();
            emit frameReady();
            return;
        } else if (m_source.kind == MediaKind::PluginMedia
            && navigateRelativeInPanel(1)) {
            qDebug().noquote() << QStringLiteral(
                "[CanvasVideo %1] playback ended — advancing to the next item in the list")
                    .arg(reinterpret_cast<quintptr>(this), 0, 16);
            return;
        }

        // リストを辿れなかったときは、その理由が分かるように一度だけ出す
        // （一覧が取れていないのか、単に終端なのかの切り分け用）。
        if (m_source.kind == MediaKind::PluginMedia) {
            const QList<PanelRow> dbgRows = orderedPanelRows();
            int dbgNow = -1;
            for (int i = 0; i < dbgRows.size(); ++i)
                if (dbgRows.at(i).kind == QStringLiteral("NOW")) { dbgNow = i; break; }
            qDebug().noquote() << QStringLiteral(
                "[CanvasVideo %1] playback ended — list advance unavailable "
                "(rows=%2 nowIndex=%3 snapshot=%4 nextUrl=%5)")
                    .arg(reinterpret_cast<quintptr>(this), 0, 16)
                    .arg(dbgRows.size()).arg(dbgNow).arg(m_panelSnapshot.size())
                    .arg(m_nextUrl.isEmpty() ? QStringLiteral("(none)") : m_nextUrl);
        }

        // 2026-07-17: 連続再生（PbResolvedMedia::nextUrl）。プラグインが
        // 「次に続けて解決してほしいURL」を返していた場合は、スリープへ
        // 移行する代わりにこのバブル上でそのURLへ差し替えて再生を続ける。
        // 本体はこれが「プレイリストの次動画」なのか単なる関連動画なのか
        // 一切関知しない（PbPluginApi.hのコメント参照）。
        if (m_source.kind == MediaKind::PluginMedia && !m_nextUrl.isEmpty()) {
            const QString next = m_nextUrl;
            m_nextUrl.clear();
            qDebug().noquote() << QStringLiteral(
                "[CanvasVideo %1] playback ended — continuing with next URL: %2")
                    .arg(reinterpret_cast<quintptr>(this), 0, 16).arg(next);
            // 2026-09-12: 自動の連続再生でも履歴を積む。ラベルの
            // コントローラの「前」が、クリック移動だけでなく勝手に
            // 進んだ分もきちんと辿れるようにするため。
            navigateToUrl(next, /*pushHistory=*/true);
            return;
        }
        // 2026-07-17追記: 最初の解決時点ではプラグイン側のプレイリスト
        // メンバー一覧取得（大きなMix/Radioリストだと数十秒かかりうる）が
        // 間に合わず、nextUrlが空のまま返ってきていた場合の救済。ここ
        // までの間（＝この動画の再生時間そのもの、通常は数十秒〜数分）
        // プラグイン側の裏取得が進んでいた可能性が高いので、同じURLを
        // もう一度だけ問い合わせて、答えが変わっていないか確認する
        // （probeContinuationAndAdvance参照）。本体側は相変わらず
        // 「プレイリスト」という概念自体は知らず、単に「もう一度聞いたら
        // 答えが変わっているかもしれない」という汎用的な振る舞いに
        // 留めている。
        //
        // 2026-07-18改訂: 従来はPluginMediaなら無条件に聞き直していたため、
        // 連続再生と無関係な単発動画でも再生終了のたびに解決処理一式
        // （解決ツール起動、数秒＋プラグイン側の同時実行枠1つ）が無駄に走って
        // いた。プラグインが「聞き直せば答えが変わる可能性がある」と
        // 申告した場合（m_nextUrlPending=true。PbResolvedMedia::
        // nextUrlPending参照）に限り聞き直す。falseなら「次は無い」が
        // 確定なので従来通りスリープへ。
        if (m_source.kind == MediaKind::PluginMedia && m_nextUrlPending) {
            probeContinuationAndAdvance();
            return;
        }
        qDebug("[CanvasVideo %p] playback ended — release decode resources",
               static_cast<void*>(this));
        m_controller.requestSleepAfterPlaybackEnd();
        emit frameReady();
    });
    connect(&AppSettings::instance(), &AppSettings::changed,
            this, [this] {
        if (!m_seekBarVisible && m_seekTimeModeLocked) {
            m_controller.setOffscreenAudioOnlyEnabled(
                AppSettings::instance().streamOffscreenResourceSavingEnabled());
        }
    });
}

CanvasVideo::~CanvasVideo()
{
    cancelThumbnailProviderRequest();
    // 呼び出し側が releaseGlResources() を呼び忘れた場合の保険。
    // ただしこの時点でGLコンテキストが有効である保証はないため、
    // mpvハンドル自体の解放（MpvPlayerのデストラクタ）は行われるが、
    // FBO等のGPUリソース解放はスキップされる可能性がある。
    // 正常経路では CanvasWidget 側で makeCurrent() 済みに
    // releaseGlResources() を呼んでから delete すること。
    //
    // 2026-07-15: 破棄そのものと🚫バグ対策の修復は destroyMpvPlayer()
    // （MpvTeardown.h）に一元化した。呼び出し元（ゴミ箱削除等）が既に
    // refreshDropTargets() を呼ぶ経路もあるが、二重に呼んでも無害
    // （OLE生存確認＋ドロップ登録の再確認は冪等）なので、ここでも常に
    // 呼ぶことで「呼び出し元が呼び忘れる」リスクを構造的に無くす。
    if (m_mpvPlayer) {
        qInfo("[CanvasVideo %p] [dtor] direct delete m_mpvPlayer=%p",
              static_cast<void*>(this), static_cast<void*>(m_mpvPlayer));
        destroyMpvPlayer(this, m_mpvPlayer);
    }
    // GL破棄待ちキューの保険（正常経路ではreleaseGlResources()で空になっている）。
    for (MpvPlayer *player : std::as_const(m_playersAwaitingGlDelete)) {
        qInfo("[CanvasVideo %p] [dtor] delete queued player=%p",
              static_cast<void*>(this), static_cast<void*>(player));
        destroyMpvPlayer(this, player);
    }
    m_playersAwaitingGlDelete.clear();
    if (m_thumbnailLoader) {
        m_thumbnailLoader->deleteLater();
        m_thumbnailLoader = nullptr;
    }
}

void CanvasVideo::releaseGlResources(Renderer &r)
{
    // GL文脈外からキューされた破棄待ちプレイヤーがあれば、GL current下の
    // ここで確実に片付ける（バブル削除時にpaintGL()がもう来ないため）。
    drainPlayersAwaitingGlDelete();

    if (m_mpvPlayer)
        m_mpvPlayer->releaseVideoResources();

    if (m_oldTexId != 0) {
        r.deleteTexture(m_oldTexId);
        m_oldTexId = 0;
    }
    if (m_thumbTexId != 0) {
        r.deleteTexture(m_thumbTexId);
        m_thumbTexId = 0;
        m_thumbReady = false;
    }
    m_thumbnailGpuReleasePending = false;
}

void CanvasVideo::applyEffectiveVolume()
{
    // 2026-09-14: サラウンドゲイン(m_surroundGain)の乗算レイヤーを廃止。
    const float effectiveVolume = qBound(0.0f, m_userVolume * m_masterGain, 1.0f);
    if (m_mpvPlayer)
        m_controller.setVolume(effectiveVolume);
}

QRectF CanvasVideo::itemRect() const
{
    return QRectF(m_pos.x(), m_pos.y(), m_origW * m_scale, m_origH * m_scale);
}

void CanvasVideo::loadUrl(const QString &url)
{
    m_nativeVideoSize = {};
    m_renderResolutionPolicy.reset();
    m_liveEdgeBufferMs = 0;
    m_liveBufferSeekApplied = false;
    m_controller.setOffscreenAudioOnlyEnabled(true);
    m_source = sourceFromUrl(url, MediaKind::Unknown);
    m_streamUrl.clear();
    m_streamHttpHeaders.clear();
    m_deferredLoad = false;
    m_deferredFetchInProgress = false;
    m_playAfterDeferredLoad = true;
    m_resumePositionMs = 0;
    m_currentQuality = 480;
    qWarning("[CanvasVideo] External media URL loading is disabled in core: %s",
             qPrintable(url));
}

void CanvasVideo::loadDirectUrl(const QString &url)
{
    loadMediaSource(sourceFromUrl(url, MediaKind::LocalVideo));
}

void CanvasVideo::loadMediaSource(const MediaSource &source)
{
    cancelThumbnailProviderRequest();
    m_thumbnailImage = {};
    m_thumbnailPending = {};
    m_thumbnailPixelSize = {};
    m_cachedThumbnailPngBase64.clear();
    m_cachedThumbnailPngBase64Key = -1;
    m_thumbnailResidency = ThumbnailResidency::Cold;
    m_thumbnailProviderFallbackActive = false;
    m_thumbnailProviderCheckMs = 0;
    m_thumbnailProviderAvailable = false;
    if (m_thumbTexId != 0) {
        m_thumbnailGpuReleasePending = true;
        emit frameReady();
    }
    m_nativeVideoSize = {};
    m_renderResolutionPolicy.reset();
    m_liveEdgeBufferMs = 0;
    m_liveBufferSeekApplied = false;
    m_controller.setOffscreenAudioOnlyEnabled(true);
    // 連続再生（playbackEnded直後の続行呼び出し含む）で既に消費済みのため
    // 基本的には空のはずだが、他経路からの呼び出しでも前の内容を持ち越さ
    // ないよう明示的にクリアしておく（実際の値は resolveViaPlugin() の
    // 解決成功時に改めて設定される）。
    m_nextUrl.clear();
    m_nextUrlPending = false;
    // 2026-09-22: 本体のリストの中でプラグインの動画からローカルの動画へ移った
    // とき、前の動画のラベル（プラグインのロゴ等）を持ち越さない。プラグインの
    // メディアは resolveViaPlugin() が同じことをする。
    if (source.kind != MediaKind::PluginMedia) {
        m_labelIconSvg.clear();
        m_labelText.clear();
        m_labelHoverable = false;
        m_labelPinnable  = false;
        m_labelProgress  = false;
    }
    m_source = source;
    // file:// URL / 生のWindowsパスのどちらで来ても扱えるよう、実際の
    // ローカルファイルパスをここで一度だけ求めておく（タイトル表示・
    // Windows Shellサムネイル取得の両方で使う）。
    const QString localPath = QUrl(m_source.originalPathOrUrl).toLocalFile();
    const QString localFilePath = localPath.isEmpty() ? m_source.originalPathOrUrl : localPath;
    if (m_source.title.isEmpty())
        m_source.title = QFileInfo(localFilePath).fileName();
    m_streamHttpHeaders.clear();
    m_deferredLoad = false;
    m_deferredFetchInProgress = false;
    m_playAfterDeferredLoad = true;
    m_resumePositionMs = 0;
    // 2026-09-23: 長押しの切り替え（Solo → Mix 等）・その元に戻すは、同じ動画の
    // 別の見え方へ移るだけなので、今の再生位置から続ける。
    if (m_pendingNavigateResumeMs > 0) {
        m_resumePositionMs = m_pendingNavigateResumeMs;
        m_pendingNavigateResumeMs = 0;
    }
    m_currentQuality = 480;
    // 2026-07-18追加: 新しいソースの読み込み（連続再生でのnextUrl差し替え
    // 含む）は、直前の動画で何回自動リトライを消費していたかとは無関係の
    // 別物として扱う。ここでリセットしないと、1本前の動画で403に複数回
    // 当たっただけで次の動画の自動リトライ余地まで食い潰されてしまう。
    m_autoRetryOnErrorCount = 0;
    if (m_source.kind == MediaKind::LocalAudio) {
        const QPointF previousCenter = itemRect().center();
        m_origW = 160;
        m_origH = 160;
        m_renderW = 160;
        m_renderH = 160;
        setScale(m_scale);
        setCenter(previousCenter);
    }
    // ネイティブ縦横比への追従は動画バブルごとに一度だけ行う
    // （下記paintGL()参照）。CanvasVideoインスタンスは新規バブルごとに
    // 生成されるため通常falseのままだが、念のためロード時にリセットする。
    m_localVideoAspectApplied = false;
    m_pluginProvidedDims = false;
    if (!m_source.thumbnailPathOrUrl.isEmpty())
        m_thumbnailUrl = m_source.thumbnailPathOrUrl;

    // 2026-07-08: ローカル動画には（プラグイン解決動画のthumbnailUrlと違い）
    // これまでサムネイル取得元が一切無かった（一時停止アイドル解放や
    // Sleep中は真っ黒のプレースホルダになるだけだった）。Windows Explorerの
    // サムネイルキャッシュを流用して埋める。再生開始前から使えるよう、
    // ロード直後にリクエストしておく（プラグイン動画のonUrlFetched()内
    // loadThumbnail()呼び出しと同じタイミング感）。
    // LocalAudioも同様（mp3/wav等のID3埋め込みジャケット）。Explorerは
    // ジャケット付き音声ファイルもサムネイル表示できるため同じ経路で
    // 取得できる。ジャケットが無いファイルはSIIGBF_THUMBNAILONLYで
    // 単に失敗するだけなので、paintLocalAudioCard()側の従来表示
    // （タイトル+♪+擬似シークバー）へ自然にフォールバックする。
    if ((m_source.kind == MediaKind::LocalVideo || m_source.kind == MediaKind::LocalAudio)
     && m_thumbnailUrl.isEmpty())
        loadLocalThumbnail(localFilePath);
    // 2026-10-12: Web動画は保存済み（webvideo/）ならそのファイルからサムネイルを取る。
    if (m_source.kind == MediaKind::WebVideo && m_thumbnailUrl.isEmpty()) {
        const QString cachedFile = WebVideoCache::completedCacheFile(m_source.originalPathOrUrl);
        if (!cachedFile.isEmpty())
            loadLocalThumbnail(cachedFile);
    }

    if (m_source.kind == MediaKind::WebVideo) {
        // 2026-10-12: 保存済みならローカルファイルから、まだならURLから直接
        // （mpv がダウンロードしながら）再生する。裏では保存を始めておく。
        m_streamUrl = WebVideoCache::playableUrlFor(m_source.originalPathOrUrl);
        WebVideoCache::ensureCached(m_source.originalPathOrUrl);
        startPlayback();
        return;
    }

    if (m_source.kind == MediaKind::PluginMedia) {
        // originalPathOrUrl はページURLであり、まだ再生可能な
        // ストリームURLではない。プラグインの非同期解決を待つ。
        m_streamUrl.clear();
        resolveViaPlugin(m_source.originalPathOrUrl);
        return;
    }

    m_streamUrl = m_source.originalPathOrUrl;
    startPlayback();
}

void CanvasVideo::resolveViaPlugin(const QString &url)
{
    qDebug() << "[Plugin][CanvasVideo] resolving via plugin:" << url;

    // 新しい解決を始めるので、前回の失敗表示は一旦クリアする
    // （リトライ中に赤いエラー表示が出続けるのを防ぐ）。
    m_pluginResolveFailure.clear();
    // 前回の解決で設定された連続再生用URLも一旦クリアする。今回の
    // resolve()結果（成功時）で改めて設定し直す。ここでクリアしないと、
    // 失敗時や「次はない」動画への再解決時に古い値が残り続けてしまう。
    m_nextUrl.clear();
    m_nextUrlPending = false;
    // 同様にフォーカスラベルも一旦クリアする（繋ぎ用の sticky は残す。
    // 解決が終わるまでの空白を埋めるため。結果が空なら onUrlFetched が
    // sticky も捨てる）。
    m_labelIconSvg.clear();
    m_labelText.clear();
    m_labelHoverable = false;
    m_labelPinnable  = false;
    m_labelProgress  = false;
    m_pluginLabelKnown = false;   // 2026-09-22: 答えが返るまで本体のラベルも出さない
    emit frameReady();

    // findHandler() を直接使うのは、解決に使ったプラグインの pluginId() を
    // m_source.resolverPluginId へ記録するため（セッション保存・エラー表示用の
    // ヒント。本体側の分岐には使わない）。
    IPbMediaPlugin *plugin = PluginMediaResolver::findHandler(url);
    if (!plugin) {
        const QString hint = m_source.resolverPluginId.isEmpty()
            ? QString()
            : QStringLiteral(" (requires plugin: %1)").arg(m_source.resolverPluginId);
        const QString message = QStringLiteral("No compatible plugin found%1").arg(hint);
        qWarning().noquote() << QStringLiteral("[Plugin][CanvasVideo] no plugin handler for: %1 - %2").arg(url, message);
        // 復元直後の再生試行だった場合、ここで固まったままにならないよう
        // 再試行できる状態に戻しておく（例: プラグインを入れ直して再度クリック）。
        m_deferredFetchInProgress = false;
        m_pluginResolveFailure.set(message, static_cast<int>(PbResolveErrorCode::NoHandler));
        m_navigatePlayPending = false;
        endNavigation();
        emit frameReady();
        return;
    }

    m_source.resolverPluginId = plugin->pluginId();

    QPointer<CanvasVideo> guard(this);
    // 2026-09-12: この解決を要求した時点のURL。解決の待ち時間中に
    // 別の動画へ移動していた場合（ラベルのコントローラで続けて選んだ等）、
    // 古い解決結果がそのまま onUrlFetched() まで流れて新しい動画を
    // 上書きしてしまい、「何回か移動すると再生できなくなる」状態に
    // なっていた。probeContinuationAndAdvance() と同じ考え方で、
    // 自分が要求したURLのままかどうかを確認してから反映する。
    const QString requestedUrl = url;
    // 初回読み込みは AppSettings::quality()（既定480p）を要求する。
    // 2026-07-03: 以前はバブル/フォーカスで画質を使い分けていたため、
    // 新規アイテムは常に非フォーカスのバブルとして追加される
    // （CanvasWidget::addMediaSource()）ことから「バブル時のデフォルト」を
    // 初回解決に使っていたが、設定を1本化したため単純に quality() を使う。
    PluginMediaResolver::resolve(plugin, url, AppSettings::instance().quality(),
                                  [guard, requestedUrl](const PbResolvedMedia &result) {
        if (!guard) {
            qDebug() << "[Plugin][CanvasVideo] resolve callback: video already destroyed, ignore";
            return;
        }
        if (guard->m_source.originalPathOrUrl != requestedUrl) {
            qDebug().noquote() << QStringLiteral(
                "[Plugin][CanvasVideo] resolve callback: source changed meanwhile "
                "(requested=%1, current=%2), ignore")
                    .arg(requestedUrl, guard->m_source.originalPathOrUrl);
            return;
        }
        if (!result.success || result.streamUrl.isEmpty()) {
            const QString message = result.errorMessage.isEmpty()
                ? QStringLiteral("Failed to load the video")
                : result.errorMessage;
            qWarning().noquote() << QStringLiteral("[Plugin][CanvasVideo] resolve failed: %1 (%2)")
                                        .arg(guard->m_source.originalPathOrUrl, message);
            guard->m_deferredFetchInProgress = false;
            guard->m_pluginResolveFailure.set(message, static_cast<int>(result.errorCode));
            // 2026-09-13: 解決が失敗した時点で切り替えは終わり。ここで
            // 解除しないと、移動先が来ないまま「切り替え中」の見た目
            // （停止マークを出さない）が残ってしまう。
            guard->m_navigatePlayPending = false;
            guard->endNavigation();
            emit guard->frameReady();
            return;
        }
        if (!result.title.isEmpty())
            guard->m_source.title = result.title;

        // 連続再生用URL（空文字列 = 次はない）。playbackEnded 受信時に
        // 参照される（本体はこれが何を意味するか一切解釈しない）。
        guard->m_nextUrl = result.nextUrl;
        // nextUrlが空の場合の「聞き直せば変わる可能性」ヒント
        // （PbResolvedMedia::nextUrlPending / m_nextUrlPending宣言参照）。
        guard->m_nextUrlPending = result.nextUrlPending;

        // フォーカスラベル（両方空 = 何も表示しない）。実際の描画は
        // paintPlaybackOverlay() 呼び出し時に CanvasVideoLabelPainter が行う。
        guard->m_labelIconSvg = result.labelIconSvg;
        guard->m_labelText = result.labelText;
        // 2026-09-22: プラグインの答えが返った。テキストが空なら本体の
        // 基本ラベル（Solo / List）が出る（paintLabelAndPanel）。繋ぎ用の
        // sticky は描画時に「今のラベル」で上書きされるので、ここで捨てる
        // 必要はなくなった。
        guard->m_pluginLabelKnown = true;
        guard->m_labelHoverable = result.labelHoverable;
        guard->m_labelPinnable  = result.labelPinnable;
        guard->m_labelProgress  = result.labelProgress;
        emit guard->playbackUiChanged();

        // Phase 8+: プラグイン制御シークバー（配信/動画判別用）。
        // プラグインが明示的に設定した値をそのまま採用する。本体側は
        // これが「配信かどうか」等のサービス固有の意味を一切解釈しない。
        // seekBarColorはプラグインAPI側がQt6::Core依存で完結するよう
        // 軽量なPbColorで渡されるため、ここでQColorへ変換する
        // （isSet=falseならホスト既定色を使う＝無効QColorのままにする）。
        guard->m_seekBarVisible = result.seekBarVisible;
        guard->m_seekBarColor = result.seekBarColor.isSet
            ? QColor(result.seekBarColor.r, result.seekBarColor.g,
                     result.seekBarColor.b, result.seekBarColor.a)
            : QColor();
        guard->m_seekTimeMode = result.seekBarTimeMode;
        guard->m_seekTimeModeLocked = result.seekBarTimeModeLocked;
        // ライブ配信の初期バッファ。新しい解決のたびにワンショットフラグを
        // リセットし、tickLiveEdgeBufferSeek() が改めて1回だけ処理する。
        guard->m_liveEdgeBufferMs = result.liveEdgeBufferMs;
        guard->m_liveBufferSeekApplied = false;
        // 通常動画は常にAudioOnly対象。ライブ配信だけは島設定で切り替える。
        // liveEdgeBufferMs=0 のライブ（SRT等）もあるため、初期バッファ値では
        // なく既存のライブUIプロファイルで判定する。
        guard->m_controller.setOffscreenAudioOnlyEnabled(
            result.seekBarVisible || !result.seekBarTimeModeLocked
                || AppSettings::instance().streamOffscreenResourceSavingEnabled());
        // m_currentQuality はデバッグ表示（[Memory][Bubble] q=...p）等が
        // 読む値のため、プレースホルダの480固定のままにせず実際に解決
        // された高さへ更新しておく。
        guard->m_currentQuality = result.resolvedHeight;

        // 既存の（旧・本体内蔵解決版由来の）URL解決完了経路にそのまま合流させる。
        // ここで m_streamUrl / m_streamHttpHeaders が設定され、startPlayback() が
        // 呼ばれる（startPlayback() 内で m_deferredLoad / m_deferredFetchInProgress
        // も false へリセットされる）。
        // resolvedWidth/resolvedHeight を渡すことで、内部レンダー解像度
        // （m_renderW/m_renderH）が実際のストリーム解像度に合わせて
        // 初期化される（以前は常に0,0が渡され、常に480x270固定だった）。
        guard->onUrlFetched(result.streamUrl, result.resolvedWidth, result.resolvedHeight,
                            result.thumbnailUrl, result.httpHeaders, result.audioStreamUrl);
    });
}

void CanvasVideo::probeContinuationAndAdvance()
{
    // 2026-07-17追加: playbackEnded 受信時点で m_nextUrl が空だった場合の
    // 最後の救済策。最初の resolve() 時点ではプラグイン側の非同期な
    // 裏取得（例: 大きなプレイリストのメンバー一覧取得）がまだ間に
    // 合っておらず nextUrl が空のまま返ってきていた可能性がある。今この
    // 動画は最初から最後まで再生し終わっているので、その裏取得には
    // 十分な時間（この動画の再生時間そのもの）が経過しているはずで、
    // 同じURLをもう一度だけ resolve() し直せば答えが変わっているかも
    // しれない。
    //
    // resolveViaPlugin() をそのまま使わないのは、あちらは
    // 「streamUrlを実際にmpvへロードする」ところまで一直線に進む前提の
    // 実装（m_nextUrl/ラベル/シークバー状態のクリア→onUrlFetched()→
    // startPlayback()）だから。ここでは nextUrl の値だけを覗き見たい
    // （streamUrlは既に再生し終わった今の動画のものなので使わない）ので、
    // 専用の軽量パスにしてある。
    //
    // 本体側はここでも「なぜ後から答えが変わりうるのか」は一切知らない。
    // 単に「もう一度聞いてみる」という汎用的な振る舞いに留めている。
    if (m_source.kind != MediaKind::PluginMedia || m_source.originalPathOrUrl.isEmpty()) {
        m_controller.requestSleepAfterPlaybackEnd();
        emit frameReady();
        return;
    }

    IPbMediaPlugin *plugin = PluginMediaResolver::findHandler(m_source.originalPathOrUrl);
    if (!plugin) {
        m_controller.requestSleepAfterPlaybackEnd();
        emit frameReady();
        return;
    }

    const QString probedUrl = m_source.originalPathOrUrl;
    qDebug().noquote() << QStringLiteral(
        "[CanvasVideo %1] playback ended with no nextUrl yet — probing plugin once more: %2")
            .arg(reinterpret_cast<quintptr>(this), 0, 16).arg(probedUrl);

    QPointer<CanvasVideo> guard(this);
    PluginMediaResolver::resolve(plugin, probedUrl, AppSettings::instance().quality(),
        [guard, probedUrl](const PbResolvedMedia &result) {
        if (!guard) {
            qDebug() << "[Plugin][CanvasVideo] continuation probe: video already destroyed, ignore";
            return;
        }
        // 探りを入れている間に、既に別の動画へ切り替わっていた場合
        // （ユーザー操作等）、この結果はもう関係ないので無視する。
        if (guard->m_source.kind != MediaKind::PluginMedia
            || guard->m_source.originalPathOrUrl != probedUrl) {
            qDebug() << "[Plugin][CanvasVideo] continuation probe: source changed meanwhile, ignore";
            return;
        }

        if (result.success && !result.nextUrl.isEmpty()) {
            qDebug().noquote() << QStringLiteral(
                "[CanvasVideo %1] continuation probe succeeded — continuing with: %2")
                    .arg(reinterpret_cast<quintptr>(guard.data()), 0, 16).arg(result.nextUrl);
            // 2026-09-12: こちらの経路でも履歴を積む（ラベルの
            // コントローラの「前」用。navigateToUrl()のコメント参照）。
            guard->navigateToUrl(result.nextUrl, /*pushHistory=*/true);
            return;
        }

        qDebug() << "[Plugin][CanvasVideo] continuation probe: still no next URL, going to sleep";
        guard->m_controller.requestSleepAfterPlaybackEnd();
        emit guard->frameReady();
    });
}

void CanvasVideo::restoreLazy(const QString &sourceUrl,
                              const QString &thumbnailUrl,
                              qint64 resumePositionMs,
                              float volume,
                              int originalWidth,
                              int originalHeight,
                              MediaKind kind,
                              const QString &title,
                              const QString &pluginIdHint)
{
    cancelThumbnailProviderRequest();
    m_thumbnailProviderCheckMs = 0;
    m_thumbnailProviderAvailable = false;
    m_thumbnailProviderFallbackActive = false;
    m_nativeVideoSize = {};
    m_renderResolutionPolicy.reset();
    m_source = sourceFromUrl(sourceUrl, kind);
    if (!title.isEmpty())
        m_source.title = title;
    m_source.thumbnailPathOrUrl = thumbnailUrl;
    m_source.resolverPluginId = pluginIdHint;
    m_streamUrl.clear();
    m_streamHttpHeaders.clear();
    // 前のバブル内容（あれば）の連続再生用URLを持ち越さない。
    m_nextUrl.clear();
    m_nextUrlPending = false;
    // 実際の再生時にbeginDeferredLoadForPlayback()→resolveViaPlugin()→
    // onUrlFetched()が改めて設定するまでは「解像度未提供」として扱う。
    m_pluginProvidedDims = false;
    m_deferredLoad = true;
    m_deferredFetchInProgress = false;
    m_playAfterDeferredLoad = false;
    m_resumePositionMs = qMax<qint64>(0, resumePositionMs);
    m_currentQuality = 480;
    if (originalWidth > 0 && originalHeight > 0) {
        m_origW = originalWidth;
        m_origH = originalHeight;
        // セッション復元時点では実際のストリーム解像度は分からないため、
        // ひとまず保存値をそのまま使う。実際に再生開始されると
        // beginDeferredLoadForPlayback() → resolveViaPlugin() →
        // onUrlFetched() が改めて実解像度で m_renderW/m_renderH を
        // 更新する（PluginMedia の場合）。
        // 2026-09-14: onUrlFetched()と同じ理由で、保存値そのまま（＝ネイティブ
        // 解像度）ではなく480段へ量子化した値を初期FBOサイズにする。
        const QSize seed = VideoRenderResolutionPolicy::quantizedTarget(
            QSize(originalWidth, originalHeight), 480.0);
        m_renderW = seed.isValid() ? seed.width()  : originalWidth;
        m_renderH = seed.isValid() ? seed.height() : originalHeight;
    }
    setVolume(volume);
    m_thumbnailUrl = thumbnailUrl;

    // 2026-07-08: セッション復元直後（m_deferredLoad=true、まだ一度も
    // 再生していない）の状態でも、ローカル動画/音声ならWindows Explorerの
    // サムネイルを表示したい。beginDeferredLoadForPlayback()は
    // loadMediaSource()を経由しない別経路（直接startPlayback()を呼ぶ）
    // なので、ここで独立して要求しておく必要がある。
    if ((m_source.kind == MediaKind::LocalVideo || m_source.kind == MediaKind::LocalAudio)
     && m_thumbnailUrl.isEmpty()) {
        const QString localPath = QUrl(sourceUrl).toLocalFile();
        loadLocalThumbnail(localPath.isEmpty() ? sourceUrl : localPath);
    }
    // 2026-10-12: Web動画は保存済み（webvideo/）ならそのファイルからサムネイルを取る。
    if (m_source.kind == MediaKind::WebVideo && m_thumbnailUrl.isEmpty()) {
        const QString cachedFile = WebVideoCache::completedCacheFile(sourceUrl);
        if (!cachedFile.isEmpty())
            loadLocalThumbnail(cachedFile);
    }
}

void CanvasVideo::onUrlFetched(const QString &streamUrl, int w, int h,
                               const QString &thumbnailUrl,
                               const QStringList &httpHeaders,
                               const QString &audioUrl)
{
    m_renderResolutionPolicy.reset();
    m_nativeVideoSize = (w > 0 && h > 0) ? QSize(w, h) : QSize{};
    m_streamUrl = streamUrl;
    m_streamAudioUrl = audioUrl;
    m_streamHttpHeaders = httpHeaders;
    // プラグインが解像度を返したかどうか。返さなかった場合はmpvの
    // デコード解像度が判明した時点で縦横比も補完する。
    m_pluginProvidedDims = (w > 0 && h > 0);
    if (w > 0 && h > 0) {
        const QPointF previousCenter = itemRect().center();
        m_origW = 480;
        m_origH = static_cast<int>(480.0 * h / w);
        setScale(m_scale);
        setCenter(previousCenter);
        // 2026-09-14: ここは以前 m_renderW=w, m_renderH=h（＝ネイティブ解像度）
        // だった。しかしバブルは既定で480px幅程度の表示しかしないため、
        // 1920x1080のFBOへ焼くのは9倍の無駄になる。しかも
        // VideoRenderResolutionPolicy の縮小デバウンスは700msあるので、
        // 「最初の0.7秒だけ9倍の画素数で焼き続ける」状態になっていた。
        // 実測: その最初の1枚で mpv_render_context_render() が21ms
        //   [MpvPlayer 0x..] slow mpv_render_context_render: 21ms (fbo=1920x1080 first=1)
        // これが「再生開始の瞬間にカクッとする」の正体。
        // 初期値を480段へ量子化しておけば最初から小さいFBOで始まり、
        // バブルが実際に大きければポリシーが180ms後に引き上げる。
        const QSize seed = VideoRenderResolutionPolicy::quantizedTarget(
            QSize(w, h), 480.0);
        m_renderW = seed.isValid() ? seed.width()  : w;
        m_renderH = seed.isValid() ? seed.height() : h;
        emit videoDimensionsReady();
    }
    if (!thumbnailUrl.isEmpty()) {
        m_thumbnailUrl = thumbnailUrl;
        if (!hasThumbnailProvider())
            loadThumbnail(thumbnailUrl);
    }
    startPlayback();
}

void CanvasVideo::ensureThumbnailLoader()
{
    if (m_thumbnailLoader)
        return;
    m_thumbnailLoader = new ThumbnailLoader(this);
    // ThumbnailLoader::thumbnailReady()はデコード・縮小・
    // Format_RGBA8888変換済みの画像を渡してくる（元のloadThumbnail()内
    // ラムダの処理と同一）。ここでの保持・再描画要求のみCanvasVideo側の
    // 責務として残す。取得元（ネットワークDL/Windows Shell）を問わず
    // 同じ経路で反映される。
    connect(m_thumbnailLoader, &ThumbnailLoader::thumbnailReady,
            this, [this](const QImage &image) {
        m_thumbnailImage = image;
        m_thumbnailPending = image;
        m_thumbnailPixelSize = image.size();
        m_thumbnailResidency = ThumbnailResidency::CpuReady;
        emit frameReady();
    });
}

bool CanvasVideo::hasThumbnailProvider() const
{
    if (m_source.kind != MediaKind::PluginMedia
        || m_source.originalPathOrUrl.isEmpty())
        return false;
    if (m_thumbnailProviderAvailable)
        return true;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_thumbnailProviderCheckMs > 0
        && nowMs - m_thumbnailProviderCheckMs < 1000)
        return false;
    m_thumbnailProviderCheckMs = nowMs;
    m_thumbnailProviderAvailable =
        PluginManager::instance().findThumbnailProvider(
            m_source.originalPathOrUrl) != nullptr;
    return m_thumbnailProviderAvailable;
}

void CanvasVideo::requestThumbnailFromProvider()
{
    if (m_thumbnailRequestId != 0 || m_source.kind != MediaKind::PluginMedia)
        return;
    IPbThumbnailProvider *provider = PluginManager::instance().findThumbnailProvider(
        m_source.originalPathOrUrl);
    if (!provider)
        return;

    const quint64 requestId = s_nextThumbnailRequestId.fetch_add(1);
    const QString requestedUrl = m_source.originalPathOrUrl;
    m_thumbnailRequestId = requestId;
    m_thumbnailResidency = ThumbnailResidency::Loading;

    PbThumbnailRequest request;
    request.requestId = requestId;
    request.sourceUrl = requestedUrl;
    request.thumbnailUrlHint = m_thumbnailUrl;
    request.maxDimension = 640;

    QPointer<CanvasVideo> guard(this);
    provider->requestThumbnail(request, [guard, requestId, requestedUrl](
                                      const PbThumbnailResult &result) {
        if (!guard || guard->m_thumbnailRequestId != requestId
            || guard->m_source.originalPathOrUrl != requestedUrl)
            return;
        if (!result.success || result.encodedImage.isEmpty()
            || result.encodedImage.size() > kMaxProviderThumbnailBytes) {
            guard->m_thumbnailRequestId = 0;
            guard->m_thumbnailResidency = ThumbnailResidency::Failed;
            guard->m_thumbnailRetryAfterMs =
                QDateTime::currentMSecsSinceEpoch() + kThumbnailRetryDelayMs;
            if (guard->m_thumbnailProviderFallbackActive
                && !guard->m_cachedThumbnailPngBase64.isEmpty()
                && !guard->m_thumbReady
                && guard->m_thumbnailPending.isNull()) {
                guard->restoreProviderFallbackAsync(
                    guard->m_thumbnailRetryAfterMs);
            }
            qWarning().noquote() << QStringLiteral(
                "[ThumbnailProvider] request failed for %1: %2")
                    .arg(requestedUrl, result.errorMessage);
            return;
        }

        const QByteArray encoded = result.encodedImage;
        QThreadPool::globalInstance()->start([guard, requestId, requestedUrl, encoded] {
            QBuffer buffer;
            buffer.setData(encoded);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            const QSize sourceSize = reader.size();
            QImage decoded;
            // A thumbnail is displayed at <=640px. Allow enough headroom for
            // normal high-resolution artwork without permitting a compressed
            // image to trigger hundreds of MiB of decoder scratch storage.
            constexpr qint64 kMaxDecodedPixels = 16LL * 1024LL * 1024LL;
            if (sourceSize.isValid()
                && static_cast<qint64>(sourceSize.width()) * sourceSize.height()
                       <= kMaxDecodedPixels) {
                if (qMax(sourceSize.width(), sourceSize.height()) > 640)
                    reader.setScaledSize(sourceSize.scaled(
                        QSize(640, 640), Qt::KeepAspectRatio));
                decoded = reader.read();
                if (!decoded.isNull())
                    decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
            }
            QMetaObject::invokeMethod(QCoreApplication::instance(),
                [guard, requestId, requestedUrl, decoded = std::move(decoded)]() mutable {
                if (!guard || guard->m_thumbnailRequestId != requestId
                    || guard->m_source.originalPathOrUrl != requestedUrl
                    || guard->m_thumbnailResidency != ThumbnailResidency::Loading)
                    return;
                guard->m_thumbnailRequestId = 0;
                if (decoded.isNull()) {
                    guard->m_thumbnailResidency = ThumbnailResidency::Failed;
                    guard->m_thumbnailRetryAfterMs =
                        QDateTime::currentMSecsSinceEpoch() + kThumbnailRetryDelayMs;
                    if (guard->m_thumbnailProviderFallbackActive
                        && !guard->m_cachedThumbnailPngBase64.isEmpty()
                        && !guard->m_thumbReady) {
                        guard->restoreProviderFallbackAsync(
                            guard->m_thumbnailRetryAfterMs);
                    }
                    return;
                }
                guard->m_thumbnailProviderFallbackActive = false;
                guard->m_cachedThumbnailPngBase64.clear();
                guard->m_cachedThumbnailPngBase64Key = -1;
                guard->m_thumbnailRetryAfterMs = 0;
                guard->m_thumbnailPixelSize = decoded.size();
                guard->m_thumbnailImage = decoded;
                guard->m_thumbnailPending = decoded;
                guard->m_thumbnailResidency = ThumbnailResidency::CpuReady;
                emit guard->frameReady();
            }, Qt::QueuedConnection);
        });
    });
}

void CanvasVideo::cancelThumbnailProviderRequest()
{
    if (m_thumbnailRequestId == 0)
        return;
    if (IPbThumbnailProvider *provider =
            PluginManager::instance().findThumbnailProvider(
                m_source.originalPathOrUrl)) {
        provider->cancelThumbnailRequest(m_thumbnailRequestId);
    }
    m_thumbnailRequestId = 0;
}

void CanvasVideo::updateThumbnailResidency(bool insidePrefetch,
                                           bool insideEvictionBoundary,
                                           bool pinned, qint64 nowMs,
                                           bool allowNewRequest)
{
    if (!hasThumbnailProvider())
        return;

    if (insidePrefetch || pinned) {
        m_thumbnailFarSinceMs = -1;
        if (allowNewRequest && !m_thumbReady && m_thumbnailPending.isNull()
            && m_thumbnailImage.isNull() && m_thumbnailRequestId == 0
            && nowMs >= m_thumbnailRetryAfterMs) {
            requestThumbnailFromProvider();
        }
        if (allowNewRequest && m_thumbnailProviderFallbackActive
            && m_thumbnailRequestId == 0
            && nowMs >= m_thumbnailRetryAfterMs) {
            requestThumbnailFromProvider();
        }
        return;
    }

    if (insideEvictionBoundary) {
        m_thumbnailFarSinceMs = -1;
        return;
    }
    if (m_thumbnailFarSinceMs < 0) {
        m_thumbnailFarSinceMs = nowMs;
        return;
    }
    if (nowMs - m_thumbnailFarSinceMs < kThumbnailEvictionGraceMs)
        return;

    cancelThumbnailProviderRequest();
    m_thumbnailImage = {};
    m_thumbnailPending = {};
    if (!m_thumbnailProviderFallbackActive) {
        m_cachedThumbnailPngBase64.clear();
        m_cachedThumbnailPngBase64Key = -1;
    }
    m_thumbnailResidency = ThumbnailResidency::Cold;
    m_thumbnailRetryAfterMs = 0;
    if (m_thumbTexId != 0 && !m_thumbnailGpuReleasePending) {
        m_thumbnailGpuReleasePending = true;
        emit frameReady();
    }
}

void CanvasVideo::applyPendingThumbnailGpuRelease(Renderer &r)
{
    if (!m_thumbnailGpuReleasePending)
        return;
    if (m_thumbTexId != 0)
        r.deleteTexture(m_thumbTexId);
    m_thumbTexId = 0;
    m_thumbW = 0;
    m_thumbH = 0;
    m_thumbReady = false;
    m_thumbnailGpuReleasePending = false;
}

quint64 CanvasVideo::thumbnailCpuBytesEstimate() const
{
    const quint64 imageBytes = m_thumbnailImage.isNull()
        ? 0 : static_cast<quint64>(m_thumbnailImage.sizeInBytes());
    const quint64 pendingBytes = m_thumbnailPending.isNull()
        ? 0 : static_cast<quint64>(m_thumbnailPending.sizeInBytes());
    if (!m_thumbnailImage.isNull() && !m_thumbnailPending.isNull()
        && m_thumbnailImage.cacheKey() == m_thumbnailPending.cacheKey())
        return qMax(imageBytes, pendingBytes);
    return imageBytes + pendingBytes;
}

quint64 CanvasVideo::thumbnailGpuBytesEstimate() const
{
    return m_thumbTexId == 0 ? 0
        : static_cast<quint64>(m_thumbW) * static_cast<quint64>(m_thumbH) * 4ULL;
}

QString CanvasVideo::thumbnailResidencyName() const
{
    switch (m_thumbnailResidency) {
    case ThumbnailResidency::Cold: return QStringLiteral("Cold");
    case ThumbnailResidency::Loading: return QStringLiteral("Loading");
    case ThumbnailResidency::CpuReady: return QStringLiteral("CpuReady");
    case ThumbnailResidency::GpuResident: return QStringLiteral("GpuResident");
    case ThumbnailResidency::Failed: return QStringLiteral("Failed");
    }
    return QStringLiteral("Unknown");
}

bool CanvasVideo::thumbnailRequestEligible(qint64 nowMs) const
{
    // A freshly added plugin bubble is already resolving the same source for
    // playback. Wait for onUrlFetched() to provide thumbnailUrl instead of
    // starting a duplicate plugin metadata request in parallel. Restored lazy
    // bubbles may request immediately using their saved URL or provider cache.
    if (m_source.kind == MediaKind::PluginMedia
        && !m_deferredLoad
        && m_streamUrl.isEmpty()
        && m_thumbnailUrl.isEmpty()) {
        return false;
    }
    return m_thumbnailRequestId == 0
        && nowMs >= m_thumbnailRetryAfterMs
        && (m_thumbnailProviderFallbackActive
            || (!m_thumbReady
                && m_thumbnailPending.isNull()
                && m_thumbnailImage.isNull()));
}

void CanvasVideo::loadThumbnail(const QString &url)
{
    m_thumbnailUrl = url;
    ensureThumbnailLoader();
    m_thumbnailLoader->requestThumbnail(url);
}

void CanvasVideo::loadLocalThumbnail(const QString &filePath)
{
    ensureThumbnailLoader();
    m_thumbnailLoader->requestLocalThumbnail(filePath);
}

void CanvasVideo::setRestoredThumbnailImage(const QImage &image)
{
    if (image.isNull())
        return;
    // 旧セッションで縮小前の大きいサムネイルが保存されていた場合に備え、
    // 復元時にも同じ上限を適用する（ThumbnailLoaderのダウンロード経路と
    // 共有のロジック）。
    m_thumbnailImage = ThumbnailLoader::downscaleThumbnailIfNeeded(image).convertToFormat(QImage::Format_RGBA8888);
    m_thumbnailPending = m_thumbnailImage;
    m_thumbnailPixelSize = m_thumbnailImage.size();
    m_thumbnailResidency = ThumbnailResidency::CpuReady;
    emit frameReady();
}

void CanvasVideo::setRestoredProviderThumbnailFallback(
    const QImage &image, const QString &base64)
{
    if (image.isNull() || base64.isEmpty())
        return;
    setRestoredThumbnailImage(image);
    m_cachedThumbnailPngBase64 = base64;
    m_cachedThumbnailPngBase64Key = m_thumbnailImage.cacheKey();
    m_thumbnailProviderFallbackActive = true;
}

void CanvasVideo::restoreProviderFallbackAsync(qint64 retryAfterMs)
{
    if (!m_thumbnailProviderFallbackActive
        || m_cachedThumbnailPngBase64.isEmpty()
        || !m_thumbnailPending.isNull()
        || m_thumbReady) {
        return;
    }

    const QString base64 = m_cachedThumbnailPngBase64;
    QPointer<CanvasVideo> guard(this);
    QThreadPool::globalInstance()->start([guard, base64, retryAfterMs] {
        QImage decoded;
        const QByteArray bytes = QByteArray::fromBase64(base64.toLatin1());
        if (!bytes.isEmpty())
            decoded.loadFromData(bytes);
        if (!decoded.isNull()) {
            decoded = ThumbnailLoader::downscaleThumbnailIfNeeded(decoded)
                          .convertToFormat(QImage::Format_RGBA8888);
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(),
            [guard, decoded = std::move(decoded), retryAfterMs]() mutable {
                if (!guard || !guard->m_thumbnailProviderFallbackActive
                    || decoded.isNull() || guard->m_thumbReady
                    || !guard->m_thumbnailPending.isNull()) {
                    return;
                }
                guard->m_thumbnailImage = decoded;
                guard->m_thumbnailPending = decoded;
                guard->m_thumbnailPixelSize = decoded.size();
                guard->m_thumbnailResidency = ThumbnailResidency::CpuReady;
                guard->m_thumbnailRetryAfterMs = retryAfterMs;
                emit guard->frameReady();
            }, Qt::QueuedConnection);
    });
}

void CanvasVideo::seedThumbnailPngBase64(const QString &base64)
{
    m_cachedThumbnailPngBase64 = base64;
    m_cachedThumbnailPngBase64Key = m_thumbnailImage.cacheKey();
}

QString CanvasVideo::thumbnailPngBase64ForSession() const
{
    if (hasThumbnailProvider()) {
        return m_thumbnailProviderFallbackActive
            ? m_cachedThumbnailPngBase64 : QString();
    }
    if (m_thumbnailImage.isNull())
        return QString();
    if (!m_cachedThumbnailPngBase64.isEmpty()
        && m_cachedThumbnailPngBase64Key == m_thumbnailImage.cacheKey()) {
        return m_cachedThumbnailPngBase64;
    }
    QByteArray pngData;
    QBuffer buffer(&pngData);
    buffer.open(QIODevice::WriteOnly);
    m_thumbnailImage.save(&buffer, "PNG");
    m_cachedThumbnailPngBase64 = QString::fromLatin1(pngData.toBase64());
    m_cachedThumbnailPngBase64Key = m_thumbnailImage.cacheKey();
    return m_cachedThumbnailPngBase64;
}

// 2026-07-13: ライブメディアの「一時停止」を「切断」へ差し替える。
//
// 【背景】SRT等のプッシュ型ライブ配信は、受信側が一時停止して消費を
// 止めると、mpvのdemuxer先読み上限（約12秒）到達後にソケットからの
// 読み出しが止まり、libsrtの受信キュー(8192pkt)が溢れて、以降届く
// パケット全てに "SRT.qr: No room to store incoming packet" 警告が
// stderrへ出続ける（1080p60で毎秒数百行。コンソール接続時はこの洪水で
// アプリ全体が応答なしに見える実害が報告された）。
//
// ライブに「一時停止した位置からの再開」は元々存在しないため、
// 一時停止＝切断、再生＝ライブ最先端へ再接続、という一般的なライブ
// プレイヤーの挙動へ揃える。ライブ判定には、プラグインが解決結果で
// 指定する既存の汎用ヒント（PbResolvedMedia::seekBarVisible=false →
// m_seekBarVisible）を使う（SRT画面共有・ライブ配信の両方が
// 該当。ローカルファイル・通常動画は常にtrueのため対象外）。
//
// 再接続側は既存経路をそのまま使う: 切断はSleep（mpv解放）なので、
// 次の再生クリックはtogglePlayPause()のSleep分岐→wakeUp()で同じ
// ストリームURLを再オープンする。位置復元はしない（ライブ最先端）。
// URLが失効していた場合（ライブ配信等）はError→クリックで
// retryAfterError()がプラグイン再解決する（いずれも既存動作）。
//
// SleepReason::PausedIdle を使うのは、画面内に居るだけで自動wakeUp
// されない（＝勝手に再接続しない）特殊Sleep扱いにするため
// （BubbleController::canAutoWakeFromSleep()参照）。
//
// なお「他の動画の全画面化」等によるFocusSleep（こちらも一時停止）は、
// 既存のFOCUS_SLEEP_DEEP_RELEASE_MS(5秒)で先読み上限より先にSleepへ
// 降格して切断されるため、この差し替えの対象にしなくても洪水は起きない。
bool CanvasVideo::liveDisconnectInsteadOfPause()
{
    if (m_seekBarVisible || m_source.kind != MediaKind::PluginMedia)
        return false;
    const BubbleState bs = m_controller.state();
    if (bs != BubbleState::Playing && bs != BubbleState::Buffering
        && bs != BubbleState::Ready) {
        return false;
    }
    qDebug("[CanvasVideo %p] live media: disconnect instead of pause",
           static_cast<void*>(this));
    m_resumePositionMs = 0; // 再開時に位置を復元しない（ライブ最先端から）
    m_controller.requestSleep(BubbleController::SleepReason::PausedIdle);
    return true;
}

void CanvasVideo::togglePlayPause()
{
    // 2026-09-13: 切り替え中（移動先の解決待ち）にボタンを押された場合。
    // mpvにはまだ前の動画が載っているので、ここで play()/pause() を通すと
    // 前の動画が鳴り出す。押された意味は「移動先が届いたら再生する／
    // しない」の切り替えなので、その意思だけを反転させる。
    if (controllerHoldsStaleSource()) {
        m_navigatePlayPending = !m_navigatePlayPending;
        if (m_navigatePlayPending) {
            // 「届いたら再生する」＝切り替え中の見た目（停止マークを
            // 出さない）へ戻す。世代は動かさない（進行中の移動は
            // そのまま続いている）。
            m_navigateInFlight = true;
        } else {
            // 「届いても再生しない」＝もう切り替え中の見た目を続ける
            // 理由がないので、停止マークを出す側へ戻す。
            endNavigation();
        }
        emit playbackUiChanged();
        emit frameReady();
        return;
    }
    if (beginDeferredLoadForPlayback())
        return;
    // 2026-07-08: Errorになったバブルは、これまでクリックしても
    // m_controller.togglePlayPause()が何もしないだけで復帰手段が無く、
    // 削除して再インポートするしかなかった（実際に報告された不具合）。
    // Error状態はここで最優先に検知し、再試行処理へ回す。
    if (m_controller.state() == BubbleState::Error) {
        retryAfterError();
        return;
    }
    // ライブメディアの一時停止は切断へ差し替える（上のコメント参照）。
    // 再生方向（Sleep→wake）は下の既存分岐がそのまま担う。
    if (m_mpvPlayer && liveDisconnectInsteadOfPause())
        return;
    if (m_mpvPlayer) {
        const bool wasEnded = (m_controller.state() == BubbleState::Ended);
        m_controller.togglePlayPause();
        scheduleLocalAudioResumeWorkaroundIfNeeded(wasEnded);
        return;
    }
    if (m_controller.state() == BubbleState::Sleep) {
        m_sleepPending.setWakeResumePlayback(true);
        m_controller.wakeUp(true);
        return;
    }
    // m_mpvPlayerがまだ無い（＝再生が一度も試みられていない）場合は、
    // 通常の初回再生経路（startPlayback() → initializeMpv()）へ委ねる。
    startPlayback();
}

void CanvasVideo::retryAfterError()
{
    if (m_controller.state() != BubbleState::Error)
        return;

    // 直前まで再生できていた位置を割り出す。m_lastAttemptedResumeMsは
    // 今回失敗したロード試行が「狙っていた」再開位置（セッション復元直後の
    // 初回ロード失敗など）、m_controller.position()は「実際にどこまで
    // 再生できていたか」（しばらく再生できていたが途中でネットワーク
    // エラーになったようなケース）。両方の大きい方を採用する。
    const qint64 resumeCandidate = qMax(m_lastAttemptedResumeMs, m_controller.position());
    if (resumeCandidate > 0)
        m_resumePositionMs = resumeCandidate;

    if (m_mpvGlInitFailedPendingRetry) {
        // 2026-07-15: mpvのGL初期化失敗によるError。IPbPlayerが一度も
        // BubbleControllerへ渡っていないため BubbleController::retry()
        // （m_playerがnullだと何もしない）は使えず、initializeMpv()から
        // やり直す必要がある。
        m_mpvGlInitFailedPendingRetry = false;
        m_mpvInitializationAttempted = false;
        m_pluginResolveFailure.clear();
        qDebug("[CanvasVideo %p] retryAfterError: retrying mpv GL init (resume=%lldms)",
               static_cast<void*>(this), static_cast<long long>(m_resumePositionMs));
        m_startPlaybackWhenMpvReady = true;
        if (auto *canvas = qobject_cast<CanvasWidget*>(parent())) {
            canvas->makeCurrent();
            initializeMpv();
            canvas->doneCurrent();
        }
        return;
    }

    if (m_source.kind == MediaKind::PluginMedia) {
        // 署名付きストリームURL（動画サイト等）は時間経過やIP不一致で失効し
        // うるため、同じURLを使い回す BubbleController::retry() ではなく、
        // プラグインへ再度resolve()させて新しいURLを取り直す。
        qDebug("[CanvasVideo %p] retryAfterError: re-resolving via plugin (resume=%lldms)",
               static_cast<void*>(this), static_cast<long long>(m_resumePositionMs));
        resolveViaPlugin(m_source.originalPathOrUrl);
    } else {
        qDebug("[CanvasVideo %p] retryAfterError: BubbleController::retry() (resume=%lldms)",
               static_cast<void*>(this), static_cast<long long>(m_resumePositionMs));
        m_controller.retry(m_resumePositionMs);
    }
}

void CanvasVideo::bcPlay()
{
    // 2026-09-13: 切り替え中で、まだmpvが移動先を抱えていない間は再生させ
    // ない。この状態で play() を通すと、mpvに残っている「前の動画」が
    // 再開してしまう（リストのUIだけ移動先を指したまま、流れているのは
    // 前の動画、という不具合の直接の原因）。移動先は読み込みが終わり次第
    // stateChangedハンドラが再生を始めるので、ここは黙って見送ってよい。
    if (controllerHoldsStaleSource()) {
        qDebug("[CanvasVideo %p] bcPlay: ignored while switching "
               "(mpv still holds the previous video)", static_cast<void*>(this));
        m_navigatePlayPending = true; // 到着したら再生する意思は残す
        return;
    }
    if (beginDeferredLoadForPlayback())
        return;
    if (m_mpvPlayer) {
        const bool wasEnded = (m_controller.state() == BubbleState::Ended);
        m_controller.play();
        scheduleLocalAudioResumeWorkaroundIfNeeded(wasEnded);
        return;
    }
    if (m_controller.state() == BubbleState::Sleep) {
        m_sleepPending.setWakeResumePlayback(true);
        m_controller.wakeUp(true);
        return;
    }
    startPlayback();
}

// LocalAudioは映像フレームが存在しないため、通常の firstFrameReady（映像フレーム
// 依存）が発火せず、BubbleControllerが Loading のまま止まってしまう。
// startPlayback() の初回ロード時に使っているワークアラウンド（強制的に
// notifyQMediaPlayerReady()してからplay()する）を、Ended状態からの
// 再生（リプレイ）時にも同様に適用する。これが無いと、mpv側は実際には
// 鳴っているのに BubbleController::state() が Playing に上がらず、
// isPlaying() に依存する再生アイコン非表示・タイトルスクロールなどの
// UIがずっと「停止中」のまま固まって見える。
void CanvasVideo::scheduleLocalAudioResumeWorkaroundIfNeeded(bool wasEnded)
{
    if (!wasEnded || m_source.kind != MediaKind::LocalAudio)
        return;
    QTimer::singleShot(0, this, [this] {
        if (m_source.kind != MediaKind::LocalAudio)
            return;
        if (m_controller.state() == BubbleState::Loading)
            m_controller.notifyQMediaPlayerReady();
        m_controller.play();
    });
}

void CanvasVideo::bcPause()
{
    // ライブメディアの一時停止は切断へ差し替える
    // （詳細はliveDisconnectInsteadOfPause()のコメント参照）。
    if (m_mpvPlayer && liveDisconnectInsteadOfPause())
        return;
    // m_mpvPlayerが無ければ再生自体が始まっていないので何もしない。
    if (m_mpvPlayer) m_controller.pause();
}

void CanvasVideo::bcSeek(qint64 ms)
{
    // m_controller.seek()はプレイヤー未接続なら安全にno-opする
    // （BubbleController::seek()参照）。
    m_controller.seek(ms);
}

void CanvasVideo::bcSeekFast(qint64 ms)
{
    m_controller.seekFast(ms);
}

void CanvasVideo::setPlaybackSpeed(double speed)
{
    m_controller.setSpeed(speed);
}

void CanvasVideo::initializeMpv()
{
    if (m_mpvPlayer) return;
    m_mpvInitializationAttempted = true;

    m_mpvPlayer = new MpvPlayer(this);
    // 2026-09-15 非同期化 P2 / 2026-09-16 P7: レンダーコンテキストの生成・描画・
    // 破棄はワーカーで行う（唯一の経路）。ワーカーが無ければGL初期化失敗扱い。
    VideoRenderThread *videoRenderThread = VideoRenderThread::shared();
    const bool glInitOk = videoRenderThread
        && m_mpvPlayer->initializeGlAsync(videoRenderThread);
    if (glInitOk) {
        // 生成の成否はワーカーで確定する。失敗時は下の同期失敗と同じ扱いにする。
        connect(m_mpvPlayer, &MpvPlayer::asyncGlInitFailed, this, [this]() {
            qWarning("[CanvasVideo] MpvPlayer GL 初期化失敗（ワーカー）");
            m_controller.setPlayer(nullptr);
            destroyMpvPlayer(this, m_mpvPlayer);
            m_startPlaybackWhenMpvReady = false;
            m_mpvGlInitFailedPendingRetry = true;
            m_pluginResolveFailure.set(
                QStringLiteral("mpv GL init failed"),
                static_cast<int>(PbResolveErrorCode::Unknown));
            m_controller.reportError(QStringLiteral("mpv GL init failed"));
        }, Qt::QueuedConnection);
    }
    if (!glInitOk) {
        qWarning("[CanvasVideo] MpvPlayer GL 初期化失敗");
        destroyMpvPlayer(this, m_mpvPlayer);
        m_startPlaybackWhenMpvReady = false;
        // 2026-07-15: 以前はここでQMediaPlayerフォールバックへ黙って
        // 切り替えていた（DASH等の別音声URLに対応できず無音になる等、
        // 劣化した体験になるため撤去）。代わりに既存のError状態＋
        // クリック再試行の仕組みに乗せる。m_mpvGlInitFailedPendingRetryを
        // 見て、retryAfterError()側でinitializeMpv()からやり直す。
        //
        // 【不具合調査メモ】このelseに実際に入るのはGPUリソース枯渇や
        // ドライバの一時的な不調などが原因と推測される（詳細はログ上部の
        // "[CanvasVideo] MpvPlayer GL 初期化失敗"直前の行、および
        // [MpvPlayer ...] initializeGl関連ログを参照）。症状としては
        // バブルに赤枠＋「ERR 000」が出て静止画のまま止まる。クリックで
        // retryAfterError()のm_mpvGlInitFailedPendingRetry分岐へ入り、
        // ここへ再突入する。もし「クリックしても何も起きない」
        // 「ERR 000のまま何度リトライしても直らない」場合は、
        // m_mpvGlInitFailedPendingRetryのリセット漏れやGPU側の恒久的な
        // 問題（同時に開いているバブル数が多すぎる等）を疑うこと。
        m_mpvGlInitFailedPendingRetry = true;
        m_pluginResolveFailure.set(
            QStringLiteral("mpv GL init failed"),
            static_cast<int>(PbResolveErrorCode::Unknown));
        m_controller.reportError(QStringLiteral("mpv GL init failed"));
        return;
    }

    m_controller.setPlayer(m_mpvPlayer);
    applyEffectiveVolume();

    // 2026-07-03: frameUpdated()はmpvの内部スレッド（render update
    // callback）から発火する可能性がある。DirectConnectionだとこの
    // ラムダがそのmpv内部スレッド上でそのまま実行されてしまい、
    // 非atomicなm_hiddenFrameUpdatedCountへの書き込みや、qDebug()の
    // ファイルI/Oがmpv内部スレッドをブロックしうる（データレース、かつ
    // -O2/-O0でタイミングが変わるため再現性がDebug/Releaseで異なる
    // 一因になりうる）。QueuedConnectionにしてメインスレッド側で
    // 安全に処理する。
    connect(m_mpvPlayer, &MpvPlayer::frameUpdated,
            this, [this](){
                if (m_focusSleepPaintDebugFrames > 0) {
                    qDebug("[FocusSleep][frameUpdated] received video=%p state=%s dirtyBefore=%d pos=%.2fs",
                           static_cast<void*>(this),
                           qPrintable(bubbleStateToString(m_controller.state())),
                           m_mpvFrameDirty.load(),
                           m_controller.position() / 1000.0);
                }
                m_hiddenFrameUpdatedCount++;
                m_mpvFrameDirty.store(true);
            },
            Qt::QueuedConnection);
    // BubbleController already forwards the same player signal through
    // BubbleController::frameUpdated, which the constructor connects to
    // frameReady. A second direct connection queued two repaint requests per
    // decoded frame (Qt usually coalesced them, but the event/scheduler work
    // was still duplicated).
    connect(m_mpvPlayer, &MpvPlayer::positionChanged,
            this, &CanvasVideo::playbackPositionChanged,
            Qt::QueuedConnection);

    if (m_startPlaybackWhenMpvReady && !m_streamUrl.isEmpty()) {
        m_startPlaybackWhenMpvReady = false;
        QTimer::singleShot(0, this, &CanvasVideo::startPlayback);
    }

    qDebug("[CanvasVideo] MpvPlayer 接続完了 (%p)", static_cast<void*>(this));
}

void CanvasVideo::warmupPlaybackSurface()
{
    // 2026-09-16 P7: 非同期プレイヤーのバッファはワーカーが初回描画時に確保する。
}

void CanvasVideo::wakeFromFocusSleep(bool resumePlayback)
{
    const qint64 savedPos = m_controller.position();
    qDebug("[FocusSleep] leave video=%p wasPlaying=%d savedPos=%.2fs",
           static_cast<void*>(this),
           resumePlayback,
           savedPos / 1000.0);
    qDebug("[FocusSleep][pointer] wake enter video=%p player=%p state=%s",
           static_cast<void*>(this),
           static_cast<void*>(m_mpvPlayer),
           qPrintable(bubbleStateToString(m_controller.state())));
    if (m_mpvPlayer) {
        qDebug("[FocusRecover] target video=%p player=%p stateBefore=%s videoSuppressed=%d vid=%s pause=%s fbo=%d tex=%u",
               static_cast<void*>(this),
               static_cast<void*>(m_mpvPlayer),
               qPrintable(bubbleStateToString(m_controller.state())),
               m_mpvPlayer->isVideoSuppressed(),
               qPrintable(m_mpvPlayer->debugPropertyString("vid")),
               qPrintable(m_mpvPlayer->debugPropertyFlagString("pause")),
               m_mpvPlayer->hasFbo(),
               m_mpvPlayer->fboTextureId());
    }

    m_controller.wakeUpFromFocusSleep(resumePlayback);

    if (m_mpvPlayer) {
        qDebug("[FocusRecover] controller restored video=%p player=%p",
               static_cast<void*>(this),
               static_cast<void*>(m_mpvPlayer));
    }

    qDebug("[FocusSleep] renderCtx=%d fbo=%d fboTex=%u",
           m_mpvPlayer && m_mpvPlayer->hasRenderContext(),
           m_mpvPlayer && m_mpvPlayer->hasFbo(),
           m_mpvPlayer ? m_mpvPlayer->fboColorTextureId() : 0);
    if (m_mpvPlayer) {
        qDebug("[FocusRecover] stateAfter video=%p player=%p state=%s videoSuppressed=%d vid=%s pause=%s fbo=%d tex=%u",
               static_cast<void*>(this),
               static_cast<void*>(m_mpvPlayer),
               qPrintable(bubbleStateToString(m_controller.state())),
               m_mpvPlayer->isVideoSuppressed(),
               qPrintable(m_mpvPlayer->debugPropertyString("vid")),
               qPrintable(m_mpvPlayer->debugPropertyFlagString("pause")),
               m_mpvPlayer->hasFbo(),
               m_mpvPlayer->fboTextureId());
    }
    qDebug("[FocusSleep][pointer] wake after controller video=%p player=%p state=%s",
           static_cast<void*>(this),
           static_cast<void*>(m_mpvPlayer),
           qPrintable(bubbleStateToString(m_controller.state())));

    m_audioOnlyDrawLogOnce = false;
    m_hiddenPerfLogState = BubbleState::Idle;
    m_focusSleepPending = false;  // Idle期にセットした場合も復帳時にリセットする
    qDebug("[FocusSleep] clear thumbnailOnly");

    m_mpvFrameDirty.store(true);
    m_focusSleepPaintDebugFrames = 0;
    qDebug("[FocusSleep] mark frame dirty");

    emit frameReady();
    qDebug("[FocusSleep] restored state=%s target=%s pos=%.2fs",
           qPrintable(bubbleStateToString(m_controller.state())),
           resumePlayback ? "Playing" : "Paused",
           m_controller.position() / 1000.0);
}

void CanvasVideo::wakeFromDeepFocusSleep(bool resumePlayback)
{
    m_sleepPending.setWakeResumePlayback(resumePlayback);
    if (resumePlayback) {
        bcPlay();
    } else {
        const bool hadPlayer = (m_mpvPlayer != nullptr);
        m_controller.wakeUp(false);
        if (hadPlayer)
            m_sleepPending.setWakeResumePlayback(true);
    }
}

void CanvasVideo::logMemoryDiagnostics() const
{
    const BubbleState bs = m_controller.state();
    const bool isAudioOnly = (bs == BubbleState::AudioOnly);
    const bool isPausedHidden = (bs == BubbleState::PausedHidden);
    const bool isFocusSleep = (bs == BubbleState::FocusSleep);
    const quint64 thumbBytes =
        static_cast<quint64>(m_thumbW) * static_cast<quint64>(m_thumbH) * 4ULL;
    const quint64 fboBytes = m_mpvPlayer ? m_mpvPlayer->estimatedFboBytes() : 0ULL;

    qDebug("[Memory][CanvasVideo %p] state=%s audioOnly=%d pausedHidden=%d focusSleep=%d pos=%.2fs duration=%.2fs",
           static_cast<const void*>(this),
           qPrintable(bubbleStateToString(bs)),
           isAudioOnly,
           isPausedHidden,
           isFocusSleep,
           m_controller.position() / 1000.0,
           m_controller.duration() / 1000.0);

    qDebug("[Memory][CanvasVideo %p] objects mpv=%d renderCtx=%d fbo=%d thumbnail=%d",
           static_cast<const void*>(this),
           m_mpvPlayer && m_mpvPlayer->hasMpvHandle(),
           m_mpvPlayer && m_mpvPlayer->hasRenderContext(),
           m_mpvPlayer && m_mpvPlayer->hasFbo(),
           m_thumbReady && m_thumbTexId != 0);

    qDebug("[Memory][CanvasVideo %p] gl fbo=%dx%d fboTex=%u fboMem=%.2fMB thumb=%dx%d thumbTex=%u thumbMem=%.2fMB holdingInHidden=%d",
           static_cast<const void*>(this),
           m_mpvPlayer ? m_mpvPlayer->fboWidth() : 0,
           m_mpvPlayer ? m_mpvPlayer->fboHeight() : 0,
           m_mpvPlayer ? m_mpvPlayer->fboColorTextureId() : 0,
           fboBytes / 1048576.0,
           m_thumbW,
           m_thumbH,
           m_thumbTexId,
           thumbBytes / 1048576.0,
           isAudioOnly || isPausedHidden || isFocusSleep);

    if (m_mpvPlayer)
        m_mpvPlayer->logDiagnostics("Memory");

    // ---- Phase 8 Step 2: Sleep/Flow先読み保留状態のdump ----
    // 追加の診断ログ。既存のログ行・意味は一切変更していない
    // （新しい1行を末尾に足しただけ）。~30秒間隔の
    // syncLogicMedium() 経由でのみ呼ばれるため、頻度・負荷は
    // 既存のMemory診断ログと同等。
    qDebug("[Sleep][Dump][CanvasVideo %p] glRelease=%d mpvPlayerTeardown=%d mpvRecreateForWake=%d wakeResumePlayback=%d queuedGlDeletes=%d fullyReleased=%d",
           static_cast<const void*>(this),
           m_sleepPending.glReleasePending(),
           m_sleepPending.mpvPlayerTeardownPending(),
           m_sleepPending.mpvRecreateForWakePending(),
           m_sleepPending.wakeResumePlayback(),
           m_playersAwaitingGlDelete.size(),
           isDecodeFullyReleased());
}

void CanvasVideo::startPlayback()
{
    if (m_streamUrl.isEmpty()) return;
    m_deferredLoad = false;
    m_deferredFetchInProgress = false;

    if (m_mpvPlayer) {
        // 再開位置は loadUrl() 完了後に別途 seek() するのではなく、loadUrl()
        // （→ MpvPlayer::open()）へ直接渡し、mpv の loadfile と同時に
        // "start=" オプションとして適用する。open() 後に seek() を送る
        // 方式だと、mpv がまだファイルを開いていないタイミングでコマンドが
        // 届いて無視されることがあった（セッションを完全終了して再起動した
        // 場合にだけ再生位置が反映されず最初から再生されてしまう不具合の
        // 原因だった。トレイに隠すだけの復帰では動画オブジェクト自体が
        // 生き続けるためこの経路を通らず問題が出なかった）。
        const qint64 resumeMs = m_resumePositionMs;
        m_resumePositionMs = 0;
        // このロード試行が失敗した場合のretryAfterError()用に、狙っていた
        // 再開位置を退避しておく（m_resumePositionMsは成否を待たずここで
        // 0に戻ってしまうため）。
        m_lastAttemptedResumeMs = resumeMs;
        // 2026-07-08: LocalAudioは映像トラックを常に無効化して開く
        // （埋め込みジャケット画像をmpvが動画トラックとしてhwdec経由で
        // デコードしようとしてクラッシュする不具合の対策。ジャケットは
        // 別途Windows Shell経由のサムネイルとして表示するため、mpv側の
        // 映像デコードは元々不要）。
        const bool disableVideoTrack = (m_source.kind == MediaKind::LocalAudio);
        // 【2026-09-12】再生中のまま別URLをロードしようとすると、状態遷移表
        // （BubbleState.h の isBubbleStateTransitionValid）で Playing → Loading
        // が不正なため loadUrl() 内の transitionTo(Loading) が弾かれる。すると
        // mpvだけが新しいファイルを開き、状態機械はPlayingのまま＝
        // onPlayerFirstFrameReady()の「Loadingなら再生開始」分岐に入れず、
        // loadUrl()が送ったpause=1のまま1コマ目で固まる。
        //
        // navigateToUrl()でも移動開始時に同じ一時停止をしているが、そちらだけ
        // では足りない。解決（プラグイン）に1〜2秒かかる間に、直前の動画が
        // Loading→Ready→Playing と進んで状態がPlayingへ戻ることがあるため
        // （実機ログ: 再生終了→続きを探る間に同じ動画が再ロードされ、その
        // 再生開始が解決完了より後に来たケース）。実際にロードする直前の
        // ここで見るのが唯一取りこぼしのない位置なので、最終確認として置く。
        const BubbleState stBeforeLoad = m_controller.state();
        if (stBeforeLoad == BubbleState::Playing
            || stBeforeLoad == BubbleState::Buffering) {
            m_controller.pause();
        }
        m_controller.loadUrl(m_source.originalPathOrUrl, m_streamUrl, m_currentQuality,
                              m_streamHttpHeaders, resumeMs, m_streamAudioUrl,
                              disableVideoTrack);
        if (m_source.kind == MediaKind::LocalAudio) {
            qDebug("[CanvasVideo %p] LocalAudio loaded via mpv URL: %s",
                   static_cast<void*>(this),
                   qPrintable(m_streamUrl));
            QTimer::singleShot(0, this, [this] {
                if (m_source.kind != MediaKind::LocalAudio)
                    return;
                if (m_controller.state() == BubbleState::Loading)
                    m_controller.notifyQMediaPlayerReady();
                m_controller.play();
            });
        }
        // 全画面中に Idle 状態で enterFocusSleep された場合、
        // loadUrl で Loading に遷移した後にすかさず FocusSleep へ辺る。
        if (m_focusSleepPending) {
            m_focusSleepPending = false;
            qDebug("[FocusSleep] startPlayback: m_focusSleepPending=true -> requestFocusSleep after loadUrl");
            m_controller.requestFocusSleep();
        }
    } else if (!m_mpvInitializationAttempted) {
        // 2026-07-11 追記4: レイヤー切り替え直後に全バブル分initializeMpv()を
        // 先回りして済ませておく方式（このファイル内の追記1〜3。
        // CanvasSessionSerializer.cpp参照）はどちらも撤回済み
        // （バブル数に比例して切り替えが重くなる上、遅延版は効果が無く
        // 別の不具合まで生んだ）。代わりに、実際にこのバブルが再生
        // されようとした「今」、このバブル1個分だけをその場で同期的に
        // 初期化する。CanvasVideoは生成時に親としてCanvasWidgetを持つ
        // （new CanvasVideo(this)）ため、それ経由でGLコンテキストを
        // current化してinitializeMpv()を呼べる。
        //
        // m_startPlaybackWhenMpvReadyを先に立てておくと、initializeMpv()が
        // 成功した場合はその末尾でこのフラグを見てstartPlayback()を
        // 自動的に呼び直してくれる（＝この関数呼び出しの内側で再入し、
        // 今度はm_mpvPlayerが非nullなので上のif分岐に入ってloadUrl()まで
        // 完了する）。initializeGl()に失敗した場合は、initializeMpv()側で
        // Error状態へ遷移する（クリックでretryAfterError()から再試行）。
        m_startPlaybackWhenMpvReady = true;
        if (auto *canvas = qobject_cast<CanvasWidget*>(parent())) {
            canvas->makeCurrent();
            initializeMpv();
            canvas->doneCurrent();
        }
    }
    // 2026-07-15: 以前はここに「mpv初期化を試したが失敗した場合」の
    // QMediaPlayerフォールバックがあったが撤去した。initializeMpv()の
    // GL失敗時にError状態へ遷移するようになったため、m_mpvPlayerがnullで
    // m_mpvInitializationAttemptedがtrueのままstartPlayback()が呼ばれる
    // ことはない（retryAfterError()がm_mpvInitializationAttemptedを
    // falseへ戻してからinitializeMpv()を呼び直す）。
}

// ================================================================
//  paintGL
//  AudioOnly 状態の描画ロジックとデバッグログを含む
// ================================================================
void CanvasVideo::paintGL(Renderer &r, const QRectF &screenRect,
                          bool isActive, double pulseAlpha, double screenScale,
                          double contentCornerFactor)
{
    // このフレームで実際に描いた最前面のコンテンツだけを後段の
    // GlassHighlightEffectへ渡す。早期returnや黒フォールバックでは無効の
    // ままとなり、ガラスは従来の白いライティングだけへ自然に戻る。
    m_glassContentSource = {};
    applyPendingThumbnailGpuRelease(r);

    const double bezel = mediaBezelWidth(screenRect);
    const QRectF boardScreen = screenRect.adjusted(
        -bezel, -bezel, bezel, bezel);
    const float contentRadius = static_cast<float>(
        contentCornerRadius(screenRect, contentCornerFactor));

    // Sleepへの遷移で立てられたフラグを、GLコンテキストがcurrentな
    // ここ（paintGL内）で消費し、mpv側FBO/テクスチャを解放する。
    // Sleep中は下のsuppressRenderToFboでrenderToFbo自体が止まっているため、
    // 解放しても新規描画には影響しない（サムネイルfallback表示に切り替わる）。
    //
    // 2026-07-03: メモリ節約 — Sleep突入時にMpvPlayerオブジェクト自体
    // （mpv_handle/render_context込み）を解放する。旧・画質切り替え機構
    // （Phase 9で削除済み）と同じく、破棄はGLコンテキストcurrent下で
    // 同期的に行う（deleteLater()は使わない）。
    //
    // 2026-07-03: メモリ節約 — Sleep中に解放済みのMpvPlayerをwakeUp()時に
    // 作り直す。initializeMpv()は「m_mpvPlayerが非nullなら何もしない」
    // ガードのみなので、削除済み（nullptr）であれば安全に呼び直せる。
    // 作り直せたら、保留していたwakeUp()（BubbleControllerはSleepのまま
    // 据え置いていた）を今度こそ実行する。
    //
    // Phase 8 Step 4: 上記3つの消費処理本体は VideoSleepCoordinator へ
    // 移設した。呼び出し順序・GLコンテキストcurrent前提・条件・ログ文言は
    // 一切変更していない（元のif 3ブロックをそのまま関数化しただけ）。
    // GLコンテキストcurrentが保証されるここで、GL文脈外からキューされた
    // MpvPlayerを破棄する（queuePlayerForGlDelete()参照）。
    drainPlayersAwaitingGlDelete();
    m_sleepPending.consumeGlRelease(this, m_mpvPlayer);
    m_sleepPending.consumeMpvPlayerTeardown(this, m_mpvPlayer, m_controller);
    m_sleepPending.consumeMpvRecreateForWake(this, m_mpvPlayer, m_controller,
                                              [this]() { initializeMpv(); });

    const BubbleState bs        = m_controller.state();
    const bool        isAudioOnly = (bs == BubbleState::AudioOnly);
    const bool        isLocalAudioMedia = (m_source.kind == MediaKind::LocalAudio);
    const bool        isHiddenPaused = (bs == BubbleState::PausedHidden
                                     || bs == BubbleState::ReadyHidden
                                     || bs == BubbleState::FocusSleep);
    const bool        suppressRenderToFbo = videoRenderSuppressedFor(bs, m_source.kind);

    if (m_hiddenPerfLogState != bs) {
        m_hiddenPerfLogState = bs;
        m_hiddenRenderToFboCount = 0;
        m_hiddenFrameUpdatedCount = 0;
        m_hiddenPerfLogTimer.restart();
    } else if (!m_hiddenPerfLogTimer.isValid()) {
        m_hiddenPerfLogTimer.start();
    }

    // Decode resolution and render-FBO resolution serve different purposes.
    // Keep decoding untouched, but size each video FBO for the bubble's
    // physical on-screen footprint. Tiering, debounce and hysteresis live in
    // VideoRenderResolutionPolicy so camera zoom cannot reallocate every frame.
    if (m_mpvPlayer && !isLocalAudioMedia) {
        const QSize decoded = m_mpvPlayer->videoSize();
        if (decoded.isValid())
            m_nativeVideoSize = decoded;

        if (m_nativeVideoSize.isValid()) {
            const QSize currentTarget(m_renderW, m_renderH);
            const QSize target = m_renderResolutionPolicy.update(
                m_nativeVideoSize,
                screenRect,
                r.devicePixelRatio(),
                currentTarget);
            if (target.isValid() && target != currentTarget) {
                m_renderW = target.width();
                m_renderH = target.height();
                qDebug("[CanvasVideo %p] render FBO %dx%d -> %dx%d "
                       "(native=%dx%d, display=%.0fx%.0f, dpr=%.2f)",
                       static_cast<void *>(this),
                       currentTarget.width(), currentTarget.height(),
                       target.width(), target.height(),
                       m_nativeVideoSize.width(), m_nativeVideoSize.height(),
                       screenRect.width(), screenRect.height(),
                       static_cast<double>(r.devicePixelRatio()));
            }
        }

        // Local files and plugins without resolved dimensions adopt the
        // decoder's aspect ratio once. Plugins that supplied dimensions have
        // already applied their aspect ratio in onUrlFetched().
        const bool shouldAdoptDecodedAspect =
            m_source.kind == MediaKind::LocalVideo
            || m_source.kind == MediaKind::WebVideo
            || (m_source.kind == MediaKind::PluginMedia && !m_pluginProvidedDims);
        if (shouldAdoptDecodedAspect
         && !m_localVideoAspectApplied
         && decoded.isValid()) {
            const QPointF previousCenter = itemRect().center();
            m_origW = 480;
            m_origH = static_cast<int>(480.0 * decoded.height() / decoded.width());
            setScale(m_scale);
            setCenter(previousCenter);
            m_localVideoAspectApplied = true;
            emit videoDimensionsReady();
        }
    }

    // ---- FBO レンダリング ----
    // 2026-09-15 非同期化 P2 / 2026-09-16 P7（同期経路撤去）: 描画はワーカー。
    // ここでは「目標サイズ」と「描いてよいか」を渡し、ワーカーが仕上げた
    // 最新の1枚を受け取るだけ。以降このフレームの currentFrame()
    // （描画・ガラス・アンビエント）はこの1枚を指す。
    if (m_mpvPlayer && m_mpvPlayer->isAsyncRender()) {
        m_mpvPlayer->updateAsyncRenderState(m_renderW, m_renderH, !suppressRenderToFbo);
        m_mpvPlayer->latchAsyncFrame();
        m_mpvFrameDirty.store(false);
    }
    // AudioOnly → Playing に戻ったらフラグをリセット
    if (!isAudioOnly && !isHiddenPaused && m_audioOnlyDrawLogOnce)
        m_audioOnlyDrawLogOnce = false;

    if ((isAudioOnly || isHiddenPaused) && m_hiddenPerfLogTimer.elapsed() >= 1000) {
        qDebug("[CanvasVideo %p] [%s] perf renderToFbo/s=%d frameUpdated/s=%d pos=%.2fs",
               static_cast<void*>(this),
               qPrintable(bubbleStateToString(bs)),
               m_hiddenRenderToFboCount,
               m_hiddenFrameUpdatedCount,
               m_controller.position() / 1000.0);
        m_hiddenRenderToFboCount = 0;
        m_hiddenFrameUpdatedCount = 0;
        m_hiddenPerfLogTimer.restart();
    }

    // ライブ配信の初期バッファシーク（durationが判明した直後に1回だけ）。
    tickLiveEdgeBufferSeek();

    // 2026-10-12: 枠を焼き付けたテクスチャを使うときは描かない（BubbleLayerCache）
    if (r.drawsChrome())
        paintOverlay(r, screenRect, isActive, pulseAlpha, screenScale, contentCornerFactor);

    // 2026-07-08: サムネイルテクスチャの生成/更新をLocalAudioの早期return
    // より前に移した。以前はここが動画専用の描画ブロックの一部だったため、
    // LocalAudioバブル（早期returnでその描画ブロックへ到達しない）では
    // m_thumbnailPendingが永久にGPUへアップロードされずm_thumbTexIdが
    // 0のままになっていた。音声ファイルの埋め込みジャケット表示
    // （paintLocalAudioCard()）にもこのテクスチャをそのまま使うため、
    // メディア種別を問わず先に処理しておく。
    if (m_thumbnailUploadAllowedForFrame && !m_thumbnailPending.isNull()) {
        m_thumbW = m_thumbnailPending.width();
        m_thumbH = m_thumbnailPending.height();
        if (m_thumbTexId == 0)
            m_thumbTexId = r.createTexture(m_thumbnailPending);
        else
            r.updateTexture(m_thumbTexId, m_thumbnailPending);
        m_thumbnailPending = QImage{};
        m_thumbReady = true;
        m_thumbnailResidency = ThumbnailResidency::GpuResident;
        if (hasThumbnailProvider()) {
            m_thumbnailImage = {};
            if (!m_thumbnailProviderFallbackActive) {
                m_cachedThumbnailPngBase64.clear();
                m_cachedThumbnailPngBase64Key = -1;
            }
        }
    }

    if (m_source.kind == MediaKind::LocalAudio) {
        paintLocalAudioCard(r, screenRect, contentRadius);
        return;
    }

    if (m_oldTexId != 0) {
        r.drawRoundedTexture(m_oldTexId, screenRect, contentRadius, m_fadeAlpha);
        m_glassContentSource =
            {m_oldTexId, QRectF(0.0, 0.0, 1.0, 1.0), m_fadeAlpha};
        m_fadeAlpha -= 0.016f;
        if (m_fadeAlpha <= 0.0f) {
            r.deleteTexture(m_oldTexId);
            m_oldTexId  = 0;
            m_fadeAlpha = 1.0f;
            m_glassContentSource = {};
        }
    }

    // 2026-07-03: bs == Sleep も対象に追加。SleepはMpvPlayerオブジェクト
    // 自体を解放するため（メモリ節約）、m_mpvPlayerがnullでも真っ黒な矩形
    // ではなくサムネイルにフォールバックできるようにする。
    //
    // 2026-07-11 追記5: !m_mpvPlayer を追加。CanvasVideo::startPlayback()を
    // 「再生されるまでinitializeMpv()を呼ばない」設計に変更した結果、
    // セッション復元直後でまだ一度もクリックされていない（＝Idle状態で
    // Sleep/FlowStandbyでもない）バブルは、Sleep等を経由せずとも
    // m_mpvPlayerがnullのまま。この条件が無いと下のelse if (m_mpvPlayer)
    // にも次のelse if (m_hasTexture)にも該当せず、最後のelseで
    // サムネイルではなく真っ黒な矩形が描かれてしまっていた
    // （レイヤー切り替え直後にサムネイルが消えて見える不具合の原因）。
    if ((bs == BubbleState::Sleep || !m_mpvPlayer)
        && m_thumbReady && m_thumbTexId != 0
        && !(m_focusSleepPaintDebugFrames > 0
             && bs != BubbleState::FocusSleep
             && m_mpvPlayer
             && m_mpvPlayer->currentFrame().isValid())) {
        if (m_focusSleepPaintDebugFrames > 0) {
            qDebug("[FocusSleep][paintGL] video=%p state=%s dirty=%d frameValid=0 thumbTex=%u usingThumbFallback=1 reason=sleepOrNoPlayer pos=%.2fs",
                   static_cast<void*>(this),
                   qPrintable(bubbleStateToString(bs)),
                   m_mpvFrameDirty.load(),
                   m_thumbTexId,
                   m_controller.position() / 1000.0);
            m_focusSleepPaintDebugFrames--;
        }
        if (m_focusSleepPaintDebugFrames > 0) {
            const PbVideoFrame frame = m_mpvPlayer ? m_mpvPlayer->currentFrame() : PbVideoFrame{};
            qDebug() << "[FocusSleep][drawDecision]"
                     << "video=" << this
                     << "player=" << m_mpvPlayer
                     << "state=" << bubbleStateToString(bs)
                     << "frameValid=" << frame.isValid()
                     << "frameTex=" << frame.textureId
                     << "mpvFboTex=" << (m_mpvPlayer ? m_mpvPlayer->fboTextureId() : 0)
                     << "thumbTex=" << m_thumbTexId
                     << "usingThumbFallback=" << true
                     << "finalDrawTex=" << m_thumbTexId
                     << "reason=flowStandby";
        }
        drawThumbnailCover(r, screenRect, contentRadius);
    } else if (m_mpvPlayer) {
        PbVideoFrame frame = m_mpvPlayer->currentFrame();
        if (frame.isValid()) {
            if (m_focusSleepPaintDebugFrames > 0) {
                qDebug() << "[FocusSleep][drawDecision]"
                         << "video=" << this
                         << "player=" << m_mpvPlayer
                         << "state=" << bubbleStateToString(bs)
                         << "frameValid=" << frame.isValid()
                         << "frameTex=" << frame.textureId
                         << "mpvFboTex=" << m_mpvPlayer->fboTextureId()
                         << "thumbTex=" << m_thumbTexId
                         << "usingThumbFallback=" << false
                         << "finalDrawTex=" << frame.textureId
                         << "reason=videoFrame";
            }
            if (m_focusSleepPaintDebugFrames > 0) {
                qDebug("[FocusSleep][paintGL] video=%p state=%s dirty=%d frameValid=1 tex=%u thumbTex=%u usingThumbFallback=0 pos=%.2fs",
                       static_cast<void*>(this),
                       qPrintable(bubbleStateToString(bs)),
                       m_mpvFrameDirty.load(),
                       frame.textureId,
                       m_thumbTexId,
                       m_controller.position() / 1000.0);
                m_focusSleepPaintDebugFrames--;
            }
            // AudioOnly 中も Playing 中も同じ drawTexture で描画。
            // AudioOnly 時は FBO の「最後のフレーム」が静止画として表示される。
            r.drawRoundedTexture(frame.textureId, screenRect, contentRadius);
            m_glassContentSource =
                {frame.textureId, QRectF(0.0, 0.0, 1.0, 1.0), 1.0f};
        } else if (m_thumbReady && m_thumbTexId != 0) {
            if (!suppressRenderToFbo)
                m_mpvFrameDirty.store(true);
            if (m_focusSleepPaintDebugFrames > 0) {
                qDebug() << "[FocusSleep][drawDecision]"
                         << "video=" << this
                         << "player=" << m_mpvPlayer
                         << "state=" << bubbleStateToString(bs)
                         << "frameValid=" << frame.isValid()
                         << "frameTex=" << frame.textureId
                         << "mpvFboTex=" << m_mpvPlayer->fboTextureId()
                         << "thumbTex=" << m_thumbTexId
                         << "usingThumbFallback=" << true
                         << "finalDrawTex=" << m_thumbTexId
                         << "reason=currentFrameInvalid";
            }
            if (m_focusSleepPaintDebugFrames > 0) {
                qDebug("[FocusSleep][paintGL] video=%p state=%s dirty=%d frameValid=0 thumbTex=%u usingThumbFallback=1 reason=currentFrameInvalid pos=%.2fs",
                       static_cast<void*>(this),
                       qPrintable(bubbleStateToString(bs)),
                       m_mpvFrameDirty.load(),
                       m_thumbTexId,
                       m_controller.position() / 1000.0);
                m_focusSleepPaintDebugFrames--;
            }
            if (isAudioOnly) {
                // まれなケース: FBO が未生成のままで AudioOnly に突入した
                qDebug("[CanvasVideo %p] [AudioOnly] FBO 未生成 → サムネイルで代替  thumbTex=%u",
                       static_cast<void*>(this), m_thumbTexId);
            }
            drawThumbnailCover(r, screenRect, contentRadius);
        } else {
            if (!suppressRenderToFbo)
                m_mpvFrameDirty.store(true);
            if (m_focusSleepPaintDebugFrames > 0) {
                qDebug() << "[FocusSleep][drawDecision]"
                         << "video=" << this
                         << "player=" << m_mpvPlayer
                         << "state=" << bubbleStateToString(bs)
                         << "frameValid=" << frame.isValid()
                         << "frameTex=" << frame.textureId
                         << "mpvFboTex=" << m_mpvPlayer->fboTextureId()
                         << "thumbTex=" << m_thumbTexId
                         << "usingThumbFallback=" << false
                         << "finalDrawTex=" << 0
                         << "reason=noFrameNoThumb";
            }
            if (m_focusSleepPaintDebugFrames > 0) {
                qDebug("[FocusSleep][paintGL] video=%p state=%s dirty=%d frameValid=0 thumbTex=%u usingThumbFallback=0 reason=noFrameNoThumb pos=%.2fs",
                       static_cast<void*>(this),
                       qPrintable(bubbleStateToString(bs)),
                       m_mpvFrameDirty.load(),
                       m_thumbTexId,
                       m_controller.position() / 1000.0);
                m_focusSleepPaintDebugFrames--;
            }
            if (m_oldTexId == 0)
                r.drawRect(screenRect, QColor(10,10,10),
                           Qt::transparent, 0.0f, contentRadius);
        }
    } else {
        if (m_oldTexId == 0)
            r.drawRect(screenRect, QColor(10,10,10),
                       Qt::transparent, 0.0f, contentRadius);
    }

    // AudioOnly 中は音声継続中なので一時停止アイコンを表示しない。
    // 2026-07-15: isPlaying()によるゲートは外した。paintPauseIcon()内部で
    // 出現/消失アニメーションを自前管理しており、再生再開直後も縮小＋
    // フェードアウトを描き切るまでは呼び続ける必要があるため。
    if (!isAudioOnly)
        paintPauseIcon(r, screenRect);

    // Phase 8: Plugin API — プラグイン解決失敗時のエラー表示
    // （文字を使わず赤塗り＋×＋3桁コードのみ。詳細は m_pluginResolveFailure.message に
    // 保持しログにのみ出す）。
    if (m_pluginResolveFailure.failed)
        paintResolveFailedOverlay(r, screenRect, screenScale);
}

// ================================================================
//  Phase 8: Plugin API — 解決失敗表示
//
//  paintLocalAudioCard() のタイトル描画と同じ考え方を踏襲する。
//  Renderer::drawText() はQPainterでQImageにラスタライズしテクスチャ
//  キャッシュする方式のため、表示用フォントをそのままズームに追従させて
//  qRound()で整数ptに丸めると、ズームのたびに実測幅がわずかにブレる。
//  静的な中央寄せ表示だけなら毎フレームその場で測り直しても実害は
//  小さいが、念のため「サイズ計算は screenScale ではなく実際の
//  screenRect（常に正しくカメラ変換済み）から自前で ss を算出する」
//  という同じ手法を踏襲し、ボックスとの追従ズレを避ける。
//
//  文字は最小限（3桁のエラーコードのみ）に留める方針は維持する。
//  コードの意味は PbResolveErrorCode（PbPluginApi.h）を参照。
// ================================================================
void CanvasVideo::paintResolveFailedOverlay(Renderer &r, const QRectF &screenRect, double /*screenScale*/)
{
    // 160基準デザインに対するローカルスケール。paintLocalAudioCard() と
    // 同様に、渡された screenScale ではなく実際の screenRect サイズから
    // 自前で算出することで、掴み上げ等のアイテム固有アニメーションとも
    // 常に整合させる。
    const double ss = qMin(screenRect.width(), screenRect.height()) / 160.0;

    r.drawRect(screenRect, QColor(90, 15, 15, 215),
               QColor(255, 70, 70, 235), static_cast<float>(qMax(1.5, 2.0 * ss)),
               static_cast<float>(6.0 * ss));

    const QPointF c = screenRect.center();
    const double crossSize = qMin(screenRect.width(), screenRect.height()) * 0.16;
    const float  crossWidth = static_cast<float>(qMax(2.0, crossSize * 0.22));
    const QColor crossColor(255, 235, 235, 245);
    r.drawLine({c.x() - crossSize, c.y() - crossSize}, {c.x() + crossSize, c.y() + crossSize},
               crossColor, crossWidth);
    r.drawLine({c.x() + crossSize, c.y() - crossSize}, {c.x() - crossSize, c.y() + crossSize},
               crossColor, crossWidth);

    // ---- 3桁エラーコード ----
    // ここは静的な中央寄せ表示のみ（marqueeのような時間依存アニメーションを
    // 伴わない）ため、paintLocalAudioCard のような「計測専用のズーム非依存
    // 基準フォント」までは不要。ズームに応じて丸めたフォントの実測幅で
    // その場で中央寄せすれば、フレームごとの再計算だけで十分。
    // サイズの基準を screenScale ではなく ss（実際の screenRect から算出）に
    // している点が、カメラ・アイテムアニメーションとの追従ズレを防ぐ肝。
    // もし将来ここに折返し・marquee等を追加する場合は、paintLocalAudioCard
    // の s_titleRefFont と同じ「計測用固定フォント分離」手法を使うこと。
    const QString codeText = QStringLiteral("ERR %1")
        .arg(qBound(0, m_pluginResolveFailure.errorCode, 999), 3, 10, QChar('0'));

    QFont codeFont(QStringLiteral("Consolas"), qMax(1, qRound(13.0 * ss)));
    codeFont.setBold(true);
    QFontMetrics codeFm(codeFont);
    const double textW = codeFm.horizontalAdvance(codeText);

    r.drawText(codeText,
               {c.x() - textW * 0.5, c.y() + crossSize * 1.6},
               codeFont, QColor(255, 220, 220, 235));
}

void CanvasVideo::paintPauseIcon(Renderer &r, const QRectF &screenRect)
{
    // 2026-07-15: 出現時「ぽよん」ポップ／消失時「縮小＋フェードアウト」の
    // アニメーションを壁時計ベースで進める。このメソッドはisPlaying()の
    // 値に関わらず毎フレーム呼ばれる想定（呼び出し側のゲートを外している）。
    // 完全に消え切ったら早期returnするだけなので、呼び続けても無駄は少ない。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    double dtMs = (m_pauseIconLastTickMs > 0) ? double(nowMs - m_pauseIconLastTickMs) : 16.0;
    dtMs = qBound(0.0, dtMs, 100.0); // タブ切り替え等での大ジャンプを防ぐ
    m_pauseIconLastTickMs = nowMs;

    // 2026-09-13: リストで次の動画を選んだときは、解決（プラグイン）とロードの
    // 間だけ実際に再生が止まる。そこで停止マークを出してしまうと、他の
    // 動画サービスには無い「一度止まってから次へ行く」挙動に見える。
    // 止まっている事実は変えられない（次の動画のURLが分かるまで開けない）が、
    // 見た目は再生中のまま繋ぐ。
    const bool isPlayingNow = m_controller.isPlaying() || isSwitchingVideo();
    if (isPlayingNow != m_pauseIconWasPlaying) {
        if (!isPlayingNow) {
            // 再生 → 停止：0からぽよんと弾ませ直す（丸・マークとも一緒に）。
            m_pauseIconPopActive = true;
            m_pauseIconPopElapsedMs = 0.0;
            m_pauseIconScale = 0.0f;
            m_pauseIconMarkScale = 0.0f;
        } else {
            // 停止 → 再生：ポップは打ち切り、以降はフェードで消す。
            // 黒い丸の背景は通常サイズ(1.0)のまま維持し、now_play.svgの
            // マークだけを一瞬1.5倍まで大きくしてから0へ縮めていく
            // （切り替わりを目立たせるため。丸自体は拡大させない）。
            m_pauseIconPopActive = false;
            m_pauseIconScale = 1.0f;
            m_pauseIconMarkScale = 1.5f;
        }
        m_pauseIconWasPlaying = isPlayingNow;
    }

    if (!isPlayingNow) {
        // ---- 出現: OutBackで「ぽよん」と弾みながら拡大（丸・マーク共通） ----
        if (m_pauseIconPopActive) {
            m_pauseIconPopElapsedMs += dtMs;
            static QEasingCurve s_pauseIconPopEase(QEasingCurve::OutBack);
            const double durationMs = 300.0;
            const double t = qBound(0.0, m_pauseIconPopElapsedMs / durationMs, 1.0);
            const float eased = static_cast<float>(s_pauseIconPopEase.valueForProgress(t));
            m_pauseIconScale = eased;
            m_pauseIconMarkScale = eased;
            if (t >= 1.0) {
                m_pauseIconPopActive = false;
                m_pauseIconScale = 1.0f;
                m_pauseIconMarkScale = 1.0f;
            }
        }
        // フェードインは弾みより少し速く（見た目の出だしを揃えるため）。
        m_pauseIconAlpha += (1.0f - m_pauseIconAlpha)
                             * static_cast<float>(qMin(1.0, 14.0 * dtMs / 1000.0));
    } else {
        // ---- 消失: 丸も1.0から縮小しつつフェードアウト。マークは
        //      1.5倍から0へ、丸とは独立に縮めていく。 ----
        const float k = static_cast<float>(qMin(1.0, 12.0 * dtMs / 1000.0));
        m_pauseIconScale     -= m_pauseIconScale     * k;
        m_pauseIconMarkScale -= m_pauseIconMarkScale * k;
        m_pauseIconAlpha     -= m_pauseIconAlpha     * k;
        if (m_pauseIconScale     < 0.01f) m_pauseIconScale     = 0.0f;
        if (m_pauseIconMarkScale < 0.01f) m_pauseIconMarkScale = 0.0f;
        if (m_pauseIconAlpha     < 0.01f) m_pauseIconAlpha     = 0.0f;
    }

    if (m_pauseIconAlpha <= 0.001f || (m_pauseIconScale <= 0.001f && m_pauseIconMarkScale <= 0.001f))
        return; // 完全に消えている間は描画不要

    // 固定ピクセルの半径ではなく、実際の screenRect サイズ（常に正しく
    // カメラ変換済み）から自前でスケールを算出する。固定値のままだと
    // カメラズームやバブル自体のリサイズでボードは拡大縮小するのに
    // アイコンだけサイズが変わらず、不釣り合いに見えてしまう
    // （＝カメラの影響を受けてしまう）。
    const double cx = screenRect.center().x();
    const double cy = screenRect.center().y();
    const float  baseRadius = static_cast<float>(qMin(screenRect.width(), screenRect.height()) * 0.13);
    const float  radius = baseRadius * m_pauseIconScale;
    const int    circleAlpha = qBound(0, static_cast<int>(120 * m_pauseIconAlpha), 120);
    // 2026-09-11: ここに背後ぼかし(BackdropBlur::blurNow)を入れていたが、
    // 一旦取り外した。
    //
    // この丸はカメラ変換後のscreenRectから大きさを決めているため、寄ると
    // ビューポートを超えるほど巨大になる。その状態でブラー領域の境界が
    // マークとずれる症状が再現しており、こちらで合成の整数計算を数値検証した
    // 限り(クリップ有無・巨大領域とも誤差0)、renderRect側の座標計算は
    // 一致している。つまり原因はこの呼び出しに渡している値か、あるいは
    // 静的レイヤーキャッシュ(CanvasStaticLayerCache)との相互作用など、
    // 実行時にしか観測できない側にある。
    //
    // 見た目が壊れたまま残すよりは外しておく。再挑戦する場合は、
    // renderRectへ渡るcap/dst/maskの実測値をログに出して、
    // 「マークの円」と「ブラーのマスク」が同じ矩形になっているかを
    // 実機で突き合わせるところから始めること。
    r.drawCircle({cx, cy}, radius, QColor(0, 0, 0, circleAlpha));
    // 停止中は now_stop.svg（「▶ 押せば再生できる」の意味）、再生開始した
    // 瞬間（＝このアイコンが縮小＋フェードアウトしていく間）は now_play.svg
    // （「|| 押せば一時停止できる」の意味）に切り替える。マークのサイズは
    // 丸のradiusではなくbaseRadius基準で独立にスケールする。
    const GLuint texId = playStateIconTexture(r, isPlayingNow);
    const double iconSize = baseRadius * m_pauseIconMarkScale * 1.1;
    r.drawTexture(texId, QRectF(cx - iconSize / 2.0, cy - iconSize / 2.0, iconSize, iconSize),
                  m_pauseIconAlpha);
}

// サムネイルをアスペクト比を保ったままscreenRectを覆う形（中央クロップ、
// CSSのobject-fit: cover相当）で描く。
// 2026-07-11: 動画サイトのサムネイルは動画本編の縦横比に関係なく16:9
// （hqdefaultは4:3）で届くため、縦長動画などバブル矩形（＝動画の実アスペクト）
// と比率が合わない場合、従来の「矩形いっぱいに引き伸ばし」では歪んで
// 見えていた。はみ出す方向を切り落として歪みなく表示する。
void CanvasVideo::drawThumbnailCover(Renderer &r, const QRectF &screenRect,
                                     float radius, float alpha)
{
    if (m_thumbTexId == 0)
        return;
    const QSizeF thumbSize(m_thumbnailPixelSize);
    if (thumbSize.isEmpty() || screenRect.width() <= 0.0 || screenRect.height() <= 0.0) {
        // 画像サイズ不明時は従来どおり（引き伸ばし）にフォールバック。
        r.drawRoundedTexture(m_thumbTexId, screenRect, radius, alpha);
        m_glassContentSource =
            {m_thumbTexId, QRectF(0.0, 0.0, 1.0, 1.0), alpha};
        return;
    }
    const double thumbAR = thumbSize.width() / thumbSize.height();
    const double rectAR  = screenRect.width() / screenRect.height();
    QRectF uv(0.0, 0.0, 1.0, 1.0);
    if (thumbAR > rectAR) {
        // サムネイルの方が横長 → 左右を切り落とす。
        const double used = rectAR / thumbAR;
        uv = QRectF((1.0 - used) / 2.0, 0.0, used, 1.0);
    } else if (thumbAR < rectAR) {
        // サムネイルの方が縦長 → 上下を切り落とす。
        const double used = thumbAR / rectAR;
        uv = QRectF(0.0, (1.0 - used) / 2.0, 1.0, used);
    }
    r.drawRoundedTexture(m_thumbTexId, screenRect, uv, radius, alpha);
    m_glassContentSource = {m_thumbTexId, uv, alpha};
}

void CanvasVideo::paintLocalAudioCard(Renderer &r, const QRectF &sr,
                                      float contentRadius)
{
    // 2026-07-08: 埋め込みジャケット（Windows Explorer経由でloadLocalThumbnail()
    // が取得済み）があれば、タイトル/♪アイコン/擬似シークバーのカードは
    // 描かず、サムネイル画像で丸ごと置き換える。m_thumbTexIdの生成/更新は
    // paintGL()側（LocalAudioの早期returnより前）で行っている。
    if (m_thumbReady && m_thumbTexId != 0) {
        drawThumbnailCover(r, sr, contentRadius);
        // 2026-07-08: paintOverlay()は既にpaintLocalAudioCard()より前に
        // 呼ばれ終わっており、一時停止アイコンもその中で描かれている。
        // しかしここで全面にサムネイルを描いたことで、そのアイコンが
        // 下に埋もれて見えなくなってしまう（報告いただいた不具合の原因）。
        // サムネイルの上から、この下の非サムネイル時のカードと同じ見た目の
        // アイコンを重ねて描き直す（w/h/中心はここではまだ未計算のため sr から
        // 直接算出する）。
        // 2026-07-15: 直接描画していた一時停止アイコンをpaintPauseIcon()
        // 呼び出しに統一し、出現/消失アニメーションを共有する。
        paintPauseIcon(r, sr);
        return;
    }

    // \u97f3\u58f0Bubble\u30ab\u30fc\u30c9\u5168\u4f53\u3092OpenGL\u30d7\u30ea\u30df\u30c6\u30a3\u30d6\u306e\u307f\u3067\u63cf\u753b\u3059\u308b\uff08QImage/QPainter\u3067\u306e
    // \u4e8b\u524d\u30e9\u30b9\u30bf\u30e9\u30a4\u30ba\u30fb\u30c6\u30af\u30b9\u30c1\u30e3\u713c\u304d\u8fbc\u307f\u306f\u884c\u308f\u306a\u3044\uff09\u3002sr\uff08\u5b9f\u30b9\u30af\u30ea\u30fc\u30f3\u77e9\u5f62\uff09\u306b
    // \u76f4\u63a5\u30c6\u30ad\u30b9\u30c8\u30fb\u56f3\u5f62\u3092\u63cf\u304f\u305f\u3081\u3001\u62e1\u5927\u6642\u3082\u307c\u3084\u3051\u306a\u3044\u3002
    const QString title = m_source.title.isEmpty()
        ? QFileInfo(QUrl(m_source.originalPathOrUrl).toLocalFile()).fileName()
        : m_source.title;

    const double w = sr.width();
    const double h = sr.height();
    const double ss = qMin(w, h) / 160.0; // 160\u57fa\u6e96\u30c7\u30b6\u30a4\u30f3\u306b\u5bfe\u3059\u308b\u30b9\u30b1\u30fc\u30eb

    // Windows\u306e\u30a2\u30af\u30bb\u30f3\u30c8\u30ab\u30e9\u30fc\u3092\u266a\u30a2\u30a4\u30b3\u30f3\u3068\u30d7\u30ed\u30b0\u30ec\u30b9\u30d0\u30fc\u306e\u8272\u306b\u4f7f\u3046\u3002
    // \u30ec\u30b8\u30b9\u30c8\u30ea\u8aad\u307f\u53d6\u308a\u306f\u8efd\u3044\u3068\u306f\u3044\u3048\u6bce\u30d5\u30ec\u30fc\u30e0\u547c\u3076\u5fc5\u8981\u306f\u306a\u3044\u306e\u3067\u3001
    // DynamicIsland \u3068\u540c\u69d82\u79d2\u9593\u9694\u3067\u30ad\u30e3\u30c3\u30b7\u30e5\u3059\u308b\uff08\u5168Bubble\u3067\u5171\u6709\uff09\u3002
    // 2026-09-16: 設定のカラーも反映するよう、動画UIと同じ cachedAccentColor() を使う
    // （キャッシュの考え方は同じ。設定が変わったときは即座に取り直す）。
    const QColor s_accentColor = cachedAccentColor();

    // \u80cc\u666f\u30ab\u30fc\u30c9
    r.drawRect(sr, QColor(18, 20, 26, 232), QColor(255, 255, 255, 46),
               1.0f, (float)(10.0 * ss));

    // \u30ce\u30fc\u30c8\u30a2\u30a4\u30b3\u30f3\uff08\u30a2\u30af\u30bb\u30f3\u30c8\u30ab\u30e9\u30fc\uff09
    QFont noteFont(QStringLiteral("Arial"), qMax(1, qRound(34 * ss)));
    noteFont.setBold(true);
    QFontMetrics noteFm(noteFont);
    const QString note = QStringLiteral("\u266a");
    QRect noteBr = noteFm.boundingRect(note);
    r.drawText(note,
               {sr.left() + (w - noteBr.width()) * 0.5, sr.top() + h * 0.11},
               noteFont, QColor(s_accentColor.red(), s_accentColor.green(), s_accentColor.blue(), 230));

    // \u30bf\u30a4\u30c8\u30eb\uff08\u53ce\u307e\u308b\u5834\u5408\u306f\u4e2d\u592e\u56fa\u5b9a\u3001\u9577\u3059\u304e\u308b\u5834\u5408\u306f\u3086\u3063\u304f\u308a\u5168\u4f53\u3092\u30b9\u30af\u30ed\u30fc\u30eb\u8868\u793a\uff09
    //
    // \u91cd\u8981: \u30b9\u30af\u30ed\u30fc\u30eb\u306e\u300c\u30bf\u30a4\u30df\u30f3\u30b0\u30fb\u4f4d\u7f6e\u300d\u306f\u3001\u30ba\u30fc\u30e0\u306b\u8ffd\u5f93\u3057\u3066\u4e38\u3081\u3089\u308c\u308b\u8868\u793a\u7528
    // \u30d5\u30a9\u30f3\u30c8\uff08\u4e0b\u306etitleFont\u3001qRound(12*ss)\u3067\u30ba\u30fc\u30e0\u306e\u305f\u3073\u306b\u6574\u6570pt\u304c\u5909\u308f\u308b\uff09\u304b\u3089
    // \u5b8c\u5168\u306b\u5207\u308a\u96e2\u3059\u3002\u8868\u793a\u7528\u30d5\u30a9\u30f3\u30c8\u3067\u5e45\u3092\u6e2c\u3063\u3066\u3057\u307e\u3046\u3068\u3001\u30ba\u30fc\u30e0\u3067pt\u6570\u306e\u4e38\u3081\u304c
    // 1\u6bb5\u968e\u5909\u308f\u308b\u305f\u3073\u306b\u5b9f\u6e2c\u5e45\u304c\u5fae\u5999\u306b\u30d6\u30ec\u3066\u5468\u671f\u30fb\u4f4d\u7f6e\u304c\u98db\u3076\uff08\u4eca\u56de\u306e\u4e0d\u5177\u5408\u306e\u539f\u56e0\uff09\u3002
    // \u305d\u306e\u305f\u3081\u8a08\u6e2c\u5c02\u7528\u306b\u300c\u30ba\u30fc\u30e0\u306b\u4f9d\u5b58\u3057\u306a\u3044\u56fa\u5b9apt\u306e\u57fa\u6e96\u30d5\u30a9\u30f3\u30c8\u300d\u3092\u5225\u9014\u7528\u610f\u3057\u3001
    // \u30bf\u30a4\u30df\u30f3\u30b0\u8a08\u7b97\u306f\u5e38\u306b\u305d\u306e\u57fa\u6e96\u5024\u306e\u307f\u3067\u884c\u3046\u3002
    static const QFont s_titleRefFont = [] {
        QFont f(QStringLiteral("Arial"), 12);
        f.setBold(true);
        return f;
    }();
    static const QFontMetrics s_titleRefFm(s_titleRefFont);
    const double fullTextWLocal = s_titleRefFm.horizontalAdvance(title); // \u30ba\u30fc\u30e0\u975e\u4f9d\u5b58

    QFont titleFont(QStringLiteral("Arial"), qMax(1, qRound(12 * ss)));
    titleFont.setBold(true);
    QFontMetrics titleFm(titleFont);
    const double titleBoxW = w * 0.79;
    const double titleBoxL = sr.left() + (w - titleBoxW) * 0.5;
    const double titleY    = sr.top() + h * 0.487;
    const double titleBoxWLocal = 160.0 * 0.79; // \u30ba\u30fc\u30e0\u975e\u4f9d\u5b58\u306e\u5b9a\u6570\uff08\u57fa\u6e96\u30c7\u30b6\u30a4\u30f3\u5e45\uff09

    // \u307d\u3088\u3093\uff08\u63b4\u307f\u4e0a\u3052\uff09\u30a2\u30cb\u30e1\u30fc\u30b7\u30e7\u30f3\u4e2d\u30fb\u30c9\u30e9\u30c3\u30b0\u4e2d\u30fb\u30ea\u30b5\u30a4\u30ba\u4e2d\u306f\u30b9\u30af\u30ed\u30fc\u30eb\uff0f
    // \u30af\u30ea\u30c3\u30d7\u306e\u63cf\u753b\u304c\u304c\u304f\u3064\u304f\u305f\u3081\u3001\u63b4\u3093\u3060\u77ac\u9593\u306b\u5373\u5ea7\u306b\uff08\u30d5\u30a7\u30fc\u30c9\u306a\u3057\u3067\uff09\u6d88\u3057\u3001
    // \u96e2\u3057\u305f\u77ac\u9593\u304b\u3089\u3075\u308f\u3063\u3068\u30d5\u30a7\u30fc\u30c9\u30a4\u30f3\u3055\u305b\u3066\u3054\u307e\u304b\u3059\u3002liftScale() \u306f\u63b4\u3093\u3067
    // \u3044\u308b\u9593\u305a\u3063\u30681.0\u3088\u308a\u5927\u304d\u3044\u5024\u3092\u4fdd\u3064\uff08CanvasWidget::startLiftUp/Down\uff09\u305f\u3081\u3001
    // \u30c9\u30e9\u30c3\u30b0\u79fb\u52d5\u30fb\u30ea\u30b5\u30a4\u30ba\u30c9\u30e9\u30c3\u30b0\u306e\u3069\u3061\u3089\u3082\u3053\u308c\u4e00\u672c\u3067\u691c\u51fa\u3067\u304d\u308b\u3002
    const bool lifted = liftScale() > 1.001;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    double titleAlpha = 1.0;
    if (lifted) {
        m_titleFadeStartMs = 0; // \u30d5\u30a7\u30fc\u30c9\u72b6\u614b\u3092\u30ea\u30bb\u30c3\u30c8\u3057\u3001\u6b21\u306b\u96e2\u3057\u305f\u6642\u306b0\u304b\u3089\u518d\u30d5\u30a7\u30fc\u30c9\u3055\u305b\u308b
        titleAlpha = 0.0;
    } else {
        if (m_titleWasLifted) {
            // \u96e2\u3057\u305f\u76f4\u5f8c\u306e\u6700\u521d\u306e\u30d5\u30ec\u30fc\u30e0 \u2192 \u30d5\u30a7\u30fc\u30c9\u30a4\u30f3\u958b\u59cb
            m_titleFadeStartMs = nowMs;
        }
        if (m_titleFadeStartMs != 0) {
            constexpr double kFadeInMs = 350.0;
            const double elapsed = double(nowMs - m_titleFadeStartMs);
            titleAlpha = qBound(0.0, elapsed / kFadeInMs, 1.0);
        }
    }
    m_titleWasLifted = lifted;
    const int titleAlphaI = (int)std::lround(235.0 * titleAlpha);

    if (titleAlphaI <= 0) {
        // 掴み上げ・ドラッグ・リサイズ中は完全に非表示（GL描画自体をスキップ）。
    } else if (fullTextWLocal <= titleBoxWLocal || !m_controller.isPlaying()) {
        // 収まる場合、または再生中でない場合（一時停止・停止中）は先頭で固定表示。
        const double fullTextW = titleFm.horizontalAdvance(title);
        if (fullTextW <= titleBoxW) {
            r.drawText(title,
                       {titleBoxL + (titleBoxW - fullTextW) * 0.5, titleY},
                       titleFont, QColor(245, 245, 245, titleAlphaI));
        } else {
            QRectF titleBox(titleBoxL, titleY - 2.0, titleBoxW, titleFm.height() + 4.0);
            r.setClipRect(titleBox);
            r.drawText(title, {titleBoxL, titleY}, titleFont, QColor(245, 245, 245, titleAlphaI));
            r.clearClip();
        }
    } else {
        // \u30bf\u30a4\u30df\u30f3\u30b0\u306f\u5168\u3066\u300c160\u57fa\u6e96\u306e\u30ed\u30fc\u30ab\u30eb\u5ea7\u6a19\u7cfb\u30fb\u30ba\u30fc\u30e0\u975e\u4f9d\u5b58\u306e\u57fa\u6e96\u30d5\u30a9\u30f3\u30c8\u300d
        // \u306e\u307f\u3067\u8a08\u7b97\u3057\u3001\u6700\u5f8c\u306b ss \u3092\u639b\u3051\u3066\u753b\u9762\u5ea7\u6a19\u3078\u5909\u63db\u3059\u308b\u3002\u30ba\u30fc\u30e0\u4e2d\u306b\u5468\u671f\u3084
        // \u4f4d\u76f8\u304c\u5909\u308f\u3089\u306a\u3044\u305f\u3081\u3001\u5e38\u306b\u5de6\u30b9\u30af\u30ed\u30fc\u30eb\u306e\u307e\u307e\u6ed1\u3089\u304b\u306b\u52d5\u304f\u3002
        const double maxOffsetLocal = fullTextWLocal - titleBoxWLocal;

        const double speedLocal = 22.0;  // px/\u79d2\uff08\u30ed\u30fc\u30ab\u30eb\u5ea7\u6a19\u30fb\u3086\u3063\u304f\u308a\uff09
        const double pauseStartSec = 2.0; // \u5148\u982d\u3067\u6570\u79d2\u505c\u6b62
        const double pauseEndSec   = 0.8; // \u672b\u5c3e\u3067\u5c11\u3057\u505c\u6b62\u3057\u3066\u304b\u3089\u30eb\u30fc\u30d7
        const double travelSec = maxOffsetLocal / speedLocal;
        const double cycleSec  = pauseStartSec + travelSec + pauseEndSec;
        const double t = std::fmod(QDateTime::currentMSecsSinceEpoch() / 1000.0, cycleSec);

        double offsetLocal;
        if (t < pauseStartSec)
            offsetLocal = 0.0;                                   // \u5148\u982d\u3067\u505c\u6b62
        else if (t < pauseStartSec + travelSec)
            offsetLocal = (t - pauseStartSec) * speedLocal;       // \u5de6\u3078\u30b9\u30af\u30ed\u30fc\u30eb
        else
            offsetLocal = maxOffsetLocal;                         // \u672b\u5c3e\u3067\u505c\u6b62\u2192\u30eb\u30fc\u30d7
        offsetLocal = qBound(0.0, offsetLocal, maxOffsetLocal);

        // \u9032\u6357\u3092\u300c\u5272\u5408\u300d\u306b\u5909\u63db\u3057\u3066\u304b\u3089\u3001\u5b9f\u969b\u306b\u63cf\u753b\u3059\u308b\u30d5\u30a9\u30f3\u30c8\uff08\u30ba\u30fc\u30e0\u3067\u6574\u6570pt\u306b
        // \u4e38\u3081\u3089\u308c\u308b\uff09\u306e\u5b9f\u6e2c\u5e45\u306b\u639b\u3051\u76f4\u3059\u3002\u57fa\u6e96\u30d5\u30a9\u30f3\u30c8\u306e\u7406\u8ad6\u5024\u3092\u305d\u306e\u307e\u307ess\u3067
        // \u62e1\u5927\u3059\u308b\u3068\u3001\u4e38\u3081\u306e\u5883\u754c\u3092\u8de8\u3050\u305f\u3073\u306b\u5b9f\u6e2c\u5e45\u3068\u308f\u305a\u304b\u306b\u30ba\u30ec\u3066\u5c0f\u523b\u307f\u306b
        // \u30d6\u30ec\u3066\u898b\u3048\u3066\u3044\u305f\u305f\u3081\u3001\u5e38\u306b\u300c\u4eca\u30d5\u30ec\u30fc\u30e0\u306e\u5b9f\u6e2c\u5024\u300d\u306b\u5408\u308f\u305b\u3066\u4f4d\u7f6e\u3092\u51fa\u3059\u3002
        const double progress = (maxOffsetLocal > 0.0) ? (offsetLocal / maxOffsetLocal) : 0.0;
        const double fullTextW    = titleFm.horizontalAdvance(title);
        const double maxOffsetNow = qMax(0.0, fullTextW - titleBoxW);
        const double offset = progress * maxOffsetNow;

        QRectF titleBox(titleBoxL, titleY - 2.0, titleBoxW, titleFm.height() + 4.0);
        r.setClipRect(titleBox);
        r.drawText(title, {titleBoxL - offset, titleY},
                   titleFont, QColor(245, 245, 245, titleAlphaI));
        r.clearClip();
    }

    // \u518d\u751f\u30d7\u30ed\u30b0\u30ec\u30b9\u30d0\u30fc
    QRectF track(sr.left() + w * 0.175, sr.top() + h * 0.8375, w * 0.65, h * 0.025);
    r.drawRect(track, QColor(255, 255, 255, 32), Qt::transparent, 0,
               (float)(track.height() * 0.5));
    const qint64 dur = m_controller.duration();
    const qint64 pos = m_controller.position();
    const double frac = (dur > 0) ? qBound(0.0, double(pos) / double(dur), 1.0) : 0.0;
    if (frac > 0.0) {
        QRectF fill(track.left(), track.top(), track.width() * frac, track.height());
        r.drawRect(fill, QColor(s_accentColor.red(), s_accentColor.green(), s_accentColor.blue(), 185),
                   Qt::transparent, 0, (float)(track.height() * 0.5));
    }

    if (!m_controller.isPlaying()) {
        const QPointF c = sr.center();
        const float radius = static_cast<float>(qMin(w, h) * 0.13);
        r.drawCircle(c, radius, QColor(0, 0, 0, 100));
        const double ts = radius * 0.45;
        r.drawLine({c.x() - ts * 0.5, c.y() - ts}, {c.x() + ts, c.y()},
                   QColor(255, 255, 255, 230), 2.0f);
        r.drawLine({c.x() + ts, c.y()}, {c.x() - ts * 0.5, c.y() + ts},
                   QColor(255, 255, 255, 230), 2.0f);
        r.drawLine({c.x() - ts * 0.5, c.y() + ts}, {c.x() - ts * 0.5, c.y() - ts},
                   QColor(255, 255, 255, 230), 2.0f);
    }
}

void CanvasVideo::paintOverlay(Renderer &r, const QRectF &screenRect,
                               bool isActive, double pulseAlpha, double screenScale,
                               double chromeAlpha)
{
    // 2026-10-11: バブル全画面中は枠(ベゼル)とアクティブの青枠・グロー・リサイズ印を
    // フェードで消す。contentCornerFactor は全画面フェード(通常1→全画面0)と
    // 同じ値なので、そのまま枠の不透明度として使う。
    const float chrome = static_cast<float>(qBound(0.0, chromeAlpha, 1.0));
    if (chrome <= 0.002f)
        return;
    const float baseGlobalAlpha = r.globalAlpha();
    r.setGlobalAlpha(baseGlobalAlpha * chrome);

    const double bezel = mediaBezelWidth(screenRect);
    QRectF board = screenRect.adjusted(
        -bezel, -bezel, bezel, bezel);

    const double radius = mediaBoardCornerRadius(screenRect);

    if (isActive) {
        // 2026-07-13: 以前は同心の2本ストローク（外8px/内5px）でグローを
        // 疑似表現していたが、線の重なりが縞（バンディング）として見えて
        // いたため、SDFベースの本物のグラデーショングロー
        // （Renderer::drawGlowRect）へ置き換えた。CanvasImage/CanvasLauncherも
        // 同仕様に統一。
        QColor accent(140,195,255, (int)qBound(0.0,pulseAlpha,255.0));
        QColor glowColor(120, 180, 255, (int)qBound(0.0, pulseAlpha * 0.60, 165.0));
        // 2026-07-19: ガラスハイライト導入でアクティブ/非アクティブの差が
        // 分かりにくくなったため、グローの広がりを12→22→32pxへ拡大。
        // 広がりは基本「画面上32px固定」（＝カメラの影響を受け、引くほど
        // バブル比で大きく見える）だが、バブル比での上限/下限を設ける:
        //   上限 56×ss: 引きすぎてもバブル比で巨大化しない
        //   下限 20×ss: 寄りすぎてもバブル比で小さくなりすぎない
        // （ss≈0.57〜1.6の間は32px固定、外側ではズーム比例に切り替わる）
        {
            const float ssf = static_cast<float>(screenScale);
            r.drawGlowRect(board, glowColor, radius,
                           qBound(20.0f * ssf, 32.0f, 56.0f * ssf));
        }
        r.drawRect(board, QColor(255,255,255,90), accent, 4.0f, radius);
    } else {
        // 2026-07-17: 非アクティブ時の境界線が見えにくいとのことで、白を
        // もう少し目立つアルファへ上げた（CanvasImage/CanvasLauncherと揃える）。
        // 塗りの透明度も下げて(=不透明度を上げて)少し実体感を出した。
        // 2026-10-11: グミのときは塗りを消し、線も少し弱める（ゲルの面を見せる）。
        const bool gummy = AppSettings::instance().gummyGlassActive();
        r.drawRect(board,
                   QColor(255,255,255, gummy ? AppSettings::kGummyChromeFillAlpha : 46),
                   QColor(255,255,255, gummy ? AppSettings::kGummyChromeStrokeAlpha : 130),
                   1.5f, radius);
    }

    // 2026-09-14: サラウンドモード廃止に伴い、緑のサラウンドリング描画を削除。

    // 位置・線長・線幅を比例ベゼル基準にし、リサイズとズームの両方へ追従。
    double ix = board.right()  - bezel * 0.5;
    double iy = board.bottom() - bezel * 0.5;
    const double is = resizeMarkerSize(bezel);
    const float markerLineWidth =
        static_cast<float>(resizeMarkerLineWidth(bezel));
    r.drawLine({ix-is,     iy-is*0.3}, {ix-is*0.3, iy-is},
               QColor(255,255,255,180), markerLineWidth);
    r.drawLine({ix-is*0.6, iy-is*0.3}, {ix-is*0.3, iy-is*0.6},
               QColor(255,255,255,180), markerLineWidth);
    r.setGlobalAlpha(baseGlobalAlpha);
}

void CanvasVideo::adjustVolume(float delta)
{
    const float v = qBound(0.0f, m_userVolume + delta, 1.0f);
    setVolume(v);
}

void CanvasVideo::setVolume(float volume)
{
    m_userVolume = qBound(0.0f, volume, 1.0f);
    applyEffectiveVolume();
    emit playbackUiChanged();
}

bool CanvasVideo::beginDeferredLoadForPlayback()
{
    // 通常は m_deferredLoad（セッション復元直後の未読み込み状態）の時だけ
    // ここに入るが、PluginMedia が解決失敗して赤いエラーバナーが出ている
    // 場合は、フレッシュドロップ由来（m_deferredLoad=false）であっても
    // クリックで再試行できるようにする。
    const bool retryFailedPluginMedia =
        (m_source.kind == MediaKind::PluginMedia) && m_pluginResolveFailure.failed;
    if (!retryFailedPluginMedia && (!m_deferredLoad || m_source.originalPathOrUrl.isEmpty()))
        return false;
    if (m_source.originalPathOrUrl.isEmpty())
        return false;
    if (m_deferredFetchInProgress)
        return true;

    // Phase 8: Plugin API — セッション復元された PluginMedia は、
    // originalPathOrUrl が元ページURL（例: 動画サイトのページURL）であり
    // ローカルファイルでもストリームURLでもない。ここでプラグインへ
    // 再解決を依頼する。成否は resolveViaPlugin() 内で処理される
    // （成功時は onUrlFetched() 経由で startPlayback() が呼ばれる）。
    if (m_source.kind == MediaKind::PluginMedia) {
        m_deferredFetchInProgress = true;
        m_playAfterDeferredLoad = true;
        resolveViaPlugin(m_source.originalPathOrUrl);
        return true;
    }

    // 2026-10-12: セッション復元された Web動画。保存済みならファイルから、
    // まだならURLから再生する（loadMediaSource() の WebVideo 分岐と同じ）。
    if (m_source.kind == MediaKind::WebVideo) {
        m_streamUrl = WebVideoCache::playableUrlFor(m_source.originalPathOrUrl);
        WebVideoCache::ensureCached(m_source.originalPathOrUrl);
        m_streamHttpHeaders.clear();
        m_deferredFetchInProgress = false;
        m_playAfterDeferredLoad = true;
        m_currentQuality = 480;
        startPlayback();
        return true;
    }

    // isLocalMediaUrl() は QUrl のパース結果に基づく判定なので、パスの形式次第で
    // false 誤判定になり得る。ここで即座に諦めて m_deferredLoad=false にすると、
    // 以後 m_streamUrl が二度とセットされず「クリックしても永久に再生されない」
    // 状態に陥る（実際に報告されたバグ）。そのため QUrl 判定が false でも、
    // 実ファイルとして存在するかどうかで最終判断する二段構えにする。
    const QString localPath = QUrl(m_source.originalPathOrUrl).toLocalFile();
    const bool existsAsLocalFile =
        !localPath.isEmpty() && QFileInfo::exists(localPath);

    if (!isLocalMediaUrl(m_source.originalPathOrUrl) && !existsAsLocalFile) {
        qWarning("[CanvasVideo] Ignoring restored external media URL in core: %s",
                 qPrintable(m_source.originalPathOrUrl));
        // ここでは m_deferredLoad を false にしない。ネットワーク越しの外部URL等、
        // 本当に非対応のケースであれば毎回この分岐に来るだけで実害はないが、
        // 逆にローカルファイル判定の誤検知だった場合は、フラグを立てたままに
        // しておくことで、後で判定ロジックが直ったり状況が変わった際に
        // 再度クリックすれば救済されるようにしておく。
        return true;
    }

    m_streamUrl = m_source.originalPathOrUrl;
    m_streamHttpHeaders.clear();
    m_deferredFetchInProgress = false;
    m_playAfterDeferredLoad = true;
    m_currentQuality = 480;
    startPlayback();
    return true;
}

bool CanvasVideo::setMasterGain(float gain, bool applyImmediately)
{
    const float boundedGain = qBound(0.0f, gain, 1.0f);
    if (qAbs(m_masterGain - boundedGain) < 0.001f)
        return false;
    m_masterGain = boundedGain;
    if (applyImmediately)
        applyEffectiveVolume();
    return true;
}

QRectF CanvasVideo::volSliderScreenRect(int windowH) const
{
    const int bH = 56, bY = windowH - bH, by = bY + bH/2;
    const int bx = 28, vbx = bx + 36;
    const int sh = 80, sw = 28;
    const int sy = by - 16 - sh;
    return QRectF(vbx - sw/2, sy, sw, sh + 16);
}

void CanvasVideo::queuePlayerForGlDelete(MpvPlayer *player)
{
    if (!player)
        return;
    m_playersAwaitingGlDelete.append(player);
    emit frameReady(); // 次のpaintGL()を早め、破棄を実行させる
}

void CanvasVideo::drainPlayersAwaitingGlDelete()
{
    // 呼び出し元がGLコンテキストcurrentを保証すること
    // （paintGL()冒頭・releaseGlResources()から呼ばれる）。
    //
    // 2026-07-15: この関数はFlowプリロードのキャンセル（cancelFlowPreload()）
    // から遅延破棄されたMpvPlayerを実際に delete する経路。かつては
    // 破棄後の🚫バグ対策（refreshDropTargets()呼び出し）が唯一漏れていた
    // 経路だったが、destroyMpvPlayer()（MpvTeardown.h）に一元化したことで
    // 他の破棄経路と同じ扱いになった。
    for (MpvPlayer *player : std::as_const(m_playersAwaitingGlDelete)) {
        qInfo("[CanvasVideo %p] drainPlayersAwaitingGlDelete delete player=%p",
              static_cast<void*>(this), static_cast<void*>(player));
        destroyMpvPlayer(this, player, /*releaseGlBeforeDelete=*/true);
    }
    m_playersAwaitingGlDelete.clear();
}

namespace {
// 部品キャッシュのキーを組み立てる混ぜ合わせ（板のキーと同じ式）。
inline quint64 uiPartKeyMix(quint64 h, quint64 v)
{
    return (h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
}
} // namespace

// 2026-09-14: 部品1つを画像へ焼いて貼る（CanvasVideo.h の UiPartCache参照）。
//
// 【見た目が変わらない根拠】このオーバーレイは半透明ウィジェットで、背後の
// ぼかしはGL側の別レイヤーが受け持っている。ガラスの加算(Plus)・乗算
// (Multiply)はいずれも「透明な下地」に対して合成されているので、透明な
// 画像へ描いてから貼るのと完全に同じ結果になる（リストの板で既に使って
// いるのと同じ理屈）。
//
// 【フェードはキーに入れて焼き込む】貼るときの不透明度で薄める方式は
// 使わない。Plus/Multiplyの掛かり方が変わって色がずれるため。段階は
// 1/64なので、バーの出入り（約0.3秒）の間に十数回作り直すだけで済む。
void CanvasVideo::drawCachedUiPart(QPainter &painter, UiPartCache &cache,
                                   const QRectF &logicalRect, quint64 key,
                                   const std::function<void(QPainter &)> &draw)
{
    if (logicalRect.width() <= 0.0 || logicalRect.height() <= 0.0)
        return;
    const qreal dpr = painter.device()
        ? painter.device()->devicePixelRatioF() : 1.0;
    // 2026-09-16 F-2: 部品の左上をデバイス画素の格子へ吸着させる。
    //
    // 【なぜ】音量ボタンはホバーで 1.0→0.75 へ縮む（m_volButtonScale）。
    // 呼び出し側の partRect は中心(vbx,by)から ext ぶん広げた矩形なので、
    // extが連続的に変わると左上が端数で動く。ここで端数のまま
    //   cp.translate(-logicalRect.topLeft())  … 中身を画像内でサブピクセルずらし
    //   painter.drawImage(logicalRect.topLeft(), ...) … 転送先は整数へ丸められる
    // をやると、2つの丸めが毎フレーム違う向きに出て、丸の中心が±1px揺れる。
    // 「中心基準になっていなくてガクガクする」の正体がこれ。
    // 描く前と貼る前で同じ吸着済み座標を使えば、位相が固定されて揺れない。
    const QPointF snapped(std::floor(logicalRect.left() * dpr) / dpr,
                          std::floor(logicalRect.top()  * dpr) / dpr);
    const QRectF alignedRect(snapped,
                             QSizeF(logicalRect.right()  - snapped.x(),
                                    logicalRect.bottom() - snapped.y()));
    const QSize px(qMax(1, static_cast<int>(std::ceil(alignedRect.width()  * dpr)) + 1),
                   qMax(1, static_cast<int>(std::ceil(alignedRect.height() * dpr)) + 1));
    if (cache.key != key || cache.image.isNull() || cache.image.size() != px
        || !qFuzzyCompare(cache.dpr + 1.0, dpr + 1.0)) {
        QImage img(px, QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(dpr);
        img.fill(Qt::transparent);
        {
            QPainter cp(&img);
            cp.setRenderHint(QPainter::Antialiasing, true);
            cp.setRenderHint(QPainter::TextAntialiasing, true);
            cp.setRenderHint(QPainter::SmoothPixmapTransform, true);
            // 呼び出し側は画面座標のまま描けばよい。
            cp.translate(-alignedRect.topLeft());
            draw(cp);
        }
        cache.image = img;
        cache.key   = key;
        cache.dpr   = dpr;
    }
    painter.drawImage(alignedRect.topLeft(), cache.image);
}

void CanvasVideo::paintPlaybackOverlay(QPainter &painter, int W, int H,
                                       BackdropBlur::Regions *blurRegions)
{
    qint64 pos     = m_controller.position();
    qint64 dur     = m_controller.duration();
    // 2026-09-13: 切り替え中は再生中の見た目のまま（paintPauseIcon参照）。
    bool   playing = (m_controller.state() == BubbleState::Playing)
                     || isSwitchingVideo();
    float  vol     = m_userVolume;

    const int bH = 56, bY = H - bH;
    const int by = bY + bH/2;

    // シークバー本体・背景・時刻はフェード(m_playbarFadeAlpha)。
    // 再生停止ボタン・音量アイコンは「現れる時」だけポップ
    // (m_playbarPopScaleVol/Play、音量が先・再生停止ボタンが少し遅れて弾む)、
    // 「消える時」はバウンドさせずフェード(fadeAlpha)だけで消す。
    const float fadeAlpha = m_playbarFadeAlpha;
    const float popScaleVol  = m_playbarPopScaleVol;
    const float popScalePlay = m_playbarPopScalePlay;

    // 2026-09-12: 下端の暗いグラデーション（バーの下敷き）は撤廃。
    // 丸ボタン・シークバー・音量スライダーが各自ガラスの台を持つように
    // なったので、映像の下1/4を暗くする必要がなくなった。

    const int bx = 28;
    if (popScalePlay > 0.001f && fadeAlpha > 0.001f) {
        const double btnR = 16.0 * popScalePlay;
        // 背後ぼかし(Blur.h)。半径をフェードに比例させ、バーの出現/消滅に
        // 合わせてぼかしも滑らかに立ち上がる/消えるようにする。
        if (blurRegions) {
            // 半径は固定のまま、効き具合(opacity)にUI自身のフェード値を渡す。
            // painter.opacity()を掛けているので、m_contentOpacityのような
            // 上位のフェードにも自動的に追従する。
            blurRegions->add(QRectF(bx - btnR, by - btnR, btnR * 2.0, btnR * 2.0),
                             btnR, BackdropBlur::kDefaultUiBlurRadius,
                             painter.opacity() * fadeAlpha);
        }
        // 2026-09-14: ガラスの円＋アイコンは、押して ▶ / || が入れ替わるとき
        // とバーが出入りするとき以外まったく変わらない。キャッシュへ回す
        // （drawCachedUiPart参照）。
        const double iconSize = 25.0 * popScalePlay; // 従来比で気持ち大きめに
        const double ext = qMax(btnR, iconSize * 0.5) + 3.0;
        const QRectF partRect(bx - ext, by - ext, ext * 2.0, ext * 2.0);
        quint64 key = uiPartKeyMix(0x706c6179ULL, quint64(qRound(btnR * 64.0)));
        key = uiPartKeyMix(key, quint64(qRound(iconSize * 64.0)));
        key = uiPartKeyMix(key, quint64(qRound(fadeAlpha * 64.0)));
        key = uiPartKeyMix(key, quint64(playing ? 1 : 0));
        key = uiPartKeyMix(key, quint64(cachedAccentColor().rgba()));
        drawCachedUiPart(painter, m_playBtnCache, partRect, key,
                         [&](QPainter &cp) {
            // 島のボタンと同じガラス表現(加算ハイライト＋乗算の翳り)。
            paintPlaybackGlassCircle(cp, QPointF(bx, by), btnR, fadeAlpha);
            // 再生中は now_play.svg（||＝押すと一時停止）、停止中は
            // now_stop.svg（▶＝押すと再生）を表示する。
            const QImage icon = tintedSvgImageForPlaybackBar(
                playing ? QStringLiteral(":/icons/now_play.svg")
                        : QStringLiteral(":/icons/now_stop.svg"),
                QSize(64, 64), QColor(255, 255, 255));
            cp.save();
            cp.setOpacity(cp.opacity() * fadeAlpha);
            cp.drawImage(QRectF(bx - iconSize / 2.0, by - iconSize / 2.0,
                                iconSize, iconSize), icon);
            cp.restore();
        });
    }

    const int vbx = bx + 36;
    // 2026-07-15: ホバー時のボタン縮小/拡大アニメーション(m_volButtonScale)を
    // 掛け合わせる。popScaleVolは「再生バー全体が現れる時」のポップ、
    // m_volButtonScaleは「ホバー時にボタン自体が縮む/戻る」ポップで、
    // 役割が異なるため両方を乗算して最終スケールにする。
    if (popScaleVol > 0.001f && fadeAlpha > 0.001f) {
        const double btnR = 16.0 * popScaleVol * m_volButtonScale;
        if (blurRegions) {
            blurRegions->add(QRectF(vbx - btnR, by - btnR, btnR * 2.0, btnR * 2.0),
                             btnR, BackdropBlur::kDefaultUiBlurRadius,
                             painter.opacity() * fadeAlpha);
        }
        // 2026-09-14: こちらもキャッシュへ。変わるのは音量が4段階を跨いだ
        // ときと、カーソルが乗って縮む/戻るアニメーションの最中だけ。
        // ホバー中に作り直されるのはこの32x32だけで、溝も時刻の台も
        // 再生ボタンも貼るだけで済む（部品ごとに分けている理由）。
        const int level = soundIconLevelForVolume(vol);
        const double iconSize = 18.0 * popScaleVol * m_volButtonScale;
        const double ext = qMax(btnR, iconSize * 0.5) + 3.0;
        const QRectF partRect(vbx - ext, by - ext, ext * 2.0, ext * 2.0);
        quint64 key = uiPartKeyMix(0x766f6cULL, quint64(qRound(btnR * 64.0)));
        key = uiPartKeyMix(key, quint64(qRound(iconSize * 64.0)));
        key = uiPartKeyMix(key, quint64(qRound(fadeAlpha * 64.0)));
        key = uiPartKeyMix(key, quint64(level));
        key = uiPartKeyMix(key, quint64(cachedAccentColor().rgba()));
        drawCachedUiPart(painter, m_volBtnCache, partRect, key,
                         [&](QPainter &cp) {
            paintPlaybackGlassCircle(cp, QPointF(vbx, by), btnR, fadeAlpha);
            // マスターボリュームの+ボタンと同じSVG(ico_Sound_0〜3)を使い、
            // このバブル自身の音量(vol、0.0-1.0)に応じて段階を切り替える。
            const QImage icon = tintedSvgImageForPlaybackBar(
                QStringLiteral(":/icons/ico_Sound_%1.svg").arg(level),
                QSize(64, 64), QColor(255, 255, 255));
            cp.save();
            cp.setOpacity(cp.opacity() * fadeAlpha);
            cp.drawImage(QRectF(vbx - iconSize / 2.0, by - iconSize / 2.0,
                                iconSize, iconSize), icon);
            cp.restore();
        });
    }

    if (m_volSliderAnim > 0.01f) {
        // SliderCollapseフェーズの終盤はOutBackの反転により一瞬マイナスに
        // 振れる（1点に収まる直前の「びよん」という縮みすぎ）。高さの
        // 描画自体は0未満にできないため、ここでだけ0にクランプする。
        const int sh = qMax(0, (int)(80 * m_volSliderAnim));
        const int sw = 6;
        const int sx = vbx - sw/2;
        const int sy = by - 16 - sh;
        painter.setPen(Qt::NoPen);
        // 2026-09-12: 溝は島・ラベルと同じガラスの素材で（paintGlassTrack参照）。
        paintGlassTrack(painter, QRectF(sx, sy, sw, sh), 3.0, fadeAlpha);
        int filled = (int)(sh * vol);
        // 進んだぶんもアクセント色の家族に揃える（以前は固定の緑）。
        QColor volFill = accentLightColor();
        volFill.setAlpha(qBound(0, qRound(225 * fadeAlpha), 255));
        painter.setBrush(volFill);
        painter.drawRoundedRect(QRectF(sx, sy+sh-filled, sw, filled), 3, 3);
        int hy = sy + sh - filled;
        // ハンドルはスライダー本体の伸縮を待たず、単独ですばやく
        // フェードアウトする(m_volHandleAlpha、updateVolAnim()参照)。
        if (m_volHandleAlpha > 0.001f) {
            QColor handleColor = Qt::white;
            handleColor.setAlphaF(qBound(0.0f, m_volHandleAlpha, 1.0f));
            painter.setBrush(handleColor);
            painter.drawEllipse(QPointF(vbx, hy), 5, 5);
        }
    }

    // 進捗トラック（塗り＋ドラッグハンドル）。プラグインが
    // seekBarVisible=false を指定した場合は描かない（配信でdurが
    // 伸び続けることによる「アキレスと亀」的なちらつきを避けるため）。
    if (dur > 0 && m_seekBarVisible && fadeAlpha > 0.001f) {
        // 2026-09-13: 右端の余白を90→100へ広げた。時刻表示にもガラスの台を
        // 敷いたので、シークバーの終端と台が触れないだけの隙間を空ける
        // （台の矩形は timeTextScreenRect。CanvasWidget側のヒット判定の
        //  余白もここと同じ100に揃えてある）。
        const int sx=vbx+24, sw=W-sx-100, sy=by-2, sh=4;
        // 2026-09-12: 溝は島・ラベルと同じガラスの素材で（paintGlassTrack参照）。
        // 以前は白のベタ塗りで、隣の丸ボタンだけがガラスという不揃いだった。
        // 2026-09-14: 灰色の溝は動かない（変わるのは窓幅とフェードだけ）ので
        // キャッシュへ。この上に重なる赤い進捗は矩形1個なので毎フレーム描く。
        if (sw > 0) {
            const QRectF trackRect(sx, sy, sw, sh);
            quint64 key = uiPartKeyMix(0x7365656bULL, quint64(sw));
            key = uiPartKeyMix(key, quint64(sh));
            key = uiPartKeyMix(key, quint64(qRound(fadeAlpha * 64.0)));
            key = uiPartKeyMix(key, quint64(cachedAccentColor().rgba()));
            drawCachedUiPart(painter, m_seekTrackCache,
                             trackRect.adjusted(-2.0, -2.0, 2.0, 2.0), key,
                             [&](QPainter &cp) {
                paintGlassTrack(cp, trackRect, sh * 0.5, fadeAlpha);
            });
        }
        int played = static_cast<int>(sw * pos / dur);
        // 進んだぶんの色はプラグインが指定していればそれを尊重する
        // （サービスの色。本体は解釈しない）。無指定ならアクセント色の明色。
        QColor fillColor = m_seekBarColor.isValid() ? m_seekBarColor : accentLightColor();
        fillColor.setAlpha(qRound((m_seekBarColor.isValid() ? fillColor.alpha() : 235)
                                  * fadeAlpha));
        painter.setPen(Qt::NoPen);
        painter.setBrush(fillColor);
        painter.drawRoundedRect(QRectF(sx, sy, played, sh), sh * 0.5, sh * 0.5);

        // ドラッグハンドル（白丸）は常時表示だと主張しすぎるため、
        // シークバーにカーソルを合わせている間（ドラッグ中含む）だけ
        // フェードで出す。
        if (m_seekHandleAnim > 0.001f) {
            const float handleAlpha = fadeAlpha * m_seekHandleAnim;
            const float handleR = 6.0f * qMin(1.0f, 0.6f + 0.4f * m_seekHandleAnim);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255,255,255, qRound(255 * handleAlpha)));
            painter.drawEllipse(QPointF(sx+played, sy+sh/2.0), handleR, handleR);
        }
    }

    // 時刻テキスト。符号付きで扱えるよう、0クランプはしない。
    if (fadeAlpha > 0.001f) {
        auto fmtSigned = [](qint64 ms){
            const bool neg = ms < 0;
            qint64 s = (neg ? -ms : ms) / 1000;
            return QString("%1%2:%3").arg(neg ? "-" : "").arg(s/60).arg(s%60,2,10,QChar('0'));
        };
        const bool isCountdown = (m_seekTimeMode == PbResolvedMedia::TimeDisplayMode::Countdown);
        // m_liveEdgeBufferMs分を足して表示する。意図した分だけ遅れている
        // （＝正常な状態）ときに "0:00" と表示され、それを超えてさらに
        // 遅れた場合だけマイナス表示が出るようにするための補正（動画側は
        // m_liveEdgeBufferMs=0のため影響なし＝従来通りの残り時間表示のまま）。
        const QString timeText = isCountdown
            ? fmtSigned(pos - dur + m_liveEdgeBufferMs)
            : (fmtSigned(qMax(0LL,pos)) + " / " + fmtSigned(qMax(0LL,dur)));
        // ---- 時刻の台（2026-09-13追加）----
        // 丸ボタン・シークバーの溝と同じく、時刻にもガラスの台を敷く。
        // 【大きさは表示モードで変えない】クリックで「経過/合計」と
        // 「残り」を切り替えられるが、文字の長さに台を合わせると押すたびに
        // 台が伸び縮み・移動して見える。台は timeTextScreenRect() 一本に
        // 決め打ちし、中身の文字だけが入れ替わるようにする（＝位置は不動）。
        const QRectF pad = timeTextScreenRect(W, H);
        const double padR = pad.height() * 0.5;   // カプセル
        // 2026-09-16: GL版では台はぼかしと一緒にGL側で描く（videoGlassOnGpu）。
        const bool glTimePlate = blurRegions && videoGlassOnGpu();
        if (blurRegions) {
            blurRegions->add(pad, padR, BackdropBlur::kDefaultUiBlurRadius,
                             painter.opacity() * fadeAlpha,
                             glTimePlate ? videoGlassStyle(videoUiBodyColor())
                                         : BackdropBlur::GlassPlateStyle());
        }
        // 島・ラベルの板と同じライティング（加算で輝く縁／乗算で沈む縁）。
        // 2026-09-14: この板がいちばん高い。paintGlassPlate() は縁の減衰を
        // 作るために同じパスをグラデーションのペンでのべ56本重ねて描くため。
        // 台の大きさは表示モードで変わらない（timeTextScreenRectに決め打ち）
        // ので、窓のサイズとフェード以外では一切変わらない＝キャッシュ向き。
        // 中身の数字とホバーの白はこの下で毎フレーム描く。
        if (!glTimePlate) {
            quint64 key = uiPartKeyMix(0x74696d65ULL,
                                       quint64(qRound(pad.width() * 4.0)));
            key = uiPartKeyMix(key, quint64(qRound(pad.height() * 4.0)));
            key = uiPartKeyMix(key, quint64(qRound(fadeAlpha * 64.0)));
            key = uiPartKeyMix(key, quint64(cachedAccentColor().rgba()));
            drawCachedUiPart(painter, m_timePlateCache,
                             pad.adjusted(-2.0, -2.0, 2.0, 2.0), key,
                             [&](QPainter &cp) {
                paintGlassPlate(cp, roundedPathLR(pad, padR, padR), pad,
                                videoUiBodyColor(), static_cast<double>(fadeAlpha));
            });
        }

        // ホバー反応はラベルと同じ「薄い白を重ねる」やり方（押せることの
        // 合図）。プラグインが表示モードを固定している（押しても切り替わら
        // ない）ときは、反応させても嘘になるので出さない。
        if (m_timeHoverAnim > 0.005f && !m_seekTimeModeLocked) {
            painter.save();
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255,
                                    qRound(30 * m_timeHoverAnim * fadeAlpha)));
            painter.drawRoundedRect(pad, padR, padR);
            painter.restore();
        }

        QFont f("Arial", 9);
        const QColor textColor(255,255,255, qRound(200 * fadeAlpha));
        painter.setFont(f);
        painter.setPen(textColor);
        // カウントダウン表示("-0:00")は通常表示("12:34 / 45:00")より短いが、
        // どちらも同じ台の中央に置く（台が動かないので、切り替えても
        // 文字だけが差し替わって見える）。
        // 2026-09-12: 下敷きの暗いグラデーションを撤廃したので、
        // 時間表示もほかのUI文字と同じく影を落とす。
        drawShadowedText(painter, pad, Qt::AlignCenter, timeText,
                         textColor, fadeAlpha);
    }
    // ラベル／リストの描画は paintLabelAndPanel() に切り出してある
    // （固定ラベルとして非アクティブなバブルからも描くため）。他のバブルの
    // 固定ラベルが左上を占めている間は、こちらは描かない。
    if (!m_labelSuppressedByPin)
        paintLabelAndPanel(painter, W, H, blurRegions, m_playbarFadeAlpha);
}
// ラベル（左上のロゴ＋テキスト）と、そこから伸びるコントローラの描画。
// fade には通常 m_playbarFadeAlpha を、固定ラベルとして描くときは 1.0 を
// 渡す（固定中は再生バーの表示条件と切り離して出しっぱなしにする）。
// 2026-09-22: 本体の基本ラベルのロゴ。プラグインがロゴを渡さないとき
// （ローカルの動画・音声、ロゴを持たないプラグイン）に使う。
// 動画サイト用プラグインの再生ボタンの丸を灰色にしたもの。本体はどのサービスでもない
// ので、色は付けない。描画のきまり（円に切り抜く）は PbResolvedMedia::
// labelIconSvg と同じ。
const QByteArray &CanvasVideo::coreLabelIconSvg()
{
    static const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>"
        "<rect width='64' height='64' rx='32' fill='#80858f'/>"
        "<path d='M25 20 L46 32 L25 44 Z' fill='#ffffff'/>"
        "</svg>");
    return svg;
}

void CanvasVideo::paintLabelAndPanel(QPainter &painter, int W, int H,
                                     BackdropBlur::Regions *blurRegions,
                                     float fade)
{
    m_labelFadeAlpha = fade;

    // 引っ張っている間（と、離してバネで戻る間）は全体を下へずらす。
    painter.save();
    if (m_labelPull > 0.01)
        painter.translate(0.0, m_labelPull);
    // 2026-09-23: 長押しの実行に失敗したとき、ラベルごと小さく首を振る。
    if (m_labelHoldShakeMs >= 0.0) {
        constexpr double kShakeMs = 380.0;
        const double t = qBound(0.0, m_labelHoldShakeMs / kShakeMs, 1.0);
        const double amp = 4.0 * (1.0 - t) * (1.0 - t);
        painter.translate(amp * std::sin(t * 2.0 * M_PI * 3.5), 0.0);
    }

    // ---- フォーカスラベルと、そこから伸びるコントローラ ----
    //
    // 開いている間はラベルの台を描かせず、代わりに「ラベルの形から
    // カードの形へ伸びていく1枚の板」をこちら側で描く（島UIと同じ
    // 考え方。2枚重ねると角が二重に見えてしまう）。板は t=0 でラベルの
    // 台と完全に同じ形なので、開き始めに形が飛ばない。
    m_labelPanelRect = QRectF();
    m_labelPanelRowRects.clear();
    m_labelPanelRowUrls.clear();
    m_labelPanelRowIndices.clear();
    m_labelPanelNowRowRect = QRectF();
    m_labelBtnPrev = m_labelBtnPlay = m_labelBtnNext = QRectF();

    // 台（ガラスの板）は畳んでいる間も常にこちら側で描く。ラベル側にも
    // 台を描く機能は残っているが、両方が描くと角が二重に見えるので、
    // こちらが描くときはラベル側は中身（ロゴ＋テキスト）だけにする。
    const bool panelActive = (m_labelPanelAnim > 0.001f);

    // 2026-09-12: 次の動画へ移る間もラベル（＝リストの頭）を保つ。
    // resolveViaPlugin()は新しい解決を始める時点でラベルを一旦空にする
    // （失敗時や別サイトの動画へ移ったときに古い表示が残らないように）。
    // そのため、コントローラを開いたまま次の動画へ進むと、解決〜デコード
    // 開始までの数秒間だけリストがまるごと消えていた。開いている間に
    // 限り、直前の（空でない）ラベルを描画用に使い回して繋ぐ。中身の行は
    // プラグインのキャッシュから組まれるのでそのまま出せる。
    //
    // 2026-09-22: 本体の基本ラベル。ラベルはプラグインのメディアだけのもの
    // ではなくなった。どの動画バブルにも本体がラベルを出し、プラグインは
    // それを上書きできる:
    //   テキスト … プラグインが空なら本体が決める（Solo / List）。
    //              Mix・Live などはプラグインが足すもの。
    //   ロゴ     … プラグインが空なら本体の既定ロゴ（灰色）。
    //   List     … 本体の List はバブル融合で吸い込んだ動画があるとき
    //              （stackCount() > 1）。本体は List の中身の意味を知らない。
    // プラグインのメディアは、解決の答えが返るまで（m_pluginLabelKnown）は
    // まだ何者か分からないので、本体のラベルも出さず sticky で繋ぐ。
    const bool labelKnown =
        m_source.kind != MediaKind::PluginMedia || m_pluginLabelKnown;
    if (labelKnown) {
        const bool pluginText = !m_labelText.isEmpty();
        m_labelIconSvgSticky = m_labelIconSvg.isEmpty() ? coreLabelIconSvg() : m_labelIconSvg;
        if (hasCoreList()) {
            // 本体のリスト。並びを持っているのは本体なので、プラグインの
            // テキスト（その動画単体の意味）より優先して List を出す。
            // 一覧を開ける・固定できる・再生位置を出せる（プラグインの List と同じ）。
            m_labelTextSticky      = QStringLiteral("List");
            m_labelHoverableSticky = true;
            m_labelPinnableSticky  = true;
            m_labelProgressSticky  = true;
        } else {
            m_labelTextSticky = pluginText ? m_labelText : QStringLiteral("Solo");
            // 反応・固定・再生位置の表示は、プラグインのラベルならプラグインの指定。
            // 本体の Solo は表示だけ（押しても何も起きない）。
            m_labelHoverableSticky = pluginText && m_labelHoverable;
            m_labelPinnableSticky  = pluginText && m_labelPinnable;
            m_labelProgressSticky  = pluginText && m_labelProgress;
        }

        // 2026-09-23: ラベルの長押し（PbLabelActionApi.h）。
        //   本体のリスト … 無し（一覧はクリックで開く）
        //   ローカル     … 本体が受け持つ（同じフォルダで List にする）。Solo のときだけ。
        //   プラグイン   … プラグインに聞く（例: 単発の動画を関連動画のリストに切り替える）。
        // プラグインへは毎フレーム聞かず、URLとラベルが変わったときだけ聞き直す
        // （配信だと分かった等で答えが変わるのは、解決が終わってラベルが変わるとき）。
        // 2026-09-23改訂: 動作の持ち主は担当プラグインが決める（PbLabelActionApi.h）。
        // 本体が持っているのは「長押しという操作」と、自分の動作が1つ
        // （同じフォルダのものを並べる）だけ。担当プラグインがいないメディア
        // （ローカルのファイル）は聞く相手がいないので本体の動作になる。
        bool holdAvailable = false;
        bool holdCore = false;
        QString holdHint;
        QString holdTarget;
        if (!hasCoreList()) {
            if (m_source.kind == MediaKind::PluginMedia) {
                const QString key = m_source.originalPathOrUrl + QChar(0x1f) + m_labelText;
                if (key != m_labelHoldQueryUrl) {
                    m_labelHoldQueryUrl = key;
                    const PbLabelHoldAction a =
                        PluginManager::instance().labelHoldAction(m_source.originalPathOrUrl);
                    m_labelHoldQueryKind = static_cast<int>(a.kind);
                    m_labelHoldQueryHint = a.hint;
                    m_labelHoldQueryTarget = a.targetLabel;
                    qDebug().noquote() << QStringLiteral(
                        "[CanvasVideo %1] label hold: plugin says kind=%2 hint=\"%3\" for %4")
                        .arg(reinterpret_cast<quintptr>(this), 0, 16)
                        .arg(m_labelHoldQueryKind).arg(m_labelHoldQueryHint,
                             m_source.originalPathOrUrl.left(90));
                }
                const auto kind = static_cast<PbLabelHoldActionKind>(m_labelHoldQueryKind);
                holdAvailable = (kind != PbLabelHoldActionKind::None);
                holdCore = (kind == PbLabelHoldActionKind::Core);
                holdHint = holdCore ? coreLabelHoldHint() : m_labelHoldQueryHint;
                holdTarget = holdCore ? coreLabelHoldTarget() : m_labelHoldQueryTarget;
            } else if (m_source.kind == MediaKind::LocalVideo
                       || m_source.kind == MediaKind::LocalAudio) {
                holdAvailable = !pluginText;
                holdCore = true;
                holdHint = coreLabelHoldHint();
                holdTarget = coreLabelHoldTarget();
            }
        }
        m_labelHoldAvailableSticky = holdAvailable;
        m_labelHoldCore = holdCore;
        m_labelHoldHint = holdHint;
        m_labelHoldTargetText = holdTarget;
        // 押せることの合図（ホバーで明るくなる）も出す。
        if (holdAvailable)
            m_labelHoverableSticky = true;
    }
    // 2026-09-13改訂: 条件を外し、**次の動画へ移る間は常に**直前のラベルで
    // 繋ぐ。従来は「リストを開いている／固定している」ときだけだったため、
    // ふつうに自動再生で次の曲へ進むと、解決中の1〜2秒だけラベルが消えて
    // また出る、というちらつきが起きていた。
    // 【sticky が残り続けない理由】解決が終わった時点で、結果が空なら
    // sticky も空にしている（onUrlFetched）。ラベルの無い動画へ移った
    // 場合は、その瞬間に消える。
    // 2026-09-22: 描画は常に sticky（＝上で決めた今のラベル、解決中なら直前の
    // ラベル）を使う。
    const QByteArray labelIconSvg = m_labelIconSvgSticky;
    const QString    labelText    = m_labelTextSticky;
    m_labelHoverableEffective = m_labelHoverableSticky;
    m_labelPinnableEffective  = m_labelPinnableSticky;
    m_labelProgressEffective  = m_labelProgressSticky;
    m_labelHoldAvailableEffective = m_labelHoldAvailableSticky;
    // 固定できないものへ差し替わったら固定は自動的に外す（Liveや単発動画に
    // 移った場合など）。
    // 【2026-09-12】ただし「まだラベルが分かっていないだけ」の状態では外さない。
    // 解決中（resolveViaPlugin がラベルを一旦空にする）は labelPinnable も
    // false に見えるが、それは「固定不可」ではなく「未確定」。ここで外すと
    // 次の動画へ移るたびに固定が落ちる。答えが返ってラベルが埋まった時点で
    // 改めて判定する。
    if (m_labelPinned && !labelText.isEmpty() && !m_labelPinnableEffective)
        m_labelPinned = false;

    // 入れ替え先の語も渡す（台の幅を先に確定させ、入れ替えの最中に形を動かさない）。
    QRectF labelRect = m_labelPainter.measure(W, H, labelIconSvg, labelText,
                                              m_labelHoldTargetText);
    if (!labelRect.isEmpty())
        m_labelRectForPanel = labelRect;   // 縮める前の「本来の幅」を保つ
    else if (panelActive)
        labelRect = m_labelRectForPanel; // 移動直後のロード中も形を保つ

    // 2026-09-13: 島とぶつかるときの最小化。ここで台の幅そのものを縮めて
    // おけば、板（labelPanelPlateGeometry）も縁の再生位置表示も同じ形を
    // 見るので、別々に追従させる必要がない。ラベル側の描画には同じ進捗を
    // 渡して、ロゴを台の中心へ寄せ・テキストを消してもらう。
    const double labelCompact = qBound(0.0f, m_labelCompactAnim, 1.0f);
    if (labelCompact > 0.0001 && !labelRect.isEmpty()) {
        labelRect.setWidth(CanvasVideoLabelPainter::compactPillWidth(
            labelRect.width(), labelRect.height(), labelCompact));
    }
    QList<PanelRow> rows;
    QRectF plate;
    const bool wantPlate = !labelRect.isEmpty() && !labelText.isEmpty();
    QElapsedTimer panelStatTimer;
    bool panelCacheHit = false;
    if (wantPlate && m_labelFadeAlpha > 0.01f) {
        panelStatTimer.start();
        if (panelActive)
            rows = orderedPanelRows();

        const PlateGeometry geo = labelPanelPlateGeometry(labelRect, W, H, rows.size());
        plate = geo.plate;
        const bool withRows = panelActive && rows.size() > 1;

        // 2026-09-12: 描いた結果を画像に取っておき、内容が変わっていない
        // フレームはそれを貼るだけにする。実測でこの板＋行の描画だけで
        // 4〜5ms/フレーム（開いた直後は最大410ms）かかっていた。ガラスの
        // 縁はグラデーションのペンで輪郭を何十本も重ねるため、面積ではなく
        // ストローク数で高くつく。
        //
        // 見た目が変わらない根拠: このオーバーレイは半透明ウィジェットで、
        // 背景のぼかしはGL側の別レイヤーが受け持つ。つまりガラスの加算・
        // 乗算合成はいずれも「透明な下地」に対して行われており、透明な
        // 画像へ描いてから貼るのと完全に同じ結果になる。
        const PanelCacheKeyParts parts = labelPanelCacheParts(rows, geo, withRows);
        const quint64 key = hashPanelCacheParts(parts);
        const qreal dpr = painter.device()
            ? painter.device()->devicePixelRatioF() : 1.0;
        const QRectF cacheRect = plate.adjusted(-12.0, -12.0, 12.0, 12.0);

        // ---- 板と行を別々のキャッシュにする（2026-09-14 B-2）------------
        //
        // 【なぜ】スクロール中の実測（[LabelPanel][stat]）:
        //     build=6.82ms why=scroll | plate=4.98ms rows=1.78ms
        // 板が74%を占めているのに、why= は scroll だけ。つまり板の形も
        // アクセント色も何ひとつ変わっていないのに、行が動いただけで
        // ガラスの板を丸ごと焼き直していた。
        //
        // 板のキーは「形・角丸・アクセント色・dpr」だけにする。スクロール量や
        // 行の内容は入れない。行は従来どおりの全キーで別画像へ焼き、板の上へ
        // 重ねる。
        //
        // 重ねても結果が変わらない根拠: paintLabelPanelRows() は行を自前の
        // 透明な QImage へ描き、端をぼかしてから drawImage で貼っている。
        // それ以外も setOpacity + SourceOver だけで、板の画素を読む合成
        // （Plus / Multiply / DestinationIn）は板側にしか無い。
        const quint64 plateKey = hashPlateCacheParts(parts);
        const QSize px((qMax(1.0, cacheRect.width())  * dpr) + 1,
                       (qMax(1.0, cacheRect.height()) * dpr) + 1);
        const bool dprSame = qFuzzyCompare(m_panelCacheDpr + 1.0, dpr + 1.0);

        // 焼くときは fade=1.0（濃さは貼るときに掛ける。B-3）。板と行の
        // どちらも内部で m_labelFadeAlpha を見るので、まとめて差し替える。
        const float bakedFade = m_labelFadeAlpha;

        // ---- 板 ----
        // 2026-09-16: GL版では板はぼかしと一緒にGL側で描くので、画像へ焼かない
        // （伸び縮みの間はここが毎フレーム外れて焼き直しになっていた）。
        const bool glPlate = blurRegions && videoGlassOnGpu();
        if (glPlate) {
            if (!m_panelPlateCache.isNull())
                m_panelPlateCache = QImage();   // 使わないのでメモリを返す
        } else if (plateKey == m_panelPlateKey && !m_panelPlateCache.isNull()
            && dprSame && m_panelPlateCache.size() == px) {
            ++m_panelStatPlateHits;
        } else {
            ++m_panelStatPlateMisses;
            QElapsedTimer t;
            t.start();
            QImage img(px, QImage::Format_ARGB32_Premultiplied);
            img.setDevicePixelRatio(dpr);
            img.fill(Qt::transparent);
            {
                m_labelFadeAlpha = 1.0f;
                QPainter cp(&img);
                cp.setRenderHint(QPainter::Antialiasing, true);
                cp.setRenderHint(QPainter::SmoothPixmapTransform, true);
                cp.translate(-cacheRect.topLeft());
                paintLabelPanelPlate(cp, geo, m_labelFadeAlpha);
                cp.end();
                m_labelFadeAlpha = bakedFade;
            }
            m_panelPlateCache = img;
            m_panelPlateKey   = plateKey;
            m_panelStatPlateMs += t.nsecsElapsed() / 1e6;
        }

        // ---- 行 ----
        // 2026-09-14 B-4: 帯キャッシュが使える条件なら、そちらを使う。
        // 使えないのは (a) 並べ替えドラッグ中（並び自体が毎フレーム変わる）
        // (b) 伸び縮みの最中（reveal<1。濃さを後から掛けると影の付き方が
        // わずかに変わるため、この間は従来どおり焼く）。
        {
            const double reveal = qBound(
                0.0, (static_cast<double>(m_labelPanelAnim) - 0.55) / 0.45, 1.0);
            const bool rowDragging = (m_rowDragIndex >= 0 && m_rowDragMoved
                                      && m_rowDragIndex < rows.size());
            m_panelUseStrip = withRows && !rowDragging && reveal >= 0.999;
            if (m_panelUseStrip) {
                QElapsedTimer t;
                t.start();
                m_panelUseStrip =
                    ensureLabelPanelStrip(plate, labelRect, rows, parts, dpr);
                m_panelStatStripMs += t.nsecsElapsed() / 1e6;
            }
        }
        if (m_panelUseStrip) {
            // 旧経路の行画像は貼らない。ヒット判定の矩形は貼るときに作る。
            m_panelCacheRowsDrawn = false;
            // 2026-09-23: 旧経路の画像のキーも捨てる。残しておくと、同じ行を
            // もう一度掴んだ最初のフレーム（前回のドラッグ開始と同じ並び・
            // 隙間・スクロール）でキーが一致してキャッシュが当たり、その一方で
            // 上の「貼らない」印が残ったままになる → 掴んだ行以外が全部
            // 消えて見えていた（隙間が動いてキーが変わるまで）。
            m_panelCacheKey = 0;
        } else if (key == m_panelCacheKey && !m_panelCache.isNull()
            && dprSame && m_panelCache.size() == px) {
            panelCacheHit = true;
            ++m_panelStatHits;
            // 当たった画像は同じキー（＝同じ withRows）で焼いたもの。
            // 貼るかどうかの印もそれに合わせる（念のため）。
            m_panelCacheRowsDrawn = withRows;
        } else {
            // 2026-09-14: 外れた理由を記録する（describePanelCacheMiss）。
            ++m_panelStatMisses;
            if (!m_panelCache.isNull())
                m_panelStatMissMask |= diffPanelCacheParts(m_panelKeyParts, parts);
            m_panelKeyParts = parts;
            QElapsedTimer t;
            t.start();
            QImage img(px, QImage::Format_ARGB32_Premultiplied);
            img.setDevicePixelRatio(dpr);
            img.fill(Qt::transparent);
            if (withRows) {
                m_labelFadeAlpha = 1.0f;
                QPainter cp(&img);
                cp.setRenderHint(QPainter::Antialiasing, true);
                cp.setRenderHint(QPainter::TextAntialiasing, true);
                cp.setRenderHint(QPainter::SmoothPixmapTransform, true);
                cp.translate(-cacheRect.topLeft());
                paintLabelPanelRows(cp, plate, labelRect, rows);
                cp.end();
                m_labelFadeAlpha = bakedFade;
            }
            m_panelCache       = img;
            m_panelCacheKey    = key;
            m_panelCacheRowsDrawn = withRows;
            // ヒット判定用の矩形は描いたときの値を覚えておき、キャッシュを
            // 貼るフレームでも同じものを使う。
            m_panelCacheRowRects   = m_labelPanelRowRects;
            m_panelCacheRowUrls    = m_labelPanelRowUrls;
            m_panelCacheRowIndices = m_labelPanelRowIndices;
            m_panelCacheNowRowRect = m_labelPanelNowRowRect;
            m_panelStatRowsMs += t.nsecsElapsed() / 1e6;
        }
        m_panelCacheDpr    = dpr;
        m_panelCacheOrigin = cacheRect.topLeft();
        m_panelStatBuildMs = m_panelStatPlateMs + m_panelStatRowsMs;

        // 焼いた画像は fade=1.0 の絵なので、ここで濃さを掛ける（B-3）。
        // 板 → 行 の順に重ねる。
        {
            const qreal savedOpacity = painter.opacity();
            painter.setOpacity(savedOpacity * static_cast<double>(m_labelFadeAlpha));
            if (!glPlate)
                painter.drawImage(m_panelCacheOrigin, m_panelPlateCache);
            if (m_panelUseStrip)
                blitLabelPanelStrip(painter, plate, labelRect, rows);
            else if (m_panelCacheRowsDrawn)
                painter.drawImage(m_panelCacheOrigin, m_panelCache);
            painter.setOpacity(savedOpacity);
        }

        if (blurRegions) {
            // 【2026-09-12】引っ張り(m_labelPull)はpainterのtranslateで効かせて
            // いるが、ぼかしの領域はウィジェット座標で直接渡すため変換が
            // 効かない。ここで同じ分だけ下げておかないと、板だけ下がって
            // ぼかしが元の位置に取り残される。
            blurRegions->add(plate.translated(0.0, m_labelPull),
                             (geo.leftRadius + geo.rightRadius) * 0.5,
                             BackdropBlur::kDefaultUiBlurRadius,
                             painter.opacity() * m_labelFadeAlpha,
                             glPlate ? videoGlassStyle(cachedAccentColor())
                                     : BackdropBlur::GlassPlateStyle());
        }
    }

    // 2026-09-12: リストが開くとき、テキスト（＋鍵マーク）は板の右端へ
    // ついていき、空いた中央に簡易コントロールを置く。ラベルは島の下位に
    // あって「島に触れると畳む」関係なので、右上にボタンを置くとウィンドウ
    // 幅によって島と当たってしまうため（中央なら当たらない）。
    // 板の右端は伸び縮みに合わせて動くので、その差分をそのまま渡せば
    // アニメーションにも自然に追従する。
    const double labelTextShift = (!plate.isEmpty() && !labelRect.isEmpty())
        ? qMax(0.0, plate.right() - labelRect.right()) : 0.0;
    // 2026-09-23: 長押しでためている間、ラベルの文字が入れ替わっていく
    // （Solo → List / Mix …）。途中でやめればゲージと一緒に戻る。
    const double labelMorph =
        (!m_labelHoldTargetText.isEmpty() && m_labelHoldTargetText != labelText)
            ? qBound(0.0, m_labelHold, 1.0) : 0.0;
    const QRectF labelPill = m_labelPainter.paint(painter, W, H, m_labelFadeAlpha,
                                                  labelIconSvg, labelText,
                                                  /*drawPill=*/!wantPlate,
                                                  labelTextShift, labelCompact,
                                                  m_labelHoldTargetText, labelMorph);
    if (blurRegions && !labelPill.isEmpty()) {
        // 板と同じ理由で、引っ張っている分だけ下げる（上のコメント参照）。
        blurRegions->add(labelPill.translated(0.0, m_labelPull),
                         labelPill.height() * 0.5,
                         BackdropBlur::kDefaultUiBlurRadius,
                         painter.opacity() * m_labelFadeAlpha);
    }

    // ---- 台の縁に沿った再生位置（2026-09-13追加）----
    // 出すかどうかはプラグインが決める（PbResolvedMedia::labelProgress）。
    // 輪郭は「いま実際に描いた台」のもの（閉じていればカプセル、開いて
    // いればカード）。輪郭そのものを辿るので、伸び縮みの最中でも破綻しない。
    // 板のキャッシュの外に描く（中身が毎秒変わるため。イコライザと同じ扱い）。
    if (m_labelProgressEffective && m_labelFadeAlpha > 0.01f) {
        const QRectF outline = !plate.isEmpty() ? plate : labelPill;
        const double pillH = !labelRect.isEmpty() ? labelRect.height()
                                                  : outline.height();
        if (!outline.isEmpty() && pillH > 1.0 && m_labelProgressFade > 0.01) {
            // 位置は m_labelProgressShown（尺が分からない間は保持した値）、
            // 濃さにはフェードを掛ける。曲の変わり目で消えて出直す。
            paintLabelProgressRing(painter, outline,
                                   CanvasVideoLabelPainter::pillLeftRadius(pillH),
                                   pillH * 0.5, m_labelProgressShown,
                                   static_cast<double>(m_labelFadeAlpha)
                                       * m_labelProgressFade);
        }
    }

    // ---- 長押しのゲージ（2026-09-23追加）----
    // 再生位置の光と同じく台の縁を辿る。こちらは「たまった分」を線で残す
    // （時計の針が塗り残していくように、上の中央から時計回り）。
    if ((m_labelHold > 0.002 || m_labelHoldHintAlpha > 0.01 || m_labelHoldTrack > 0.01)
        && m_labelFadeAlpha > 0.01f) {
        const QRectF outline = !plate.isEmpty() ? plate : labelPill;
        const double pillH = !labelRect.isEmpty() ? labelRect.height()
                                                  : outline.height();
        if (!outline.isEmpty() && pillH > 1.0
            && (m_labelHold > 0.002 || m_labelHoldTrack > 0.01)) {
            double alpha = static_cast<double>(m_labelFadeAlpha);
            if (m_labelHoldPhase == LabelHoldPhase::Waiting)   // 結果待ちは息をする
                alpha *= 0.72 + 0.28 * std::cos(m_labelHoldWaitMs / 1000.0 * 2.0 * M_PI * 1.2);
            if (m_labelHoldPhase == LabelHoldPhase::Done)      // 成功は光って消える
                alpha *= m_labelHoldFlash;
            paintLabelHoldGauge(painter, outline,
                                CanvasVideoLabelPainter::pillLeftRadius(pillH),
                                pillH * 0.5, m_labelHold, alpha, m_labelHoldTrack);
        }
        // 何が起きるかをラベルの下へ小さく出す（言葉はプラグイン／本体が決める）
        if (m_labelHoldHintAlpha > 0.01 && !m_labelHoldHintShown.isEmpty()
            && !labelPill.isEmpty()) {
            painter.save();
            QFont f = painter.font();
            f.setPixelSize(11);
            f.setWeight(QFont::DemiBold);
            painter.setFont(f);
            const QFontMetricsF fm(f);
            const double tw = fm.horizontalAdvance(m_labelHoldHintShown);
            const QRectF tr(labelPill.left() + 6.0, labelPill.bottom() + 5.0,
                            tw + 2.0, fm.height());
            const double a = m_labelHoldHintAlpha * m_labelFadeAlpha;
            painter.setPen(QColor(0, 0, 0, qBound(0, qRound(110 * a), 255)));
            painter.drawText(tr.translated(0.0, 1.0), Qt::AlignLeft | Qt::AlignVCenter,
                             m_labelHoldHintShown);
            painter.setPen(QColor(255, 255, 255, qBound(0, qRound(225 * a), 255)));
            painter.drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, m_labelHoldHintShown);
            painter.restore();
        }
    }

    // ---- ラベルのホバー反応（2026-09-12追加）----
    // プラグインが labelHoverable=true と言ったラベルだけ、カーソルを
    // 重ねている間ほんのり明るくする（＝押せることの合図）。押しても何も
    // 起きないラベルには何も出さない。反応の中身（薄い白を重ねる）は本体の
    // 裁量で、プラグインからは指定できない。
    // 開いている最中〜開いた後は既に「押した結果」が出ているので薄れさせる。
    if (m_labelHoverAnim > 0.01f && m_labelFadeAlpha > 0.01f
        && !m_labelPainter.lastPaintedRect().isEmpty()) {
        // 2026-09-12: 伸び縮みの最中は、ホバーの反応も板と同じように動かす。
        //
        // 開くとき … 台と同じ中心から**拡大しながら**フェードして消える
        //            （ただの平行移動ではなく、拡大変換で台の動きに合わせる）
        // 畳むとき … 途中では出さず、**閉じ切ってから**出す
        //
        // 両方を1本のカーブで済ませている。透明度を (1 - t)^3 にすると、
        // 開き始めで素早く消え、畳むときは t がほぼ0になるまで出てこない。
        // t = 板の伸び具合（m_labelPanelAnim）。
        const QRectF hoverRect = m_labelPainter.lastPaintedRect();
        const double t = qBound(0.0, static_cast<double>(m_labelPanelAnim), 1.0);
        const double fade = (1.0 - t) * (1.0 - t) * (1.0 - t);
        const double amount = m_labelHoverAnim
                              * static_cast<double>(m_labelFadeAlpha) * fade;
        if (amount > 0.005) {
            // 拡大率。1.0（等倍）から、板が開き切るまでにこの値まで伸びる。
            constexpr double kHoverExpand = 0.5;
            const double scale = 1.0 + kHoverExpand * t;
            const QPointF c = hoverRect.center();
            painter.save();
            painter.translate(c);
            painter.scale(scale, scale);
            painter.translate(-c);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, qRound(30 * amount)));
            const double r = hoverRect.height() * 0.5;
            painter.drawRoundedRect(hoverRect, r, r);
            painter.restore();
        }
    }

    if (!plate.isEmpty() && panelActive && rows.size() > 1) {
        // 行はキャッシュ画像の中に描き込み済み。ヒット判定用の矩形だけ
        // 復元する（キャッシュを作ったときの値。中身が同じなので同じ位置）。
        if (panelCacheHit) {
            m_labelPanelRowRects   = m_panelCacheRowRects;
            m_labelPanelRowUrls    = m_panelCacheRowUrls;
            m_labelPanelRowIndices = m_panelCacheRowIndices;
            m_labelPanelNowRowRect = m_panelCacheNowRowRect;
        }
        m_labelPanelRect = plate;
        m_labelPanelHeaderHeight = labelRect.height();
    }

    // 計測（メンバのコメント参照）。板を描いたフレームだけ数える。
    if (wantPlate && panelStatTimer.isValid()) {
        const double ms = panelStatTimer.nsecsElapsed() / 1e6;
        ++m_panelStatFrames;
        m_panelStatMsSum += ms;
        m_panelStatMsMax = qMax(m_panelStatMsMax, ms);
        m_panelStatRows = rows.size();
        m_panelStatPlateSize = plate.size();
        if (!m_panelStatWindow.isValid())
            m_panelStatWindow.start();
        if (m_panelStatWindow.elapsed() >= 2000) {
            qDebug().noquote() << QStringLiteral(
                "[LabelPanel][stat] v=d2 frames=%1 paint=%2ms(max %3) "
                "rows=%4 plate=%5x%6 anim=%7 rowHit=%8 rowMiss=%9 build=%10ms why=%11"
                " | plate=%12ms rows=%13ms plateHit=%14 plateMiss=%15 scrollTick=%16"
                " | stripHit=%17 stripMiss=%18 strip=%19ms win=%20+%21"
                " slide=%22 redrawRows=%23 blit=%24ms")
                .arg(m_panelStatFrames)
                .arg(m_panelStatMsSum / m_panelStatFrames, 0, 'f', 2)
                .arg(m_panelStatMsMax, 0, 'f', 2)
                .arg(m_panelStatRows)
                .arg(m_panelStatPlateSize.width(), 0, 'f', 0)
                .arg(m_panelStatPlateSize.height(), 0, 'f', 0)
                .arg(static_cast<double>(m_labelPanelAnim), 0, 'f', 2)
                .arg(m_panelStatHits)
                .arg(m_panelStatMisses)
                .arg(m_panelStatMisses > 0
                         ? m_panelStatBuildMs / m_panelStatMisses : 0.0, 0, 'f', 2)
                .arg(describePanelCacheMiss(m_panelStatMissMask))
                .arg(m_panelStatPlateMisses > 0
                         ? m_panelStatPlateMs / m_panelStatPlateMisses : 0.0, 0, 'f', 2)
                .arg(m_panelStatMisses > 0
                         ? m_panelStatRowsMs / m_panelStatMisses : 0.0, 0, 'f', 2)
                .arg(m_panelStatPlateHits)
                .arg(m_panelStatPlateMisses)
                .arg(m_panelStatScrollTicks)
                .arg(m_panelStatStripHits)
                .arg(m_panelStatStripMisses)
                .arg(m_panelStatStripMisses > 0
                         ? m_panelStatStripMs / m_panelStatStripMisses : 0.0, 0, 'f', 2)
                .arg(m_panelStripFirst)
                .arg(m_panelStripCount)
                .arg(m_panelStatStripSlides)
                .arg(m_panelStatStripRedrawRows)
                .arg(m_panelStatBlitCalls > 0
                         ? m_panelStatBlitMs / m_panelStatBlitCalls : 0.0, 0, 'f', 3);
            m_panelStatWindow.restart();
            m_panelStatMsSum = m_panelStatMsMax = 0.0;
            m_panelStatFrames = 0;
            m_panelStatHits = m_panelStatMisses = 0;
            m_panelStatBuildMs = 0.0;
            m_panelStatPlateMs = m_panelStatRowsMs = 0.0;
            m_panelStatPlateHits = m_panelStatPlateMisses = 0;
            m_panelStatMissMask = 0;
            m_panelStatScrollTicks = 0;
            m_panelStatStripHits = m_panelStatStripMisses = 0;
            m_panelStatStripMs = 0.0;
            m_panelStatStripSlides = m_panelStatStripRedrawRows = 0;
            m_panelStatBlitMs = 0.0;
            m_panelStatBlitCalls = 0;
        }
    }
    // ---- 再生中の行の簡易イコライザ＋音符（キャッシュ画像の外に重ねる）----
    // 毎フレーム動くので、板のキャッシュ（4〜5ms）を毎回捨てないよう
    // ここで上から描く。音とは無関係の、それっぽく揺れるだけの飾り。
    if (!m_labelPanelNowRowRect.isEmpty() && panelActive
        && !m_labelPanelViewRect.isEmpty() && m_labelFadeAlpha > 0.01f) {
        const double reveal =
            qBound(0.0, (static_cast<double>(m_labelPanelAnim) - 0.55) / 0.45, 1.0);
        if (reveal > 0.01) {
            const double eqAlpha = reveal * static_cast<double>(m_labelFadeAlpha);
            // 棒はリストの中だけ。
            painter.save();
            painter.setClipRect(m_labelPanelViewRect);
            paintNowPlayingEqualizer(painter, m_labelPanelNowRowRect, eqAlpha,
                                     /*drawBars=*/true, /*drawNotes=*/false);
            painter.restore();
            // 音符は板の外へ飛んでいってよい（クリップしない）。行が
            // スクロールでリストの外へ出ているときだけ止める。
            if (m_labelPanelNowRowRect.top() >= m_labelPanelViewRect.top() - 2.0
                && m_labelPanelNowRowRect.bottom() <= m_labelPanelViewRect.bottom() + 2.0) {
                paintNowPlayingEqualizer(painter, m_labelPanelNowRowRect, eqAlpha,
                                         /*drawBars=*/false, /*drawNotes=*/true);
            }
        }
    }

    // ---- 並べ替え中: つかんだ行をカーソルに追従させて描く ----
    // 見た目そのまま（サムネイル・タイトル・ピン）を持ち上げるので、
    // いま何をつかんでいるか見失わない。抜けた場所には隙間が空いていて、
    // そこへ落ちる（隙間はリスト本体側で描いている）。
    if (m_rowDragIndex >= 0 && m_rowDragMoved && !m_labelPanelViewRect.isEmpty()) {
        const QList<PanelRow> rowsNow = orderedPanelRows();
        if (m_rowDragIndex < rowsNow.size()) {
            const PanelRow &row = rowsNow.at(m_rowDragIndex);
            const double reveal =
                qBound(0.0, (static_cast<double>(m_labelPanelAnim) - 0.55) / 0.45, 1.0);
            const double a = reveal * static_cast<double>(m_labelFadeAlpha);
            const QRectF viewRect = m_labelPanelViewRect;
            double top = m_rowDragY - m_rowDragGrabDy;
            // リストの外へは出さない（少しはみ出す程度は許す）。
            top = qBound(viewRect.top() - kPanelRowHeight * 0.35, top,
                         viewRect.bottom() - kPanelRowHeight * 0.65);
            const QRectF floatRect(viewRect.left(), top,
                                   viewRect.width(), kPanelRowHeight);
            painter.save();
            painter.setClipRect(viewRect.adjusted(-6.0, -6.0, 6.0, 6.0));
            // 2026-09-23: 右へ引っ張ると、板の上にいる間は行が板の右の縁へ
            // 押し付けられて横にぐにっと潰れる（横に縮み、そのぶん縦に少し
            // 膨らむ）。「リストから出たくない」と最後まで踏ん張る感じ。
            // 引くほど強くなるが上限がある（ゴムの手応え）。板の外へ出た
            // 瞬間に CanvasWidget がバブルとして取り出す。
            const double pull = rowPullAmount();
            if (pull > 0.0) {
                const double squash = 1.0 - std::exp(-pull / 70.0);   // 0〜1
                const double sx = 1.0 - 0.48 * squash;   // 横に潰れる
                const double sy = 1.0 + 0.14 * squash;   // 潰れたぶん縦に膨らむ
                // 右の縁を支点にする（右の壁に押し付けられている）
                const QPointF anchor(floatRect.right(), floatRect.center().y());
                painter.setClipRect(viewRect.adjusted(-6.0, -12.0, 6.0, 12.0));
                painter.translate(anchor);
                painter.scale(sx, sy);
                painter.translate(-anchor);
            }
            // 持ち上がっている感じを出すため、薄い影と下地を敷く。
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, qRound(60 * a)));
            painter.drawRoundedRect(floatRect.adjusted(-3.0, 1.0, 5.0, 3.0), 11, 11);
            painter.setBrush(QColor(255, 255, 255, qRound(30 * a)));
            painter.drawRoundedRect(floatRect.adjusted(-4.0, 0.0, 4.0, 0.0), 10, 10);
            paintPanelRowContent(painter, floatRect, row,
                                 row.kind == QStringLiteral("NOW"), a,
                                 /*requestThumbs=*/false);
            painter.restore();
        }
    }

    // ---- ヘッダの簡易コントロール（◀◀ / ▶ ・|| / ▶▶）----
    // リストが開いている間だけ、「List」の右側に置く。行と同じタイミングで
    // 遅れて出す（板が伸び切る前に出るとはみ出して見えるため）。
    if (!plate.isEmpty() && panelActive && m_labelFadeAlpha > 0.01f) {
        const double reveal =
            qBound(0.0, (static_cast<double>(m_labelPanelAnim) - 0.55) / 0.45, 1.0);
        if (reveal > 0.01) {
            const QRectF headerRect(plate.left(), plate.top(),
                                    plate.width(), labelRect.height());
            paintLabelHeaderButtons(painter, headerRect,
                                    reveal * static_cast<double>(m_labelFadeAlpha));
        }
    }

    painter.restore();

    // ヒット判定用の矩形は「実際に描かれた位置」に合わせる（引っ張り中は
    // その分だけ下にある）。
    if (m_labelPull > 0.01) {
        if (!m_labelPanelRect.isEmpty())
            m_labelPanelRect = m_labelPanelRect.translated(0.0, m_labelPull);
        for (QRectF &r : m_labelPanelRowRects)
            r = r.translated(0.0, m_labelPull);
        for (QRectF *r : {&m_labelBtnPrev, &m_labelBtnPlay, &m_labelBtnNext}) {
            if (!r->isEmpty())
                *r = r->translated(0.0, m_labelPull);
        }
    }

    // 鍵マーク。固定中は出しっぱなし、引っ張っている最中はしきい値を
    // 超えたところで現れて「離せば固定される」ことを示す。
    {
        // 鍵マークもテキストと一緒に右へついていく（テキストと同じずらし量）。
        const QRectF badgeAnchor =
            m_labelPainter.lastPaintedRect().translated(labelTextShift, 0.0);
        if (!badgeAnchor.isEmpty() && m_labelFadeAlpha > 0.01f) {
            double amount = m_labelPinned ? 1.0 : 0.0;
            if (m_labelPullActive) {
                const double t = qBound(0.0,
                    (m_labelPull - kLabelPullThreshold) / 10.0, 1.0);
                amount = m_labelPinned ? (1.0 - t) : t; // 引き切ると反転する
            }
            if (amount > 0.01) {
                paintLabelLockBadge(painter,
                                    badgeAnchor.translated(0.0, m_labelPull),
                                    amount * static_cast<double>(m_labelFadeAlpha));
            }
        }
    }
}


// ラベル周りのアニメーションだけを進める。再生バーとは切り離してある
// （固定ラベルは非アクティブなバブルでも動き続ける必要があるため）。
void CanvasVideo::updateLabelAnims(double dtSec)
{
    // ---- ラベル→コントローラの伸び縮み（2026-09-12追加）----
    // 開くときは少しゆっくり、閉じるときは速く。
    {
        // 2026-09-12: 開きを少しゆっくりに（9.0 → 6.5、時定数 ≒ 110→155ms）。
        // 島と入れ替わるときに開き始めが早すぎて忙しなく見えたため。
        const bool expand = labelPanelExpandTarget();
        const float panelTarget = expand ? 1.0f : 0.0f;
        const float panelSpeed  = expand ? 6.5f : 14.0f;
        m_labelPanelAnim += (panelTarget - m_labelPanelAnim)
                            * qMin(1.0f, panelSpeed * static_cast<float>(dtSec));
        m_labelPanelAnim = qBound(0.0f, m_labelPanelAnim, 1.0f);
        if (!expand && m_labelPanelAnim < 0.002f)
            m_labelPanelAnim = 0.0f;
    }

    // ---- 島とぶつかるときの最小化（2026-09-13追加）----
    // 縮むのは「島が来た」＝待たせたくない場面なので速く、戻るのは
    // リストの開閉と同じ速さにして、入れ替わりが1つの動きに見えるようにする。
    {
        const float target = m_labelCompact ? 1.0f : 0.0f;
        const float speed  = m_labelCompact ? 14.0f : 6.5f;
        m_labelCompactAnim += (target - m_labelCompactAnim)
                              * qMin(1.0f, speed * static_cast<float>(dtSec));
        m_labelCompactAnim = qBound(0.0f, m_labelCompactAnim, 1.0f);
        if (std::abs(m_labelCompactAnim - target) < 0.002f)
            m_labelCompactAnim = target;
    }

    // ---- 固定ラベルの出入り（2026-09-12追加）----
    // 別の動画を全画面にした等で固定ラベルが引っ込むとき、パッと消えずに
    // フェードさせる（固定していないラベルが非アクティブで消えるときと
    // 同じ見え方に揃える）。出るときも同じ速さで戻る。
    {
        const float target = m_pinnedOverlayVisible ? 1.0f : 0.0f;
        constexpr float kPinnedFadeSpeed = 11.0f; // 時定数 ≒ 90ms
        m_pinnedOverlayFade += (target - m_pinnedOverlayFade)
                               * qMin(1.0f, kPinnedFadeSpeed * static_cast<float>(dtSec));
        m_pinnedOverlayFade = qBound(0.0f, m_pinnedOverlayFade, 1.0f);
        if (std::abs(m_pinnedOverlayFade - target) < 0.002f)
            m_pinnedOverlayFade = target;
    }

    // ---- 台の縁の再生位置の出入り（2026-09-13追加）----
    // 曲が変わるとき、尺が分からない時間（解決中）が必ず挟まる。そこで
    // ただ描くのをやめるとパッと消えて、次の曲でパッと現れる。一度
    // フェードアウトさせてから、新しい曲の頭でフェードインさせる。
    {
        constexpr double kProgressFadeSpeed = 5.5; // 時定数 ≒ 180ms
        const qint64 dur = duration();
        const double now = (dur > 0)
            ? qBound(0.0, static_cast<double>(position()) / dur, 1.0) : -1.0;
        if (now >= 0.0) {
            // 大きく巻き戻った＝別の曲になった。尺が途切れずに切り替わった
            // 場合でも、頭が一周ぶん飛んで見えないよう仕切り直す。
            if (now + 0.2 < m_labelProgressShown)
                m_labelProgressFade = 0.0;
            m_labelProgressShown = now;
        }
        // 【終わりぎわは自分から消える】曲の変わり目は「尺が分からなくなる
        // →フェードアウト」に任せていたが、解決が速いとその空白がほとんど
        // 無く、頭が一周し切った瞬間にパッと消えて見えていた。残り時間で
        // 先に薄くしていき、頂点へ帰り着くのと同時に消え切るようにする。
        constexpr double kProgressEndFadeMs = 1200.0;
        double endFade = 1.0;
        if (now >= 0.0 && dur > 0) {
            const double remainMs = static_cast<double>(dur - position());
            endFade = qBound(0.0, remainMs / kProgressEndFadeMs, 1.0);
        }
        const double target =
            (m_labelProgressEffective && now >= 0.0) ? endFade : 0.0;
        m_labelProgressFade += (target - m_labelProgressFade)
                               * qMin(1.0, kProgressFadeSpeed * dtSec);
        m_labelProgressFade = qBound(0.0, m_labelProgressFade, 1.0);
        if (std::abs(m_labelProgressFade - target) < 0.002)
            m_labelProgressFade = target;
    }

    // ---- ラベルのホバー（2026-09-12追加）----
    // プラグインが許可したラベルのときだけ立ち上がる。許可が無い／
    // カーソルが外れたら0へ戻る。
    {
        // 2026-09-12: 引っ張っている最中は、カーソルがラベルから外れても
        // ホバーを保つ。掴んでいる以上「触っている」のが事実なので、
        // 下げている途中に反応が消えると掴みが外れたように見える。
        const float hoverTarget =
            ((m_labelHover || m_labelPullActive) && m_labelHoverableEffective)
                ? 1.0f : 0.0f;
        const float hoverSpeed = 14.0f;
        m_labelHoverAnim += (hoverTarget - m_labelHoverAnim)
                            * qMin(1.0f, hoverSpeed * static_cast<float>(dtSec));
        m_labelHoverAnim = qBound(0.0f, m_labelHoverAnim, 1.0f);
    }

    // ---- リストのスクロール ----
    // 2026-09-14 PhaseC-1: スクロールだけは 120Hz の syncLogic() から
    // tickLabelScroll() で進める（イコライザ30Hz／その他60Hz／
    // スクロール120Hz の三分割）。ここでは進めない。


    // ---- イコライザ／音符の位相と「鳴っている度合い」----
    m_eqPhase += dtSec;
    if (m_eqPhase > 1.0e6)
        m_eqPhase = 0.0;
    {
        // 止めた瞬間にカクッと消えないよう、0/1へなだらかに寄せる
        // （時定数 ≒ 280ms）。棒の高さと音符の濃さに掛ける。
        const double target = isSoundingForEqualizer() ? 1.0 : 0.0;
        const double diff = target - m_eqLevel;
        if (std::abs(diff) < 0.003)
            m_eqLevel = target;
        else
            m_eqLevel += diff * (1.0 - std::exp(-3.6 * dtSec));
    }

    // ---- 並べ替え中、まわりの行が退く動き ----
    // 落とし先が変わった瞬間にパッと入れ替わらないよう、隙間の位置を
    // 連続値で補間する（時定数 ≒ 90ms）。
    if (m_rowDragIndex >= 0) {
        const double target = rowGapSlotTarget();
        const double diff = target - m_rowGapAnim;
        // 0.02行 ≒ 1px 未満になったら詰める（それ以上は目に見えない）。
        if (std::abs(diff) < 0.02)
            m_rowGapAnim = target;
        else
            m_rowGapAnim += diff * (1.0 - std::exp(-9.0 * dtSec));
    }

    // ---- 引っ張り（固定操作）----
    // 指を離したら target=0 なので、そのままバネで元へ戻る。
    {
        const double diff = m_labelPullTarget - m_labelPull;
        if (std::abs(diff) < 0.15)
            m_labelPull = m_labelPullTarget;
        else
            m_labelPull += diff * (1.0 - std::exp(-20.0 * dtSec));
    }
    tickLabelHold(dtSec);
}

void CanvasVideo::tickLabelAnims(double dtMs)
{
    updateLabelAnims(qMax(0.0, dtMs) / 1000.0);
}

// ---- リストのスクロールをなめらかに追従させる（120Hz）----
// ホイールは m_labelPanelScrollTarget だけを動かし、描画に使う
// m_labelPanelScroll をここで指数的に寄せる。dtに対して
// 1 - exp(-speed*dt) を使うので、フレームレートが変わっても
// 同じ速さで見える（*speed*dt の線形近似だとフレーム落ち時に鈍る）。
//
// 2026-09-14 PhaseC-1: updateLabelAnims()（60Hz）から切り出した。
// スクロールは指で送っている間ずっと目で追う動きなので、ここだけ
// カメラと同じ 120Hz で進める。渡す dtSec は「前回この関数を呼んで
// からの実経過」なので、速さ・到達時間は 60Hz のときと変わらない。
// 戻り値は「まだ動いているか」＝このフレームで再描画が要るか。
bool CanvasVideo::tickLabelScroll(double dtSec)
{
    if (m_labelPanelAnim <= 0.001f) {
        // 畳んでいる間はアニメーションさせない（開いた瞬間に
        // 残りぶんが流れ出すのを防ぐ）。
        m_labelPanelScroll = m_labelPanelScrollTarget;
        return false;
    }
    const double diff = m_labelPanelScrollTarget - m_labelPanelScroll;
    if (std::abs(diff) < 0.25) {
        m_labelPanelScroll = m_labelPanelScrollTarget; // 0.5px未満は詰める
        return false;
    }
    const double kScrollSpeed = 9.5; // 時定数 ≒ 105ms
    m_labelPanelScroll += diff * (1.0 - std::exp(-kScrollSpeed * dtSec));
    ++m_panelStatScrollTicks;
    return true;
}

// リストを開いて再生中は、イコライザが動き続けるので常にフレームが要る。
// 2026-09-12: これだけが理由でフレームが要る場合は、オーバーレイ全体では
// なくイコライザの矩形だけを描き直せばよいので、判定を分けてある。
bool CanvasVideo::equalizerNeedsFrames() const
{
    return m_labelPanelAnim > 0.5f && m_labelPanelOpen
        && (isSoundingForEqualizer() || m_eqLevel > 0.01);
}

bool CanvasVideo::labelAnimsNeedFrames() const
{
    return equalizerNeedsFrames() || labelAnimsNeedFramesExceptEqualizer();
}

// イコライザ以外のラベル関連アニメーション（伸び縮み・ホバー・スクロール・
// 引っ張り・並べ替えの隙間）。こちらが動いている間はオーバーレイ全体の
// 再描画が要る。
bool CanvasVideo::labelAnimsNeedFramesExceptEqualizer() const
{
    const float panelTarget = labelPanelExpandTarget() ? 1.0f : 0.0f;
    // 【注意】ホバーの行き先は updateLabelAnims() と必ず同じ式にすること。
    // ずれると「まだ動いているのにフレームを要求しない」＝途中で固まる。
    const float hoverTarget =
        ((m_labelHover || m_labelPullActive) && m_labelHoverableEffective)
            ? 1.0f : 0.0f;
    return std::abs(m_labelPanelAnim - panelTarget) > 0.001f
        || std::abs(m_labelCompactAnim - (m_labelCompact ? 1.0f : 0.0f)) > 0.001f
        || std::abs(m_labelHoverAnim - hoverTarget) > 0.001f
        // 固定ラベルの出入りフェード
        || std::abs(m_pinnedOverlayFade - (m_pinnedOverlayVisible ? 1.0f : 0.0f)) > 0.001f
        // 2026-09-14 C-2: スクロールはここに含めない。スクロールだけが
        // 動いているフレームは CanvasWidget::syncLogic() が
        // labelPanelDamageRect() の範囲だけを描き直す（オーバーレイ全体を
        // 描き直すと全画面で3ms前後かかるため）。ここに残すと60Hz側が
        // 全体更新を出してしまい、部分更新の意味が無くなる。
        || std::abs(m_labelPullTarget - m_labelPull) > 0.01
        // 2026-09-23: 長押しのゲージ・言葉・震え
        || labelHoldNeedsFrames()
        // 並べ替え中、隙間が目的の位置へ動いている間
        || (m_rowDragIndex >= 0
            && std::abs(rowGapSlotTarget() - m_rowGapAnim) > 0.02);
}

void CanvasVideo::setLabelPinned(bool pinned)
{
    if (!pinned) {
        m_labelPinned = false;
        return;
    }
    m_labelPinned = m_labelPinnableEffective;
}

void CanvasVideo::beginLabelPull()
{
    m_labelPullActive = true;
    m_labelPullRaw    = 0.0;
    m_labelPullTarget = 0.0;
    // 2026-09-23: 長押しできるラベルなら、ここからためはじめる。前の結果待ち・
    // 演出の途中なら重ねない。
    m_labelHoldSwallowRelease = false;
    if (m_labelHoldAvailableEffective
        && (m_labelHoldPhase == LabelHoldPhase::Idle
            || m_labelHoldPhase == LabelHoldPhase::Charging)) {
        m_labelHoldPhase = LabelHoldPhase::Charging;
        m_labelHoldMs = 0.0;
        m_labelHoldHintShown = m_labelHoldHint;
    }
}

void CanvasVideo::updateLabelPull(double dyPx)
{
    if (!m_labelPullActive)
        return;
    // 2026-09-23: 固定できないラベル（Solo など）は引っ張っても伸ばさない。
    // 引っ張り＝固定（鍵）の操作で、List / Mix になってからのもの。長押しの
    // ために押せるようになっただけの Solo で伸びると、できない操作に見える。
    if (!m_labelPinnableEffective && !m_labelPinned) {
        m_labelPullRaw = 0.0;
        m_labelPullTarget = 0.0;
        return;
    }
    m_labelPullRaw = qMax(0.0, dyPx);
    // 引くほど重くなるゴム。上限 kLabelPullMax へ漸近する。
    m_labelPullTarget = kLabelPullMax * std::tanh(m_labelPullRaw / kLabelPullMax);
}

bool CanvasVideo::endLabelPull()
{
    if (m_labelHoldPhase == LabelHoldPhase::Charging)
        m_labelHoldPhase = LabelHoldPhase::Idle;      // 途中で離した → 戻る
    const bool wasActive = m_labelPullActive;
    const bool pastThreshold = (m_labelPullTarget >= kLabelPullThreshold);
    m_labelPullActive = false;
    m_labelPullRaw    = 0.0;
    m_labelPullTarget = 0.0; // バネで元の位置へ戻る
    if (!wasActive || !pastThreshold)
        return false;
    if (!m_labelPinnableEffective && !m_labelPinned)
        return false; // 固定できないラベルは引っ張っても何も起きない
    m_labelPinned = !m_labelPinned;
    return true;
}

void CanvasVideo::cancelLabelPull()
{
    if (m_labelHoldPhase == LabelHoldPhase::Charging)
        m_labelHoldPhase = LabelHoldPhase::Idle;
    m_labelPullActive = false;
    m_labelPullRaw    = 0.0;
    m_labelPullTarget = 0.0; // バネで元の位置へ戻る（固定はトグルしない）
}

// ---- アンビエント発光（2026-09-13追加）--------------------------------
// 設計の意図は CanvasVideo.h のメソッド群のコメントを参照。ここは実装だけ。

namespace {
// 平均色を採り直す間隔。色は下の時定数でゆっくり寄せるので、この程度でも
// 見た目は完全に滑らかに見える（毎フレーム採る必要はない）。
constexpr qint64 kAmbientSampleIntervalMs = 150;
// 色が目標へ寄る速さ。小さいほどゆっくり＝カットが変わってもチカチカしない。
constexpr double kAmbientColorSpeed = 1.8;   // 時定数 ≒ 0.55秒
// 濃さの出入り。色より少し速い（消えるときにもたつかないように）。
constexpr double kAmbientAlphaSpeed = 2.6;
// 出しきったときの濃さ。縁のガラスハイライトと二重に光ると安っぽく
// 見えるので、かなり控えめから始める。
constexpr double kAmbientPeakAlpha = 44.0;
// 暗い映像で無理に光らせない／白い映像で色が抜けないための補正。
// 採った色を一度HSV的に持ち上げる（彩度を上げ、明るさを底上げする）。
QColor liftAmbientColor(const QColor &raw)
{
    int h = 0, sv = 0, v = 0;
    raw.getHsv(&h, &sv, &v);
    if (h < 0) h = 0;                       // 無彩色は色相0扱い
    sv = qBound(0, qRound(sv * 1.45), 255); // 少し鮮やかに
    v  = qBound(70, qMax(v, 70), 255);      // 暗すぎる映像でも最低限光る
    return QColor::fromHsv(h, sv, v);
}
} // namespace

bool CanvasVideo::ambientSampleDue() const
{
    if (!m_ambientSampleClock.isValid()) {
        m_ambientSampleClock.start();
        return true;
    }
    return m_ambientSampleClock.elapsed() >= kAmbientSampleIntervalMs;
}

void CanvasVideo::setAmbientSampledColor(const AmbientSample &sample)
{
    m_ambientSampleClock.restart();
    if (!sample.valid || !sample.average.isValid())
        return;
    // 色を4bitに丸めて、細かな揺れで影の焼き直しが起きないようにする
    m_contentAvgColor = QColor((sample.average.red() & 0xF0) | 0x08,
                               (sample.average.green() & 0xF0) | 0x08,
                               (sample.average.blue() & 0xF0) | 0x08);

    const QColor raw[AmbientSideCount] = {
        sample.average,
        sample.left.isValid()   ? sample.left   : sample.average,
        sample.right.isValid()  ? sample.right  : sample.average,
        sample.top.isValid()    ? sample.top    : sample.average,
        sample.bottom.isValid() ? sample.bottom : sample.average,
    };
    for (int i = 0; i < AmbientSideCount; ++i) {
        const QColor lifted = liftAmbientColor(raw[i]);
        m_ambientTarget[i] = {static_cast<double>(lifted.red()),
                              static_cast<double>(lifted.green()),
                              static_cast<double>(lifted.blue())};
    }

    // 光の筋の強さに使う明るさ。持ち上げる前の素の値で測る
    // （持ち上げた後だと全部明るくなってしまい、差が出ない）。
    m_ambientBrightnessTarget = qBound(0.0, sample.average.valueF(), 1.0);

    if (!m_ambientHasColor) {
        // 初回は寄せる元が無いのでそのまま入れる（黒から色が湧いて
        // 見えるのを避ける）。
        for (int i = 0; i < AmbientSideCount; ++i)
            m_ambientCur[i] = m_ambientTarget[i];
        m_ambientBrightness = m_ambientBrightnessTarget;
        m_ambientHasColor = true;
    }
}

void CanvasVideo::updateAmbientGlow(double dtSec)
{
    if (dtSec <= 0.0)
        return;
    const auto ease = [dtSec](double cur, double target, double speed) {
        return cur + (target - cur) * qMin(1.0, speed * dtSec);
    };
    for (int i = 0; i < AmbientSideCount; ++i) {
        m_ambientCur[i].r = ease(m_ambientCur[i].r, m_ambientTarget[i].r, kAmbientColorSpeed);
        m_ambientCur[i].g = ease(m_ambientCur[i].g, m_ambientTarget[i].g, kAmbientColorSpeed);
        m_ambientCur[i].b = ease(m_ambientCur[i].b, m_ambientTarget[i].b, kAmbientColorSpeed);
    }
    m_ambientBrightness = ease(m_ambientBrightness, m_ambientBrightnessTarget,
                               kAmbientColorSpeed);

    // 音が鳴っている＝「再生中」のバブルだけ光らせる。全部光ると祭りに
    // なるので、止まっているものは静かに消す（isSoundingForEqualizer()は
    // Playing / AudioOnly / Buffering を「鳴っている」とみなす共通判定）。
    const double alphaTarget =
        (m_ambientHasColor && isSoundingForEqualizer()) ? kAmbientPeakAlpha : 0.0;
    m_ambientAlpha = ease(m_ambientAlpha, alphaTarget, kAmbientAlphaSpeed);
    if (std::abs(m_ambientAlpha - alphaTarget) < 0.15)
        m_ambientAlpha = alphaTarget;
    if (m_ambientAlpha <= 0.0 && alphaTarget <= 0.0)
        m_ambientHasColor = m_ambientHasColor && isSoundingForEqualizer();
}

QColor CanvasVideo::ambientGlowColor(int side) const
{
    if (m_ambientAlpha <= 0.4 || side < 0 || side >= AmbientSideCount)
        return QColor(0, 0, 0, 0);
    const AmbientChannel &c = m_ambientCur[side];
    return QColor(qBound(0, qRound(c.r), 255),
                  qBound(0, qRound(c.g), 255),
                  qBound(0, qRound(c.b), 255),
                  qBound(0, qRound(m_ambientAlpha), 255));
}

bool CanvasVideo::ambientGlowNeedsFrames() const
{
    // 鳴っている間は動画フレームごとに同じ矩形が描き直されるので、
    // こちらからフレームを要求する必要はない（要求すると再生中のバブルが
    // 動画フレームとUIの二重で回って無駄になる）。
    if (isSoundingForEqualizer())
        return false;
    return m_ambientAlpha > 0.4;
}

// 2026-09-15 E-2: 停止マークのアニメが進行中か（ヘッダのコメントに経緯）。
// paintPauseIcon() と同じ条件を、状態を進めずに判定するだけ。
bool CanvasVideo::pauseIconNeedsFrames() const
{
    // paintPauseIcon() と同じ「再生中とみなす」判定。切り替え中（解決待ち）は
    // 停止マークを出さないので再生中側に含める。
    const bool isPlayingNow = m_controller.isPlaying() || isSwitchingVideo();
    if (isPlayingNow != m_pauseIconWasPlaying)
        return true;    // 切り替わった直後。次の描画でアニメが始まる
    if (!isPlayingNow) {
        // 出現中（ぽよんのポップ、またはフェードイン）だけ要る。
        // 出切って静止したら false ＝ もう要求しない。
        return m_pauseIconPopActive || m_pauseIconAlpha < 0.999f;
    }
    // 消失中（縮小＋フェードアウト）。再生中は動画フレームでも描き直されるが、
    // 音声のみ・低フレームレートの動画では足りないのでここでも見る。
    return m_pauseIconAlpha > 0.001f
        || m_pauseIconScale > 0.001f
        || m_pauseIconMarkScale > 0.001f;
}

void CanvasVideo::paintPinnedLabelOverlay(QPainter &painter, int W, int H,
                                          BackdropBlur::Regions *blurRegions)
{
    if (!m_labelPinned || m_pinnedOverlayFade <= 0.01f)
        return;
    // 固定中は再生バーのフェードと無関係に出す。ただし固定ラベルが
    // 引っ込む／出てくるときの出入りだけは m_pinnedOverlayFade で薄める。
    paintLabelAndPanel(painter, W, H, blurRegions, m_pinnedOverlayFade);
}

// ヘッダの簡易コントロール。台（丸い枠）は描かず、アイコンだけを置く。
// 再生/一時停止は再生バーと同じSVG（now_play/now_stop）、前後は
// skip_prev/skip_next。押せる範囲はアイコンより少し広めに取る。
void CanvasVideo::paintLabelHeaderButtons(QPainter &painter,
                                          const QRectF &headerRect,
                                          double alpha)
{
    if (alpha <= 0.01 || headerRect.width() < 120.0)
        return;

    const double icon = 17.0;
    const double hit  = 24.0;   // 押せる範囲（見た目より広め）
    const double gap  = 2.0;
    const double rightPad = 8.0;
    const double cy = headerRect.center().y();

    // 中央に ◀◀ / 再生 / ▶▶ の順で並べる。右上は島と当たりうるので空ける
    // （テキストと鍵マークがそちらへ寄る）。
    Q_UNUSED(rightPad);
    const double cx = headerRect.center().x();
    const QPointF cPrev(cx - (hit + gap), cy);
    const QPointF cPlay(cx, cy);
    const QPointF cNext(cx + (hit + gap), cy);

    const auto hitRect = [hit](const QPointF &c) {
        return QRectF(c.x() - hit * 0.5, c.y() - hit * 0.5, hit, hit);
    };
    m_labelBtnPrev = hitRect(cPrev);
    m_labelBtnPlay = hitRect(cPlay);
    m_labelBtnNext = hitRect(cNext);

    // 2026-09-12: アイコンは「いま音が鳴っているか」で決める。画面外
    // （AudioOnly）や読み込み中（Buffering）でも再生は続いているので、
    // Playingだけを見ると▶と||が実際の挙動と食い違う。
    const bool playing = isSoundingForEqualizer();
    const QColor ink(255, 255, 255);
    const auto drawIcon = [&](const QPointF &c, const QString &res, double scale) {
        const QImage img = tintedSvgImageForPlaybackBar(res, QSize(64, 64), ink);
        const double s = icon * scale;
        painter.drawImage(QRectF(c.x() - s * 0.5, c.y() - s * 0.5, s, s), img);
    };

    painter.save();
    painter.setOpacity(painter.opacity() * alpha * 0.92);
    drawIcon(cPrev, QStringLiteral(":/icons/skip_prev.svg"), 1.0);
    // 再生/停止のSVGは余白込みで描かれているぶん、前後より少し大きめに。
    drawIcon(cPlay, playing ? QStringLiteral(":/icons/now_play.svg")
                            : QStringLiteral(":/icons/now_stop.svg"), 1.35);
    drawIcon(cNext, QStringLiteral(":/icons/skip_next.svg"), 1.0);
    painter.restore();
}

// 一覧の中で現在位置から delta 個ずれた項目へ移動する。
// 一覧が取れていない場合は履歴（前）と連続再生用URL（次）で代用する。
bool CanvasVideo::navigateRelativeInPanel(int delta)
{
    if (delta == 0)
        return false;

    // 2026-09-22: 本体のリストは並びの前後へ。端では止まる。
    // 2026-09-23: ファイルが見つからないローカルの動画は飛ばす（並びからは
    // 消さない。後でファイルが戻ってくることもあるため）。
    if (hasCoreList()) {
        const int cur = coreListCurrentIndex();
        const int step = delta > 0 ? 1 : -1;
        int target = cur < 0 ? (delta > 0 ? 0 : -1) : cur + delta;
        while (target >= 0 && target < m_coreList.size() && !coreListItemAvailable(target))
            target += step;
        return navigateToCoreListItem(target);
    }

    const QList<PanelRow> rows = orderedPanelRows();
    int nowIndex = -1;
    for (int i = 0; i < rows.size(); ++i) {
        if (rows.at(i).kind == QStringLiteral("NOW")) { nowIndex = i; break; }
    }
    if (nowIndex >= 0) {
        const int target = nowIndex + delta;
        if (target >= 0 && target < rows.size()) {
            const QString url = rows.at(target).url;
            if (!url.isEmpty() && url != m_source.originalPathOrUrl) {
                navigateToUrl(url, /*pushHistory=*/true);
                return true;
            }
        }
        return false; // 端。何もしない。
    }

    if (delta > 0 && !m_nextUrl.isEmpty()) {
        navigateToUrl(m_nextUrl, true);
        return true;
    }
    if (delta < 0 && !m_playHistory.isEmpty()) {
        navigateToUrl(m_playHistory.last().url, false);
        return true;
    }
    return false;
}

// 再生中の行の右に置く簡易イコライザと、そこから飛ぶ音符。
// 【音とは無関係】実際の音量やスペクトルは見ていない。位相の違う正弦波を
// 重ねて「それっぽく」揺らしているだけ（本物にすると音声を毎フレーム
// 解析することになり、この飾りに見合わない）。一時停止中は静かになる。
// 「いま音が鳴っているか」。イコライザを止めてよいのは本当に止まっている
// ときだけ。バブルが画面外にあると状態は AudioOnly（映像だけ停止・音は継続）
// になり、読み込み直後は Buffering を通るので、それらも「鳴っている」に
// 含める（以前は Playing だけを見ていたため、画面外に出した途端に棒が
// 一番下で止まっていた）。
bool CanvasVideo::isSoundingForEqualizer() const
{
    // 2026-09-13: 切り替え中（次の動画の解決・ロード待ち）も「鳴っている」
    // 側に含める。ここで止めると、リストのヘッダの ▶/‖ が切り替えのたびに
    // 一瞬 ▶ へ落ちてチラつく（paintPauseIcon側と同じ理由）。
    if (isSwitchingVideo())
        return true;
    switch (m_controller.state()) {
    case BubbleState::Playing:
    case BubbleState::AudioOnly:
    case BubbleState::Buffering:
        return true;
    default:
        return false;
    }
}

// 2026-09-12: イコライザだけを描き直すための矩形（ウィジェット座標）。
// リストを開いて再生している間、イコライザは毎フレーム動く。そのために
// 再生UIのオーバーレイ全体を再描画していたのを、この矩形だけに絞る。
//
// 含める範囲: 棒（行の右端の内側）＋音符の飛翔範囲（板の上へ最大
// kEqNoteRise 昇り、左右へ kEqNoteSway 揺れる）。音符はクリップせずに
// 描いているので、板の外まで見ておく必要がある。
// 何も描かれない状況（閉じている・スクロールで行が見えていない等）では
// 空を返す。呼び出し側はその場合だけ全体更新へ落とす。
QRectF CanvasVideo::equalizerDamageRect() const
{
    if (m_labelPanelNowRowRect.isEmpty() || m_labelPanelViewRect.isEmpty())
        return QRectF();
    if (m_labelFadeAlpha <= 0.01f || m_labelPanelAnim <= 0.55f)
        return QRectF();

    const QRectF rowRect = m_labelPanelNowRowRect;
    const double baseY = rowRect.center().y() + kEqBaselineDrop;
    const double right = rowRect.right() - kEqRightInset;
    const double left  = right - kEqBarsWidth;

    // 音符は left+2 を基準に、揺れ(±kEqNoteSway)・2つ目のずれ(+9)・
    // グリフの幅ぶん右へ出る。上端は棒の頂点からさらに kEqNoteRise 昇った
    // 位置で、そこからグリフの高さぶん上。
    const double noteLeft   = left + 2.0 - kEqNoteSway;
    const double noteRight  = left + 2.0 + kEqNoteSway + 9.0 + 12.0;
    const double noteTop    = baseY - kEqMaxHeight - 3.0 - kEqNoteRise - 14.0;

    QRectF r(QPointF(qMin(left, noteLeft), noteTop),
             QPointF(qMax(right, noteRight), baseY + 2.0));
    // 丸め・アンチエイリアスの取りこぼし防止。
    return r.adjusted(-3.0, -3.0, 3.0, 3.0);
}

// 2026-09-14 C-2: スクロールだけが動いているフレームで描き直す範囲。
// 板（＋キャッシュの余白と同じ12px、丸め対策に+2）に、板の外へ飛び出す
// イコライザの音符を足したもの。空を返したときは呼び出し側が全体更新へ。
QRectF CanvasVideo::labelPanelDamageRect() const
{
    if (m_labelPanelRect.isEmpty() || m_labelFadeAlpha <= 0.01f)
        return QRectF();
    QRectF r = m_labelPanelRect.adjusted(-14.0, -14.0, 14.0, 14.0)
                   .translated(0.0, m_labelPull);
    const QRectF eq = equalizerDamageRect();
    if (!eq.isEmpty())
        r = r.united(eq);
    return r;
}

void CanvasVideo::paintNowPlayingEqualizer(QPainter &painter, const QRectF &rowRect,
                                           double alpha, bool drawBars, bool drawNotes)
{
    if (alpha <= 0.01)
        return;
    const double level = qBound(0.0, m_eqLevel, 1.0);

    const double barW = kEqBarWidth, gap = kEqBarGap;
    const int    bars = kEqBarCount;
    const double maxH = kEqMaxHeight;
    const double baseY = rowRect.center().y() + kEqBaselineDrop;
    // 右端から少し内側へ（行の右肩に寄りすぎないように）。
    const double right = rowRect.right() - kEqRightInset;
    const double left  = right - kEqBarsWidth;

    painter.save();
    painter.setPen(Qt::NoPen);
    // 棒。止まっているときは低いまま揃える。
    for (int i = 0; drawBars && i < bars; ++i) {
        const double p = m_eqPhase * (2.6 + i * 0.55) + i * 1.7;
        const double t = 0.22 + 0.39 * (std::sin(p) + 1.0) * 0.5
                              + 0.30 * (std::sin(p * 1.7 + 0.9) + 1.0) * 0.5;
        // level=0（止まっている）では最低の高さへ静かに沈む。
        const double h = qBound(2.0, maxH * (0.14 + (t - 0.14) * level), maxH);
        const double x = left + i * (barW + gap);
        painter.setBrush(QColor(255, 255, 255, qRound(215 * alpha)));
        painter.drawRoundedRect(QRectF(x, baseY - h, barW, h), 1.4, 1.4);
    }

    // 音符。棒の上から2つ、ゆっくり昇りながら消える。板の外へはみ出しても
    // よいので、こちらはクリップせずに描く（呼び出し側参照）。
    if (level > 0.01 && drawNotes) {
        QFont noteFont(QStringLiteral("Arial"), 8);
        painter.setFont(noteFont);
        for (int n = 0; n < 2; ++n) {
            const double u = std::fmod(m_eqPhase * 0.32 + n * 0.5, 1.0);
            const double rise = kEqNoteRise * u;            // 昇る量（板の外まで）
            const double sway = kEqNoteSway * std::sin(u * 6.28 + n * 2.0);
            // 出はじめと消え際をなだらかに。
            const double fade = std::sin(u * 3.14159);
            const QChar glyph = (n % 2 == 0) ? QChar(0x266A) : QChar(0x266B);
            painter.setPen(QColor(255, 255, 255,
                                  qRound(200 * alpha * level
                                         * qBound(0.0, fade, 1.0))));
            painter.drawText(QPointF(left + 2.0 + sway + n * 9.0,
                                     baseY - maxH - 3.0 - rise),
                             QString(glyph));
        }
    }
    painter.restore();
}

// プラグインから来た一覧に、ユーザーが手で決めた並び（m_panelOrderOverride）
// を重ねて返す。並べ替えは本体の中だけの話で、プラグインへは何も伝えない
// （プラグインは「サービス上の並び」を返すのが仕事なので、そこを本体の
// 都合で書き換えに行かない）。
QList<CanvasVideo::PanelRow> CanvasVideo::orderedPanelRows() const
{
    QList<PanelRow> rows = buildPanelRows();
    if (m_panelOrderOverride.isEmpty() || rows.size() < 2)
        return rows;

    // 2026-09-12: 下の並べ替えは50件×50件の総当たりになる。毎フレーム
    // （しかも1フレームに複数回）呼ばれるので、元の一覧の世代と並べ替えの
    // 版が変わらないうちは結果を使い回す。
    if (m_orderedRowsGen == m_panelRowsGen
        && m_orderedRowsRev == m_panelOrderRevision
        && m_orderedRowsCache.size() == rows.size()) {
        return m_orderedRowsCache;
    }

    QList<PanelRow> out;
    out.reserve(rows.size());
    // まず記憶している順に取り出し、
    for (const QString &url : m_panelOrderOverride) {
        for (int i = 0; i < rows.size(); ++i) {
            if (rows.at(i).url == url) {
                out << rows.takeAt(i);
                break;
            }
        }
    }
    // 記憶に無いもの（後から増えた項目）は元の順のまま後ろへ。
    out += rows;
    m_orderedRowsCache = out;
    m_orderedRowsGen   = m_panelRowsGen;
    m_orderedRowsRev   = m_panelOrderRevision;
    return out;
}

int CanvasVideo::panelRowIndexAt(const QPointF &pos) const
{
    if (!m_labelPanelOpen || m_labelPanelViewRect.isEmpty()
        || !m_labelPanelViewRect.contains(pos))
        return -1;
    for (int k = 0; k < m_labelPanelRowRects.size(); ++k) {
        if (m_labelPanelRowRects.at(k).contains(pos))
            return m_labelPanelRowIndices.value(k, -1);
    }
    return -1;
}

// つかんだ行を抜いた並びの中で、いま隙間があるべき位置。
int CanvasVideo::rowGapSlotTarget() const
{
    if (m_rowDragIndex < 0)
        return -1;
    return qMax(0, m_rowDropIndex - (m_rowDropIndex > m_rowDragIndex ? 1 : 0));
}

void CanvasVideo::beginRowDrag(int rowIndex, double pressY, double pressX)
{
    m_rowDragIndex = rowIndex;
    m_rowDropIndex = rowIndex;
    m_rowDragMoved = false;
    m_rowDragY     = pressY;
    m_rowDragPressX = pressX;
    m_rowDragX      = pressX;
    m_rowGapAnim   = rowIndex; // つかんだ位置から始めるので最初は動かない
    // つかんだ行の「どこを持ったか」。追従させるときにこの分だけずらすと、
    // カーソルと行の位置関係が変わらない。
    m_rowDragGrabDy = kPanelRowHeight * 0.5;
    for (int k = 0; k < m_labelPanelRowIndices.size(); ++k) {
        if (m_labelPanelRowIndices.at(k) == rowIndex) {
            m_rowDragGrabDy = qBound(0.0, pressY - m_labelPanelRowRects.at(k).top(),
                                     kPanelRowHeight);
            break;
        }
    }
}

void CanvasVideo::updateRowDrag(const QPointF &pos)
{
    if (m_rowDragIndex < 0)
        return;
    m_rowDragY     = pos.y();
    m_rowDragX     = pos.x();
    m_rowDragMoved = true;

    // 落とし先は行の矩形ではなく座標から求める（並べ替え中は行が入れ替わって
    // 動くので、矩形を追いかけると位置がぶれる）。行の境目を基準にした
    // 「挿入位置」を返す。
    if (m_labelPanelViewRect.isEmpty())
        return;
    const int rowCount =
        qMax(1, qRound(m_labelPanelContentHeight / kPanelRowHeight));
    const double rel = pos.y() - m_labelPanelViewRect.top() + m_labelPanelScroll;
    const int slot = static_cast<int>(std::floor(rel / kPanelRowHeight + 0.5));
    m_rowDropIndex = qBound(0, slot, rowCount);
}

void CanvasVideo::endRowDrag()
{
    const int from = m_rowDragIndex;
    int to = m_rowDropIndex;
    const bool moved = m_rowDragMoved;
    m_rowDragIndex = m_rowDropIndex = -1;
    m_rowDragMoved = false;
    m_rowDragGrabDy = 0.0;
    if (from < 0)
        return;

    const QList<PanelRow> rows = orderedPanelRows();
    if (from >= rows.size())
        return;

    if (!moved || to < 0) {
        // 2026-09-22: 本体のリストは行の番号がそのまま並びの番号。
        if (hasCoreList()) {
            if (from != coreListCurrentIndex())
                navigateToCoreListItem(from);
            return;
        }
        // 動かしていない＝ただのクリック。離した時点でその動画へ移動する。
        const QString url = rows.at(from).url;
        if (url.isEmpty() || url == m_source.originalPathOrUrl)
            return; // 「今の動画」行。何もしない。
        // 履歴の一番上と同じURL＝「戻る」操作。積み直すと前後に往復
        // するだけになってしまうので、積まずに取り除く。
        if (!m_playHistory.isEmpty() && m_playHistory.last().url == url) {
            m_playHistory.removeLast();
            navigateToUrl(url, /*pushHistory=*/false);
        } else {
            navigateToUrl(url, /*pushHistory=*/true);
        }
        return;
    }

    // 並べ替えを確定して、以後この順で表示する。
    if (to > from)
        --to; // 自分を抜いた分だけ詰まる
    to = qBound(0, to, rows.size() - 1);
    if (to == from)
        return;
    // 2026-09-22: 本体のリストは並びそのものを動かす（融合で他へ渡すときも、
    // 終わって次へ進むときも、この順になる）。
    if (hasCoreList()) {
        if (from < m_coreList.size() && to < m_coreList.size()) {
            int cur = coreListCurrentIndex();
            m_coreList.move(from, to);
            if (cur == from)
                cur = to;
            else if (from < cur && to >= cur)
                --cur;
            else if (from > cur && to <= cur)
                ++cur;
            m_coreListIndex = cur;
            invalidatePanelRows();
        }
        return;
    }
    QStringList order;
    order.reserve(rows.size());
    for (const PanelRow &r : rows)
        order << r.url;
    order.move(from, to);
    m_panelOrderOverride = order;
    ++m_panelOrderRevision;
}

// ------------------------------------------------------------
//  2026-09-23: 一覧からの取り出し
// ------------------------------------------------------------
namespace {
// これ以上右へ引っ張ってから板の外へ出たときだけ取り出す。縦に並べ替えて
// いて板の上下の端から少しはみ出しただけで取り出してしまわないように。
constexpr double kRowExtractMinPull = 28.0;
}

double CanvasVideo::rowPullAmount() const
{
    if (m_rowDragIndex < 0 || !m_rowDragMoved || !hasCoreList())
        return 0.0;
    return qMax(0.0, m_rowDragX - m_rowDragPressX);
}

bool CanvasVideo::rowExtractReady(const QPointF &pos) const
{
    if (rowPullAmount() < kRowExtractMinPull || m_labelPanelRect.isEmpty())
        return false;
    return !m_labelPanelRect.contains(pos);
}

MediaSource CanvasVideo::takeRowForExtract()
{
    const int index = m_rowDragIndex;
    m_rowDragIndex = m_rowDropIndex = -1;
    m_rowDragMoved = false;
    m_rowDragGrabDy = 0.0;
    if (!hasCoreList() || index < 0 || index >= m_coreList.size())
        return MediaSource();

    // 今の動画の表示名・サムネイルを並びへ反映してから外す
    m_coreList = listItems();
    const int cur = coreListCurrentIndex();
    const MediaSource taken = m_coreList.takeAt(index);

    if (index == cur) {
        // 再生中の行を取り出した → このバブルは隣の動画へ移る
        // （同じ位置に来た次の動画、最後の行だったなら1つ前）。
        const int next = qMin(index, static_cast<int>(m_coreList.size()) - 1);
        const MediaSource target = m_coreList.at(next);
        if (m_coreList.size() < 2) {
            m_coreList.clear();
            m_coreListIndex = -1;
        } else {
            m_coreListIndex = next;
        }
        invalidatePanelRows();
        navigateToSource(target, /*pushHistory=*/false);
    } else {
        if (m_coreList.size() < 2) {
            // 残りは今の動画1本だけ → 本体のリストではなくなる（Solo）
            m_coreList.clear();
            m_coreListIndex = -1;
            closeLabelPanel();
        } else {
            m_coreListIndex = (cur > index) ? cur - 1 : cur;
        }
        invalidatePanelRows();
    }
    emit playbackUiChanged();
    return taken;
}

void CanvasVideo::cancelRowDrag()
{
    m_rowDragIndex = m_rowDropIndex = -1;
    m_rowDragMoved = false;
}

// 鍵マーク（固定中の目印）。ラベルの右上に小さく重ねる。
void CanvasVideo::paintLabelLockBadge(QPainter &painter, const QRectF &labelRect,
                                      double amount)
{
    const double r = 8.0;
    const QPointF c(labelRect.right() - 2.0, labelRect.top() + 2.0);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(18, 18, 26, qRound(215 * amount)));
    painter.drawEllipse(c, r, r);

    const double bodyW = 7.0, bodyH = 5.5;
    const QRectF body(c.x() - bodyW * 0.5, c.y() - 0.2, bodyW, bodyH);
    const QColor ink(255, 255, 255, qRound(235 * amount));
    painter.setBrush(ink);
    painter.drawRoundedRect(body, 1.6, 1.6);

    // つる（上の弧）。
    QPen pen(ink);
    pen.setWidthF(1.4);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const QRectF arc(c.x() - 2.6, c.y() - 4.4, 5.2, 5.2);
    painter.drawArc(arc, 0 * 16, 180 * 16);
    painter.restore();
}


// ------------------------------------------------------------
//  ラベルのコントローラ（2026-09-12追加）
// ------------------------------------------------------------

namespace {
// 左右で角丸半径が違う角丸矩形。ラベルの台（左はアイコンと同心、右は
// カプセル）から、カードの一様な角丸へ連続的に変形させるために使う。
QPainterPath roundedPathLR(const QRectF &r, double leftRadius, double rightRadius)
{
    const double maxR = qMin(r.width(), r.height()) * 0.5;
    const double lr = qBound(0.0, leftRadius, maxR);
    const double rr = qBound(0.0, rightRadius, maxR);
    const double x0 = r.left(), x1 = r.right(), y0 = r.top(), y1 = r.bottom();
    QPainterPath path;
    path.moveTo(x0 + lr, y0);
    path.lineTo(x1 - rr, y0);
    path.arcTo(QRectF(x1 - rr * 2.0, y0, rr * 2.0, rr * 2.0), 90.0, -90.0);
    path.lineTo(x1, y1 - rr);
    path.arcTo(QRectF(x1 - rr * 2.0, y1 - rr * 2.0, rr * 2.0, rr * 2.0), 0.0, -90.0);
    path.lineTo(x0 + lr, y1);
    path.arcTo(QRectF(x0, y1 - lr * 2.0, lr * 2.0, lr * 2.0), 270.0, -90.0);
    path.lineTo(x0, y0 + lr);
    path.arcTo(QRectF(x0, y0, lr * 2.0, lr * 2.0), 180.0, -90.0);
    path.closeSubpath();
    return path;
}


// 島と同じガラス表現で板を塗る。DynamicIsland::paintEvent()の
// 「ガラス表現」ブロックの移植で、光の言語（加算で輝く縁／乗算で沈む縁）を
// 揃えてある。形は呼び出し側が組んだpathへ追従するので、ラベルの
// カプセルでもカードでもそのまま使える。
// 【2026-09-12 検討済み・不採用】伸び縮みの最中だけ縁の帯を減らす
// （simplified）案を入れたが撤回した。動いている最中でも縁が薄くなるのは
// 分かってしまい、「伸び切ってから効果が後付けされる」ように見えるため。
// アニメーション中も常に本来の見た目で描く。
void paintGlassPlate(QPainter &painter, const QPainterPath &path,
                     const QRectF &r, const QColor &accent, double alpha)
{
    const auto withAlpha = [](QColor c, int a) { c.setAlpha(a); return c; };
    const QColor shadeColor = accentShadeColor();

    const double top = r.top(), bottom = r.bottom();
    const double left = r.left(), right = r.right();
    const double innerH = qMax(1.0, bottom - top);
    // 島は畳んだ高さ(42px)を基準に光の距離を決めている。こちらのラベルも
    // 同じ見え方になるよう、その基準高さを上限として使う。
    const double baseH = qMin(innerH, 42.0);
    const double lightSpan = qMin(baseH * 0.95, innerH * 0.36);
    const double s1 = qMin(0.34, (baseH * 0.62) / innerH);
    const double s2 = 1.0 - s1;

    // 縁から内側への減衰は、太さを変えたストロークの重ね合わせで作る
    // （1本の太いストロークだと内側の端が段差として見えてしまう）。
    // 2026-09-12: bandRect を渡すと、その範囲へクリップしてから描く。
    // 上辺／下辺の光はその帯の外側では完全に透明なので、見た目は変わらず
    // ラスタライズの量だけ減る。板が縦に長い（リストを開いた状態）ほど
    // 効き、1本あたり板の周長ぶん走っていた無駄がなくなる。
    // 2026-09-12: 帯の本数は板の大きさに応じて減らす。帯はグラデーションの
    // 減衰(幅4px程度)を滑らかに見せるためのもので、12本だと1本あたり
    // 0.35px以下＝人の目には過剰。リストのカードのように周長が長い板では
    // ストローク1本ごとの負荷が効いてくるため、大きいときは半分にする
    // （小さい島やラベルの台は従来どおり12本のまま）。
    const int bandCount = (innerH > 72.0) ? 6 : 12;
    // 2026-09-14: ブラシとペンをループの外で1回だけ作る（太さだけ差し替える）。
    //
    // QPen(QBrush(gradient), w) を毎回作ると、Qtのラスタエンジンがブラシごとに
    // グラデーションの色テーブル（1024段）を用意し直す。1回の呼び出しで12本、
    // 板ぜんぶで56本ぶん走っていた。
    //
    // 島（DynamicIsland::paintEvent）の同じ形のコードで実測したところ、
    // この一点だけで縁の描画が 6.68ms → 3.29ms（約半分）になった。
    // 見た目は一切変わらない（描く内容は同一で、作り直しを省いただけ）。
    const auto strokeBands = [&](int count, double span, const QGradient &gradient,
                                 const QRectF &bandRect = QRectF()) {
        count = qMin(count, bandCount);
        painter.save();
        if (!bandRect.isEmpty())
            painter.setClipRect(bandRect, Qt::IntersectClip);
        QPen pen(QBrush(gradient), 0.0);
        for (int i = 0; i < count; ++i) {
            pen.setWidthF(span * 2.0 * (1.0 - static_cast<double>(i) / count));
            painter.setPen(pen);
            painter.drawPath(path);
        }
        painter.restore();
    };

    painter.save();
    painter.setOpacity(painter.opacity() * alpha);

    // 本体（アクセント色を薄く。背後はBackdropBlurでぼけている）
    painter.setPen(Qt::NoPen);
    painter.setBrush(withAlpha(accent, kPlateBodyAlpha));
    painter.drawPath(path);

    painter.save();
    painter.setClipPath(path);
    painter.setBrush(Qt::NoBrush);

    // 光を散らすフロスト
    painter.fillRect(r, QColor(255, 255, 255, 10));

    // 加算: 上辺／下辺の光
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    QLinearGradient topLight(QPointF(0, top), QPointF(0, top + lightSpan));
    topLight.setColorAt(0.00, QColor(255, 255, 255, 14));
    topLight.setColorAt(0.45, QColor(255, 255, 255, 4));
    topLight.setColorAt(1.00, QColor(255, 255, 255, 0));
    strokeBands(12, baseH * 0.045, topLight,
                QRectF(left, top - 2.0, right - left, lightSpan + 4.0));

    QLinearGradient bottomLight(QPointF(0, bottom - lightSpan), QPointF(0, bottom));
    bottomLight.setColorAt(0.00, QColor(255, 255, 255, 0));
    bottomLight.setColorAt(0.55, QColor(255, 255, 255, 3));
    bottomLight.setColorAt(1.00, QColor(255, 255, 255, 9));
    strokeBands(12, baseH * 0.035, bottomLight,
                QRectF(left, bottom - lightSpan - 2.0, right - left, lightSpan + 4.0));

    // 加算: 左上から差す主光と右下へ抜ける副光（斜めからの照明に見せる）
    const double dx = (right - left) * 0.55;
    const double dy = innerH * 0.55;
    QLinearGradient topLeftLight(QPointF(left, top), QPointF(left + dx, top + dy));
    topLeftLight.setColorAt(0.00, QColor(255, 255, 255, 20));
    topLeftLight.setColorAt(0.55, QColor(255, 255, 255, 3));
    topLeftLight.setColorAt(1.00, QColor(255, 255, 255, 0));
    strokeBands(12, baseH * 0.05, topLeftLight);

    QLinearGradient bottomRightLight(QPointF(right - dx, bottom - dy),
                                     QPointF(right, bottom));
    bottomRightLight.setColorAt(0.00, QColor(255, 255, 255, 0));
    bottomRightLight.setColorAt(0.45, QColor(255, 255, 255, 2));
    bottomRightLight.setColorAt(1.00, QColor(255, 255, 255, 11));
    strokeBands(12, baseH * 0.04, bottomRightLight);

    // 乗算: 上下の光が届かない左右の縁を沈める
    painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    QLinearGradient sideShade(QPointF(0, top), QPointF(0, bottom));
    sideShade.setColorAt(0.00, withAlpha(shadeColor, 0));
    sideShade.setColorAt(s1,   withAlpha(shadeColor, 30));
    sideShade.setColorAt(s2,   withAlpha(shadeColor, 30));
    sideShade.setColorAt(1.00, withAlpha(shadeColor, 0));
    strokeBands(8, baseH * 0.05, sideShade);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.restore();

    // 乗算: 輪郭線（ガラス板の切り口）
    painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    QLinearGradient contour(QPointF(0, top), QPointF(0, bottom));
    contour.setColorAt(0.00, withAlpha(shadeColor, 120));
    contour.setColorAt(s1,   withAlpha(shadeColor, 230));
    contour.setColorAt(s2,   withAlpha(shadeColor, 230));
    contour.setColorAt(1.00, withAlpha(shadeColor, 190));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QBrush(contour), 1.0));
    painter.drawPath(path);
    painter.restore();
}
} // namespace

// ---- 一覧のスナップショット（2026-09-12追加）--------------------------------
//
// 本体は「プラグインが言ったこと」をそのまま覚えているだけで、意味は解釈
// しない。詳しくは CanvasVideo.h の m_panelSnapshot のコメント参照。

namespace {
// 常に約この件数を保つ。現在位置から前後へ割り振る。
constexpr int kSnapshotWindowMax    = 50;
constexpr int kSnapshotWindowBefore = 25;
} // namespace

void CanvasVideo::mergePluginItemsIntoSnapshot(const QList<PbPlaylistItem> &items) const
{
    if (items.isEmpty())
        return;

    // 既知のURLの位置を引くための索引。
    QHash<QString, int> indexByUrl;
    indexByUrl.reserve(m_panelSnapshot.size());
    for (int i = 0; i < m_panelSnapshot.size(); ++i)
        indexByUrl.insert(m_panelSnapshot.at(i).url, i);

    for (const PbPlaylistItem &it : items) {
        if (it.url.isEmpty())
            continue;
        const auto found = indexByUrl.constFind(it.url);
        if (found != indexByUrl.constEnd()) {
            // 既に知っている項目。位置（＝ユーザーが並べ替えた順）はそのまま、
            // 中身だけ新しい方で上書きする（タイトルが後から埋まる等）。
            PanelSnapshotItem &dst = m_panelSnapshot[found.value()];
            if (!it.title.isEmpty())        dst.title       = it.title;
            if (!it.thumbnailUrl.isEmpty()) dst.thumbUrl    = it.thumbnailUrl;
            if (it.durationSec > 0)         dst.durationSec = it.durationSec;
            continue;
        }
        // 知らない項目（後から増えた分）はプラグインの順のまま後ろへ足す。
        // スナップショット自身はプラグインが言った順を保つ器で、ユーザーの
        // 並べ替えは m_panelOrderOverride 側が持つ（表示・移動はそちらを見る）。
        indexByUrl.insert(it.url, m_panelSnapshot.size());
        m_panelSnapshot.append(PanelSnapshotItem{it.url, it.title,
                                                it.thumbnailUrl, it.durationSec});
    }
}

// 「いま再生している項目」が列のどれかを決める。優先順は次のとおり。
//  1. プラグインが currentIndex を答えた → その項目のURL（これが最も確か）
//  2. バブルのURLがそのまま列にある → それ
//  3. タイトルが列の中でただ1つだけ一致する → それ
//  4. 直前に分かっていた現在位置がまだ列にある → それを維持
//     （プラグインが一時的に currentIndex=-1 を返しても見失わないため）
//  5. どれでもない → 空（NOW行なし）
//
// 【なぜ2だけでは足りないか】ユーザーがドロップした最初の1本は、URLの
// 表記がプラグインの列に載る形と一致するとは限らない（余計なクエリが
// 付いている等）。その場合 2 が外れて現在位置を見失い、再生中の行の
// イコライザが出ない・▶▶ が効かない、という形で現れる。
QString CanvasVideo::resolvePanelCurrentUrl(const PbPlaylistContext &ctx) const
{
    if (ctx.valid && ctx.currentIndex >= 0 && ctx.currentIndex < ctx.items.size()) {
        const QString &fromPlugin = ctx.items.at(ctx.currentIndex).url;
        if (!fromPlugin.isEmpty()) {
            m_panelCurrentUrl = fromPlugin;
            return fromPlugin;
        }
    }

    const QString &bubbleUrl = m_source.originalPathOrUrl;
    for (const PanelSnapshotItem &it : m_panelSnapshot) {
        if (it.url == bubbleUrl) {
            m_panelCurrentUrl = bubbleUrl;
            return bubbleUrl;
        }
    }

    // 3. タイトルで引き当てる。プラグインが解決時に返したタイトル
    //    （＝このバブルの title()）と、同じプラグインが列に載せたタイトルを
    //    突き合わせるだけなので、URLの意味を解釈したことにはならない。
    //    同名が複数ある場合は諦める（誤爆させない）。
    const QString myTitle = title();
    if (!myTitle.isEmpty()) {
        QString hit;
        int hits = 0;
        for (const PanelSnapshotItem &it : m_panelSnapshot) {
            if (it.title == myTitle) { hit = it.url; if (++hits > 1) break; }
        }
        if (hits == 1 && !hit.isEmpty()) {
            m_panelCurrentUrl = hit;
            return hit;
        }
    }

    // 4. 直前に分かっていた現在位置がまだ列にあるなら維持する。
    if (!m_panelCurrentUrl.isEmpty()) {
        for (const PanelSnapshotItem &it : m_panelSnapshot) {
            if (it.url == m_panelCurrentUrl)
                return m_panelCurrentUrl;
        }
    }

    m_panelCurrentUrl.clear();
    return QString();
}

// 別の列へ移ったかどうか。
// 【2026-09-12 改訂】以前は「現在のURLが列の中に無ければ捨てる」だったが、
// これは現在位置を特定できないだけの場合まで巻き込んで、一覧と並べ替えを
// 毎フレーム捨てては作り直していた（ドロップ直後の1本目で発生）。
// 判定を「列そのものが入れ替わったか」に変える。
//  ・プラグインが答えた一覧と手元の列にURLが1つも共通しない → 別の列
//  ・プラグインが「この動画は列の一部ではない」と確定で答えた
//    （valid=false かつ pending=false）→ 列から出た
// どちらもURLの意味は見ていない。
void CanvasVideo::dropPanelSnapshotIfListChanged(const PbPlaylistContext &ctx) const
{
    if (m_panelSnapshot.isEmpty())
        return;

    bool leftList = false;
    if (ctx.valid && !ctx.items.isEmpty()) {
        QSet<QString> known;
        known.reserve(m_panelSnapshot.size());
        for (const PanelSnapshotItem &it : m_panelSnapshot)
            known.insert(it.url);
        leftList = true;
        for (const PbPlaylistItem &it : ctx.items) {
            if (known.contains(it.url)) { leftList = false; break; }
        }
    } else if (!ctx.valid && !ctx.pending) {
        // プラグインが「列の一部ではない」と確定で答えた場合。
        // ただしセッションから戻した直後は、プラグインがまだこのURLを
        // 知らないだけでも同じ答え（valid=false / pending=false）になる。
        // 覚えている列に自分自身が載っているなら、それは「戻したばかりの
        // 自分の列」なので捨てない。
        leftList = true;
        for (const PanelSnapshotItem &it : m_panelSnapshot) {
            if (it.url == m_source.originalPathOrUrl) { leftList = false; break; }
        }
    }
    if (!leftList)
        return;

    m_panelSnapshot.clear();
    m_panelCurrentUrl.clear();
    if (!m_panelOrderOverride.isEmpty()) {
        m_panelOrderOverride.clear();
        ++m_panelOrderRevision;
    }
}

void CanvasVideo::trimPanelSnapshotWindow(const QString &currentUrl) const
{
    const int total = m_panelSnapshot.size();
    if (total <= kSnapshotWindowMax)
        return;

    int cur = -1;
    for (int i = 0; i < total; ++i) {
        if (m_panelSnapshot.at(i).url == currentUrl) { cur = i; break; }
    }
    if (cur < 0)
        cur = 0; // 現在位置が列の中に無い（差し替え直後等）。先頭から残す。

    // 前25件を基準に幅50の窓を取り、端にぶつかった分は反対側へ回す
    // （＝先頭付近なら後ろを多めに、末尾付近なら前を多めに残す）。
    int start = cur - kSnapshotWindowBefore;
    int end   = start + kSnapshotWindowMax;   // 末尾は含まない
    if (start < 0)      { end -= start; start = 0; }
    if (end > total)    { start -= (end - total); end = total; }
    start = qMax(0, start);

    m_panelSnapshot = m_panelSnapshot.mid(start, end - start);

    // 窓から出た項目の並べ替え記録は捨てる（無限に伸びるのを防ぐ）。
    // 戻ってきた項目はプラグインの順で後ろに付き直す。
    if (!m_panelOrderOverride.isEmpty()) {
        QSet<QString> alive;
        alive.reserve(m_panelSnapshot.size());
        for (const PanelSnapshotItem &it : m_panelSnapshot)
            alive.insert(it.url);
        QStringList kept;
        kept.reserve(m_panelOrderOverride.size());
        for (const QString &url : m_panelOrderOverride)
            if (alive.contains(url))
                kept << url;
        if (kept.size() != m_panelOrderOverride.size()) {
            m_panelOrderOverride = kept;
            ++m_panelOrderRevision;   // 描画・並べ替えキャッシュを作り直させる
        }
    }
}

QJsonObject CanvasVideo::panelSnapshotToJson() const
{
    if (m_panelSnapshot.isEmpty() && m_panelOrderOverride.isEmpty())
        return QJsonObject();

    QJsonArray items;
    for (const PanelSnapshotItem &it : m_panelSnapshot) {
        QJsonObject o;
        o.insert(QStringLiteral("u"), it.url);
        if (!it.title.isEmpty())    o.insert(QStringLiteral("t"),  it.title);
        if (!it.thumbUrl.isEmpty()) o.insert(QStringLiteral("th"), it.thumbUrl);
        if (it.durationSec > 0)     o.insert(QStringLiteral("d"),  it.durationSec);
        items.append(o);
    }

    QJsonObject json;
    if (!items.isEmpty())
        json.insert(QStringLiteral("items"), items);
    if (!m_panelOrderOverride.isEmpty())
        json.insert(QStringLiteral("order"),
                    QJsonArray::fromStringList(m_panelOrderOverride));
    return json;
}

void CanvasVideo::restorePanelSnapshotFromJson(const QJsonObject &json)
{
    m_panelSnapshot.clear();
    const QJsonArray items = json.value(QStringLiteral("items")).toArray();
    m_panelSnapshot.reserve(items.size());
    for (const QJsonValue &v : items) {
        const QJsonObject o = v.toObject();
        const QString url = o.value(QStringLiteral("u")).toString();
        if (url.isEmpty())
            continue;
        m_panelSnapshot.append(PanelSnapshotItem{
            url,
            o.value(QStringLiteral("t")).toString(),
            o.value(QStringLiteral("th")).toString(),
            o.value(QStringLiteral("d")).toInt()});
    }

    m_panelOrderOverride.clear();
    const QJsonArray order = json.value(QStringLiteral("order")).toArray();
    for (const QJsonValue &v : order) {
        const QString url = v.toString();
        if (!url.isEmpty())
            m_panelOrderOverride << url;
    }
    ++m_panelOrderRevision;

    // 復元直後に一覧を出せるよう、組み立て済みのキャッシュを捨てておく。
    m_panelRowsCache.clear();
    m_panelRowsCacheUrl.clear();
    ++m_panelRowsGen;
}

// 2026-09-22: 一覧の行のサムネイルの仕上げ（表示サイズの2倍へ縮めて16:9寄りに
// 中央で切り抜く）。プラグインから来た画像とローカルの動画のサムネイルで共通。
// ワーカースレッドから呼ぶ。
static QImage finishPanelThumbImage(QImage image)
{
    if (image.isNull())
        return image;
    const QSize target(qRound(kThumbWidth * 2.0), qRound(kThumbHeight * 2.0));
    const QSize expanded = image.size().scaled(target, Qt::KeepAspectRatioByExpanding);
    image = image.scaled(expanded, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QRect crop((expanded.width()  - target.width())  / 2,
                     (expanded.height() - target.height()) / 2,
                     target.width(), target.height());
    return image.copy(crop).convertToFormat(QImage::Format_RGBA8888);
}

// 2026-09-22: 本体のリストに並ぶローカルの動画の行サムネイル用のキー。
// 行の thumbUrl にこの接頭辞＋ファイルパスを入れておき、requestPanelThumbnail()
// でプラグインではなく Explorer のサムネイルから取る。
static const QString kPanelLocalThumbPrefix = QStringLiteral("pb-local-thumb:");

static QString panelLocalThumbKey(const MediaSource &s)
{
    if (s.kind != MediaKind::LocalVideo && s.kind != MediaKind::LocalAudio)
        return QString();
    const QString local = QUrl(s.originalPathOrUrl).toLocalFile();
    return kPanelLocalThumbPrefix + (local.isEmpty() ? s.originalPathOrUrl : local);
}

QList<CanvasVideo::PanelRow> CanvasVideo::buildPanelRows() const
{
    // 2026-09-12: 毎フレーム組み直さない。中身が変わりうるのは
    //  (1) 再生中の動画が変わったとき（＝現在位置の行が動く）
    //  (2) プラグインの裏取得が進んで一覧が増えたとき
    // だけで、(2)は急ぐ必要がないので一定間隔で見直せば足りる。
    constexpr qint64 kRowsCacheLifeMs = 1000;
    if (!m_panelRowsCache.isEmpty()
        && m_panelRowsCacheUrl == m_source.originalPathOrUrl
        && m_panelRowsCacheAge.isValid()
        && m_panelRowsCacheAge.elapsed() < kRowsCacheLifeMs) {
        return m_panelRowsCache;
    }

    // 2026-09-22: 本体のリスト。並びは本体が持っているのでプラグインには聞かない。
    if (hasCoreList()) {
        const int cur = coreListCurrentIndex();
        QList<PanelRow> rows;
        rows.reserve(m_coreList.size());
        for (int i = 0; i < m_coreList.size(); ++i) {
            const MediaSource &s = m_coreList.at(i);
            const bool isCurrent = (i == cur);
            QString rowTitle = isCurrent ? title() : s.title;
            if (rowTitle.isEmpty())
                rowTitle = s.originalPathOrUrl;
            // ローカルの動画は Explorer のサムネイル、プラグインの動画は画像のURL。
            QString thumb = panelLocalThumbKey(isCurrent ? m_source : s);
            if (thumb.isEmpty()) {
                thumb = s.thumbnailPathOrUrl;
                if (isCurrent && thumb.isEmpty())
                    thumb = m_thumbnailUrl;
            }
            rows << PanelRow{isCurrent ? QStringLiteral("NOW") : QString::number(i + 1),
                             rowTitle, s.originalPathOrUrl, thumb, 0};
        }
        m_panelRowsCache    = rows;
        m_panelRowsCacheUrl = m_source.originalPathOrUrl;
        m_panelRowsCacheAge.restart();
        ++m_panelRowsGen;
        return rows;
    }

    // プラグインへの問い合わせは同期・キャッシュ参照のみ（PbPlaylistApi.h）。
    // 取得済みのウィンドウ全体（最大50件）をそのまま一覧として並べたいので、
    // 前後とも大きめに要求する（手元にある分だけが返る）。
    const PbPlaylistContext ctx =
        PluginManager::instance().playlistContext(m_source.originalPathOrUrl, 200, 200);

    // 別の列へ移っていたら、覚えていた一覧はここで捨てる。
    dropPanelSnapshotIfListChanged(ctx);

    QList<PanelRow> rows;
    const auto keep = [this](QList<PanelRow> built) {
        m_panelRowsCache    = built;
        m_panelRowsCacheUrl = m_source.originalPathOrUrl;
        m_panelRowsCacheAge.restart();
        ++m_panelRowsGen;   // 並べ替え結果のキャッシュを無効化するための世代
        return built;
    };

    // 2026-09-12: プラグインの答えはスナップショットへ溶かし込み、表示は
    // 常にスナップショットから組む。こうしておくと
    //  ・セッション復元直後は通信ゼロでそのまま出せる（答えが返ればマージ）
    //  ・プラグインがまだ答えられない間（pending）も前回の一覧を出せる
    //  ・窓の切り詰め（約50件）が一箇所で済む
    if (ctx.valid && !ctx.items.isEmpty())
        mergePluginItemsIntoSnapshot(ctx.items);

    // 現在位置を決めてから窓を切り詰める（切り詰めの基準になるため）。
    const QString currentUrl = resolvePanelCurrentUrl(ctx);
    trimPanelSnapshotWindow(currentUrl);

    // 現在位置が特定できないと、再生中の行のイコライザが出ず、▶▶ と
    // 自動送りも列を辿れない。黙って壊れると気付けないので一度だけ出す。
    if (currentUrl.isEmpty() && !m_panelSnapshot.isEmpty()
        && m_panelCurrentMissUrl != m_source.originalPathOrUrl) {
        m_panelCurrentMissUrl = m_source.originalPathOrUrl;
        qDebug().noquote() << QStringLiteral(
            "[LabelPanel][CanvasVideo %1] current row not identified "
            "(rows=%2 pluginIdx=%3 valid=%4 pending=%5) url=%6")
                .arg(reinterpret_cast<quintptr>(this), 0, 16)
                .arg(m_panelSnapshot.size()).arg(ctx.currentIndex)
                .arg(ctx.valid).arg(ctx.pending).arg(m_source.originalPathOrUrl);
    }

    if (!m_panelSnapshot.isEmpty()) {
        for (int i = 0; i < m_panelSnapshot.size(); ++i) {
            const PanelSnapshotItem &it = m_panelSnapshot.at(i);
            const bool isCurrent = (it.url == currentUrl);
            rows << PanelRow{isCurrent ? QStringLiteral("NOW")
                                       : QString::number(i + 1),
                             isCurrent && it.title.isEmpty() ? title() : it.title,
                             it.url, it.thumbUrl, it.durationSec};
        }
        return keep(rows);
    }

    // 並びが取れない場合（navigator未実装のプラグイン、まだ裏取得が
    // 終わっていない等）の最小構成。履歴と連続再生用URLだけで組む。
    if (!m_playHistory.isEmpty()) {
        const PlayHistoryEntry &h = m_playHistory.last();
        rows << PanelRow{QStringLiteral("PREV"), h.title, h.url, QString(), 0};
    }
    rows << PanelRow{QStringLiteral("NOW"), title(), m_source.originalPathOrUrl,
                     QString(), 0};
    if (!m_nextUrl.isEmpty())
        rows << PanelRow{QStringLiteral("NEXT"), QString(), m_nextUrl, QString(), 0};

    return keep(rows);
}

bool CanvasVideo::handleLabelPanelWheel(const QPointF &pos, int angleDelta)
{
    if (!m_labelPanelOpen || m_labelPanelRect.isEmpty()
        || !m_labelPanelRect.contains(pos))
        return false;

    const double maxScroll = qMax(0.0, m_labelPanelContentHeight
                                       - m_labelPanelViewRect.height());
    if (maxScroll <= 0.0)
        return true; // 全部見えている。キャンバスのズームには渡さない。

    // ホイール1ノッチ(120)で約1.5行分。ここでは「行き先」だけを動かし、
    // 実際の位置は updatePlaybackBarAnim() で毎フレーム寄せていく
    // （1ノッチごとにカクッと飛ぶのを避けるため）。連続して回した分は
    // 行き先に積み上がるので、速く回せばそのぶん速く流れる。
    m_labelPanelScrollTarget = qBound(0.0,
        m_labelPanelScrollTarget - (angleDelta / 120.0) * kPanelRowHeight * 1.5,
        maxScroll);
    return true;
}

// 板の形だけを求める（描かない）。2026-09-12: 描いた結果をキャッシュする
// ようにしたため、「描く前に矩形を知る」必要が出て切り出した。
CanvasVideo::PlateGeometry
CanvasVideo::labelPanelPlateGeometry(const QRectF &labelRect, int windowW, int windowH,
                                     int rowCount) const
{
    PlateGeometry geo;

    static QEasingCurve s_ease(QEasingCurve::OutCubic);
    // 行が1つ（＝現在の動画しか出せない）ときは伸ばす意味がないので、
    // ラベルの台の形のまま据え置く。
    const double t = (rowCount > 1)
        ? s_ease.valueForProgress(qBound(0.0f, m_labelPanelAnim, 1.0f))
        : 0.0;

    // 開き切ったときの形。ラベルの台を頭（ヘッダ）として、その下へ行が伸びる。
    //
    // 幅: 画面上部中央の島（DynamicIsland）と重ならないことを優先する。
    // 島の左端までの空きに収め、それが狭すぎる場合だけ最低幅で妥協する
    // （島が無い/取れない場合はウィンドウ幅から普通に決める）。
    // 島が出ている間の退避は「一度たたむ」形で行う（setLabelPanelSuppressed。
    // CanvasWidget側のtickが毎フレーム設定し、ここは普段どおり伸び具合
    // （m_labelPanelAnim）に従って描くだけ）。
    const double available = windowW - labelRect.left() - 16.0;
    const double targetW = qBound(kPanelMinWidth,
                                  qMin(kPanelWidth, windowW * 0.42),
                                  qMax(kPanelMinWidth, available));

    // 高さ: 行数ぶん伸ばすが、ウィンドウの下端（再生バーの手前）までに収め、
    // 収まらない分はスクロールで見る。
    const double rowsTopOffset = labelRect.height() + kPanelPadding;
    const double contentH = kPanelRowHeight * rowCount;
    // 2026-09-12: 画面が高いほど板も長く伸びていたが、板の裏はBackdropBlurで
    // ぼかしているため、伸びたぶんだけ毎フレームのぼかし対象が広がる
    // （実測で448x1792まで育っていた＝大きいウィンドウほど重い直接の原因）。
    // 一度に見せる行数に上限を設け、それ以上はスクロールで見てもらう。
    // 2026-09-13: 上限を7行→10行へ。窓が十分に高いときは動画UI（再生バー）の
    // 手前まで伸ばしてよい、という方針に合わせた。ただし「窓が高いほど
    // 際限なく伸びる」のは避ける（ぼかし対象が広がって重くなる）ので、
    // 10行ぶんで頭打ちにする。実際の高さは
    //   行数ぶん / 10行ぶん / 再生バーの手前まで
    // のいちばん小さいものになる。
    constexpr int kMaxVisibleRows = 10;
    const double cappedH = rowsTopOffset + kPanelRowHeight * kMaxVisibleRows
                           + kPanelPadding;
    // 下端の余白。2026-09-13: 数字の決め打ち(96)をやめ、動画UI（再生バーの
    // 丸ボタン）の位置から導く。
    //
    //   「動画UIの下端 〜 ウィンドウの底」と
    //   「動画UIの上端 〜 リストの底」を同じ間合いにする。
    //
    // 丸ボタンは by = H - kPlaybarCenterFromBottom を中心とする半径
    // kPlaybarButtonR の円（paintPlaybackOverlay と同じ値）なので、
    //   下の間合い = 28 - 16 = 12
    //   UIの上端   = H - 44
    //   リストの底 = H - 44 - 12 = H - 56
    // となり、余白は 56。ウィンドウが高くなればそのぶんリストは伸び、
    // 上限は kMaxVisibleRows 行ぶんで頭打ち（それ以上は伸びずスクロール）。
    constexpr double kPlaybarCenterFromBottom = 28.0; // paintPlaybackOverlay の by
    constexpr double kPlaybarButtonR          = 16.0; // 同 btnR（ポップ等倍時）
    constexpr double kPlaybarBottomGap =
        kPlaybarCenterFromBottom - kPlaybarButtonR;   // UIの下端〜窓の底
    constexpr double kPlaybarClearance =
        kPlaybarCenterFromBottom + kPlaybarButtonR + kPlaybarBottomGap;
    const double maxPlateH = qMin(cappedH,
                                  qMax(rowsTopOffset + kPanelRowHeight + kPanelPadding,
                                       windowH - labelRect.top() - kPlaybarClearance));
    const double targetH = qMin(rowsTopOffset + contentH + kPanelPadding, maxPlateH);

    geo.plate = QRectF(labelRect.left(), labelRect.top(),
                       labelRect.width()  + (targetW - labelRect.width())  * t,
                       labelRect.height() + (targetH - labelRect.height()) * t);

    // 角丸はラベルの台のまま（左はアイコンと同心、右はカプセル）。
    // 【2026-09-12】以前はここから kPanelRadius(14) へ補間していたが、
    // ラベルの台の丸み（高さの半分＝約16）より小さいため、開いた瞬間に
    // 角が少し角張って見えていた（アイコンとの収まりも崩れる）。
    // 伸びても丸みは変えない方が、同じ板が伸びたように見える。
    geo.leftRadius  = CanvasVideoLabelPainter::pillLeftRadius(labelRect.height());
    geo.rightRadius = labelRect.height() * 0.5;
    geo.valid = true;
    return geo;
}

// キャッシュした板の画像が使えるかどうかの判定に使う値。見た目に影響する
// ものだけを混ぜる（paintPlaybackOverlay のキャッシュのコメント参照）。
CanvasVideo::PanelCacheKeyParts CanvasVideo::labelPanelCacheParts(
    const QList<PanelRow> &rows, const PlateGeometry &geo, bool withRows) const
{
    const auto mix = [](quint64 h, quint64 v) {
        return (h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
    };
    PanelCacheKeyParts p;
    p.plateL = qRound(geo.plate.left()   * 4.0);
    p.plateT = qRound(geo.plate.top()    * 4.0);
    p.plateW = qRound(geo.plate.width()  * 4.0);
    p.plateH = qRound(geo.plate.height() * 4.0);
    p.radL   = qRound(geo.leftRadius  * 4.0);
    p.radR   = qRound(geo.rightRadius * 4.0);
    // フェードは板全体の不透明度として効くので、段階が変われば描き直す。
    // 2026-09-14: フェードの濃さはキーに入れない。
    // 以前は 64段階で入れていたため、フェード中は毎フレーム別キーになり、
    // 2.2msの再構築が走り続けていた（[LabelPanel][stat] の why=fade）。
    // 板も行も fade=1.0 で焼いておき、貼るときに painter の不透明度で
    // 掛けるようにしたので、濃さが変わってもキャッシュはそのまま使える。
    // （値は残しておく。0固定なので差分マスクにも出ない）
    p.fadeStep = 0;
    p.accent   = cachedAccentColor().rgba();
    p.withRows = withRows ? 1 : 0;
    if (withRows) {
        p.scroll    = qRound(m_labelPanelScroll);
        p.orderRev  = m_panelOrderRevision;
        // 並べ替え中は隙間の位置で見た目が変わる。
        p.dragIndex = m_rowDragIndex + 1;
        // 【重要】1/8行（≒5.5px）刻みで丸めていたときは、行が5px単位で
        // 飛んでガクガクして見えた。実際に動く量はピクセルなので、
        // ピクセル単位で丸める（＝1px動いたら描き直す）。
        p.gapAnim   = qRound(m_rowGapAnim * kPanelRowHeight) + 1;
        p.dragMoved = m_rowDragMoved ? 1 : 0;
        p.rowCount  = rows.size();
        // サムネイルが届いた／失敗したら描き直す。
        p.thumbs      = m_panelThumbs.size();
        p.thumbFailed = m_panelThumbFailed.size();
        quint64 h = 1469598103934665603ULL;
        for (const PanelRow &r : rows) {
            h = mix(h, qHash(r.url));
            h = mix(h, qHash(r.kind));
            h = mix(h, qHash(r.title));
        }
        p.rowsHash = h;
    }
    return p;
}

// 2026-09-14: 板（ガラスの台）だけを決める値。スクロール量も行の内容も
// 入れない。これが変わらない限り、行がいくら動いても板は焼き直さない。
// 【追加するときの注意】paintLabelPanelPlate() が新しく参照するように
// なった状態は、必ずここにも足すこと。
quint64 CanvasVideo::hashPlateCacheParts(const PanelCacheKeyParts &p)
{
    const auto mix = [](quint64 h, quint64 v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    };
    quint64 h = 0xcbf29ce484222325ULL;
    h = mix(h, static_cast<quint64>(p.plateL));
    h = mix(h, static_cast<quint64>(p.plateT));
    h = mix(h, static_cast<quint64>(p.plateW));
    h = mix(h, static_cast<quint64>(p.plateH));
    h = mix(h, static_cast<quint64>(p.radL));
    h = mix(h, static_cast<quint64>(p.radR));
    h = mix(h, static_cast<quint64>(p.accent));
    return h;
}

quint64 CanvasVideo::hashPanelCacheParts(const PanelCacheKeyParts &p)
{
    const auto mix = [](quint64 h, quint64 v) {
        return (h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
    };
    quint64 h = 1469598103934665603ULL;
    h = mix(h, static_cast<quint64>(p.plateL));
    h = mix(h, static_cast<quint64>(p.plateT));
    h = mix(h, static_cast<quint64>(p.plateW));
    h = mix(h, static_cast<quint64>(p.plateH));
    h = mix(h, static_cast<quint64>(p.radL));
    h = mix(h, static_cast<quint64>(p.radR));
    h = mix(h, static_cast<quint64>(p.fadeStep));
    h = mix(h, static_cast<quint64>(p.withRows));
    h = mix(h, static_cast<quint64>(p.accent));
    if (p.withRows) {
        h = mix(h, static_cast<quint64>(p.scroll));
        h = mix(h, static_cast<quint64>(p.orderRev));
        h = mix(h, static_cast<quint64>(p.dragIndex));
        h = mix(h, static_cast<quint64>(p.gapAnim));
        h = mix(h, static_cast<quint64>(p.dragMoved));
        h = mix(h, static_cast<quint64>(p.rowCount));
        h = mix(h, static_cast<quint64>(p.thumbs));
        h = mix(h, static_cast<quint64>(p.thumbFailed));
        h = mix(h, p.rowsHash);
    }
    return h;
}

quint32 CanvasVideo::diffPanelCacheParts(const PanelCacheKeyParts &a,
                                         const PanelCacheKeyParts &b)
{
    quint32 mask = 0;
    if (a.plateL != b.plateL || a.plateT != b.plateT)   mask |= 1u << 0;
    if (a.plateW != b.plateW || a.plateH != b.plateH)   mask |= 1u << 1;
    if (a.radL != b.radL || a.radR != b.radR)           mask |= 1u << 2;
    if (a.fadeStep != b.fadeStep)                       mask |= 1u << 3;
    if (a.accent != b.accent)                           mask |= 1u << 4;
    if (a.withRows != b.withRows)                       mask |= 1u << 5;
    if (a.scroll != b.scroll)                           mask |= 1u << 6;
    if (a.orderRev != b.orderRev)                       mask |= 1u << 7;
    if (a.dragIndex != b.dragIndex)                     mask |= 1u << 8;
    if (a.gapAnim != b.gapAnim)                         mask |= 1u << 9;
    if (a.dragMoved != b.dragMoved)                     mask |= 1u << 10;
    if (a.rowCount != b.rowCount)                       mask |= 1u << 11;
    if (a.thumbs != b.thumbs)                           mask |= 1u << 12;
    if (a.thumbFailed != b.thumbFailed)                 mask |= 1u << 13;
    if (a.rowsHash != b.rowsHash)                       mask |= 1u << 14;
    return mask;
}

QString CanvasVideo::describePanelCacheMiss(quint32 mask)
{
    if (mask == 0)
        return QStringLiteral("-");
    static const char *const names[] = {
        "platePos", "plateSize", "radius", "fade", "accent", "withRows",
        "scroll", "orderRev", "dragIndex", "gapAnim", "dragMoved",
        "rowCount", "thumbs", "thumbFailed", "rows"
    };
    QStringList out;
    for (int i = 0; i < 15; ++i) {
        if (mask & (1u << i))
            out << QString::fromLatin1(names[i]);
    }
    return out.join(QLatin1Char('+'));
}

quint64 CanvasVideo::labelPanelCacheKey(const QList<PanelRow> &rows,
                                        const PlateGeometry &geo,
                                        bool withRows) const
{
    return hashPanelCacheParts(labelPanelCacheParts(rows, geo, withRows));
}

void CanvasVideo::paintLabelPanelPlate(QPainter &painter, const PlateGeometry &geo,
                                       float fade)
{
    // 島と同じガラス表現で塗る（paintGlassPlate参照）。アクセント色も
    // 島と共有しているので、並んだときに同じ素材に見える。
    paintGlassPlate(painter, roundedPathLR(geo.plate, geo.leftRadius, geo.rightRadius),
                    geo.plate, cachedAccentColor(), fade);
}

// ---- ラベルの縁に出す再生位置（2026-09-13追加）------------------------
//
// 台（閉じているときはカプセル、開いているときはカード）の輪郭に沿って
// 光を一周させる。形は毎フレーム変わりうるが、輪郭そのものを辿るので
// 伸び縮みの最中でも破綻しない。
//
// 出すかどうかはプラグインが決める（PbResolvedMedia::labelProgress）。
// Live のように終わりが無いものでは出さない。位置そのものは本体が持って
// いる値（position/duration）で、プラグインは関与しない。
namespace {
// 線の太さ。台のガラスの縁と喧嘩しない程度に細く。
constexpr double kProgressRingWidth   = 1.8;
// 【合成は加算】ふつうに上から塗ると、ガラスの縁が持っている艶と影を
// 塗りつぶしてしまう（＝質感が消えてのっぺりする）。加算なら暗い所は
// 暗いまま残り、明るい所がさらに明るくなるだけなので、縁の陰影の上に
// 光が乗ったように見える。
// （反転合成も試した。どんな下地でも必ず見える利点はあるが、下地の色に
//   よって光の色が変わってしまい落ち着かないため加算へ戻した。）
//
// 【尾も下地も描かない】時計の針と同じで、上から出て時計回りに一周して
// 上へ帰ってくる、という動きは説明が無くても伝わる。通り過ぎた側に線を
// 残すとその部分のガラスの質感が隠れ続けるので、走っている頭だけを出す。
// 戻したくなったときのために定数は残してある（0で無効）。
constexpr int    kProgressTrackAlpha  = 0;
// 先端（いま再生している位置）の強さ。
constexpr int    kProgressLitAlpha    = 190;
// 通り過ぎた側の強さ。0＝完全に透過（＝何も残さない）。
constexpr int    kProgressTailAlpha   = 0;
// 【先端の鋭さ】先端から何pxぶんを「光っている頭」とみなすか。
// 再生が進んでも長さが変わらないよう、進み具合ではなく実寸で決める。
// ただし台の大きさには合わせる必要がある。リストを開くと台がカードへ
// 伸びて周長が3〜4倍になるため、閉じているとき用の実寸のままだと頭が
// 相対的に小さくなって見えなくなる（実機で確認）。
// 「最低これだけ」と「周長のこれだけ」の大きいほうを採る。
constexpr double kProgressHeadLenPx   = 22.0;
constexpr double kProgressHeadLenFrac = 0.09;
// 頭の中での落ち方。1より大きいほど、先端の直前だけが際立つ。
constexpr double kProgressHeadFalloff = 2.2;
// 折れ線の細かさ（px/点）。
constexpr double kProgressStepPx      = 3.0;
// 頭を何段階の濃さに分けるか。【重要】1本ずつ濃さを変えて短い線を並べると、
// 線の継ぎ目が二重に合成されて数px間隔の粒に見える（実機で確認）。
// 同じ濃さの区間は「1本の連続した折れ線」としてまとめて描き、継ぎ目の数を
// この段数まで減らす。端は突き合わせ（FlatCap）にして重ならないようにする。
constexpr int    kProgressHeadBands   = 12;
} // namespace

void CanvasVideo::paintLabelProgressRing(QPainter &painter, const QRectF &rect,
                                         double leftRadius, double rightRadius,
                                         double progress, double alpha) const
{
    if (alpha <= 0.01 || rect.isEmpty())
        return;
    const QPainterPath outline = roundedPathLR(rect, leftRadius, rightRadius);
    const double len = outline.length();
    if (len <= 2.0)
        return;
    progress = qBound(0.0, progress, 1.0);

    // 【始点＝台の上辺の中央】roundedPathLR() は「左上の角丸が終わった
    // ところ」から時計回りに始まるので、そのままだと光が左上から出て
    // 左上へ帰ってくる。中央から出て中央へ帰るほうが、ラベルの見た目の
    // 中心と一致して落ち着く。輪郭上の位置をずらして辿るだけで、形は
    // いじらない。
    // 上辺は最初の直線なので、その中央までの距離をそのまま割合にできる。
    const double topRun = qMax(0.0, rect.center().x() - (rect.left() + leftRadius));
    const double startPercent = qBound(0.0, topRun / len, 1.0);
    const auto outlinePointAt = [&](double u) {
        // u は始点からの割合。一周を超えたら先頭へ回り込む。
        double t = startPercent + u;
        t -= std::floor(t);
        return outline.pointAtPercent(qBound(0.0, t, 1.0));
    };

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    // 加算合成。ガラスの縁の陰影を潰さずに光だけを足す（上のコメント参照）。
    painter.setCompositionMode(QPainter::CompositionMode_Plus);

    // 一周ぶんの下地。既定では描かない（kProgressTrackAlpha = 0）。
    if (kProgressTrackAlpha > 0) {
        QPen track(QColor(255, 255, 255,
                          qBound(0, qRound(kProgressTrackAlpha * alpha), 255)));
        track.setWidthF(kProgressRingWidth);
        painter.setPen(track);
        painter.drawPath(outline);
    }

    if (progress > 0.0005) {
        // 進んだぶんを「尾」と「頭」に分けて描く。
        //   尾 … 一定の弱さで一気に1本。通り過ぎた道筋を示すだけ。
        //   頭 … 先端から kProgressHeadLenPx ぶんだけ。ここで濃さを
        //         段階的に上げて、鋭い光として走らせる。
        // 頭の長さを実寸で取るので、再生が進んでも鋭さが変わらない。
        const double arcLen  = len * progress;
        const double headLen = qMin(arcLen,
                                    qMax(kProgressHeadLenPx,
                                         len * kProgressHeadLenFrac));
        const double headU   = headLen / len;            // 頭の割合
        const double tailEnd = progress - headU;         // 尾の終わり

        QPen seg;
        seg.setWidthF(kProgressRingWidth);
        seg.setCapStyle(Qt::FlatCap);    // 端どうしを突き合わせる
        seg.setJoinStyle(Qt::RoundJoin); // 角丸の曲がり角はなめらかに

        const auto strokeRange = [&](double u0, double u1, int alphaValue) {
            if (u1 - u0 <= 1e-6)
                return;
            const int n = qBound(2, qRound(len * (u1 - u0) / kProgressStepPx), 300);
            QPainterPath poly;
            for (int i = 0; i <= n; ++i) {
                const QPointF pt = outlinePointAt(u0 + (u1 - u0) * i / n);
                if (i == 0) poly.moveTo(pt); else poly.lineTo(pt);
            }
            seg.setColor(QColor(255, 255, 255,
                                qBound(0, qRound(alphaValue * alpha), 255)));
            painter.setPen(seg);
            painter.drawPath(poly);
        };

        // 尾。既定では描かない（kProgressTailAlpha = 0）。
        if (kProgressTailAlpha > 0)
            strokeRange(0.0, qMax(0.0, tailEnd), kProgressTailAlpha);

        // 頭（先端へ向かって強くなる）
        for (int b = 0; b < kProgressHeadBands; ++b) {
            const double t0 = static_cast<double>(b)     / kProgressHeadBands;
            const double t1 = static_cast<double>(b + 1) / kProgressHeadBands;
            const double mid = (t0 + t1) * 0.5;          // 0=頭の根元, 1=先端
            const double mix = std::pow(mid, kProgressHeadFalloff);
            const int a = qRound(kProgressTailAlpha
                                 + (kProgressLitAlpha - kProgressTailAlpha) * mix);
            if (a <= 0)
                continue;
            strokeRange(qMax(0.0, tailEnd) + headU * t0,
                        qMax(0.0, tailEnd) + headU * t1, a);
        }
    }
    painter.restore();

    m_labelProgressPainted = progress;
}

// 前回描いたときから、見て分かるだけ進んだか。毎フレーム描き直すと
// オーバーレイ全体の再描画が復活してしまうので、1/512周ぶん
// （カプセルなら1px弱）動いたときだけ描き直す。
bool CanvasVideo::labelProgressNeedsRepaint() const
{
    if (m_labelFadeAlpha <= 0.01f)
        return false;
    // 出入りのフェードが動いている間は毎フレーム描き直す（短時間）。
    const qint64 dur = duration();
    // 【注意】行き先の式は updateLabelAnims() と必ず揃えること。ずれると
    // 「まだ動いているのにフレームを要求しない」＝途中で止まって見える。
    double target = 0.0;
    if (m_labelProgressEffective && dur > 0) {
        constexpr double kProgressEndFadeMs = 1200.0;
        target = qBound(0.0, static_cast<double>(dur - position())
                                 / kProgressEndFadeMs, 1.0);
    }
    if (std::abs(m_labelProgressFade - target) > 0.002)
        return true;
    if (!m_labelProgressEffective || dur <= 0)
        return false;
    const double now = qBound(0.0, static_cast<double>(position()) / dur, 1.0);
    if (m_labelProgressPainted < 0.0)
        return now > 0.0;
    constexpr double kStep = 1.0 / 512.0;
    return std::abs(now - m_labelProgressPainted) >= kStep;
}

// ===== 行の「帯」キャッシュ（2026-09-14 B-4）==============================
//
// 【なぜ】行のキャッシュキーに qRound(m_labelPanelScroll) が入っていたため、
// 1px 動くだけで27行ぶん（サムネイル・文字・影）を描き直していた。実測で
//     [LabelPanel][stat] build=2.7ms why=scroll rowMiss=105/120
// ＝スクロール中はほぼ毎フレーム全部焼き直し。中身は何ひとつ変わって
// いないのに、見えている窓がずれただけ。
//
// 【どうする】行を「縦に長い1枚の帯」として焼く。キーは中身だけで、
// スクロール量は入れない。スクロールは drawImage の y をずらすだけになる。
// 行数が多いプレイリストでメモリが膨らまないよう、全行ではなく
// 「見えている範囲の前後 kStripPadRows 行」だけを窓として焼き、その窓から
// 出たときだけ焼き直す（＝8行ぶんスクロールするまでキャッシュが効く）。
//
// 【見た目が変わらない根拠】
//  - 行の中身（paintPanelRowContent）は rowRect からの相対座標だけで描いて
//    いるので、帯の中のどこに置いても同じ絵になる。
//  - 濃さは焼くとき 1.0 固定にして、貼るときに painter の不透明度で掛ける
//    （B-3 と同じ考え方）。伸び縮みの最中（reveal < 1）は影の付き方が
//    濃さに対して線形でないので、帯は使わず従来経路へ落とす。
//  - 並べ替えドラッグ中は行の並び自体が毎フレーム変わるので、同じく従来経路。
//  - 上下の端のぼかしはスクロール量で変わるので、貼るときに従来と同じ式で
//    掛ける（一度スクラッチ画像へ受けてから DestinationIn。板を削らないため）。
namespace {
constexpr int kStripPadRows = 8;   // 見えている範囲の前後に焼いておく行数
}

// 帯の「中身」のキー。窓の位置（先頭行・行数）は入れない。窓がずれただけの
// ときに、前の帯から重なっている行を貼り直せるようにするため（B-5）。
quint64 CanvasVideo::hashStripContentKey(const PanelCacheKeyParts &p,
                                         int widthQ, int dprQ)
{
    const auto mix = [](quint64 h, quint64 v) {
        return (h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
    };
    quint64 h = 1469598103934665603ULL;
    h = mix(h, p.rowsHash);
    h = mix(h, static_cast<quint64>(p.rowCount));
    h = mix(h, static_cast<quint64>(p.orderRev));
    h = mix(h, static_cast<quint64>(p.thumbs));
    h = mix(h, static_cast<quint64>(p.thumbFailed));
    h = mix(h, static_cast<quint64>(widthQ));
    h = mix(h, static_cast<quint64>(dprQ));
    return h;
}

// 帯を用意する。使える状態にできたら true。
bool CanvasVideo::ensureLabelPanelStrip(const QRectF &plateRect,
                                        const QRectF &labelRect,
                                        const QList<PanelRow> &rows,
                                        const PanelCacheKeyParts &parts,
                                        qreal dpr)
{
    if (rows.isEmpty())
        return false;
    const double viewTop = plateRect.top() + labelRect.height() + kPanelPadding;
    const QRectF viewRect(plateRect.left() + kPanelPadding, viewTop,
                          plateRect.width() - kPanelPadding * 2.0,
                          qMax(0.0, plateRect.bottom() - kPanelPadding - viewTop));
    if (viewRect.width() < 1.0 || viewRect.height() < 1.0)
        return false;

    const int n = rows.size();
    const double maxScroll =
        qMax(0.0, kPanelRowHeight * n - viewRect.height());
    const double scroll = qBound(0.0, m_labelPanelScroll, maxScroll);
    const int visFirst = qBound(
        0, static_cast<int>(std::floor(scroll / kPanelRowHeight)), n - 1);
    const int visLast = qBound(
        0, static_cast<int>(std::floor((scroll + viewRect.height()) / kPanelRowHeight)),
        n - 1);

    const int widthQ = qRound(viewRect.width() * 4.0);
    const int dprQ   = qRound(dpr * 100.0);

    // 「中身」が同じかどうか（窓の位置は含めない）。同じなら、窓がずれた
    // ときに重なっている行を前の帯から貼り直せる（B-5）。
    const quint64 contentKey = hashStripContentKey(parts, widthQ, dprQ);
    const bool sameContent = !m_panelStripCache.isNull()
        && m_panelStripContentKey == contentKey;

    // いまの帯が「見えている範囲」を覆っていて、中身も変わっていないなら
    // そのまま使う。ここが当たっている間はスクロールしても焼き直さない。
    if (sameContent
        && m_panelStripFirst <= visFirst
        && m_panelStripFirst + m_panelStripCount - 1 >= visLast) {
        ++m_panelStatStripHits;
        return true;
    }

    const int first = qMax(0, visFirst - kStripPadRows);
    const int last  = qMin(n - 1, visLast + kStripPadRows);
    const int count = last - first + 1;

    const QSize px(static_cast<int>(viewRect.width() * dpr) + 1,
                   static_cast<int>(kPanelRowHeight * count * dpr) + 1);
    if (px.width() < 1 || px.height() < 1
        || static_cast<qint64>(px.width()) * px.height() > 32LL * 1024 * 1024)
        return false;   // 想定外に大きいときは従来経路へ

    QImage img(px, QImage::Format_ARGB32_Premultiplied);
    if (img.isNull())
        return false;
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);

    // ---- 窓がずれただけなら、重なっている行は前の帯から貼る（B-5）----
    // 【なぜ】50行のリストを勢いよく送ると、窓から出るたびに27行ぶんを
    // 焼き直していた。実測 strip=5.5ms × 2秒で19回＝約103ms。平均は下がって
    // いても、この5.5msの山がフレーム落ちとして「重い」と感じる原因になる。
    // 帯の中身はスクロール量に依存しないので、窓がずれただけのときは
    // ただの平行移動で済む。実際に描き直すのは新しく入ってきた行だけ。
    int overlapFirst = first;
    int overlapLast  = first - 1;   // 空
    if (sameContent) {
        overlapFirst = qMax(first, m_panelStripFirst);
        overlapLast  = qMin(last, m_panelStripFirst + m_panelStripCount - 1);
    }
    const bool hasOverlap = (overlapLast >= overlapFirst);

    {
        QPainter sp(&img);
        if (hasOverlap) {
            // 前の帯の該当部分をそのまま貼る（等倍・オフセットのみ）。
            // 元画像の座標はデバイスピクセルで指定する。
            const double rowPx = kPanelRowHeight * dpr;
            const QRectF src(0.0, rowPx * (overlapFirst - m_panelStripFirst),
                             m_panelStripCache.width(),
                             rowPx * (overlapLast - overlapFirst + 1));
            sp.drawImage(QPointF(0.0, kPanelRowHeight * (overlapFirst - first)),
                         m_panelStripCache, src);
        }
        sp.setRenderHint(QPainter::Antialiasing, true);
        sp.setRenderHint(QPainter::TextAntialiasing, true);
        sp.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (int i = first; i <= last; ++i) {
            if (hasOverlap && i >= overlapFirst && i <= overlapLast)
                continue;   // 貼り直した行は描かない
            const QRectF rowRect(0.0, kPanelRowHeight * (i - first),
                                 viewRect.width(), kPanelRowHeight);
            paintPanelRowContent(sp, rowRect, rows.at(i),
                                 rows.at(i).kind == QStringLiteral("NOW"),
                                 /*alpha=*/1.0, /*requestThumbs=*/true);
            ++m_panelStatStripRedrawRows;
        }
        sp.end();
    }
    m_panelStripCache      = img;
    m_panelStripFirst      = first;
    m_panelStripCount      = count;
    m_panelStripWidthQ     = widthQ;
    m_panelStripDprQ       = dprQ;
    m_panelStripContentKey = contentKey;
    ++m_panelStatStripMisses;
    if (hasOverlap)
        ++m_panelStatStripSlides;
    return true;
}

// 帯を貼る。ヒット判定用の矩形もここで作り直す（描画はしない）。
void CanvasVideo::blitLabelPanelStrip(QPainter &painter, const QRectF &plateRect,
                                      const QRectF &labelRect,
                                      const QList<PanelRow> &rows)
{
    if (m_panelStripCache.isNull() || rows.isEmpty())
        return;
    QElapsedTimer blitTimer;
    blitTimer.start();
    struct BlitStat {
        CanvasVideo *self; QElapsedTimer *t;
        ~BlitStat() {
            self->m_panelStatBlitMs += t->nsecsElapsed() / 1e6;
            ++self->m_panelStatBlitCalls;
        }
    } blitStat{this, &blitTimer};
    const double viewTop = plateRect.top() + labelRect.height() + kPanelPadding;
    const QRectF viewRect(plateRect.left() + kPanelPadding, viewTop,
                          plateRect.width() - kPanelPadding * 2.0,
                          qMax(0.0, plateRect.bottom() - kPanelPadding - viewTop));
    if (viewRect.width() < 1.0 || viewRect.height() < 1.0)
        return;

    m_labelPanelViewRect      = viewRect;
    m_labelPanelContentHeight = kPanelRowHeight * rows.size();

    // 現在の曲が変わったら、その行が見える位置へ寄せ直す
    // （paintLabelPanelRowsBody と同じ処理。帯経路では本体を通らないため）。
    int currentIndex = -1;
    for (int i = 0; i < rows.size(); ++i) {
        if (rows.at(i).kind == QStringLiteral("NOW")) { currentIndex = i; break; }
    }
    const double maxScroll = qMax(0.0, m_labelPanelContentHeight - viewRect.height());
    if (m_labelPanelScrollAnchorUrl != m_source.originalPathOrUrl) {
        const bool firstAnchor = m_labelPanelScrollAnchorUrl.isEmpty();
        m_labelPanelScrollAnchorUrl = m_source.originalPathOrUrl;
        if (currentIndex >= 0) {
            const double want =
                qBound(0.0, kPanelRowHeight * (currentIndex - 1), maxScroll);
            m_labelPanelScrollTarget = want;
            const bool listVisible = m_labelPanelOpen && m_labelPanelAnim > 0.05f;
            if (firstAnchor || !listVisible)
                m_labelPanelScroll = want;
        }
    }
    m_labelPanelScrollTarget = qBound(0.0, m_labelPanelScrollTarget, maxScroll);
    m_labelPanelScroll       = qBound(0.0, m_labelPanelScroll, maxScroll);

    // 2026-09-14 C-3: 貼る位置を「論理ピクセル単位」ではなく
    // 「デバイスピクセル単位」で丸める。
    // 【なぜ】従来は qRound(scroll) で1論理ピクセル刻みだった。dpr=1.5 では
    // 1論理px = 1.5デバイスpx なので、実際の画面では1.5ドットずつ飛んでいた。
    // これがゆっくり流れているときの「コリコリ」の正体。デバイスピクセルに
    // 合わせれば刻みは1.5倍細かくなり、かつグリッドに乗ったままなので
    // 文字はにじまない（半端な位置に貼ると再サンプリングでぼける）。
    const qreal snapDpr = qMax(0.5, m_panelStripCache.devicePixelRatio());
    const double scroll = std::round(m_labelPanelScroll * snapDpr) / snapDpr;

    // ヒット判定用の矩形（本体と同じ式。描かずに計算だけ）。
    for (int i = 0; i < rows.size(); ++i) {
        const QRectF rowRect(viewRect.left(),
                             viewTop + kPanelRowHeight * i - scroll,
                             viewRect.width(), kPanelRowHeight);
        if (rowRect.bottom() < viewRect.top() - 1.0
            || rowRect.top() > viewRect.bottom() + 1.0)
            continue;
        m_labelPanelRowRects   << rowRect;
        m_labelPanelRowUrls    << rows.at(i).url;
        m_labelPanelRowIndices << i;
        if (rows.at(i).kind == QStringLiteral("NOW"))
            m_labelPanelNowRowRect = rowRect;
    }

    const double fade = qMin(kPanelEdgeFade, viewRect.height() * 0.25);
    const double topFade    = (fade > 0.0)
        ? fade * qBound(0.0, scroll / fade, 1.0) : 0.0;
    const double bottomFade = (fade > 0.0)
        ? fade * qBound(0.0, (maxScroll - scroll) / fade, 1.0) : 0.0;
    const QPointF stripPos(viewRect.left(),
                           viewTop + kPanelRowHeight * m_panelStripFirst
                               - scroll);

    painter.save();
    // クリップは本体（paintLabelPanelRowsBody）と必ず同じものにすること。
    painter.setClipPath(roundedPathLR(
        plateRect,
        CanvasVideoLabelPainter::pillLeftRadius(labelRect.height()),
        labelRect.height() * 0.5));
    painter.setClipRect(viewRect, Qt::IntersectClip);

    if (topFade <= 0.0 && bottomFade <= 0.0) {
        painter.drawImage(stripPos, m_panelStripCache);   // マスク不要
        painter.restore();
        return;
    }

    // 端をぼかすには一度別の画像へ受ける（直接 DestinationIn を掛けると
    // 下の板まで抜けてしまう）。画像は使い回してフレームごとの確保を避ける。
    const qreal dpr = m_panelStripCache.devicePixelRatio();
    const QSize px(static_cast<int>(viewRect.width()  * dpr) + 1,
                   static_cast<int>(viewRect.height() * dpr) + 1);
    if (m_panelStripScratch.size() != px
        || !qFuzzyCompare(m_panelStripScratch.devicePixelRatio() + 1.0, dpr + 1.0)) {
        m_panelStripScratch = QImage(px, QImage::Format_ARGB32_Premultiplied);
        if (!m_panelStripScratch.isNull())
            m_panelStripScratch.setDevicePixelRatio(dpr);
    }
    if (m_panelStripScratch.isNull()) {
        painter.drawImage(stripPos, m_panelStripCache);
        painter.restore();
        return;
    }
    m_panelStripScratch.fill(Qt::transparent);
    {
        QPainter lp(&m_panelStripScratch);
        lp.drawImage(stripPos - viewRect.topLeft(), m_panelStripCache);

        // 濃さの変化は二乗（本体と同じ。直線だと真ん中が灰色の帯に見える）。
        const double h = viewRect.height();
        QLinearGradient mask(0.0, 0.0, 0.0, h);
        constexpr int kFadeStops = 5;
        mask.setColorAt(0.0, QColor(0, 0, 0, topFade > 0.0 ? 0 : 255));
        if (topFade > 0.0) {
            for (int i = 1; i <= kFadeStops; ++i) {
                const double t = static_cast<double>(i) / kFadeStops;
                mask.setColorAt(topFade * t / h,
                                QColor(0, 0, 0, qRound(255.0 * t * t)));
            }
        }
        if (bottomFade > 0.0) {
            for (int i = kFadeStops; i >= 1; --i) {
                const double t = static_cast<double>(i) / kFadeStops;
                mask.setColorAt(1.0 - bottomFade * t / h,
                                QColor(0, 0, 0, qRound(255.0 * t * t)));
            }
        }
        mask.setColorAt(1.0, QColor(0, 0, 0, bottomFade > 0.0 ? 0 : 255));
        lp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        lp.fillRect(QRectF(0.0, 0.0, viewRect.width(), h), mask);
    }
    painter.drawImage(viewRect.topLeft(), m_panelStripScratch);
    painter.restore();
}

// 行を透明な画像へ描いてから、上下の端をグラデーションで薄くして貼る。
// 板そのものは下に残したいので、板ごと DestinationIn で削るわけには
// いかない。だから「行だけ」を別レイヤーにしてそこへマスクをかける。
void CanvasVideo::paintLabelPanelRows(QPainter &painter, const QRectF &plateRect,
                                      const QRectF &labelRect,
                                      const QList<PanelRow> &rows)
{
    const double reveal = qBound(0.0, (static_cast<double>(m_labelPanelAnim) - 0.55) / 0.45, 1.0);
    if (reveal <= 0.01)
        return;

    // 本体と同じ式で「行が見えている領域」を先に求める。
    const double viewTop = plateRect.top() + labelRect.height() + kPanelPadding;
    const QRectF viewRect(plateRect.left() + kPanelPadding, viewTop,
                          plateRect.width() - kPanelPadding * 2.0,
                          qMax(0.0, plateRect.bottom() - kPanelPadding - viewTop));
    if (viewRect.width() < 1.0 || viewRect.height() < 1.0)
        return;

    // 端のぼかし幅。狭いときは領域の1/4まで。
    const double fade = qMin(kPanelEdgeFade, viewRect.height() * 0.25);
    const double maxScroll = qMax(0.0, kPanelRowHeight * rows.size() - viewRect.height());
    const double scroll    = qBound(0.0, m_labelPanelScroll, maxScroll);

    // 【2026-09-12】以前は「隠れている行があるか」の二択で、端に着いた
    // 瞬間に帯がパッと消えていた（＝薄いまま近づいて、最後に唐突に
    // 通常表示へ戻る）。隠れている量に応じて帯そのものを縮めることで、
    // 端へ近づくほど自然に薄れていって、着いたときには残っていない。
    // 帯の幅ぶん（fade px）残っていれば最大、0なら帯も0。
    const double topFade    = (fade > 0.0)
        ? fade * qBound(0.0, scroll / fade, 1.0) : 0.0;
    const double bottomFade = (fade > 0.0)
        ? fade * qBound(0.0, (maxScroll - scroll) / fade, 1.0) : 0.0;

    if (topFade <= 0.0 && bottomFade <= 0.0) {
        paintLabelPanelRowsBody(painter, plateRect, labelRect, rows); // マスク不要
        return;
    }

    const qreal dpr = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    QImage layer(QSize(static_cast<int>(viewRect.width()  * dpr) + 1,
                       static_cast<int>(viewRect.height() * dpr) + 1),
                 QImage::Format_ARGB32_Premultiplied);
    if (layer.isNull()) {
        paintLabelPanelRowsBody(painter, plateRect, labelRect, rows);
        return;
    }
    layer.setDevicePixelRatio(dpr);
    layer.fill(Qt::transparent);
    {
        QPainter lp(&layer);
        lp.setRenderHint(QPainter::Antialiasing, true);
        lp.setRenderHint(QPainter::TextAntialiasing, true);
        lp.setRenderHint(QPainter::SmoothPixmapTransform, true);
        lp.save();
        lp.translate(-viewRect.topLeft()); // 本体は画面の座標のまま描ける
        paintLabelPanelRowsBody(lp, plateRect, labelRect, rows);
        lp.restore();

        // 上下だけを薄くする（中央は 1.0 のまま＝何も変わらない）。
        // 濃さの変化は直線ではなく二乗にしてある。直線だと帯の真ん中が
        // ちょうど半分の濃さで「灰色の帯」に見えるが、二乗にすると端の
        // 近くだけが一気に薄くなり、溶けて消えるように見える。
        const double h = viewRect.height();
        QLinearGradient mask(0.0, 0.0, 0.0, h);
        constexpr int kFadeStops = 5; // 端→内側の刻み（多いほど滑らか）
        mask.setColorAt(0.0, QColor(0, 0, 0, topFade > 0.0 ? 0 : 255));
        if (topFade > 0.0) {
            for (int i = 1; i <= kFadeStops; ++i) {
                const double t = static_cast<double>(i) / kFadeStops;
                mask.setColorAt(topFade * t / h,
                                QColor(0, 0, 0, qRound(255.0 * t * t)));
            }
        }
        if (bottomFade > 0.0) {
            for (int i = kFadeStops; i >= 1; --i) {
                const double t = static_cast<double>(i) / kFadeStops;
                mask.setColorAt(1.0 - bottomFade * t / h,
                                QColor(0, 0, 0, qRound(255.0 * t * t)));
            }
        }
        mask.setColorAt(1.0, QColor(0, 0, 0, bottomFade > 0.0 ? 0 : 255));
        lp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        lp.fillRect(QRectF(0.0, 0.0, viewRect.width(), h), mask);
    }
    painter.drawImage(viewRect.topLeft(), layer);
}

// 行1つぶんの中身（ピン・サムネイル・種別/長さ・タイトル）を描く。
// 並べ替えでつかんだ行は、リストの中とは別にカーソルへ追従させて
// 描きたいので、ここだけ切り出して2箇所から呼べるようにしてある。
void CanvasVideo::paintPanelRowContent(QPainter &painter, const QRectF &rowRect,
                                       const PanelRow &row, bool isCurrent,
                                       double alpha, bool requestThumbs)
{
    const QFont kindFont(QStringLiteral("Arial"), 7, QFont::DemiBold);
    const QFont titleFont(QStringLiteral("Arial"), 9);
    const QFontMetrics titleFm(titleFont);

    // 行の左のピン。再生中だけ白く、それ以外は控えめ
    // （どれが今なのかを一列の点で追えるようにするための目印）。
    {
        const QPointF pinCenter(rowRect.left() + kRowPinCenterX,
                                rowRect.center().y());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255,
                                qRound((isCurrent ? 235 : 55) * alpha)));
        const double pinR = isCurrent ? kRowPinRadius : kRowPinRadius - 0.8;
        painter.drawEllipse(pinCenter, pinR, pinR);
    }

    // 再生中の行は敷いて「今ここ」を示す。
    // 2026-09-12: α20だと動画の絵の上ではほぼ見えなかったので、敷きを
    // 濃くし（52）、さらに細い縁を足して輪郭が分かるようにした。
    if (isCurrent) {
        // 枠の左端はロゴの左端に揃える（kRowIconLeftX）。左右の余白は必ず
        // 同じにすること（左だけ詰めると重心がずれて見える）。
        // 【2026-09-12】左右へはみ出させないこと。行はリストの表示領域
        // （viewRect）でクリップされているので、外へ広げると角の丸みが
        // そこで切り落とされて「角が欠けた四角」に見える。
        const QRectF fill(rowRect.left()  + kRowIconLeftX,
                          rowRect.top()   + 2.0,
                          qMax(0.0, rowRect.width() - kRowIconLeftX * 2.0),
                          rowRect.height() - 4.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, qRound(52 * alpha)));
        painter.drawRoundedRect(fill, 10, 10);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(255, 255, 255, qRound(70 * alpha)), 1.0));
        painter.drawRoundedRect(fill.adjusted(0.5, 0.5, -0.5, -0.5), 9.5, 9.5);
    }

    // サムネイル。まだ手元に無ければこの行の分だけ取りに行く。
    const QRectF thumbRect(rowRect.left() + kRowPinWidth,
                           rowRect.center().y() - kThumbHeight * 0.5,
                           kThumbWidth, kThumbHeight);
    const QImage thumb = row.thumbUrl.isEmpty()
        ? QImage() : m_panelThumbs.value(row.thumbUrl);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, qRound(18 * alpha)));
    painter.drawRoundedRect(thumbRect, 8, 8);
    if (!thumb.isNull()) {
        painter.save();
        QPainterPath clip;
        clip.addRoundedRect(thumbRect, 8, 8);
        painter.setClipPath(clip, Qt::IntersectClip);
        painter.setOpacity(painter.opacity() * alpha);
        // 取得時に枠ちょうど（の2倍）へ切り抜き済みなので、ここでは
        // そのまま流し込むだけ（requestPanelThumbnail()参照）。
        painter.drawImage(thumbRect, thumb);
        painter.restore();
    } else if (requestThumbs && !row.thumbUrl.isEmpty()) {
        // 2026-09-12: 要求は描画中に出さない。プラグインの
        // requestThumbnail() は最初の1回だけ数百ms固まることがある
        // （Windowsのプロキシ自動検出など、QNetworkAccessManagerの
        // 初回セットアップ。実測で描画1回が403msになっていた）。
        // ここでは欲しいURLを控えるだけにして、描き終わってから出す。
        queuePanelThumbnail(row.url, row.thumbUrl);
    }

    // ピン⇔サムネイルと同じ間隔をあけてタイトルへ（左右の余白が揃う）。
    const double textLeft = thumbRect.right() + kRowGap;
    // 再生中の行は右端にイコライザが乗るので、その分だけ詰める。
    const double textRight = rowRect.right() - (isCurrent ? kRowEqWidth : 0.0);
    const QRectF textArea(textLeft, rowRect.top() + 5.0,
                          qMax(10.0, textRight - textLeft),
                          rowRect.height() - 10.0);

    // 種別（PREV / NOW / NEXT）と長さ。
    painter.setFont(kindFont);
    QString meta = row.kind;
    if (row.durationSec > 0) {
        meta += QStringLiteral("  %1:%2")
                    .arg(row.durationSec / 60)
                    .arg(row.durationSec % 60, 2, 10, QLatin1Char('0'));
    }
    // 2026-09-12: ここだけ影を落とさない。番号と長さに影を付けると、
    // 影のぶん太って見えてタイトルより目立ってしまう（主役はタイトル）。
    painter.setPen(QColor(255, 255, 255, qRound((isCurrent ? 190 : 120) * alpha)));
    painter.drawText(QRectF(textArea.left(), textArea.top(), textArea.width(), 12.0),
                     Qt::AlignLeft | Qt::AlignVCenter, meta);

    // タイトル。取得できていない場合はURLで代用する。
    QString text = row.title;
    if (text.isEmpty())
        text = row.url;
    painter.setFont(titleFont);
    const QRectF titleRect(textArea.left(), textArea.top() + 12.0,
                           textArea.width(), textArea.height() - 12.0);
    // 省略結果は使い回す（幅は普段変わらないのでほぼ毎回当たる）。
    const int elideWidth = static_cast<int>(titleRect.width());
    const QString elideKey = QString::number(elideWidth) + QLatin1Char('|') + text;
    auto cached = m_panelElideCache.constFind(elideKey);
    if (cached == m_panelElideCache.constEnd()) {
        if (m_panelElideCache.size() > 200)
            m_panelElideCache.clear();
        cached = m_panelElideCache.insert(
            elideKey, titleFm.elidedText(text, Qt::ElideRight, elideWidth));
    }
    drawShadowedText(painter, titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                     cached.value(),
                     QColor(255, 255, 255, qRound((isCurrent ? 240 : 200) * alpha)),
                     alpha);
}

void CanvasVideo::paintLabelPanelRowsBody(QPainter &painter, const QRectF &plateRect,
                                          const QRectF &labelRect,
                                          const QList<PanelRow> &rows)
{
    // 中身は板が開き切る手前から遅れて出す（伸び切る前に文字が出ると
    // 板からはみ出して見えるため）。
    const double reveal = qBound(0.0, (static_cast<double>(m_labelPanelAnim) - 0.55) / 0.45, 1.0);
    if (reveal <= 0.01)
        return;
    const double alpha = reveal * static_cast<double>(m_labelFadeAlpha);

    painter.save();
    // クリップの丸みは板の丸み（labelPanelPlateGeometry）と必ず揃えること。
    // ずれると角のところだけ行がはみ出す／欠ける。
    painter.setClipPath(roundedPathLR(
        plateRect,
        CanvasVideoLabelPainter::pillLeftRadius(labelRect.height()),
        labelRect.height() * 0.5));

    // 行が見えている領域（ヘッダの下）。ここで切り取り、はみ出した分は
    // ホイールでスクロールして見る。
    const double viewTop = plateRect.top() + labelRect.height() + kPanelPadding;
    const QRectF viewRect(plateRect.left() + kPanelPadding, viewTop,
                          plateRect.width() - kPanelPadding * 2.0,
                          qMax(0.0, plateRect.bottom() - kPanelPadding - viewTop));
    m_labelPanelViewRect      = viewRect;
    m_labelPanelContentHeight = kPanelRowHeight * rows.size();

    // 現在の動画が変わったら、その行が見える位置へ寄せ直す（上から2行目の
    // 位置に置く）。ユーザーがホイールで動かした分はそのまま保持したいので、
    // 「現在位置が変わったとき」だけに限る。
    int currentIndex = -1;
    for (int i = 0; i < rows.size(); ++i) {
        if (rows.at(i).kind == QStringLiteral("NOW")) { currentIndex = i; break; }
    }
    const double maxScroll = qMax(0.0, m_labelPanelContentHeight - viewRect.height());
    if (m_labelPanelScrollAnchorUrl != m_source.originalPathOrUrl) {
        const bool firstAnchor = m_labelPanelScrollAnchorUrl.isEmpty();
        m_labelPanelScrollAnchorUrl = m_source.originalPathOrUrl;
        if (currentIndex >= 0) {
            const double want =
                qBound(0.0, kPanelRowHeight * (currentIndex - 1), maxScroll);
            // 2026-09-12: 行き先だけを動かし、実際の位置はいつもの追従
            // （updateLabelAnims の kScrollSpeed）に任せる。自動送り・
            // ◀◀/▶▶・行クリックのどれで移動しても、リストが目的の行まで
            // 流れていくように見える。
            m_labelPanelScrollTarget = want;
            // ただし、リストが見えていない間（初回・畳んでいる最中）は
            // 流す意味がないので即座に合わせる。開いた瞬間に前の位置から
            // スクロールし始めると、開く動きと喧嘩して落ち着かない。
            const bool listVisible = m_labelPanelOpen && m_labelPanelAnim > 0.05f;
            if (firstAnchor || !listVisible)
                m_labelPanelScroll = want;
        }
    }
    m_labelPanelScrollTarget = qBound(0.0, m_labelPanelScrollTarget, maxScroll);
    m_labelPanelScroll       = qBound(0.0, m_labelPanelScroll, maxScroll);

    painter.setClipRect(viewRect, Qt::IntersectClip);

    // 並べ替え中か。gapSlot は「つかんだ行を抜いた並び」の中で隙間を
    // 空ける位置（＝いま離せばそこへ入る）。
    const bool dragging = (m_rowDragIndex >= 0 && m_rowDragMoved
                           && m_rowDragIndex < rows.size());
    // 隙間の位置は連続値（m_rowGapAnim）。整数のときは
    //   ずれ = (k >= gap) ? 1 : 0
    // だが、間の値では 0→1 をなめらかに渡すので、行が滑って退いて見える。
    const double gapPos = dragging
        ? qBound(0.0, m_rowGapAnim, static_cast<double>(qMax(0, rows.size() - 1)))
        : 0.0;

    for (int i = 0; i < rows.size(); ++i) {
        // 並べ替え中は、つかんでいる行を抜いた並び（＝離したときの結果）で
        // 描き、落ちる位置には1行ぶんの隙間を空ける。つかんだ行そのものは
        // このあとカーソルに追従させて別に描く。
        double slot = i;
        if (dragging) {
            if (i == m_rowDragIndex)
                continue; // つかんでいる行はここでは描かない
            const int k = i - (i > m_rowDragIndex ? 1 : 0); // 抜いたあとの位置
            slot = k + qBound(0.0, k - gapPos + 1.0, 1.0);  // 隙間のぶん退く
        }
        const PanelRow &row = rows.at(i);
        const QRectF rowRect(viewRect.left(),
                             viewTop + kPanelRowHeight * slot
                                 - (dragging ? m_labelPanelScroll
                                             : qRound(m_labelPanelScroll)),
                             viewRect.width(), kPanelRowHeight);
        // 画面外の行は描かないし、サムネイルも取りに行かない。
        if (rowRect.bottom() < viewRect.top() - 1.0
            || rowRect.top() > viewRect.bottom() + 1.0)
            continue;
        m_labelPanelRowRects   << rowRect;
        m_labelPanelRowUrls    << row.url;
        m_labelPanelRowIndices << i;

        const bool isCurrent = (row.kind == QStringLiteral("NOW"));
        if (isCurrent)
            m_labelPanelNowRowRect = rowRect;

        paintPanelRowContent(painter, rowRect, row, isCurrent, alpha,
                             /*requestThumbs=*/true);
    }
    painter.restore();
}

// 描画中に見つけた「まだ手元に無いサムネイル」を控えておき、描き終わって
// から順に要求する（paintLabelPanelRows のコメント参照）。
void CanvasVideo::queuePanelThumbnail(const QString &sourceUrl, const QString &thumbUrl)
{
    if (thumbUrl.isEmpty() || m_panelThumbs.contains(thumbUrl)
        || m_panelThumbPending.contains(thumbUrl)
        || m_panelThumbFailed.contains(thumbUrl))
        return;
    for (const auto &q : m_panelThumbQueue) {
        if (q.second == thumbUrl)
            return;
    }
    m_panelThumbQueue.append({sourceUrl, thumbUrl});
    if (m_panelThumbQueueScheduled)
        return;
    m_panelThumbQueueScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_panelThumbQueueScheduled = false;
        processPanelThumbnailQueue();
    });
}

void CanvasVideo::processPanelThumbnailQueue()
{
    // 同時に走らせる本数を絞る（通信の初期化とデコード完了がまとめて
    // 来ると展開アニメーションがカクつくため）。残りは次の到着を合図に
    // また流す。
    constexpr int kMaxInFlightThumbs = 2;
    while (!m_panelThumbQueue.isEmpty()
           && m_panelThumbPending.size() < kMaxInFlightThumbs) {
        const auto next = m_panelThumbQueue.takeFirst();
        QElapsedTimer t;
        t.start();
        requestPanelThumbnail(next.first, next.second);
        const double ms = t.nsecsElapsed() / 1e6;
        if (ms > 5.0) {
            qDebug().noquote() << QStringLiteral(
                "[PanelThumb] request call blocked %1ms (%2)")
                    .arg(ms, 0, 'f', 1).arg(next.second);
        }
    }
}

void CanvasVideo::requestPanelThumbnail(const QString &sourceUrl, const QString &thumbUrl)
{
    // 失敗したURLを覚えておかないと、描画のたびに再要求して延々と
    // リトライし続ける（毎フレーム走るため実質無限ループになる）。
    if (thumbUrl.isEmpty() || m_panelThumbs.contains(thumbUrl)
        || m_panelThumbPending.contains(thumbUrl)
        || m_panelThumbFailed.contains(thumbUrl))
        return;

    // 2026-09-22: ローカルの動画（本体のリストの行）。バブル本体と同じく
    // Explorer のサムネイルを使い、プラグインの行と同じ大きさに仕上げる。
    if (thumbUrl.startsWith(kPanelLocalThumbPrefix)) {
        m_panelThumbPending.insert(thumbUrl);
        const QString filePath = thumbUrl.mid(kPanelLocalThumbPrefix.size());
        QPointer<CanvasVideo> guard(this);
        QThreadPool::globalInstance()->start([guard, thumbUrl, filePath] {
            QImage image = finishPanelThumbImage(
                ThumbnailLoader::fetchLocalThumbnailBlocking(filePath));
            QMetaObject::invokeMethod(QCoreApplication::instance(),
                [guard, thumbUrl, image = std::move(image)]() mutable {
                if (!guard)
                    return;
                guard->m_panelThumbPending.remove(thumbUrl);
                if (image.isNull()) {
                    guard->m_panelThumbFailed.insert(thumbUrl);
                    return;
                }
                if (guard->m_panelThumbs.size() > 240) {
                    guard->m_panelThumbs.clear();
                    guard->m_panelThumbFailed.clear();
                }
                guard->m_panelThumbs.insert(thumbUrl, image);
                emit guard->frameReady();
                emit guard->playbackUiChanged();
                guard->processPanelThumbnailQueue();
            });
        });
        return;
    }

    IPbThumbnailProvider *provider =
        PluginManager::instance().findThumbnailProvider(sourceUrl);
    if (!provider) {
        qDebug().noquote() << QStringLiteral(
            "[PanelThumb] no thumbnail provider for %1").arg(sourceUrl);
        // 2026-09-22: 本体のリストにはローカルの動画も並ぶ（プラグインが居ない）。
        // 覚えておかないと描画のたびに聞き直してログが流れ続ける。
        m_panelThumbFailed.insert(thumbUrl);
        return;
    }

    m_panelThumbPending.insert(thumbUrl);

    PbThumbnailRequest request;
    request.requestId = s_nextThumbnailRequestId.fetch_add(1);
    request.sourceUrl = sourceUrl;
    request.thumbnailUrlHint = thumbUrl;
    // パネルの行は82x46pxで描くので、小さい画像で十分
    // （mqdefault相当。大きい画像を掴んでもメモリを食うだけ）。
    request.maxDimension = 320;

    QPointer<CanvasVideo> guard(this);
    provider->requestThumbnail(request, [guard, thumbUrl](const PbThumbnailResult &result) {
        if (!guard)
            return;
        if (!result.success || result.encodedImage.isEmpty()) {
            guard->m_panelThumbPending.remove(thumbUrl);
            guard->m_panelThumbFailed.insert(thumbUrl);
            qWarning().noquote() << QStringLiteral(
                "[PanelThumb] failed %1: %2").arg(thumbUrl, result.errorMessage);
            return;
        }
        // 2026-09-12: デコードと縮小はUIスレッドでやらない。パネルを初めて
        // 開いた瞬間は行数ぶんのJPEGが一斉に届くため、ここで同期デコード
        // すると展開アニメーションが目に見えてカクついた（特に動画の
        // デコード開始と重なる起動直後）。本体サムネイル側
        // （requestThumbnailFromProvider）と同じくワーカースレッドへ逃がす。
        const QByteArray encoded = result.encodedImage;
        const QString mime = result.mimeType;
        QThreadPool::globalInstance()->start([guard, thumbUrl, encoded, mime] {
            QImage image;
            bool ok = image.loadFromData(encoded);
            if (ok) {
                // 2026-09-12: 表示に使う大きさ（58x33px）ちょうどの2倍まで
                // ここで縮めて、切り抜きも済ませておく。以前は240px幅のまま
                // 持っていて、描画のたびに毎フレーム・行数ぶん縮小していた
                // （リスト表示中だけCPUが数%上がる主因）。mqdefault(320x180)
                // から一度だけ縮めておけば、あとは等倍に近い転送で済む。
                // 2026-09-22: ローカルの動画の行と共通の finishPanelThumbImage() へ。
                image = finishPanelThumbImage(std::move(image));
            }
            QMetaObject::invokeMethod(QCoreApplication::instance(),
                [guard, thumbUrl, ok, encodedSize = encoded.size(), mime,
                 image = std::move(image)]() mutable {
                if (!guard)
                    return;
                // デコード完了までpendingを保持しておく（先に外すと、
                // 完了前の再描画で同じURLをもう一度要求してしまう）。
                guard->m_panelThumbPending.remove(thumbUrl);
                if (!ok || image.isNull()) {
                    guard->m_panelThumbFailed.insert(thumbUrl);
                    qWarning().noquote() << QStringLiteral(
                        "[PanelThumb] decode failed %1 (%2 bytes, mime=%3)")
                            .arg(thumbUrl).arg(encodedSize).arg(mime);
                    return;
                }
                // 2026-09-12: 溜め込みすぎ防止。リストを渡り歩くと1バブル
                // あたり無制限に増える（1枚116x66=約30KB）。上限を超えたら
                // 一度捨てて、見えている数件から取り直す。
                if (guard->m_panelThumbs.size() > 240) {
                    guard->m_panelThumbs.clear();
                    guard->m_panelThumbFailed.clear();
                }
                guard->m_panelThumbs.insert(thumbUrl, image);
                emit guard->frameReady();
                emit guard->playbackUiChanged();
                // 1本空いたので、控えてある次のぶんを流す。
                guard->processPanelThumbnailQueue();
            });
        });
    });
}

// ------------------------------------------------------------
//  ラベルのコントローラ（2026-09-12追加）
// ------------------------------------------------------------

// 2026-09-12: ラベルが「開ける」かどうかはプラグインが決める。
// リスト（IPbPlaylistNavigatorが並びを返すURL）のときだけコントローラを
// 出す。Liveやただの動画（ロゴだけのラベル）はそもそも展開する中身が
// 無いので、クリック判定も展開の仕組みも通さない。
bool CanvasVideo::labelPanelSupported() const
{
    // 2026-09-22: 本体のリストは本体が並びを持っているので、いつでも開ける。
    if (hasCoreList())
        return true;
    if (m_source.kind != MediaKind::PluginMedia
        || m_source.originalPathOrUrl.isEmpty())
        return false;

    // 問い合わせはキャッシュ参照のみ（PbPlaylistApi.h）だが、クリックのたび
    // ・描画のたびに引く必要はないので短時間だけ覚えておく。
    constexpr qint64 kSupportCacheMs = 1000;
    if (m_panelSupportUrl == m_source.originalPathOrUrl
        && m_panelSupportAge.isValid()
        && m_panelSupportAge.elapsed() < kSupportCacheMs)
        return m_panelSupportCached;

    const PbPlaylistContext ctx =
        PluginManager::instance().playlistContext(m_source.originalPathOrUrl, 1, 1);
    // valid = 並びが取れた / pending = 取得中（もうすぐ出せる）。
    // 2026-09-12: セッションから戻したスナップショットに現在のURLが載って
    // いれば、プラグインの答えを待たずに開けてよい（中身は手元にある）。
    bool haveSnapshotForCurrent = false;
    for (const PanelSnapshotItem &it : m_panelSnapshot) {
        if (it.url == m_source.originalPathOrUrl) { haveSnapshotForCurrent = true; break; }
    }
    m_panelSupportCached = ctx.valid || ctx.pending || haveSnapshotForCurrent;
    m_panelSupportUrl    = m_source.originalPathOrUrl;
    m_panelSupportAge.restart();
    return m_panelSupportCached;
}

void CanvasVideo::toggleLabelPanel()
{
    if (m_labelPanelOpen) {
        closeLabelPanel();
        return;
    }
    if (!labelPanelSupported())
        return;
    m_labelPanelOpen = true;
}

void CanvasVideo::closeLabelPanel()
{
    m_labelPanelOpen = false;
    m_labelPanelRect = QRectF();
    m_labelPanelRowRects.clear();
    m_labelPanelRowUrls.clear();
}

// ------------------------------------------------------------
//  移動（切り替え）中かどうかの管理（2026-09-13追加）
// ------------------------------------------------------------

bool CanvasVideo::controllerHoldsCurrentSource() const
{
    // BubbleController::sourceUrl() は loadUrl() 時に更新される
    // （＝mpvへ実際に渡したときの元URL）。m_source はクリックした瞬間に
    // 書き換わるので、解決（プラグイン）を待っている間だけ両者がずれる。
    return !m_source.originalPathOrUrl.isEmpty()
        && m_controller.sourceUrl() == m_source.originalPathOrUrl;
}

bool CanvasVideo::controllerHoldsStaleSource() const
{
    // mpvが「今のm_sourceとは別の動画」を抱えている状態。解決（プラグイン）を
    // 待っている間だけこうなる。この間に play() を通すと前の動画が鳴り出す。
    // sourceUrl()が空（まだ一度もロードしていない＝初回再生やセッション
    // 復元直後）は対象外。そこを止めると再生が一切始まらなくなる。
    return !m_source.originalPathOrUrl.isEmpty()
        && !m_controller.sourceUrl().isEmpty()
        && m_controller.sourceUrl() != m_source.originalPathOrUrl;
}

void CanvasVideo::beginNavigation()
{
    ++m_navigateGen;
    m_navigateInFlight = true;
    // 解決が返ってこない・ロードが始まらないまま放置されたときに、
    // 停止マークの抑制だけが永久に残らないようにする最後の保険。
    const quint32 gen = m_navigateGen;
    QTimer::singleShot(15000, this, [this, gen]{
        if (gen == m_navigateGen && m_navigateInFlight) {
            qDebug("[CanvasVideo %p] navigation watchdog: giving up switching UI state",
                   static_cast<void*>(this));
            m_navigateInFlight = false;
            emit playbackUiChanged();
            emit frameReady();
        }
    });
}

void CanvasVideo::endNavigation()
{
    if (!m_navigateInFlight)
        return;
    m_navigateInFlight = false;
    emit playbackUiChanged();
    emit frameReady();
}

void CanvasVideo::navigateToUrl(const QString &url, bool pushHistory)
{
    if (url.isEmpty())
        return;
    navigateToSource(sourceFromUrl(url, MediaKind::PluginMedia), pushHistory);
}

void CanvasVideo::navigateToSource(const MediaSource &source, bool pushHistory)
{
    const QString url = source.originalPathOrUrl;
    if (url.isEmpty())
        return;

    if (pushHistory && !m_source.originalPathOrUrl.isEmpty()) {
        m_playHistory.append(PlayHistoryEntry{m_source.originalPathOrUrl, m_source.title});
        while (m_playHistory.size() > kMaxPlayHistory)
            m_playHistory.removeFirst();
    }

    // 移動先では連続再生の情報も張り直しになる（resolveViaPlugin()が
    // 改めて設定する）ので、古い値を持ち越さない。
    m_nextUrl.clear();
    m_nextUrlPending = false;
    // 2026-09-12: コントローラは開いたままにする。行を選ぶたびに閉じると
    // 続けて前後へ辿れず使いにくいため（閉じるのは×／パネルの外を
    // クリックしたときだけ）。中身は次のフレームで新しい現在位置から
    // 組み直される。
    // 【2026-09-12 重要】再生中のバブルで別URLをロードする場合は、先に
    // 一時停止しておく。BubbleControllerの状態遷移表（BubbleState.hの
    // isBubbleStateTransitionValid）では Playing → Loading が不正で、
    // loadUrl()内のtransitionTo(Loading)が「不正な遷移を拒否」として
    // 弾かれる。するとmpvは新しいファイルを開くのに状態機械はPlayingの
    // ままになり、onPlayerFirstFrameReady()の「Loadingなら再生開始」分岐に
    // 入れず、loadUrl()が送ったpause=1のまま止まる
    // （＝画面は1コマ目で固まり、再生が進まない）。
    // Paused → Loading は正当な遷移なので、ここを通してから読み込む。
    const BubbleState st = m_controller.state();
    if (st == BubbleState::Playing || st == BubbleState::Buffering)
        m_controller.pause();

    // 2026-09-13: ここから「切り替え中」。前の移動のために予約されていた
    // 遅延再生（stateChangedハンドラのsingleShot）は、世代が変わることで
    // 自動的に無効になる。これを入れる前は、その予約が解決待ちの最中に
    // 発火して「まだmpvが抱えている前の動画」を再開させていた。
    beginNavigation();

    // ロード完了時に確実に再生を開始させる（stateChangedハンドラ参照）。
    // 【重要】必ず上の一時停止より後で立てること。先に立てると、この
    // pause()が出すstateChanged(Paused)を自分で拾って「ロードが終わった」と
    // 誤認し、解決を待っている最中に再生を再開してしまう。その結果、解決
    // 完了時点の状態が再びPlayingになり、loadUrl()のPlaying→Loadingが
    // また弾かれる（実機ログで確認）。
    m_navigatePlayPending = true;

    qDebug().noquote() << QStringLiteral("[CanvasVideo %1] navigate to: %2")
                              .arg(reinterpret_cast<quintptr>(this), 0, 16).arg(url);
    loadMediaSource(source);
}

// ============================================================
//  2026-09-22: 本体のリスト（バブル融合で作られる）
// ============================================================
QList<MediaSource> CanvasVideo::listItems() const
{
    MediaSource current = m_source;
    // 今の動画の表示名・サムネイルは、解決や読み込みで後から埋まる。
    if (current.thumbnailPathOrUrl.isEmpty())
        current.thumbnailPathOrUrl = m_thumbnailUrl;
    if (!hasCoreList())
        return {current};
    QList<MediaSource> items = m_coreList;
    const int cur = coreListCurrentIndex();
    if (cur >= 0)
        items[cur] = current;
    return items;
}

void CanvasVideo::appendListItems(const QList<MediaSource> &items)
{
    if (items.isEmpty())
        return;
    if (m_coreList.isEmpty()) {
        m_coreList = listItems();   // 今の動画1件から始める
        m_coreListIndex = 0;
    }
    m_coreList += items;
    // 本体のリストになったら、プラグインの一覧用の並べ替えは使わない
    // （並べ替えは m_coreList そのものを動かす。endRowDrag 参照）。
    m_panelOrderOverride.clear();
    ++m_panelOrderRevision;
    invalidatePanelRows();
    emit playbackUiChanged();
}

QJsonObject CanvasVideo::coreListToJson(bool includeIndex) const
{
    if (!hasCoreList())
        return QJsonObject();
    QJsonArray items;
    for (const MediaSource &s : listItems()) {
        QJsonObject o;
        o.insert(QStringLiteral("k"), mediaKindToString(s.kind));
        o.insert(QStringLiteral("u"), s.originalPathOrUrl);
        if (!s.title.isEmpty())              o.insert(QStringLiteral("t"),  s.title);
        // ローカルのサムネイルは Explorer から取り直すので保存しない
        if (!s.thumbnailPathOrUrl.isEmpty()
            && s.kind != MediaKind::LocalVideo && s.kind != MediaKind::LocalAudio)
            o.insert(QStringLiteral("th"), s.thumbnailPathOrUrl);
        if (!s.resolverPluginId.isEmpty())   o.insert(QStringLiteral("p"),  s.resolverPluginId);
        items.append(o);
    }
    QJsonObject json;
    json.insert(QStringLiteral("items"), items);
    if (includeIndex)
        json.insert(QStringLiteral("index"), coreListCurrentIndex());
    return json;
}

void CanvasVideo::restoreCoreListFromJson(const QJsonObject &json)
{
    QList<MediaSource> list;
    const QJsonArray items = json.value(QStringLiteral("items")).toArray();
    list.reserve(items.size());
    for (const QJsonValue &v : items) {
        const QJsonObject o = v.toObject();
        MediaSource s;
        s.kind = mediaKindFromString(o.value(QStringLiteral("k")).toString());
        s.originalPathOrUrl = o.value(QStringLiteral("u")).toString();
        if (s.originalPathOrUrl.isEmpty() || s.kind == MediaKind::Unknown)
            continue;
        s.title = o.value(QStringLiteral("t")).toString();
        s.thumbnailPathOrUrl = o.value(QStringLiteral("th")).toString();
        s.resolverPluginId = o.value(QStringLiteral("p")).toString();
        list << s;
    }
    if (list.size() < 2) {
        m_coreList.clear();
        m_coreListIndex = -1;
    } else {
        m_coreList = list;
        m_coreListIndex = json.value(QStringLiteral("index")).toInt(-1);
        // 保存した位置が今の動画と合わなければ、URLで探し直す（coreListCurrentIndex）
        if (m_coreListIndex < 0 || m_coreListIndex >= m_coreList.size()
            || m_coreList.at(m_coreListIndex).originalPathOrUrl != m_source.originalPathOrUrl)
            m_coreListIndex = -1;
        m_panelOrderOverride.clear();
        ++m_panelOrderRevision;
    }
    invalidatePanelRows();
    emit playbackUiChanged();
}

// ============================================================
//  2026-09-23: ラベルの長押し（PbLabelActionApi.h）
// ============================================================
namespace {
constexpr double kLabelHoldDelayMs   = 140.0;   // ふつうのクリックでゲージが出ないための遅れ
constexpr double kLabelHoldFillMs    = 620.0;   // 一周たまるまで
constexpr double kLabelHoldMovePx    = 7.0;     // これ以上動いたら長押しはやめる
constexpr double kLabelHoldTimeoutMs = 30000.0; // 結果を待つ上限
constexpr double kLabelHoldShakeMs   = 380.0;
constexpr double kLabelHoldReturnOmega = 14.0;  // 戻るときの臨界減衰バネ（融合の離脱と同じ）
// 芯は細く、その周りは本当にぼかして「もわぁ」と光らせる。
constexpr double kLabelHoldGaugeWidth = 1.1;   // 芯の太さ
constexpr double kLabelHoldGlowWidth  = 2.4;   // ぼかす前に描く線の太さ
constexpr double kLabelHoldGlowRadius = 6.0;   // ぼかしの半径(px)
constexpr double kLabelHoldGlowPad    = 14.0;  // 作業用画像の余白（半径より広く）
constexpr int    kLabelHoldGlowAlpha  = 205;   // にじみの強さ

// 横→縦の2回で1パス、3パスでガウスに近づく箱ぼかし。対象はラベルの台の
// 周りだけ（数百×数十px）なので、毎フレームやってもごく軽い。
void boxBlurArgb(QImage &img, int radius, int passes)
{
    if (img.isNull() || radius < 1)
        return;
    const int w = img.width(), h = img.height();
    if (w < 3 || h < 3)
        return;
    QImage tmp(img.size(), QImage::Format_ARGB32_Premultiplied);
    const int span = radius * 2 + 1;
    for (int pass = 0; pass < passes; ++pass) {
        // 横
        for (int y = 0; y < h; ++y) {
            const QRgb *src = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            QRgb *dst = reinterpret_cast<QRgb *>(tmp.scanLine(y));
            int a = 0, r = 0, g = 0, b = 0;
            for (int i = -radius; i <= radius; ++i) {
                const QRgb p = src[qBound(0, i, w - 1)];
                a += qAlpha(p); r += qRed(p); g += qGreen(p); b += qBlue(p);
            }
            for (int x = 0; x < w; ++x) {
                dst[x] = qRgba(r / span, g / span, b / span, a / span);
                const QRgb out = src[qBound(0, x - radius, w - 1)];
                const QRgb in  = src[qBound(0, x + radius + 1, w - 1)];
                a += qAlpha(in) - qAlpha(out); r += qRed(in) - qRed(out);
                g += qGreen(in) - qGreen(out); b += qBlue(in) - qBlue(out);
            }
        }
        // 縦
        for (int x = 0; x < w; ++x) {
            int a = 0, r = 0, g = 0, b = 0;
            for (int i = -radius; i <= radius; ++i) {
                const QRgb p = reinterpret_cast<const QRgb *>(
                    tmp.constScanLine(qBound(0, i, h - 1)))[x];
                a += qAlpha(p); r += qRed(p); g += qGreen(p); b += qBlue(p);
            }
            for (int y = 0; y < h; ++y) {
                reinterpret_cast<QRgb *>(img.scanLine(y))[x] =
                    qRgba(r / span, g / span, b / span, a / span);
                const QRgb out = reinterpret_cast<const QRgb *>(
                    tmp.constScanLine(qBound(0, y - radius, h - 1)))[x];
                const QRgb in = reinterpret_cast<const QRgb *>(
                    tmp.constScanLine(qBound(0, y + radius + 1, h - 1)))[x];
                a += qAlpha(in) - qAlpha(out); r += qRed(in) - qRed(out);
                g += qGreen(in) - qGreen(out); b += qBlue(in) - qBlue(out);
            }
        }
    }
}
constexpr double kLabelHoldHoverCueMs = 450.0;  // 合図を出すまでカーソルを置く時間
constexpr int    kLabelHoldTrackAlpha = 52;     // 道筋（まだ通っていない所）の濃さ
}

QString CanvasVideo::coreLabelHoldTarget()
{
    // 本体の動作で押しきったとき、ラベルがこの語に変わる（hasCoreList() と同じ表示）。
    return QStringLiteral("List");
}

QString CanvasVideo::coreLabelHoldHint()
{
    // 本体が持っている唯一の動作の名前。何をするかは本体が決め、使うかどうかは
    // 担当プラグイン（いなければ本体自身）が決める。
    return QStringLiteral("フォルダで List にする");
}

bool CanvasVideo::takeLabelHoldFire()
{
    const bool fire = m_labelHoldFirePending;
    m_labelHoldFirePending = false;
    return fire;
}

void CanvasVideo::updateLabelHoldPointer(double distancePx)
{
    if (m_labelHoldPhase == LabelHoldPhase::Charging && distancePx > kLabelHoldMovePx)
        m_labelHoldPhase = LabelHoldPhase::Idle;
}

void CanvasVideo::finishLabelHold(bool success)
{
    if (m_labelHoldPhase != LabelHoldPhase::Waiting)
        return;
    if (success) {
        m_labelHoldPhase = LabelHoldPhase::Done;
        m_labelHoldFlash = 1.0;
    } else {
        m_labelHoldPhase = LabelHoldPhase::Failed;
        m_labelHoldShakeMs = 0.0;
    }
    emit playbackUiChanged();
}

void CanvasVideo::tickLabelHold(double dtSec)
{
    const double dtMs = dtSec * 1000.0;
    switch (m_labelHoldPhase) {
    case LabelHoldPhase::Charging: {
        if (!m_labelPullActive || !m_labelHoldAvailableEffective) {
            m_labelHoldPhase = LabelHoldPhase::Idle;
            break;
        }
        m_labelHoldMs += dtMs;
        // 針が一定の速さで進むほうが「あとどれくらいか」が読める（直線）
        const double p = qBound(0.0, (m_labelHoldMs - kLabelHoldDelayMs) / kLabelHoldFillMs, 1.0);
        m_labelHold = qMax(m_labelHold, p);   // 戻りかけから押し直したら、そこから
        m_labelHoldVel = 0.0;
        if (p >= 1.0) {
            m_labelHold = 1.0;
            m_labelHoldPhase = LabelHoldPhase::Waiting;
            m_labelHoldWaitMs = 0.0;
            m_labelHoldFirePending = true;
            m_labelHoldSwallowRelease = true;   // 離しても固定・開閉はしない
        }
        break;
    }
    case LabelHoldPhase::Waiting:
        m_labelHold = 1.0;
        m_labelHoldWaitMs += dtMs;
        if (m_labelHoldWaitMs > kLabelHoldTimeoutMs) {
            qWarning().noquote() << QStringLiteral(
                "[CanvasVideo %1] label hold: no answer in %2ms, giving up")
                .arg(reinterpret_cast<quintptr>(this), 0, 16).arg(kLabelHoldTimeoutMs);
            finishLabelHold(false);
        }
        break;
    case LabelHoldPhase::Done:
        // 満タンのまま光って消える（時定数 ≒ 110ms）
        m_labelHoldFlash *= std::exp(-9.0 * dtSec);
        if (m_labelHoldFlash < 0.01) {
            m_labelHoldFlash = 0.0;
            m_labelHold = m_labelHoldVel = 0.0;
            m_labelHoldPhase = LabelHoldPhase::Idle;
        }
        break;
    case LabelHoldPhase::Failed:
    case LabelHoldPhase::Idle: {
        // ゲージは止まった状態から動き出し、行き過ぎずに0へ戻る
        const int n = qMax(1, static_cast<int>(std::ceil(dtSec * 240.0)));
        const double h = dtSec / n;
        const double w = kLabelHoldReturnOmega;
        for (int i = 0; i < n; ++i) {
            m_labelHoldVel += ((0.0 - m_labelHold) * w * w - m_labelHoldVel * 2.0 * w) * h;
            m_labelHold += m_labelHoldVel * h;
        }
        if (m_labelHold < 0.002 && std::abs(m_labelHoldVel) < 0.02) {
            m_labelHold = m_labelHoldVel = 0.0;
        } else if (m_labelHold < 0.0) {
            m_labelHold = m_labelHoldVel = 0.0;
        }
        if (m_labelHoldPhase == LabelHoldPhase::Failed) {
            m_labelHoldShakeMs += dtMs;
            if (m_labelHoldShakeMs >= kLabelHoldShakeMs) {
                m_labelHoldShakeMs = -1.0;
                m_labelHoldPhase = LabelHoldPhase::Idle;
            }
        }
        break;
    }
    }

    m_labelHoldShimmer += dtSec;
    if (m_labelHoldShimmer > 1.0e6)
        m_labelHoldShimmer = 0.0;

    // 押す前の合図: カーソルを置いたまま少し待つと、ゲージの道筋と言葉を出す。
    // （長押しは押してみるまで分からないので、押す前に「ここは押し続ける所」と
    //   伝える。道筋はゲージが実際に埋めていく線と同じ形。）
    const bool hoverIdle = m_labelHoldPhase == LabelHoldPhase::Idle
                        && m_labelHoldAvailableEffective && m_labelHover
                        && !m_labelPullActive && m_labelHoldShakeMs < 0.0;
    if (hoverIdle)
        m_labelHoldHoverMs += dtMs;
    else
        m_labelHoldHoverMs = 0.0;
    const bool hoverCue = hoverIdle && m_labelHoldHoverMs > kLabelHoldHoverCueMs;
    if (hoverCue)
        m_labelHoldHintShown = m_labelHoldHint;
    const bool trackOn = hoverCue
        || m_labelHoldPhase == LabelHoldPhase::Charging
        || m_labelHoldPhase == LabelHoldPhase::Waiting;
    {
        const double t = trackOn ? 1.0 : 0.0;
        m_labelHoldTrack += (t - m_labelHoldTrack) * (1.0 - std::exp(-11.0 * dtSec));
        if (std::abs(m_labelHoldTrack - t) < 0.004)
            m_labelHoldTrack = t;
    }

    // 言葉: ため始めてから（遅れの後）と結果待ちの間、そして押す前の合図のとき。
    // ためている間はラベルの文字そのものが入れ替わっていくので、下の言葉は
    // 押す前（合図）と結果待ちのときだけ出す。
    const bool hintOn = hoverCue || m_labelHoldPhase == LabelHoldPhase::Waiting;
    const double target = hintOn ? 1.0 : 0.0;
    m_labelHoldHintAlpha += (target - m_labelHoldHintAlpha) * (1.0 - std::exp(-12.0 * dtSec));
    if (std::abs(m_labelHoldHintAlpha - target) < 0.005)
        m_labelHoldHintAlpha = target;
}

bool CanvasVideo::labelHoldNeedsFrames() const
{
    return m_labelHoldPhase != LabelHoldPhase::Idle
        || m_labelHold > 0.0 || m_labelHoldHintAlpha > 0.0
        || m_labelHoldTrack > 0.0 || m_labelHoldShakeMs >= 0.0
        // 合図を出すまでの待ち時間そのものも、止まっていると進まない
        || (m_labelHoldAvailableEffective && m_labelHover
            && m_labelHoldPhase == LabelHoldPhase::Idle
            && m_labelHoldHoverMs <= kLabelHoldHoverCueMs);
}

void CanvasVideo::paintLabelHoldGauge(QPainter &painter, const QRectF &rect,
                                      double leftRadius, double rightRadius,
                                      double progress, double alpha,
                                      double trackAmount) const
{
    if (alpha <= 0.01 || rect.isEmpty()
        || (progress <= 0.0005 && trackAmount <= 0.01))
        return;
    const QPainterPath outline = roundedPathLR(rect, leftRadius, rightRadius);
    const double len = outline.length();
    if (len <= 2.0)
        return;
    progress = qBound(0.0, progress, 1.0);
    // 始点は再生位置の光と同じく上辺の中央（paintLabelProgressRing 参照）
    const double topRun = qMax(0.0, rect.center().x() - (rect.left() + leftRadius));
    const double startPercent = qBound(0.0, topRun / len, 1.0);
    const auto pointAt = [&](double u) {
        double t = startPercent + u;
        t -= std::floor(t);
        return outline.pointAtPercent(qBound(0.0, t, 1.0));
    };

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    // 加算。ガラスの縁の陰影を潰さず光だけを足す（再生位置の光と同じ理由）。
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    QPen pen;
    pen.setCapStyle(Qt::FlatCap);
    pen.setJoinStyle(Qt::RoundJoin);

    const auto strokeRange = [&](double u0, double u1, int a, double widthPx) {
        if (u1 - u0 <= 1e-6 || a <= 0)
            return;
        const int n = qBound(2, qRound(len * (u1 - u0) / 3.0), 400);
        QPainterPath poly;
        for (int i = 0; i <= n; ++i) {
            const QPointF pt = pointAt(u0 + (u1 - u0) * i / n);
            if (i == 0) poly.moveTo(pt); else poly.lineTo(pt);
        }
        pen.setWidthF(widthPx);
        pen.setColor(QColor(255, 255, 255, qBound(0, qRound(a * alpha), 255)));
        painter.setPen(pen);
        painter.drawPath(poly);
    };
    // 道筋（まだ通っていない所）。押す前の合図でもこれだけを出す。
    if (trackAmount > 0.01)
        strokeRange(0.0, 1.0, qRound(kLabelHoldTrackAlpha * trackAmount),
                    kLabelHoldGaugeWidth);
    // たまった分。先端も同じ明るさ（色が変わって見えないように）。
    if (progress > 0.0005) {
        const double twinkle =
            0.82 + 0.18 * std::sin(m_labelHoldShimmer * 2.0 * M_PI * 0.75);

        // にじみ: 同じ線を小さな作業用画像へ描いてぼかし、加算で重ねる。
        // 線を太く薄く重ねるやり方と違って段差が出ず、「もわぁ」と広がる。
        const QRectF bbox = rect.adjusted(-kLabelHoldGlowPad, -kLabelHoldGlowPad,
                                          kLabelHoldGlowPad, kLabelHoldGlowPad);
        const double dpr = qMax(1.0, painter.device()
                                ? painter.device()->devicePixelRatioF() : 1.0);
        const QSize px(qMax(4, qRound(bbox.width() * dpr)),
                       qMax(4, qRound(bbox.height() * dpr)));
        if (m_labelHoldGlowBuf.size() != px
            || !qFuzzyCompare(m_labelHoldGlowDpr, dpr)) {
            m_labelHoldGlowBuf = QImage(px, QImage::Format_ARGB32_Premultiplied);
            m_labelHoldGlowDpr = dpr;
        }
        if (!m_labelHoldGlowBuf.isNull()) {
            m_labelHoldGlowBuf.setDevicePixelRatio(dpr);
            m_labelHoldGlowBuf.fill(Qt::transparent);
            {
                QPainter gp(&m_labelHoldGlowBuf);
                gp.setRenderHint(QPainter::Antialiasing, true);
                gp.translate(-bbox.topLeft());
                QPen glowPen(QColor(255, 255, 255, 255));
                glowPen.setWidthF(kLabelHoldGlowWidth);
                glowPen.setCapStyle(Qt::RoundCap);
                glowPen.setJoinStyle(Qt::RoundJoin);
                gp.setPen(glowPen);
                gp.setBrush(Qt::NoBrush);
                const int n = qBound(2, qRound(len * progress / 3.0), 400);
                QPainterPath poly;
                for (int i = 0; i <= n; ++i) {
                    const QPointF pt = pointAt(progress * i / n);
                    if (i == 0) poly.moveTo(pt); else poly.lineTo(pt);
                }
                gp.drawPath(poly);
            }
            boxBlurArgb(m_labelHoldGlowBuf,
                        qMax(1, qRound(kLabelHoldGlowRadius * dpr / 3.0)), 3);
            painter.setOpacity(qBound(0.0, alpha * twinkle
                                          * (kLabelHoldGlowAlpha / 255.0), 1.0));
            painter.drawImage(bbox.topLeft(), m_labelHoldGlowBuf);
            painter.setOpacity(1.0);
        }

        // 芯（細い線）。にじみの中心をはっきりさせる。
        strokeRange(0.0, progress, 200, kLabelHoldGaugeWidth);
    }
    painter.restore();
}

QJsonObject CanvasVideo::buildFolderListForLabelHold() const
{
    const QString orig = m_source.originalPathOrUrl;
    const bool isUrl = orig.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive);
    const QString localPath = isUrl ? QUrl(orig).toLocalFile() : orig;
    const QFileInfo self(localPath);
    if (localPath.isEmpty() || !self.exists())
        return QJsonObject();
    const bool audio = (m_source.kind == MediaKind::LocalAudio);

    QFileInfoList files;
    const QFileInfoList all = self.absoluteDir().entryInfoList(
        QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &fi : all) {
        const QString path = fi.absoluteFilePath();
        const bool match = audio ? CanvasDropHandler::isLikelyLocalAudioFile(path)
                                 : CanvasDropHandler::isLikelyLocalVideoFile(path);
        if (match)
            files << fi;
    }
    // 名前順。数字は数として比べる（第2話 < 第10話）。
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(files.begin(), files.end(), [&](const QFileInfo &a, const QFileInfo &b) {
        return collator.compare(a.fileName(), b.fileName()) < 0;
    });

    int selfIndex = -1;
    for (int i = 0; i < files.size(); ++i) {
        if (files.at(i).absoluteFilePath().compare(self.absoluteFilePath(),
                                                   Qt::CaseInsensitive) == 0) {
            selfIndex = i;
            break;
        }
    }
    if (selfIndex < 0 || files.size() < 2)
        return QJsonObject();

    // あまりに多いフォルダでは今の動画の前後だけ（一覧が重くならないように）
    constexpr int kMaxItems = 500;
    int from = 0, to = static_cast<int>(files.size());
    if (to > kMaxItems) {
        from = qBound(0, selfIndex - kMaxItems / 2, to - kMaxItems);
        to = from + kMaxItems;
    }

    // 今の動画は元の表記のまま入れる（リスト内の位置をURLで探すため）。
    // ほかも同じ表記（file:URL か、パス＋区切り文字）に揃える。
    const bool nativeSep = !isUrl && orig.contains(QLatin1Char('\\'));
    const QString kind = mediaKindToString(m_source.kind);
    QJsonArray items;
    for (int i = from; i < to; ++i) {
        const QFileInfo &fi = files.at(i);
        QString u;
        if (i == selfIndex)
            u = orig;
        else if (isUrl)
            u = QUrl::fromLocalFile(fi.absoluteFilePath()).toString();
        else
            u = nativeSep ? QDir::toNativeSeparators(fi.absoluteFilePath())
                          : fi.absoluteFilePath();
        QJsonObject o;
        o.insert(QStringLiteral("k"), kind);
        o.insert(QStringLiteral("u"), u);
        o.insert(QStringLiteral("t"), i == selfIndex && !m_source.title.isEmpty()
                                          ? m_source.title : fi.fileName());
        items.append(o);
    }
    QJsonObject json;
    json.insert(QStringLiteral("items"), items);
    json.insert(QStringLiteral("index"), selfIndex - from);
    return json;
}

// 再生を止めない差し替え。開き直さないので、解決の答え（ラベル）は
// プラグインがここで渡したものをそのまま使う。
void CanvasVideo::applyLabelHoldSwitchInPlace(const QString &newUrl,
                                              const QByteArray &labelIconSvg,
                                              const QString &labelText,
                                              bool labelHoverable, bool labelPinnable,
                                              bool labelProgress)
{
    qDebug().noquote() << QStringLiteral(
        "[CanvasVideo %1] label hold: in-place switch to %2 (label=\"%3\")")
        .arg(reinterpret_cast<quintptr>(this), 0, 16).arg(newUrl.left(120), labelText);
    // 流している中身は同じなので、mpv（BubbleController）が覚えているURLも
    // 付け替える。ここを忘れると「mpvは前の動画を抱えたまま」と見なされ、
    // 再生/停止が効かなくなる（controllerHoldsStaleSource）。
    const QString oldUrl = m_source.originalPathOrUrl;
    m_source.originalPathOrUrl = newUrl;
    if (m_controller.sourceUrl() == oldUrl)
        m_controller.renameSourceUrl(newUrl);
    m_labelIconSvg    = labelIconSvg;
    m_labelText       = labelText;
    m_labelHoverable  = labelHoverable;
    m_labelPinnable   = labelPinnable;
    m_labelProgress   = labelProgress;
    m_pluginLabelKnown = true;
    if (!m_labelPinnable)
        m_labelPinned = false;
    // 続きの有無は変わりうる（並びが付いた／外れた）。開き直していないので
    // 解決の答えは無い。再生が終わったときに、もう一度プラグインへ聞かせる。
    m_nextUrl.clear();
    m_nextUrlPending = true;
    m_panelOrderOverride.clear();
    ++m_panelOrderRevision;
    invalidatePanelRows();
    emit playbackUiChanged();
    qDebug().noquote() << QStringLiteral("[CanvasVideo %1] label hold: in-place switch applied")
                              .arg(reinterpret_cast<quintptr>(this), 0, 16);
}

void CanvasVideo::switchSourceByLabelHold(const QString &newUrl, bool keepPlaying,
                                          const QByteArray &labelIconSvg,
                                          const QString &labelText,
                                          bool labelHoverable, bool labelPinnable,
                                          bool labelProgress)
{
    if (newUrl.isEmpty() || newUrl == m_source.originalPathOrUrl)
        return;
    ++m_labelHoldEpoch;
    m_labelHoldLastInPlace = keepPlaying;
    if (keepPlaying) {
        applyLabelHoldSwitchInPlace(newUrl, labelIconSvg, labelText,
                                    labelHoverable, labelPinnable, labelProgress);
        return;
    }
    MediaSource src = m_source;          // 同じ動画の別の見え方（タイトル等はそのまま）
    src.originalPathOrUrl = newUrl;
    m_pendingNavigateResumeMs = qMax<qint64>(0, position());
    navigateToSource(src, /*pushHistory=*/false);
}

void CanvasVideo::restoreLabelHoldState(const QString &url, quint64 epoch,
                                        const QJsonObject &pluginLabel)
{
    m_labelHoldEpoch = epoch;
    if (url.isEmpty() || url == m_source.originalPathOrUrl)
        return;
    if (m_labelHoldLastInPlace) {
        // 差し替えで切り替えたものは、戻すときも開き直さない。ラベルは
        // その時点で保存しておいたもの（プラグインが言ったこと）を戻す。
        const QString oldUrl = m_source.originalPathOrUrl;
        m_source.originalPathOrUrl = url;
        if (m_controller.sourceUrl() == oldUrl)
            m_controller.renameSourceUrl(url);
        m_labelIconSvg.clear();
        m_labelText.clear();
        m_labelHoverable = m_labelPinnable = m_labelProgress = false;
        m_pluginLabelKnown = false;
        if (!pluginLabel.isEmpty())
            restorePluginLabelFromJson(pluginLabel);
        m_nextUrl.clear();
        m_nextUrlPending = true;
        m_panelOrderOverride.clear();
        ++m_panelOrderRevision;
        invalidatePanelRows();
        emit playbackUiChanged();
        return;
    }
    MediaSource src = m_source;
    src.originalPathOrUrl = url;
    m_pendingNavigateResumeMs = qMax<qint64>(0, position());
    navigateToSource(src, /*pushHistory=*/false);
}

QJsonObject CanvasVideo::pluginLabelToJson() const
{
    if (m_source.kind != MediaKind::PluginMedia)
        return QJsonObject();
    // 解決中（答えが消えている間）は、直前に表示していたラベル（sticky）を
    // 保存する。どちらも無ければ保存しない（＝復元後は答えを待つ、従来どおり）。
    const bool known = m_pluginLabelKnown;
    const QByteArray icon = known ? m_labelIconSvg : QByteArray();
    const QString text    = known ? m_labelText : QString();
    if (!known)
        return QJsonObject();
    QJsonObject json;
    json.insert(QStringLiteral("known"), true);
    if (!icon.isEmpty())
        json.insert(QStringLiteral("icon"), QString::fromUtf8(icon));
    if (!text.isEmpty())
        json.insert(QStringLiteral("text"), text);
    if (m_labelHoverable) json.insert(QStringLiteral("hover"), true);
    if (m_labelPinnable)  json.insert(QStringLiteral("pin"), true);
    if (m_labelProgress)  json.insert(QStringLiteral("progress"), true);
    return json;
}

void CanvasVideo::restorePluginLabelFromJson(const QJsonObject &json)
{
    if (m_source.kind != MediaKind::PluginMedia
        || !json.value(QStringLiteral("known")).toBool(false))
        return;
    m_labelIconSvg   = json.value(QStringLiteral("icon")).toString().toUtf8();
    m_labelText      = json.value(QStringLiteral("text")).toString();
    m_labelHoverable = json.value(QStringLiteral("hover")).toBool(false);
    m_labelPinnable  = json.value(QStringLiteral("pin")).toBool(false);
    m_labelProgress  = json.value(QStringLiteral("progress")).toBool(false);
    // 2026-09-23: 保存していたアイコンは保存した時点のプラグインの絵。
    // 今のプラグインがURLの形から見た目を答えられるなら、そちらに合わせる
    // （IPbLabelPreviewProvider）。答えられなければ保存していた絵のまま。
    {
        const PbLabelPreview preview =
            PluginManager::instance().labelPreview(m_source.originalPathOrUrl);
        if (preview.known)
            m_labelIconSvg = preview.labelIconSvg;
    }
    // 解決の答えが返ったのと同じ扱い。実際に再生して解決し直すと、その時点で
    // いったん消えて（直前の表示で繋ぎつつ）プラグインの新しい答えに置き換わる。
    m_pluginLabelKnown = true;
    // 一覧を開けるかの判定を、戻した状態で取り直させる
    m_panelSupportUrl.clear();
    emit playbackUiChanged();
}

QString CanvasVideo::coreListIdentityKey() const
{
    if (!hasCoreList())
        return QString();
    QString key;
    for (const MediaSource &s : m_coreList) {
        key += mediaKindToString(s.kind);
        key += QLatin1Char('|');
        key += s.originalPathOrUrl;
        key += QLatin1Char('\n');
    }
    return key;
}

bool CanvasVideo::coreListItemAvailable(int index) const
{
    if (index < 0 || index >= m_coreList.size())
        return false;
    const MediaSource &s = m_coreList.at(index);
    if (s.kind != MediaKind::LocalVideo && s.kind != MediaKind::LocalAudio)
        return true;   // プラグインのメディアは解決してみないと分からない
    const QString local = QUrl(s.originalPathOrUrl).toLocalFile();
    return QFileInfo::exists(local.isEmpty() ? s.originalPathOrUrl : local);
}

int CanvasVideo::coreListCurrentIndex() const
{
    if (!hasCoreList())
        return -1;
    const QString &url = m_source.originalPathOrUrl;
    if (m_coreListIndex >= 0 && m_coreListIndex < m_coreList.size()
        && m_coreList.at(m_coreListIndex).originalPathOrUrl == url)
        return m_coreListIndex;
    for (int i = 0; i < m_coreList.size(); ++i) {
        if (m_coreList.at(i).originalPathOrUrl == url)
            return i;
    }
    return -1;
}

bool CanvasVideo::navigateToCoreListItem(int index)
{
    if (!hasCoreList() || index < 0 || index >= m_coreList.size())
        return false;
    // 離れる動画の表示名・サムネイルを並びへ書き戻しておく（戻ってきたときの一覧用）。
    const int cur = coreListCurrentIndex();
    if (cur >= 0)
        m_coreList[cur] = listItems().at(cur);
    m_coreListIndex = index;
    invalidatePanelRows();
    // 本体のリストの移動はリストの中で完結するので、「戻る」用の履歴は積まない。
    navigateToSource(m_coreList.at(index), /*pushHistory=*/false);
    return true;
}

void CanvasVideo::invalidatePanelRows()
{
    m_panelRowsCache.clear();
    m_panelRowsCacheUrl.clear();
    m_panelSupportUrl.clear();
}


bool CanvasVideo::handleLabelPanelClick(const QPointF &pos)
{
    if (!m_labelPanelOpen || m_labelPanelRect.isEmpty() || !m_labelPanelRect.contains(pos))
        return false;

    // ヘッダの簡易コントロール（◀◀ / 再生 / ▶▶）。閉じる判定より先に見る。
    if (m_labelBtnPlay.contains(pos)) {
        togglePlayPause();
        return true;
    }
    if (m_labelBtnPrev.contains(pos)) {
        navigateRelativeInPanel(-1);
        return true;
    }
    if (m_labelBtnNext.contains(pos)) {
        navigateRelativeInPanel(+1);
        return true;
    }

    // ヘッダ（ラベルと同じ高さの帯。×ボタンを含む）のクリックで閉じる。
    // パネルの外側をクリックした場合はCanvasWidget側が閉じる。
    if (pos.y() <= m_labelPanelRect.top() + m_labelPanelHeaderHeight) {
        closeLabelPanel();
        return true;
    }

    // 行そのものは「離したとき」に反応する（押したまま動かすと並べ替えの
    // ドラッグになるため）。ここでは何もしないが、パネル内のクリックは
    // 消費して下のバブルへ落とさない。実際の移動・並べ替えは
    // panelRowIndexAt() / beginRowDrag() / endRowDrag() をCanvasWidget側が使う。
    return true;
}


QRectF CanvasVideo::timeTextScreenRect(int windowW, int windowH) const
{
    // paintPlaybackOverlay() の時刻テキストの台（ガラスのカプセル）そのもの。
    // 描画・ヒット判定・ホバー判定のすべてがこの矩形を使う。
    // 2026-09-13: 窓の角からの距離を、左下の丸ボタンとそろえる。
    // 丸ボタンは中心 by・半径16なので、下端は H-12（＝窓の底から12px）。
    // 台も高さを32（ボタンの直径と同じ）にして中心を by へ置けば下は12px、
    // 右も同じ12pxにすることで「右からの距離＝下からの距離」になる。
    // 左端はシークバーの終端(W-100)から8px空ける。
    // 表示モード（経過/合計・残り）で大きさを変えないこと（位置が動く）。
    const int bH = 56, bY = windowH - bH, by = bY + bH/2;
    constexpr double kPadW = 80.0, kPadH = 32.0, kPadMargin = 12.0;
    return QRectF(windowW - kPadMargin - kPadW, by - kPadH * 0.5, kPadW, kPadH);
}

bool CanvasVideo::toggleSeekTimeModeIfUnlocked()
{
    if (m_seekTimeModeLocked)
        return false;
    m_seekTimeMode = (m_seekTimeMode == PbResolvedMedia::TimeDisplayMode::ElapsedTotal)
        ? PbResolvedMedia::TimeDisplayMode::Countdown
        : PbResolvedMedia::TimeDisplayMode::ElapsedTotal;
    emit playbackUiChanged();
    return true;
}

void CanvasVideo::tickLiveEdgeBufferSeek()
{
    // 調整不要（通常動画・ローカルファイル等）、または既に一度処理済みなら
    // 何もしない（毎フレーム呼ばれるため、ワンショットにする必要がある。
    // 繰り返しシークすると再生がガクつく原因になる）。
    if (m_liveEdgeBufferMs <= 0 || m_liveBufferSeekApplied)
        return;

    const qint64 dur = m_controller.duration();
    if (dur <= 0)
        return; // durationがまだ判明していない（ロード中）。次フレーム以降に再試行。

    m_liveBufferSeekApplied = true;
    const qint64 target = qMax<qint64>(0, dur - m_liveEdgeBufferMs);
    qDebug().noquote() << QStringLiteral(
        "[LiveEdge] initial buffer seek: dur=%1ms target=%2ms (buffer=%3ms)")
              .arg(dur).arg(target).arg(m_liveEdgeBufferMs);
    m_controller.seek(target);
}

// 2026-07-15: 音量調整ボタンのホバー入り/抜けアニメーション。
// ボタン本体とスライダーを独立したフェーズマシンで進める。
// 入り: ボタン縮小 → (縮み切ってから)スライダーが「びよーん」と伸びる
//       （このシーケンス自体は維持）。
// 抜け: ボタンの拡大とスライダーの収縮を「同時に」即座スタートする。
//       どちらも待ち合わせない（要望どおり、抜ける時はボタンがすぐ
//       弾んで元のサイズに戻る）。
// ハンドル(白丸)はスライダーの伸縮とは別に、ホバー解除ですばやく
// フェードアウトするだけの単純な指数フェード。
// フェーズごとの経過時間(ms)を自前で積んでイージングを進める
// （dtは呼び出し元(CanvasWidget)から渡される秒単位のフレームdt）。
void CanvasVideo::updateVolAnim(float dt)
{
    const double dtMs = qMax(0.0, static_cast<double>(dt) * 1000.0);
    const float  shrinkTarget = 0.75f; // ホバー中のボタン縮小率
    static QEasingCurve s_volOutBack(QEasingCurve::OutBack);

    // ---- ボタン本体（丸+アイコン）のスケール ----
    switch (m_volButtonPhase) {
    case VolButtonPhase::Normal:
        m_volButtonScale = 1.0f;
        if (m_volHover) {
            m_volButtonPhase = VolButtonPhase::ShrinkingIn;
            m_volButtonPhaseElapsedMs = 0.0;
        }
        break;

    case VolButtonPhase::ShrinkingIn: {
        if (!m_volHover) {
            m_volButtonGrowFrom = m_volButtonScale;
            m_volButtonPhase = VolButtonPhase::GrowingOut;
            m_volButtonPhaseElapsedMs = 0.0;
            break;
        }
        m_volButtonPhaseElapsedMs += dtMs;
        const double durationMs = 110.0;
        const double t = qBound(0.0, m_volButtonPhaseElapsedMs / durationMs, 1.0);
        // 縮むだけなので弾みは要らない。素直な減速で十分。
        m_volButtonScale = 1.0f - (1.0f - shrinkTarget) * static_cast<float>(t);
        if (t >= 1.0) {
            m_volButtonScale = shrinkTarget;
            m_volButtonPhase = VolButtonPhase::Shrunk;
        }
        break;
    }

    case VolButtonPhase::Shrunk:
        m_volButtonScale = shrinkTarget;
        if (!m_volHover) {
            m_volButtonGrowFrom = m_volButtonScale;
            m_volButtonPhase = VolButtonPhase::GrowingOut;
            m_volButtonPhaseElapsedMs = 0.0;
        }
        break;

    case VolButtonPhase::GrowingOut: {
        if (m_volHover) {
            // 拡大中に再ホバーされたら、その場から縮小へ戻す。
            m_volButtonPhase = VolButtonPhase::ShrinkingIn;
            m_volButtonPhaseElapsedMs = 0.0;
            break;
        }
        m_volButtonPhaseElapsedMs += dtMs;
        const double durationMs = 220.0;
        const double t = m_volButtonPhaseElapsedMs / durationMs;
        const double eased = s_volOutBack.valueForProgress(qBound(0.0, t, 1.0));
        m_volButtonScale = m_volButtonGrowFrom
                            + (1.0f - m_volButtonGrowFrom) * static_cast<float>(eased);
        if (t >= 1.0) {
            m_volButtonScale = 1.0f;
            m_volButtonPhase = VolButtonPhase::Normal;
        }
        break;
    }
    }

    // ---- スライダー本体（縦バー）の伸縮 ----
    switch (m_volSliderPhase) {
    case VolSliderPhase::Hidden:
        m_volSliderAnim = 0.0f;
        // 入り側のシーケンスは維持：ボタンが縮み切ってから伸び始める。
        if (m_volHover && m_volButtonPhase == VolButtonPhase::Shrunk) {
            m_volSliderPhase = VolSliderPhase::GrowingIn;
            m_volSliderPhaseElapsedMs = 0.0;
        }
        break;

    case VolSliderPhase::GrowingIn: {
        if (!m_volHover) {
            m_volSliderPhase = VolSliderPhase::CollapsingOut;
            m_volSliderPhaseElapsedMs = 0.0;
            break;
        }
        m_volSliderPhaseElapsedMs += dtMs;
        const double durationMs = 260.0;
        const double t = m_volSliderPhaseElapsedMs / durationMs;
        m_volSliderAnim = static_cast<float>(s_volOutBack.valueForProgress(qBound(0.0, t, 1.0)));
        if (t >= 1.0) {
            m_volSliderAnim = 1.0f;
            m_volSliderPhase = VolSliderPhase::Shown;
        }
        break;
    }

    case VolSliderPhase::Shown:
        m_volSliderAnim = 1.0f;
        if (!m_volHover) {
            m_volSliderPhase = VolSliderPhase::CollapsingOut;
            m_volSliderPhaseElapsedMs = 0.0;
        }
        break;

    case VolSliderPhase::CollapsingOut: {
        if (m_volHover) {
            // 抜けている途中で再ホバーしたら、その場から伸長へ戻す。
            m_volSliderPhase = VolSliderPhase::GrowingIn;
            m_volSliderPhaseElapsedMs = 0.0;
            break;
        }
        m_volSliderPhaseElapsedMs += dtMs;
        const double durationMs = 220.0;
        const double t = qBound(0.0, m_volSliderPhaseElapsedMs / durationMs, 1.0);
        // 1→0だが、OutBackを反転させることで「1点に戻る直前に少し
        // 縮みすぎてから収まる」ような、びよーんとした収縮感を出す。
        const double eased = s_volOutBack.valueForProgress(t);
        m_volSliderAnim = static_cast<float>(1.0 - eased);
        if (t >= 1.0) {
            m_volSliderAnim = 0.0f;
            m_volSliderPhase = VolSliderPhase::Hidden;
        }
        break;
    }
    }

    // ---- ハンドル（白丸）のフェード ----
    // スライダー本体の伸縮とは独立に、ホバー解除で素早くフェードアウト
    // する（「即座にフェードアウト」の要望どおり）。
    {
        const float target = m_volHover ? 1.0f : 0.0f;
        const float speed  = m_volHover ? 10.0f : 20.0f; // 消える時はより速く
        m_volHandleAlpha += (target - m_volHandleAlpha)
                             * static_cast<float>(qMin(1.0, speed * dt));
        m_volHandleAlpha = qBound(0.0f, m_volHandleAlpha, 1.0f);
    }
}

void CanvasVideo::updateSeekHandleAnim(float dt)
{
    float target = m_seekHover ? 1.0f : 0.0f;
    float speed  = 10.0f;
    m_seekHandleAnim += (target - m_seekHandleAnim) * qMin(1.0f, speed * dt);
}

// 時刻の台のホバー（2026-09-13追加）。ラベルのホバーと同じ速さで寄せる。
// 【注意】行き先の式は playbackUiNeedsAnimationFramesExceptEqualizer() と
// 必ず同じにすること。ずれると途中でフレーム要求が止まって固まる。
void CanvasVideo::updateTimePadAnim(float dt)
{
    const float target = timePadHoverTarget();
    const float speed  = 14.0f;
    m_timeHoverAnim += (target - m_timeHoverAnim) * qMin(1.0f, speed * dt);
    m_timeHoverAnim = qBound(0.0f, m_timeHoverAnim, 1.0f);
}

// 再生バーUIの出現/消失アニメーション。
// - シークバー本体・時刻テキスト・再生停止ボタン・音量アイコン、いずれの
//   表示/非表示も基本は単純な指数フェード(m_playbarFadeAlpha)のみ。
// - 「ポップ(弾む)」演出は triggerPlaybarActivationPop() が呼ばれた
//   直後だけ発生する特別なワンショットアニメーション。ドラッグ・全画面・
//   アイドル非表示からの再表示など、通常の表示/非表示ではポップさせない
//   （毎回弾むとうるさいため、バブルがアクティブになった瞬間だけにする）。
void CanvasVideo::updatePlaybackBarAnim(bool targetVisible, double dtMs)
{
    const double dtSec = qMax(0.0, dtMs) / 1000.0;

    // ---- フェード(シークバー本体・時刻・ボタン類とも共通) ----
    // 消える時は現れる時の1.5倍速く（フェードアウトだけ速度アップ）。
    const float fadeTarget = targetVisible ? 1.0f : 0.0f;
    const float fadeSpeed  = targetVisible ? 8.0f : 12.0f; // updateVolAnim()と同程度の速さ×1.5(消える時)
    m_playbarFadeAlpha += (fadeTarget - m_playbarFadeAlpha)
                          * qMin(1.0f, fadeSpeed * static_cast<float>(dtSec));
    m_playbarFadeAlpha = qBound(0.0f, m_playbarFadeAlpha, 1.0f);

    // ラベル関連（伸び縮み・ホバー・スクロール・引っ張り）はまとめて。
    updateLabelAnims(dtSec);

    // ---- ポップ(アクティブ化した瞬間のみのワンショット) ----
    if (m_playbarActivationPopActive) {
        m_playbarActivationPopElapsedMs += dtMs;
        static QEasingCurve s_popEase(QEasingCurve::OutBack);
        const double durationMs = 320.0;
        // 再生停止ボタンは音量アイコンよりこの分だけ遅れて弾み始める。
        const double playDelayMs = 70.0;

        const double tVol = qBound(0.0, m_playbarActivationPopElapsedMs / durationMs, 1.0);
        m_playbarPopScaleVol = static_cast<float>(s_popEase.valueForProgress(tVol));

        const double tPlayRaw = (m_playbarActivationPopElapsedMs - playDelayMs) / durationMs;
        const double tPlay = qBound(0.0, tPlayRaw, 1.0);
        m_playbarPopScalePlay = static_cast<float>(s_popEase.valueForProgress(tPlay));

        if (tVol >= 1.0 && tPlayRaw >= 1.0) {
            // 両方弾み終わったら通常状態(1.0固定)へ戻し、以後はフェードのみにする。
            m_playbarActivationPopActive = false;
            m_playbarPopScaleVol  = 1.0f;
            m_playbarPopScalePlay = 1.0f;
        }
    }
    // m_playbarActivationPopActive が false の間はポップスケールに触れない
    // （常に1.0固定のまま。表示/非表示はフェードのみで行う）。
}

bool CanvasVideo::playbackUiNeedsAnimationFrames(bool targetVisible) const
{
    return equalizerNeedsFrames()
        || playbackUiNeedsAnimationFramesExceptEqualizer(targetVisible);
}

// 2026-09-12: イコライザを除いた判定。イコライザだけが動いているフレーム
// では、オーバーレイ全体ではなく equalizerDamageRect() だけを更新したい
// （CanvasWidget::syncLogicMedium 参照）。
bool CanvasVideo::playbackUiNeedsAnimationFramesExceptEqualizer(bool targetVisible) const
{
    const float fadeTarget = targetVisible ? 1.0f : 0.0f;
    const float seekTarget = m_seekHover ? 1.0f : 0.0f;
    const float volumeHandleTarget = m_volHover ? 1.0f : 0.0f;
    return m_playbarActivationPopActive
        // ラベル周り（伸び縮み・ホバー・スクロール・引っ張り）
        || labelAnimsNeedFramesExceptEqualizer()
        || std::abs(m_playbarFadeAlpha - fadeTarget) > 0.001f
        || std::abs(m_seekHandleAnim - seekTarget) > 0.001f
        || std::abs(m_volHandleAlpha - volumeHandleTarget) > 0.001f
        || std::abs(m_timeHoverAnim - timePadHoverTarget()) > 0.001f
        || m_volButtonPhase == VolButtonPhase::ShrinkingIn
        || m_volButtonPhase == VolButtonPhase::GrowingOut
        || m_volSliderPhase == VolSliderPhase::GrowingIn
        || m_volSliderPhase == VolSliderPhase::CollapsingOut;
}

void CanvasVideo::triggerPlaybarActivationPop()
{
    m_playbarActivationPopActive = true;
    m_playbarActivationPopElapsedMs = 0.0;
    m_playbarPopScaleVol  = 0.0f;
    m_playbarPopScalePlay = 0.0f;
}

bool CanvasVideo::handlePlaybackBarClick(const QPointF &pos, int W, int H)
{
    const int bH=56, bY=H-bH;
    const int by=bY+bH/2;
    const int bx=28, vbx=bx+36;
    double mx=pos.x(), my=pos.y();

    if ((mx-bx)*(mx-bx)+(my-by)*(my-by) <= 256) {
        togglePlayPause();
        return true;
    }

    if (m_volSliderAnim > 0.5f) {
        const int sh = 80;
        const int sw = 6, sx = vbx - sw/2;
        const int sy = by - 16 - sh;
        if (mx>=sx-10 && mx<=sx+sw+10 && my>=sy && my<=sy+sh) {
            float vol = qBound(0.0f, 1.0f - (float)(my-sy)/sh, 1.0f);
            setVolume(vol);
            return true;
        }
    }

    // 音量ボタン本体（ホバーでスライダーを出す円。判定半径はホバー判定
    // ＝CanvasWidget::tick側のonBtnと同じ14px）。
    // ここで拾わずfalseを返すと、CanvasWidget::handleMousePress()が
    // 「UIではない＝動画本体を押した」と解釈してバブルのヒットテストへ
    // 進み、シングルクリック判定→togglePlayPause()まで走ってしまう
    // （＝音量ボタンを押しただけで再生が止まる不具合）。ボタン自体に
    // 押下時の動作は無い（スライダーはホバーで出る）ので、クリックを
    // 消費するだけで何もしない。
    if ((mx-vbx)*(mx-vbx)+(my-by)*(my-by) <= 196)
        return true;

    return false;
}
