#include "CanvasImage.h"
#include "Renderer.h"
#include "AppSettings.h"
#include <QColor>
#include <QFileInfo>
#include <QFont>
#include <QUrl>

CanvasImage::CanvasImage(const QPixmap &pixmap, const QPointF &pos, double scale)
    : m_pixmap(pixmap)
{
    m_pos   = pos;
    setScale(scale);
}

CanvasImage::~CanvasImage()
{
    // テクスチャはRendererが管理（CanvasWidget側でdeleteTexture呼ぶ）
}

void CanvasImage::releaseGlResources(Renderer &r)
{
    if (m_texId == 0)
        return;

    r.deleteTexture(m_texId);
    m_texId = 0;
}


// 2026-10-12: 画像の平均色（透明部分を除いた重み付き）。16×16へ縮めて求める。
static QColor averageColorOfPixmap(const QPixmap &pm)
{
    if (pm.isNull())
        return QColor();
    const QImage img = pm.toImage()
        .scaled(16, 16, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_ARGB32);
    double r = 0, g = 0, b = 0, w = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const double a = qAlpha(line[x]) / 255.0;
            r += qRed(line[x]) * a; g += qGreen(line[x]) * a; b += qBlue(line[x]) * a; w += a;
        }
    }
    if (w < 0.5)
        return QColor();
    return QColor(qRound(r / w), qRound(g / w), qRound(b / w));
}

QColor CanvasImage::contentAverageColor() const
{
    const qint64 key = m_pixmap.isNull() ? 0 : qint64(m_pixmap.cacheKey());
    if (key != m_avgColorKey) {
        m_avgColor = averageColorOfPixmap(m_pixmap);
        m_avgColorKey = key;
    }
    return m_avgColor;
}

// 2026-10-12: 枠（板・縁の線・リサイズの印）だけを描く（BubbleLayerCache の焼き付け用）
void CanvasImage::paintChromeGL(Renderer &r, const QRectF &screenRect, bool isActive,
                                double pulseAlpha, double screenScale,
                                double contentCornerFactor)
{
    const Renderer::ChromeMode prev = r.chromeMode();
    r.setChromeMode(Renderer::ChromeMode::ChromeOnly);
    paintGL(r, screenRect, isActive, pulseAlpha, screenScale, contentCornerFactor);
    r.setChromeMode(prev);
}

GlassContentSource CanvasImage::glassContentSource() const
{
    if (m_pixmap.isNull() || m_loadFailed || m_texId == 0)
        return {};

    return {m_texId, QRectF(0.0, 0.0, 1.0, 1.0), 1.0f};
}

QRectF CanvasImage::itemRect() const
{
    if (m_pixmap.isNull())
        return QRectF(m_pos.x(), m_pos.y(), 320.0 * m_scale, 180.0 * m_scale);
    return QRectF(m_pos.x(), m_pos.y(),
                  m_pixmap.width()  * m_scale,
                  m_pixmap.height() * m_scale);
}

QString CanvasImage::sourceFilePath() const
{
    return m_source.kind == MediaKind::LocalImage
        ? QUrl(m_source.originalPathOrUrl).toLocalFile()
        : QString();
}

void CanvasImage::setSourceUrl(const QString &url)
{
    m_source.kind = MediaKind::WebImage;
    m_source.originalPathOrUrl = url;
    if (m_source.title.isEmpty())
        m_source.title = QFileInfo(QUrl(url).path()).fileName();
}

void CanvasImage::setSourceFilePath(const QString &path)
{
    m_source.kind = MediaKind::LocalImage;
    m_source.originalPathOrUrl = QUrl::fromLocalFile(path).toString();
    if (m_source.title.isEmpty())
        m_source.title = QFileInfo(path).fileName();
}

void CanvasImage::paintGL(Renderer &r, const QRectF &screenRect,
                          bool isActive, double pulseAlpha, double screenScale,
                          double contentCornerFactor)
{
    // 初回: GPUテクスチャ生成
    if (!m_pixmap.isNull() && m_texId == 0)
        m_texId = r.createTexture(m_pixmap.toImage());

    // 1. ボード枠（コンテンツより下）
    const double bezel = mediaBezelWidth(screenRect);
    QRectF boardScreen = screenRect.adjusted(
        -bezel, -bezel, bezel, bezel);
    const float boardRadius =
        static_cast<float>(mediaBoardCornerRadius(screenRect));
    const float contentRadius = static_cast<float>(
        contentCornerRadius(screenRect, contentCornerFactor));

    // 2026-10-11: バブル全画面中は枠・アクティブの青枠・グロー・リサイズ印をフェードで
    // 消す。contentCornerFactorは全画面フェード(通常1→全画面0)と同じ値。
    const float chromeAlpha = static_cast<float>(qBound(0.0, contentCornerFactor, 1.0));
    const float baseGlobalAlpha = r.globalAlpha();
    r.setGlobalAlpha(baseGlobalAlpha * chromeAlpha);

    double lw = 1.5;
    if (chromeAlpha <= 0.002f || !r.drawsChrome()) {
        // 枠は完全に消えているので何も描かない。
    } else if (isActive) {
        // 2026-07-13: 同心2重ストロークの疑似グロー（縞が見える）を、
        // SDFベースの本物のグラデーショングロー（Renderer::drawGlowRect）へ
        // 置き換え（CanvasVideo/CanvasLauncherと同仕様）。
        QColor accent(140, 195, 255, (int)qBound(0.0, pulseAlpha, 255.0));
        QColor glowColor(120, 180, 255, (int)qBound(0.0, pulseAlpha * 0.60, 165.0));
        // 2026-07-19: グローの広がりは基本「画面上32px固定」、バブル比の
        // 上限56×ss/下限20×ssでクランプ（詳細はCanvasVideo.cpp側コメント参照）。
        {
            const float ssf = static_cast<float>(screenScale);
            r.drawGlowRect(boardScreen, glowColor, boardRadius,
                           qBound(20.0f * ssf, 32.0f, 56.0f * ssf));
        }
        r.drawRect(boardScreen, QColor(255,255,255,90), accent, 4.0,
                   boardRadius);
    } else {
        // 2026-07-17: 非アクティブ時の境界線が見えにくいとのことで、白を
        // もう少し目立つアルファへ上げた（CanvasVideo/CanvasLauncherと揃える）。
        // 塗りの透明度も下げて(=不透明度を上げて)少し実体感を出した。
        // 2026-10-11: グミのときは塗りを消し、線も少し弱める（ゲルの面を見せる）。
        const bool gummy = AppSettings::instance().gummyGlassActive();
        r.drawRect(boardScreen,
                   QColor(255,255,255, gummy ? AppSettings::kGummyChromeFillAlpha : 46),
                   QColor(255,255,255, gummy ? AppSettings::kGummyChromeStrokeAlpha : 130),
                   lw, boardRadius);
    }

    r.setGlobalAlpha(baseGlobalAlpha);

    // 2. 画像テクスチャ（枠の上）
    if (!r.drawsContent()) {
        // 2026-10-12: 枠だけの焼き付け（paintChromeGL）では中身を描かない
    } else if (m_pixmap.isNull() || m_loadFailed || m_texId == 0) {
        r.drawRect(screenRect, QColor(18, 20, 26, 230),
                   QColor(255, 255, 255, 45), 1.0f, contentRadius);
        QFont font("Arial", qMax(10, static_cast<int>(14 * screenScale)));
        font.setBold(true);
        r.drawText(QStringLiteral("Image load failed"),
                   screenRect.topLeft() + QPointF(18.0 * screenScale, 42.0 * screenScale),
                   font, QColor(245, 245, 245, 230));
    } else {
        r.drawRoundedTexture(m_texId, screenRect, contentRadius);
    }

    // 3. リサイズアイコン（右下）
    // 位置・線長・線幅を比例ベゼル基準にし、リサイズとズームの両方へ追従。
    if (chromeAlpha <= 0.002f || !r.drawsChrome())
        return;
    r.setGlobalAlpha(baseGlobalAlpha * chromeAlpha);
    double ix = boardScreen.right()  - bezel * 0.5;
    double iy = boardScreen.bottom() - bezel * 0.5;
    const double is = resizeMarkerSize(bezel);
    const float markerLineWidth =
        static_cast<float>(resizeMarkerLineWidth(bezel));
    r.drawLine({ix-is,      iy-is*0.3}, {ix-is*0.3, iy-is},
               QColor(255,255,255,180), markerLineWidth);
    r.drawLine({ix-is*0.6,  iy-is*0.3}, {ix-is*0.3, iy-is*0.6},
               QColor(255,255,255,180), markerLineWidth);
    r.setGlobalAlpha(baseGlobalAlpha);
}
