#include "CanvasSelectionHelper.h"

#include "CanvasItem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <QVector>

QList<CanvasItem*> CanvasSelectionHelper::itemsInCanvasRect(const QList<CanvasItem*> &items,
                                                            const QRectF &canvasRect)
{
    const QRectF rect = canvasRect.normalized();
    QList<CanvasItem*> result;
    for (CanvasItem *item : items) {
        // 2026-07-09: 以前は「中心点が矩形に入っているか」で判定していたが、
        // バブルに少しでも触れたら選択されるように、矩形同士の交差判定へ変更
        // （boardRect はクリックのヒットテストと同じ「見た目のバブル矩形」）。
        if (item && rect.intersects(item->boardRect()))
            result.append(item);
    }
    return result;
}


QSet<CanvasItem*> CanvasSelectionHelper::itemSet(const QList<CanvasItem*> &items)
{
    QSet<CanvasItem*> result;
    for (CanvasItem *item : items) {
        if (item)
            result.insert(item);
    }
    return result;
}

QList<CanvasItem*> CanvasSelectionHelper::takeItems(QSet<CanvasItem*> *items)
{
    if (!items)
        return {};

    const QList<CanvasItem*> result = items->values();
    items->clear();
    return result;
}

QHash<CanvasItem*, QPointF> CanvasSelectionHelper::itemPositions(const QList<CanvasItem*> &items)
{
    QHash<CanvasItem*, QPointF> positions;
    for (CanvasItem *item : items) {
        if (item)
            positions.insert(item, item->pos());
    }
    return positions;
}

QHash<CanvasItem*, QPointF> CanvasSelectionHelper::itemPositions(const QSet<CanvasItem*> &items)
{
    QHash<CanvasItem*, QPointF> positions;
    for (CanvasItem *item : items) {
        if (item)
            positions.insert(item, item->pos());
    }
    return positions;
}

void CanvasSelectionHelper::applyPositionOffset(const QHash<CanvasItem*, QPointF> &positions,
                                                const QPointF &offset)
{
    for (auto it = positions.constBegin(); it != positions.constEnd(); ++it) {
        if (it.key())
            it.key()->setPos(it.value() + offset);
    }
}


// ---- 2026-09-20: 範囲選択の整列 --------------------------------------------

QList<CanvasItem*> CanvasSelectionHelper::readingOrder(const QSet<CanvasItem*> &items)
{
    QList<CanvasItem*> ordered;
    for (CanvasItem *item : items)
        if (item) ordered.append(item);
    // まず中心のyで上から。QSetの列挙順は不定なので、同じyでもxで決める。
    std::stable_sort(ordered.begin(), ordered.end(), [](CanvasItem *a, CanvasItem *b) {
        const QPointF ca = a->boardRect().center(), cb = b->boardRect().center();
        if (ca.y() != cb.y()) return ca.y() < cb.y();
        return ca.x() < cb.x();
    });
    // 行の先頭バブルの高さの半分以内に中心がある間は同じ行、行の中は左から。
    QList<CanvasItem*> result;
    result.reserve(ordered.size());
    int rowStart = 0;
    while (rowStart < ordered.size()) {
        const QRectF head = ordered.at(rowStart)->boardRect();
        int rowEnd = rowStart + 1;
        while (rowEnd < ordered.size()
               && ordered.at(rowEnd)->boardRect().center().y() - head.center().y()
                      <= head.height() * 0.5)
            ++rowEnd;
        QList<CanvasItem*> row = ordered.mid(rowStart, rowEnd - rowStart);
        std::stable_sort(row.begin(), row.end(), [](CanvasItem *a, CanvasItem *b) {
            return a->boardRect().left() < b->boardRect().left();
        });
        result += row;
        rowStart = rowEnd;
    }
    return result;
}

QList<CanvasItem*> CanvasSelectionHelper::sizeOrder(const QSet<CanvasItem*> &items)
{
    QList<CanvasItem*> ordered = readingOrder(items);
    // 高さを5%刻みの段にまとめる（ほぼ同じ大きさ＝同じ段）。段が大きい順、
    // 同じ段の中は幅が大きい順。それも同じなら見た目の並び順（stable）。
    const auto heightBucket = [](CanvasItem *it) {
        const double h = qMax(1.0, it->boardRect().height());
        return static_cast<int>(std::floor(std::log(h) / std::log(1.05)));
    };
    const auto widthBucket = [](CanvasItem *it) {
        const double w = qMax(1.0, it->boardRect().width());
        return static_cast<int>(std::floor(std::log(w) / std::log(1.05)));
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](CanvasItem *a, CanvasItem *b) {
        const int ha = heightBucket(a), hb = heightBucket(b);
        if (ha != hb) return ha > hb;
        return widthBucket(a) > widthBucket(b);
    });
    return ordered;
}

namespace {
struct PackRow { int first = 0; int count = 0; double width = 0.0; double height = 0.0; };

// 幅 maxWidth で行に詰める（はみ出したら次の行へ。1行目の先頭は必ず置く）。
QVector<PackRow> packRows(const QVector<QSizeF> &sizes, double maxWidth, double spacing)
{
    QVector<PackRow> rows;
    PackRow row;
    for (int i = 0; i < sizes.size(); ++i) {
        const QSizeF sz = sizes.at(i);
        const double add = (row.count > 0 ? spacing : 0.0) + sz.width();
        if (row.count > 0 && row.width + add > maxWidth + 0.01) {
            rows.append(row);
            row = PackRow();
            row.first = i;
        }
        row.width += (row.count > 0 ? spacing : 0.0) + sz.width();
        row.height = qMax(row.height, sz.height());
        ++row.count;
    }
    if (row.count > 0)
        rows.append(row);
    return rows;
}
} // namespace

QHash<CanvasItem*, QPointF> CanvasSelectionHelper::packedPositions(const QList<CanvasItem*> &orderedIn,
                                                                   const QPointF &topLeft,
                                                                   double spacing,
                                                                   double targetAspect)
{
    QHash<CanvasItem*, QPointF> positions;
    QList<CanvasItem*> ordered;
    QVector<QSizeF> sizes;
    double itemArea = 0.0;
    double maxItemWidth = 0.0;
    double totalWidth = 0.0;
    for (CanvasItem *item : orderedIn) {
        if (!item) continue;
        const QSizeF sz = item->boardRect().size();
        if (sz.isEmpty()) continue;
        ordered.append(item);
        sizes.append(sz);
        itemArea += sz.width() * sz.height();
        maxItemWidth = qMax(maxItemWidth, sz.width());
        totalWidth += sz.width() + (sizes.size() > 1 ? spacing : 0.0);
    }
    if (ordered.isEmpty())
        return positions;
    if (!(targetAspect > 0.0))
        targetAspect = 16.0 / 9.0;

    // 行の幅の候補: 「先頭から k 個を1行に置いたときの幅」すべて＋最大の1個の幅。
    // 行の切れ目はこの候補のどれかで必ず起きるので、これで取りこぼしは無い。
    QVector<double> candidates;
    candidates.append(maxItemWidth);
    {
        double w = 0.0;
        for (int i = 0; i < sizes.size(); ++i) {
            w += (i > 0 ? spacing : 0.0) + sizes.at(i).width();
            if (w >= maxItemWidth)
                candidates.append(w);
        }
    }

    // 評価: すき間の割合（外接矩形の面積に対する空き）＋縦横比の外れ具合。
    double bestCost = std::numeric_limits<double>::max();
    QVector<PackRow> bestRows;
    for (double cand : std::as_const(candidates)) {
        const QVector<PackRow> rows = packRows(sizes, cand, spacing);
        double bw = 0.0, bh = 0.0;
        for (const PackRow &r : rows) {
            bw = qMax(bw, r.width);
            bh += r.height;
        }
        bh += spacing * qMax(0, int(rows.size()) - 1);
        if (bw <= 0.0 || bh <= 0.0) continue;
        const double waste = (bw * bh - itemArea) / (bw * bh);
        const double aspectMiss = std::abs(std::log((bw / bh) / targetAspect));
        const double cost = waste + 0.35 * aspectMiss;
        if (cost < bestCost - 1e-9) {
            bestCost = cost;
            bestRows = rows;
        }
    }
    if (bestRows.isEmpty())
        bestRows = packRows(sizes, totalWidth, spacing);

    double y = topLeft.y();
    for (const PackRow &r : std::as_const(bestRows)) {
        double x = topLeft.x();
        for (int i = r.first; i < r.first + r.count; ++i) {
            CanvasItem *item = ordered.at(i);
            const QRectF board = item->boardRect();
            const QPointF itemOffsetFromBoard = item->pos() - board.topLeft();
            // 行の中では上下中央に揃える。
            const double yInRow = (r.height - board.height()) * 0.5;
            positions.insert(item, QPointF(x, y + yInRow) + itemOffsetFromBoard);
            x += board.width() + spacing;
        }
        y += r.height + spacing;
    }
    return positions;
}

QSet<CanvasItem*> CanvasSelectionHelper::positionedItems(const QHash<CanvasItem*, QPointF> &positions)
{
    QSet<CanvasItem*> items;
    for (auto it = positions.constBegin(); it != positions.constEnd(); ++it) {
        if (it.key())
            items.insert(it.key());
    }
    return items;
}

QSet<CanvasItem*> CanvasSelectionHelper::claimExclusiveItems(const QSet<CanvasItem*> &items,
                                                             QSet<CanvasItem*> *claimedItems)
{
    QSet<CanvasItem*> exclusiveItems = items;
    if (!claimedItems)
        return exclusiveItems;

    exclusiveItems.subtract(*claimedItems);
    claimedItems->unite(exclusiveItems);
    return exclusiveItems;
}






QPointF CanvasSelectionHelper::separationOffset(const QRectF &fixedRect,
                                                const QRectF &movingRect,
                                                double epsilon)
{
    const QRectF overlap = fixedRect.intersected(movingRect);
    if (overlap.isEmpty() || overlap.width() <= 0.0 || overlap.height() <= 0.0)
        return {};

    const double overlapX = overlap.width();
    const double overlapY = overlap.height();
    const double dx = movingRect.center().x() - fixedRect.center().x();
    const double dy = movingRect.center().y() - fixedRect.center().y();

    if (overlapX < overlapY) {
        const double direction = (dx >= 0.0) ? 1.0 : -1.0;
        return QPointF(direction * (overlapX + epsilon), 0.0);
    }

    const double direction = (dy >= 0.0) ? 1.0 : -1.0;
    return QPointF(0.0, direction * (overlapY + epsilon));
}
