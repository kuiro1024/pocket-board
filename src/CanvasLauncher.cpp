#include "CanvasLauncher.h"
#include "WebLinkIcon.h"
#include "AppSettings.h"
#include <QFontMetricsF>
#include <QStringList>
#include <QColor>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QSet>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#endif

namespace {
constexpr double kLauncherSize = 96.0;

// ---- 2026-10-12: 名前を板の中（アイコンの下）に出す ----
// 値は Claude outputs/launcher-label-lab.html で決めたもの。寸法はすべて
// ズーム1倍のpx（中身96・ベゼル25・板146）で、描画時に screenScale を掛ける。
// 以前は アイコン60（inset 18）を中身の中央に置き、名前は板の下16pxに
// 16〜7ptの1行で出していた。
constexpr double kLauncherBoardSize   = kLauncherSize * (1.0 + 2.0 * CanvasItem::LAUNCHER_BEZEL_RATIO);
constexpr double kLauncherIconSize    = 60.0;   // アイコンの1辺（2026-10-12: 元の大きさへ戻した）
constexpr bool   kLauncherCenterGroup = false;  // アイコン＋名前を上下中央にそろえる（OFF: アイコンは板の中央に固定）
constexpr double kLauncherIconOffsetY = 0.0;    // 中央揃えOFF時のアイコン中心（板の中心から）
constexpr double kLabelGap            = 4.0;    // アイコンの下端から名前の上端まで（2026-10-12: -6 から下げた）
constexpr double kLabelSideMargin     = 40.0;   // 名前の左右の余白（板の外周から）
constexpr double kLabelBoxWidth       = kLauncherBoardSize - kLabelSideMargin * 2.0;
constexpr double kLabelMaxPointSize   = 10.5;
constexpr double kLabelWrapPointSize  = 4.0;    // 1行のまま縮める下限（これ以下は折り返す）
constexpr double kLabelMinPointSize   = 4.5;
constexpr int    kLabelMaxLines       = 2;
constexpr double kLabelLineGap        = 1.0;    // 行の送り（文字の高さに対する倍率）
constexpr bool   kLabelBold           = true;
constexpr int    kLabelTextAlpha      = 240;    // 2026-10-12: 210 から濃くした
constexpr int    kLabelShadowAlpha    = 0;      // 0なら影を描かない
constexpr double kLabelShadowOffset   = 0.0;
constexpr int    kLabelMultiplyGray   = 70;     // グミの文字を乗算するグレー（小さいほど濃い）
// 太字フォントのヒンティング差ではみ出さないよう、判定も描画と同じ6倍の
// 固定サイズで測り、6で割って design 単位へ戻す（2026-07-17 の修正と同じ考え方）。
constexpr int kLabelSuperSample = 6;

QFont labelSuperFont(double basePt)
{
    QFont f(QStringLiteral("Arial"));
    f.setPointSizeF(basePt * kLabelSuperSample);
    f.setBold(kLabelBold);
    return f;
}

double labelDesignWidth(const QString &text, double basePt)
{
    return QFontMetricsF(labelSuperFont(basePt)).horizontalAdvance(text) / kLabelSuperSample;
}

QString labelElide(const QString &text, double basePt, double width)
{
    return QFontMetricsF(labelSuperFont(basePt))
        .elidedText(text, Qt::ElideRight, width * kLabelSuperSample);
}

// 折り返し。区切り（空白・-・_・.）を優先し、無ければ文字単位。最後の行は省略。
QStringList labelWrap(const QString &text, double basePt, double width, int maxLines)
{
    QStringList lines;
    QString rest = text;
    while (!rest.isEmpty() && lines.size() < maxLines) {
        if (lines.size() == maxLines - 1) {
            lines << labelElide(rest, basePt, width);
            rest.clear();
            break;
        }
        if (labelDesignWidth(rest, basePt) <= width) {
            lines << rest;
            rest.clear();
            break;
        }
        int cut = 1;
        for (int i = 1; i <= rest.size(); ++i) {
            if (labelDesignWidth(rest.left(i), basePt) <= width) cut = i;
            else break;
        }
        const QString seg = rest.left(cut);
        const int brk = qMax(qMax(seg.lastIndexOf(QLatin1Char(' ')), seg.lastIndexOf(QLatin1Char('-'))),
                             qMax(seg.lastIndexOf(QLatin1Char('_')), seg.lastIndexOf(QLatin1Char('.'))));
        if (brk > cut * 0.4)
            cut = brk + 1;
        lines << rest.left(cut).trimmed();
        rest = rest.mid(cut).trimmed();
    }
    return lines;
}

struct LauncherLabelLayout {
    double pointSize = kLabelMaxPointSize;
    QStringList lines;
    double lineHeight = 0.0;   // design px
};

// 大きさの決め方（ラボと同じ）:
//   ①1行のまま最大から「縮める下限」まで縮める
//   ②行を増やして、最大から最小まで試す（全文が入るものだけ）
//   ③1行で最小まで縮めて入ればそれ、無理なら最小・最大行数で省略
LauncherLabelLayout layoutLauncherLabel(const QString &text)
{
    static QHash<QString, LauncherLabelLayout> cache;
    const auto it = cache.constFind(text);
    if (it != cache.constEnd())
        return it.value();

    LauncherLabelLayout out;
    const double wrapPt = qMax(kLabelMinPointSize, qMin(kLabelWrapPointSize, kLabelMaxPointSize));
    const auto finish = [&](double pt, const QStringList &lines) {
        out.pointSize = pt;
        out.lines = lines;
        out.lineHeight = QFontMetricsF(labelSuperFont(pt)).height() / kLabelSuperSample;
        if (cache.size() > 512) cache.clear();
        cache.insert(text, out);
        return out;
    };
    for (double pt = kLabelMaxPointSize; pt >= wrapPt - 1e-6; pt -= 0.5)
        if (labelDesignWidth(text, pt) <= kLabelBoxWidth)
            return finish(pt, {text});
    QString compact = text;
    compact.remove(QLatin1Char(' '));
    for (int n = 2; n <= kLabelMaxLines; ++n) {
        for (double pt = kLabelMaxPointSize; pt >= kLabelMinPointSize - 1e-6; pt -= 0.5) {
            const QStringList ls = labelWrap(text, pt, kLabelBoxWidth, n);
            QString joined = ls.join(QString());
            joined.remove(QLatin1Char(' '));
            if (!ls.isEmpty() && ls.size() <= n && joined.size() >= compact.size()
                && !ls.last().endsWith(QChar(0x2026)))
                return finish(pt, ls);
        }
    }
    for (double pt = wrapPt; pt >= kLabelMinPointSize - 1e-6; pt -= 0.5)
        if (labelDesignWidth(text, pt) <= kLabelBoxWidth)
            return finish(pt, {text});
    return finish(kLabelMinPointSize,
                  labelWrap(text, kLabelMinPointSize, kLabelBoxWidth, kLabelMaxLines));
}

#ifdef Q_OS_WIN
std::wstring toWide(const QString &text)
{
    return text.toStdWString();
}
#endif

QPixmap fallbackIcon(int size)
{
    QPixmap pix(size, size);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(QColor(45, 52, 66, 255));
    p.setPen(QPen(QColor(180, 200, 255, 230), qMax(2, size / 32)));
    const QRectF body(size * 0.22, size * 0.16, size * 0.56, size * 0.68);
    p.drawRoundedRect(body, size * 0.08, size * 0.08);
    p.setPen(QPen(QColor(255, 255, 255, 210), qMax(2, size / 28)));
    p.drawLine(QPointF(size * 0.42, size * 0.35), QPointF(size * 0.60, size * 0.50));
    p.drawLine(QPointF(size * 0.60, size * 0.50), QPointF(size * 0.42, size * 0.65));
    p.end();
    return pix;
}
}

CanvasLauncher::CanvasLauncher(const QString &path, TargetKind kind, const QPixmap &icon,
                               const QPointF &pos, double scale)
    : m_path(kind == TargetKind::Web ? path : QDir::toNativeSeparators(path))
    , m_displayName(displayNameForPath(path))
    , m_kind(kind)
    , m_icon(icon.isNull() ? fallbackIcon(128) : icon)
{
    m_pos = pos;
    setScale(scale);
}

CanvasLauncher::~CanvasLauncher() = default;

void CanvasLauncher::setIcon(const QPixmap &icon)
{
    if (icon.isNull())
        return;
    m_icon = icon;
    m_iconDirty = true;
}

QRectF CanvasLauncher::itemRect() const
{
    const double size = kLauncherSize * m_scale;
    return QRectF(m_pos.x(), m_pos.y(), size, size);
}

void CanvasLauncher::paintGL(Renderer &r, const QRectF &screenRect,
                             bool isActive, double pulseAlpha, double screenScale,
                             double /*contentCornerFactor*/)
{
    if (m_iconDirty) {
        if (m_texId != 0)
            r.deleteTexture(m_texId);
        m_texId = 0;
        m_iconDirty = false;
    }
    if (!m_icon.isNull() && m_texId == 0)
        m_texId = r.createTexture(m_icon.toImage());

    const double bezel = launcherBezelWidth(screenRect);
    const QRectF boardScreen = screenRect.adjusted(
        -bezel, -bezel, bezel, bezel);
    const float boardRadius =
        static_cast<float>(launcherBoardCornerRadius(screenRect));

    if (!r.drawsChrome()) {
        // 2026-10-12: 枠は焼き付けたテクスチャ（BubbleLayerCache）から貼る
    } else if (isActive) {
        // 2026-07-13: 同心2重ストロークの疑似グロー（縞が見える）を、
        // SDFベースの本物のグラデーショングロー（Renderer::drawGlowRect）へ
        // 置き換え（CanvasVideo/CanvasImageと同仕様）。
        const QColor accent(140, 195, 255, qBound(0, static_cast<int>(pulseAlpha), 255));
        const QColor glowColor(120, 180, 255, qBound(0, static_cast<int>(pulseAlpha * 0.60), 165));
        // 2026-07-19: グローの広がりは基本「画面上32px固定」、バブル比の
        // 上限56×ss/下限20×ssでクランプ（詳細はCanvasVideo.cpp側コメント参照）。
        {
            const float ssf = static_cast<float>(screenScale);
            r.drawGlowRect(boardScreen, glowColor, boardRadius,
                           qBound(20.0f * ssf, 32.0f, 56.0f * ssf));
        }
        r.drawRect(boardScreen, QColor(255, 255, 255, 90), accent, 4.0f,
                   boardRadius);
    } else {
        // 2026-07-17: 非アクティブ時の境界線が見えにくいとのことで、白を
        // もう少し目立つアルファへ上げた（CanvasVideo/CanvasImageと揃える）。
        // 塗りの透明度も下げて(=不透明度を上げて)少し実体感を出した。
        r.drawRect(boardScreen, QColor(255, 255, 255, 46),
                   QColor(255, 255, 255, 130), 1.5f, boardRadius);
    }

    // ---- アイコンと名前（2026-10-12: 名前は板の中、アイコンの下） ----
    // 枠だけの焼き付け（paintChromeGL）では、アイコンと名前を飛ばしてリサイズの印へ
    const bool drawContent = r.drawsContent();
    // design px → 画面px の倍率。カメラのズームだけでなくバブル自体の拡大縮小にも
    // 追従させるため、screenScale ではなく実際の中身の幅から求める。
    const double unit = screenRect.width() / kLauncherSize;
    LauncherLabelLayout label;
    if (!m_displayName.isEmpty())
        label = layoutLauncherLabel(m_displayName);
    const int lineCount = label.lines.size();
    const double labelHeight = lineCount > 0
        ? label.lineHeight * (1.0 + (lineCount - 1) * kLabelLineGap) : 0.0;
    // アイコン中心の上下位置（板の上端からの design px）
    double iconCenterY = kLauncherBoardSize * 0.5 + kLauncherIconOffsetY;
    if (kLauncherCenterGroup) {
        const double groupHeight = kLauncherIconSize
            + (lineCount > 0 ? kLabelGap + labelHeight : 0.0);
        iconCenterY = kLauncherBoardSize * 0.5 - groupHeight * 0.5 + kLauncherIconSize * 0.5;
    }
    const double iconCenterScreenY = boardScreen.top() + iconCenterY * unit;
    const double iconSizeScreen = kLauncherIconSize * unit;

    if (drawContent && m_texId != 0) {
        const QRectF iconRect(screenRect.center().x() - iconSizeScreen * 0.5,
                              iconCenterScreenY - iconSizeScreen * 0.5,
                              iconSizeScreen, iconSizeScreen);
        r.drawTexture(m_texId, iconRect);
    }

    if (drawContent && lineCount > 0) {
        // 2026-07-17: ズーム中の再ラスタライズを避けるため、固定の6倍サイズで
        // 一度だけラスタライズし、unit は drawText の scale にだけ反映する。
        const QFont superFont = labelSuperFont(label.pointSize);
        const QFontMetricsF superFm(superFont);
        const float labelTextScale = static_cast<float>(unit / kLabelSuperSample);
        // グミ（明るい板）は濃い文字、それ以外（暗い板）は白文字。影は文字と逆の色。
        // 2026-10-12: グミの濃い文字は乗算で重ね、ゲルの色味を残したまま暗くする
        // （色はグレー。乗算なので仕上がりはゲルの色×このグレーになる）。
        const bool darkText = AppSettings::instance().gummyGlassActive();
        const QColor textColor = darkText ? QColor(kLabelMultiplyGray, kLabelMultiplyGray,
                                                   kLabelMultiplyGray, kLabelTextAlpha)
                                          : QColor(245, 247, 252, kLabelTextAlpha);
        const QColor shadowColor = darkText ? QColor(255, 255, 255, kLabelShadowAlpha)
                                            : QColor(0, 0, 0, kLabelShadowAlpha);
        const double shadowOffset = kLabelShadowOffset * unit;
        double y = iconCenterScreenY + (kLauncherIconSize * 0.5 + kLabelGap) * unit;
        r.setClipRect(boardScreen);
        for (const QString &line : label.lines) {
            const double textWidth = superFm.horizontalAdvance(line) * labelTextScale;
            const double x = screenRect.center().x() - textWidth * 0.5;
            if (kLabelShadowAlpha > 0)
                r.drawText(line, QPointF(x + shadowOffset, y + shadowOffset),
                           superFont, shadowColor, labelTextScale);
            if (darkText) r.beginMultiplyBlend();
            r.drawText(line, QPointF(x, y), superFont, textColor, labelTextScale);
            if (darkText) r.endMultiplyBlend();
            y += label.lineHeight * kLabelLineGap * unit;
        }
        r.clearClip();
    }

    if (!r.drawsChrome())
        return;
    const double ix = boardScreen.right()  - bezel * 0.5;
    const double iy = boardScreen.bottom() - bezel * 0.5;
    const double is = resizeMarkerSize(bezel);
    const float markerLineWidth =
        static_cast<float>(resizeMarkerLineWidth(bezel));
    r.drawLine({ix-is,      iy-is*0.3}, {ix-is*0.3, iy-is},
               QColor(255,255,255,180), markerLineWidth);
    r.drawLine({ix-is*0.6,  iy-is*0.3}, {ix-is*0.3, iy-is*0.6},
               QColor(255,255,255,180), markerLineWidth);
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

QColor CanvasLauncher::contentAverageColor() const
{
    const qint64 key = m_icon.isNull() ? 0 : qint64(m_icon.cacheKey());
    if (key != m_avgColorKey) {
        m_avgColor = averageColorOfPixmap(m_icon);
        m_avgColorKey = key;
    }
    return m_avgColor;
}

// 2026-10-12: 枠（板・縁の線・リサイズの印）だけを描く（BubbleLayerCache の焼き付け用）
void CanvasLauncher::paintChromeGL(Renderer &r, const QRectF &screenRect, bool isActive,
                                   double pulseAlpha, double screenScale,
                                   double contentCornerFactor)
{
    const Renderer::ChromeMode prev = r.chromeMode();
    r.setChromeMode(Renderer::ChromeMode::ChromeOnly);
    paintGL(r, screenRect, isActive, pulseAlpha, screenScale, contentCornerFactor);
    r.setChromeMode(prev);
}

bool CanvasLauncher::launch() const
{
    // Webリンクは Windows の既定ブラウザで開く（QDesktopServiceがShellExecuteを使う）。
    if (m_kind == TargetKind::Web) {
        const bool ok = QDesktopServices::openUrl(QUrl(m_path));
        if (!ok)
            qWarning() << "[Launcher] failed to open web link:" << m_path;
        return ok;
    }
#ifdef Q_OS_WIN
    if (!isLaunchable()) {
        qWarning() << "[Launcher] target is missing:" << m_path;
        return false;
    }

    SHELLEXECUTEINFOW sei {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    const std::wstring file = toWide(m_path);
    sei.lpFile = file.c_str();
    sei.lpVerb = L"open";
    sei.nShow = SW_SHOWNORMAL;
    const BOOL ok = ShellExecuteExW(&sei);
    if (!ok)
        qWarning() << "[Launcher] ShellExecuteEx failed:" << m_path << GetLastError();
    return ok;
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(m_path));
#endif
}

bool CanvasLauncher::isLaunchable() const
{
    return validateTarget(m_path);
}

bool CanvasLauncher::isLauncherCandidate(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;
    if (info.isDir())
        return true;
    const QString suffix = info.suffix().toLower();
    return suffix == QStringLiteral("exe")
        || suffix == QStringLiteral("lnk")
        || info.isFile();
}

bool CanvasLauncher::isWebLinkCandidate(const QString &url)
{
    if (WebLinkIcon::displayHost(url).isEmpty())
        return false;
    // 日本語などが未エンコードのまま貼られることがあるのでTolerantで解析する。
    const QUrl u(url);
    if (!u.isValid())
        return false;
    const QString host = u.host().toLower();

    // 動画サイト（動画サイト用プラグインの対応ホスト）は、プラグインが未導入でも
    // Webリンクにはしない（従来どおり何も起きない/動画バブルになる）。
    static const QStringList videoHostSuffixes = {
        QStringLiteral("youtube.com"), QStringLiteral("youtu.be"),
        QStringLiteral("twitch.tv"),
        QStringLiteral("bilibili.com"), QStringLiteral("b23.tv"),
        QStringLiteral("nicovideo.jp"), QStringLiteral("nico.ms"),
        QStringLiteral("abema.tv"),
    };
    for (const QString &suffix : videoHostSuffixes) {
        if (host == suffix || host.endsWith(QLatin1Char('.') + suffix))
            return false;
    }

    // 動画/音声ファイルの直リンクもWebリンクにしない。
    static const QSet<QString> mediaExts = {
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"),
        QStringLiteral("mkv"), QStringLiteral("webm"), QStringLiteral("avi"),
        QStringLiteral("wmv"), QStringLiteral("flv"), QStringLiteral("mpeg"),
        QStringLiteral("mpg"), QStringLiteral("ts"), QStringLiteral("m2ts"),
        QStringLiteral("3gp"), QStringLiteral("m3u8"),
        QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("flac"),
        QStringLiteral("m4a"), QStringLiteral("ogg"),
    };
    const QString path = u.path();
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot >= 0 && mediaExts.contains(path.mid(dot + 1).toLower()))
        return false;
    return true;
}

CanvasLauncher::TargetKind CanvasLauncher::targetKindForPath(const QString &path)
{
    if (!WebLinkIcon::displayHost(path).isEmpty())
        return TargetKind::Web;
    const QFileInfo info(path);
    if (info.isDir())
        return TargetKind::Directory;
    if (info.suffix().compare(QStringLiteral("lnk"), Qt::CaseInsensitive) == 0)
        return TargetKind::Shortcut;
    return TargetKind::File;
}

bool CanvasLauncher::validateTarget(const QString &path)
{
    if (!WebLinkIcon::displayHost(path).isEmpty())
        return true;
    const QFileInfo info(path);
    if (!info.exists())
        return false;
    if (info.suffix().compare(QStringLiteral("lnk"), Qt::CaseInsensitive) != 0)
        return true;
    const QString target = resolveShortcutTarget(path);
    return !target.isEmpty() && QFileInfo::exists(target);
}

QPixmap CanvasLauncher::iconForPath(const QString &path, int size)
{
    // 2026-07-11: レイヤー切り替えのたびにrestoreSessionFromJson()から
    // 呼ばれるため、同じパスのランチャーが多いレイヤーへ出入りするたびに
    // QFileIconProvider（内部でWindows Shellへ問い合わせる、地味に遅い
    // 同期API）を毎回叩いていた。パス+サイズをキーにアイコンの
    // Pixmapをプロセス内キャッシュしておき、2回目以降は即座に返す。
    // ファイルのアイコンが実行中に変わるケース（拡張子変更等）は稀なため、
    // 素朴なキャッシュのみで無効化ロジックは持たせない。
    // Webリンクはfaviconが後から届く/更新されるのでここでは扱わない。
    if (!WebLinkIcon::displayHost(path).isEmpty()) {
        const QPixmap cached = WebLinkIcon::loadCached(path);
        return cached.isNull() ? WebLinkIcon::fallbackIcon(path, size) : cached;
    }
    static QHash<QString, QPixmap> cache;
    const QString cacheKey = path + QLatin1Char(':') + QString::number(size);
    const auto cached = cache.constFind(cacheKey);
    if (cached != cache.constEnd())
        return cached.value();

    QFileIconProvider provider;
    QString iconPath = path;
    if (targetKindForPath(path) == TargetKind::Shortcut) {
        const QString target = resolveShortcutTarget(path);
        if (!target.isEmpty() && QFileInfo::exists(target))
            iconPath = target;
    }

    QIcon icon = provider.icon(QFileInfo(iconPath));
    QPixmap pix = icon.pixmap(size, size);
    if (pix.isNull())
        pix = fallbackIcon(size);
    cache.insert(cacheKey, pix);
    return pix;
}

QString CanvasLauncher::displayNameForPath(const QString &path)
{
    const QString webHost = WebLinkIcon::displayHost(path);
    if (!webHost.isEmpty())
        return webHost;
    const QFileInfo info(path);
    if (info.isDir()) {
        const QString name = info.fileName();
        return name.isEmpty() ? QDir::toNativeSeparators(info.absoluteFilePath()) : name;
    }

    const QString base = info.completeBaseName();
    if (!base.isEmpty())
        return base;
    return info.fileName();
}

QString CanvasLauncher::resolveShortcutTarget(const QString &path)
{
#ifdef Q_OS_WIN
    // 2026-07-15 バグ修正（D&Dが🚫固定になる不具合の根本原因の一つ）:
    // この関数はCanvasWidget::addLauncher() -> validateTarget() 経由で、
    // .lnkファイルのドロップを処理している最中（＝Qt/OLEのIDropTarget::
    // Drop()コールバックがまだスタック上に残ったまま）にメインスレッド上で
    // 同期的に呼ばれる。ここで生のCoInitializeEx()/CoUninitialize()を
    // 呼ぶと、メインスレッドの既存OLEアパートメント（RegisterDragDropの
    // 登録元）に対して、ライブなドラッグ&ドロップ操作の途中に再入的な
    // COM初期化/終了ペアを割り込ませることになる。CoInitializeExの戻り値
    // 自体（S_FALSE=既に初期化済み）は無害だが、対になるCoUninitialize()を
    // このタイミングで呼ぶと、Qt/OLEが張っている登録を巻き添えで壊す
    // 引き金になり得る。
    // このスレッド（メインスレッド）のCOM/OLEアパートメントはアプリ全体で
    // 共有・管理されており、この関数が独占的に初期化/終了させるべきもの
    // ではないため、CoUninitialize()は呼ばない（=あえて破棄しない）。
    // MainWindow::refreshDropTargets()等、既存の診断コードも同様の理由で
    // OleInitialize()のみ呼びOleUninitialize()を省略している箇所がある
    // （「わずかな参照カウントの偏り」より「D&D登録の巻き添え破壊」の方が
    // 実害が大きいため、意図的にリークさせて安全側に倒す方針）。
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IShellLinkW *link = nullptr;
    const HRESULT createHr = CoCreateInstance(CLSID_ShellLink, nullptr,
                                              CLSCTX_INPROC_SERVER,
                                              IID_IShellLinkW,
                                              reinterpret_cast<void **>(&link));
    if (FAILED(createHr) || !link)
        return {};

    IPersistFile *persist = nullptr;
    const HRESULT persistHr = link->QueryInterface(IID_IPersistFile,
                                                   reinterpret_cast<void **>(&persist));
    if (FAILED(persistHr) || !persist) {
        link->Release();
        return {};
    }

    const std::wstring shortcutPath = toWide(path);
    const HRESULT loadHr = persist->Load(shortcutPath.c_str(), STGM_READ);
    QString target;
    if (SUCCEEDED(loadHr)) {
        wchar_t buffer[MAX_PATH] = {};
        WIN32_FIND_DATAW findData {};
        const HRESULT pathHr = link->GetPath(buffer, MAX_PATH, &findData, SLGP_UNCPRIORITY);
        if (SUCCEEDED(pathHr) && buffer[0] != L'\0')
            target = QString::fromWCharArray(buffer);
    }

    persist->Release();
    link->Release();
    return target;
#else
    Q_UNUSED(path);
    return {};
#endif
}
