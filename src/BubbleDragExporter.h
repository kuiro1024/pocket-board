#pragma once

#include <QObject>
#include <QPointF>
#include <Qt>

#include <functional>

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>

class CanvasImage;
class CanvasItem;
class QTemporaryFile;
class QMimeData;
class QPixmap;
class QWidget;

// CanvasWidget から OS へのドラッグ／クリップボード書き出しを分離する。
// CanvasWidget は入力状態だけを管理し、このクラスがバブル種別ごとの
// MIME、ドラッグプレビュー、一時ファイルの寿命を引き受ける。
class BubbleDragExporter final : public QObject
{
public:
    explicit BubbleDragExporter(QObject *parent = nullptr);

    void startExternalDrag(QWidget *sourceWidget,
                           CanvasItem *item,
                           const QPointF &grabPointCanvas,
                           std::function<bool()> canStartDrag = {},
                           std::function<void(Qt::DropAction)> dragFinished = {});
    // 2026-09-20: 複数バブルをまとめて書き出す（範囲選択中の逆輸入）。
    // primary は掴んだバブル（拡大演出・プレビュー・掴み位置の基準）。
    // items は書き出す全バブル（primary を含む。並び順がそのまま渡る順番）。
    void startExternalDrag(QWidget *sourceWidget,
                           CanvasItem *primary,
                           const QList<CanvasItem*> &items,
                           const QPointF &grabPointCanvas,
                           std::function<bool()> canStartDrag = {},
                           std::function<void(Qt::DropAction)> dragFinished = {});
    bool copyToClipboard(CanvasItem *item);
    bool copyToClipboard(const QList<CanvasItem*> &items);

private:
    enum class ExportPurpose {
        ExternalDrag,
        Clipboard,
    };

    QMimeData *createMimeData(CanvasItem *item, ExportPurpose purpose);
    // 複数バブルぶんを1つのMIMEにまとめる（1個ならcreateMimeDataと同じ）。
    QMimeData *createMimeData(const QList<CanvasItem*> &items, ExportPurpose purpose);
    Qt::DropAction executeExternalDrag(QWidget *sourceWidget,
                                       CanvasItem *primary,
                                       const QList<CanvasItem*> &items,
                                       const QPointF &grabPointCanvas);
    QPixmap createDragPreview(CanvasItem *item) const;
    // 複数書き出し時のプレビュー（掴んだバブルの絵＋右上に個数バッジ）。
    QPixmap createDragPreview(CanvasItem *primary, int count) const;
    // 2026-09-20: 画像バブル（元ファイルの無いWeb画像・貼り付けたスクショ等）を
    // 外へ渡すための一時ファイル。以前は掴むたびに毎回PNGへ再エンコードして
    // いて、大きい画像や複数選択だと掴んでから動かせるまで1〜2秒止まっていた。
    //   ・WebImageCache に元のバイト列があれば、再エンコードせずそのままコピー
    //   ・無ければ低圧縮（速い）でPNG化
    //   ・同じ画像（pixmap.cacheKey）はしばらく作ったファイルを使い回す
    QString temporaryFileForImage(CanvasImage *image);
    struct TemporaryExport {
        QPointer<QTemporaryFile> file;
        QElapsedTimer age;
    };
    QHash<qint64, TemporaryExport> m_temporaryExports;

    QPoint previewHotSpot(CanvasItem *item, const QPointF &grabPointCanvas,
                          const QPixmap &preview) const;
};
