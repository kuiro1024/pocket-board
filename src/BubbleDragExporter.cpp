#include "BubbleDragExporter.h"

#include "CanvasImage.h"
#include "CanvasItem.h"
#include "CanvasLauncher.h"
#include "CanvasVideo.h"
#include "WebImageCache.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDrag>
#include <QDebug>
#include <QEasingCurve>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QFont>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <utility>

#include <QFontMetricsF>
#include <QPen>

namespace {

constexpr int kMaxPreviewWidth = 160;
constexpr int kMaxPreviewHeight = 120;
constexpr int kMaxLauncherPreviewSize = 96;
constexpr int kFallbackPreviewSize = 72;
// WebImageCacheのセッション復元用永続キャッシュとは無関係。
// 外部アプリへファイルとして渡すために%TEMP%へ生成する複製だけの保持時間。
constexpr int kExternalDragTemporaryFileLifetimeMs = 30 * 60 * 1000;

QPixmap contentPixmap(CanvasItem *item)
{
    if (auto *image = dynamic_cast<CanvasImage *>(item))
        return image->pixmap();

    if (auto *video = canvasItemAsVideo(item)) {
        const QImage thumbnail = video->thumbnailImage();
        if (!thumbnail.isNull())
            return QPixmap::fromImage(thumbnail);
    }

    if (auto *launcher = dynamic_cast<CanvasLauncher *>(item))
        return launcher->icon();

    return {};
}

QUrl urlFromSource(const QString &source)
{
    if (source.isEmpty())
        return {};

    // 古いセッション等に生のWindowsパスが残っていても、"C:"をURLの
    // スキームと誤認せずローカルファイルとして書き出す。
    if (QFileInfo::exists(source))
        return QUrl::fromLocalFile(source);
    const QUrl url(source);
    if (url.isValid() && !url.scheme().isEmpty())
        return url;
    return {};
}

QString safeTemporaryFileStem(const QString &title)
{
    QString stem = QFileInfo(title).completeBaseName().trimmed();
    if (stem.isEmpty())
        stem = QStringLiteral("PocketBoard Image");
    stem.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")),
                 QStringLiteral("_"));
    return stem.left(60);
}

QString itemExportKind(CanvasItem *item)
{
    if (dynamic_cast<CanvasImage *>(item))
        return QStringLiteral("image");
    if (auto *video = canvasItemAsVideo(item))
        return video->mediaKind() == MediaKind::LocalAudio
            ? QStringLiteral("audio")
            : QStringLiteral("video");
    if (dynamic_cast<CanvasLauncher *>(item))
        return QStringLiteral("launcher");
    return QStringLiteral("unknown");
}

} // namespace

BubbleDragExporter::BubbleDragExporter(QObject *parent)
    : QObject(parent)
{
}

void BubbleDragExporter::startExternalDrag(
    QWidget *sourceWidget,
    CanvasItem *item,
    const QPointF &grabPointCanvas,
    std::function<bool()> canStartDrag,
    std::function<void(Qt::DropAction)> dragFinished)
{
    startExternalDrag(sourceWidget, item, QList<CanvasItem*>{item}, grabPointCanvas,
                      std::move(canStartDrag), std::move(dragFinished));
}

void BubbleDragExporter::startExternalDrag(
    QWidget *sourceWidget,
    CanvasItem *item,
    const QList<CanvasItem*> &itemsIn,
    const QPointF &grabPointCanvas,
    std::function<bool()> canStartDrag,
    std::function<void(Qt::DropAction)> dragFinished)
{
    // 書き出す一覧。primary が入っていなければ先頭に足す。
    QList<CanvasItem*> items;
    for (CanvasItem *it : itemsIn)
        if (it && !items.contains(it)) items.append(it);
    if (item && !items.contains(item))
        items.prepend(item);

    if (!sourceWidget || !item) {
        if (dragFinished)
            dragFinished(Qt::IgnoreAction);
        return;
    }

    // OSドラッグ画像は開始後に更新できないため、PB内で短い拡大を完了させて
    // から静止プレビューへ受け渡す。途中でボタンを離した／入力状態が変わった
    // 場合は、ネイティブドラッグ自体を開始しない。
    QPointer<QWidget> sourceGuard(sourceWidget);
    auto *animation = new QVariantAnimation(this);
    animation->setDuration(135);
    animation->setStartValue(qMax(1.0, item->liftScale()));
    animation->setEndValue(1.095);
    animation->setEasingCurve(QEasingCurve::OutBack);

    QObject::connect(animation, &QVariantAnimation::valueChanged, animation,
                     [sourceGuard, item, canStartDrag](const QVariant &value) {
        if (!sourceGuard || (canStartDrag && !canStartDrag()))
            return;
        item->setLiftScale(value.toDouble());
        sourceGuard->update();
    });
    QObject::connect(animation, &QVariantAnimation::finished, animation,
                     [this, sourceGuard, item, items, grabPointCanvas,
                      canStartDrag, dragFinished] {
        Qt::DropAction action = Qt::IgnoreAction;
        const bool canStart = sourceGuard
            && (QApplication::mouseButtons() & Qt::LeftButton)
            && (!canStartDrag || canStartDrag());
        if (canStart)
            action = executeExternalDrag(sourceGuard, item, items, grabPointCanvas);
        if (sourceGuard && dragFinished)
            dragFinished(action);
    });
    QObject::connect(animation, &QVariantAnimation::finished,
                     animation, &QObject::deleteLater);
    animation->start();
}

Qt::DropAction BubbleDragExporter::executeExternalDrag(
    QWidget *sourceWidget,
    CanvasItem *item,
    const QList<CanvasItem*> &items,
    const QPointF &grabPointCanvas)
{
    if (!sourceWidget || !item)
        return Qt::IgnoreAction;

    // 2026-09-20: 掴んでから動かせるまでの待ちの内訳を残す（MIME作成／プレビュー）。
    QElapsedTimer prepareClock;
    prepareClock.start();
    QMimeData *mime = createMimeData(items, ExportPurpose::ExternalDrag);
    if (!mime)
        return Qt::IgnoreAction;
    const qint64 mimeMs = prepareClock.elapsed();

    const QString exportKind = items.size() > 1
        ? QStringLiteral("multi(%1)").arg(items.size())
        : itemExportKind(item);
    const QList<QUrl> urls = mime->urls();
    const bool localFileDrag = !urls.isEmpty()
        && std::all_of(urls.cbegin(), urls.cend(), [](const QUrl &url) {
            return url.isLocalFile();
    });
    qDebug() << "[BubbleExport] drag begin"
             << "kind=" << exportKind
             << "profile=" << (localFileDrag ? "windows-file" : "link")
             << "formats=" << mime->formats()
             << "urls=" << urls;

    QDrag drag(sourceWidget);
    drag.setMimeData(mime);
    const QPixmap preview = createDragPreview(item, int(items.size()));
    if (!preview.isNull()) {
        drag.setPixmap(preview);
        drag.setHotSpot(previewHotSpot(item, grabPointCanvas, preview));
    }
    qDebug() << "[BubbleExport] prepare ms: mime=" << mimeMs
             << "preview=" << (prepareClock.elapsed() - mimeMs);
    const Qt::DropAction action = drag.exec(Qt::CopyAction, Qt::CopyAction);
    qDebug() << "[BubbleExport] drag end"
             << "kind=" << exportKind
             << "accepted=" << (action != Qt::IgnoreAction)
             << "action=" << action;
    return action;
}

bool BubbleDragExporter::copyToClipboard(const QList<CanvasItem*> &items)
{
    QMimeData *mime = createMimeData(items, ExportPurpose::Clipboard);
    if (!mime)
        return false;

    QApplication::clipboard()->setMimeData(mime);
    return true;
}

// 2026-09-20: 複数バブルの書き出し。1個ずつ作ったMIMEを束ねる。
//   ・ローカルファイル（元ファイル／一時PNG／ランチャーの実体）は URL リストに
//     まとめる（Windows では CF_HDROP＝複数ファイルのドロップになる）。
//   ・Web動画など実ファイルでないものは、ファイルと混ぜると Explorer 等が
//     受け取れなくなるので、ローカルファイルが1つでもあればテキスト（改行区切り）
//     に回す。ローカルファイルが1つも無ければ URL リスト＋テキストにする。
//   ・書き出せないバブル（読み込み失敗など）は飛ばす。
QMimeData *BubbleDragExporter::createMimeData(const QList<CanvasItem*> &items, ExportPurpose purpose)
{
    if (items.isEmpty())
        return nullptr;
    if (items.size() == 1)
        return createMimeData(items.first(), purpose);

    QList<QUrl> localUrls;
    QList<QUrl> remoteUrls;
    QStringList texts;
    for (CanvasItem *item : items) {
        // クリップボード用でも、束ねるときは画像のビットマップ（1枚しか持てない）は
        // 使わないので、ファイルとして作る ExternalDrag 形式で作る。
        std::unique_ptr<QMimeData> single(createMimeData(item, ExportPurpose::ExternalDrag));
        if (!single)
            continue;
        for (const QUrl &url : single->urls()) {
            if (url.isLocalFile()) {
                if (!localUrls.contains(url)) localUrls.append(url);
            } else if (!remoteUrls.contains(url)) {
                remoteUrls.append(url);
            }
        }
    }
    for (const QUrl &url : std::as_const(remoteUrls))
        texts << url.toString();
    if (purpose == ExportPurpose::Clipboard)
        for (const QUrl &url : std::as_const(localUrls))
            texts << url.toLocalFile();

    if (localUrls.isEmpty() && remoteUrls.isEmpty())
        return nullptr;

    auto *mime = new QMimeData();
    mime->setUrls(localUrls.isEmpty() ? remoteUrls : localUrls);
    if (!texts.isEmpty() && (purpose == ExportPurpose::Clipboard || localUrls.isEmpty()
                             || !remoteUrls.isEmpty()))
        mime->setText(texts.join(QLatin1Char('\n')));
    return mime;
}

bool BubbleDragExporter::copyToClipboard(CanvasItem *item)
{
    QMimeData *mime = createMimeData(item, ExportPurpose::Clipboard);
    if (!mime)
        return false;

    QApplication::clipboard()->setMimeData(mime);
    return true;
}

QMimeData *BubbleDragExporter::createMimeData(CanvasItem *item, ExportPurpose purpose)
{
    if (!item)
        return nullptr;

    auto *mime = new QMimeData();

    if (auto *image = dynamic_cast<CanvasImage *>(item)) {
        const QPixmap &pixmap = image->pixmap();
        if (pixmap.isNull()) {
            delete mime;
            return nullptr;
        }

        if (purpose == ExportPurpose::Clipboard)
            mime->setImageData(pixmap.toImage());

        // ローカル画像は元ファイルをそのまま渡し、GIF/WebP等の形式と
        // ファイル名を維持する。Web画像・キャプチャ画像だけ一時PNGにする。
        const QString sourcePath = image->sourceFilePath();
        if (!sourcePath.isEmpty() && QFileInfo::exists(sourcePath)) {
            mime->setUrls({QUrl::fromLocalFile(sourcePath)});
        } else {
            const QString temporaryPath = temporaryFileForImage(image);
            if (!temporaryPath.isEmpty())
                mime->setUrls({QUrl::fromLocalFile(temporaryPath)});

            const QUrl originalUrl = urlFromSource(image->sourceUrl());
            if (purpose == ExportPurpose::Clipboard
                && originalUrl.scheme().startsWith(QStringLiteral("http")))
                mime->setText(originalUrl.toString());
        }
    } else if (auto *video = canvasItemAsVideo(item)) {
        const QString source = video->sourceUrl();
        const QUrl url = urlFromSource(source);
        if (source.isEmpty() || url.isEmpty()) {
            delete mime;
            return nullptr;
        }
        mime->setUrls({url});
        // ファイルドラッグではCF_HDROP相当へ寄せるためURLだけを提示する。
        // HTTP(S)は実ファイルではないのでリンクドラッグとしてテキストも付ける。
        if (purpose == ExportPurpose::Clipboard || !url.isLocalFile())
            mime->setText(source);
    } else if (auto *launcher = dynamic_cast<CanvasLauncher *>(item)) {
        if (launcher->isWebLink()) {
            // Webリンクはブラウザ等へURLとしてドラッグ/コピーできるようにする。
            mime->setUrls({QUrl(launcher->path())});
            mime->setText(launcher->path());
        } else {
            if (launcher->path().isEmpty() || !QFileInfo::exists(launcher->path())) {
                delete mime;
                return nullptr;
            }
            mime->setUrls({QUrl::fromLocalFile(launcher->path())});
            if (purpose == ExportPurpose::Clipboard)
                mime->setText(launcher->path());
        }
    } else {
        delete mime;
        return nullptr;
    }

    if (!mime->hasUrls() && !mime->hasText() && !mime->hasImage()) {
        delete mime;
        return nullptr;
    }
    return mime;
}

QPixmap BubbleDragExporter::createDragPreview(CanvasItem *item) const
{
    const QPixmap content = contentPixmap(item);
    if (!content.isNull()) {
        const QSize bounds = dynamic_cast<CanvasLauncher *>(item)
            ? QSize(kMaxLauncherPreviewSize, kMaxLauncherPreviewSize)
            : QSize(kMaxPreviewWidth, kMaxPreviewHeight);
        // 2026-09-20: 4K級の画像を一気に Smooth で縮めると数十msかかるので、
        // 目標の2倍までは Fast で落としてから Smooth で仕上げる（見た目は同じ）。
        if (content.width() > bounds.width() * 4 || content.height() > bounds.height() * 4) {
            const QPixmap coarse = content.scaled(bounds * 2, Qt::KeepAspectRatio,
                                                  Qt::FastTransformation);
            return coarse.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        return content.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // サムネイル未取得時だけ透明背景の最小限の記号を出す。
    // 枠・カード・タイトルは付けず、掴んでいる種別だけを伝える。
    QPixmap fallback(kFallbackPreviewSize, kFallbackPreviewSize);
    fallback.fill(Qt::transparent);
    QPainter painter(&fallback);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont symbolFont(QStringLiteral("Segoe UI Symbol"), 38);
    painter.setFont(symbolFont);
    painter.setPen(QColor(255, 255, 255, 235));
    const bool audio = canvasItemAsVideo(item)
        && canvasItemAsVideo(item)->mediaKind() == MediaKind::LocalAudio;
    painter.drawText(fallback.rect(), Qt::AlignCenter,
                     audio ? QStringLiteral("♪") : QStringLiteral("▶"));
    return fallback;
}

QString BubbleDragExporter::temporaryFileForImage(CanvasImage *image)
{
    if (!image)
        return {};
    const QPixmap &pixmap = image->pixmap();
    if (pixmap.isNull())
        return {};

    // 使い回し: 受け取った側がドロップ後しばらくしてから読み直すことがある
    // ので、削除（kExternalDragTemporaryFileLifetimeMs）までの残りが十分な
    // ものだけ使う。
    constexpr qint64 kReuseLimitMs = kExternalDragTemporaryFileLifetimeMs - 10 * 60 * 1000;
    const qint64 key = pixmap.cacheKey();
    auto found = m_temporaryExports.find(key);
    if (found != m_temporaryExports.end()) {
        if (found->file && found->age.elapsed() < kReuseLimitMs
            && QFileInfo::exists(found->file->fileName()))
            return found->file->fileName();
        m_temporaryExports.erase(found);
    }

    QElapsedTimer cost;
    cost.start();
    const QString stem = safeTemporaryFileStem(image->title());
    QTemporaryFile *temporary = nullptr;
    const char *how = "";

    // 1) WebImageCache にある元のバイト列をそのままコピー（再エンコードしない）。
    //    貼り付けたスクショ（pocketboard-capture:…）もここに入っている。
    const QString sourceUrl = image->sourceUrl();
    const QString cachedPath = sourceUrl.isEmpty()
        ? QString() : WebImageCache::cachedFilePathForUrl(sourceUrl);
    if (!cachedPath.isEmpty() && QFileInfo::exists(cachedPath)) {
        QString ext = QString::fromLatin1(QImageReader::imageFormat(cachedPath)).toLower();
        if (ext == QLatin1String("jpeg")) ext = QStringLiteral("jpg");
        if (ext.isEmpty()) ext = QStringLiteral("png");
        auto *copy = new QTemporaryFile(
            QDir::tempPath() + QStringLiteral("/%1_XXXXXX.%2").arg(stem, ext), this);
        QFile source(cachedPath);
        if (copy->open() && source.open(QIODevice::ReadOnly)
            && copy->write(source.readAll()) > 0) {
            copy->close();
            temporary = copy;
            how = "copied-cache";
        } else {
            delete copy;
        }
    }

    // 2) 無ければPNGへ。圧縮を弱めて速さを優先する（quality 80 ≒ zlib 1）。
    //    受け渡し専用の一時ファイルなので、サイズより掴んだ瞬間の軽さを取る。
    if (!temporary) {
        auto *encoded = new QTemporaryFile(
            QDir::tempPath() + QStringLiteral("/%1_XXXXXX.png").arg(stem), this);
        if (encoded->open() && pixmap.save(encoded, "PNG", 80)) {
            encoded->flush();
            encoded->close();
            temporary = encoded;
            how = "encoded-png";
        } else {
            delete encoded;
        }
    }
    if (!temporary)
        return {};

    // これはDiscord等への受け渡し専用コピー。MediaSourceや
    // WebImageCacheの復元元には使われない。外部アプリはdrop後に
    // パスを読み直すことがあるため即時削除せず、十分な猶予を置く。
    QTimer::singleShot(kExternalDragTemporaryFileLifetimeMs,
                       temporary, &QObject::deleteLater);
    TemporaryExport entry;
    entry.file = temporary;
    entry.age.start();
    m_temporaryExports.insert(key, entry);
    qDebug() << "[BubbleExport] temporary image" << how << pixmap.size()
             << "ms=" << cost.elapsed();
    return temporary->fileName();
}

QPixmap BubbleDragExporter::createDragPreview(CanvasItem *primary, int count) const
{
    QPixmap base = createDragPreview(primary);
    if (count <= 1 || base.isNull())
        return base;

    // 右上に個数バッジ。プレビューの外へはみ出さないよう内側に置く
    // （はみ出す分の余白を足すと掴み位置〈hotSpot〉の計算がずれるため）。
    const QSizeF size = base.deviceIndependentSize();
    QPixmap out = base;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font(QStringLiteral("Segoe UI"));
    font.setBold(true);
    font.setPixelSize(13);
    painter.setFont(font);
    const QString label = QString::number(count);
    const double w = qMax(22.0, QFontMetricsF(font).horizontalAdvance(label) + 12.0);
    const double h = 22.0;
    const QRectF badge(size.width() - w - 4.0, 4.0, w, h);
    painter.setPen(QPen(QColor(255, 255, 255, 230), 1.5));
    painter.setBrush(QColor(0x3a, 0xa0, 0xe8));
    painter.drawRoundedRect(badge, h / 2.0, h / 2.0);
    painter.setPen(Qt::white);
    painter.drawText(badge, Qt::AlignCenter, label);
    painter.end();
    return out;
}

QPoint BubbleDragExporter::previewHotSpot(CanvasItem *item,
                                          const QPointF &grabPointCanvas,
                                          const QPixmap &preview) const
{
    const QSizeF previewSize = preview.deviceIndependentSize();
    if (!item || previewSize.isEmpty())
        return QPoint(qRound(previewSize.width() * 0.5),
                      qRound(previewSize.height() * 0.5));

    const QRectF itemRect = item->itemRect();
    if (itemRect.isEmpty())
        return QPoint(qRound(previewSize.width() * 0.5),
                      qRound(previewSize.height() * 0.5));

    const qreal relativeX = qBound(0.0,
        (grabPointCanvas.x() - itemRect.left()) / itemRect.width(), 1.0);
    const qreal relativeY = qBound(0.0,
        (grabPointCanvas.y() - itemRect.top()) / itemRect.height(), 1.0);
    return QPoint(qRound(relativeX * previewSize.width()),
                  qRound(relativeY * previewSize.height()));
}
