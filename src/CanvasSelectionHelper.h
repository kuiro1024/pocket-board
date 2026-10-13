#pragma once

#include <QHash>
#include <QList>
#include <QRectF>
#include <QSet>

class CanvasItem;

class CanvasSelectionHelper
{
public:
    static QList<CanvasItem*> itemsInCanvasRect(const QList<CanvasItem*> &items,
                                                const QRectF &canvasRect);
    static QSet<CanvasItem*> itemSet(const QList<CanvasItem*> &items);
    static QList<CanvasItem*> takeItems(QSet<CanvasItem*> *items);
    static QHash<CanvasItem*, QPointF> itemPositions(const QList<CanvasItem*> &items);
    static QHash<CanvasItem*, QPointF> itemPositions(const QSet<CanvasItem*> &items);
    static void applyPositionOffset(const QHash<CanvasItem*, QPointF> &positions,
                                    const QPointF &offset);
    // 2026-09-20: 範囲選択の整列（ホイールダブル／トリプルクリック）用。
    // 見た目の並び（行ごと・左→右）の順番。
    static QList<CanvasItem*> readingOrder(const QSet<CanvasItem*> &items);
    // 大きさ順（高さの近いもの同士が同じ行に来るよう、高さ→幅の大きい順。
    // 同じ大きさの中では見た目の並び順を保つ）。
    static QList<CanvasItem*> sizeOrder(const QSet<CanvasItem*> &items);
    // 渡された順番のまま、行に詰めて並べる。行の幅は候補の中から
    // 「すき間の少なさ」と「全体の縦横比が targetAspect に近いこと」で選ぶ。
    // 各行の中では上下中央に揃える。topLeft は並べた全体の左上。
    static QHash<CanvasItem*, QPointF> packedPositions(const QList<CanvasItem*> &ordered,
                                                       const QPointF &topLeft,
                                                       double spacing,
                                                       double targetAspect);
    static QSet<CanvasItem*> positionedItems(const QHash<CanvasItem*, QPointF> &positions);
    static QSet<CanvasItem*> claimExclusiveItems(const QSet<CanvasItem*> &items,
                                                 QSet<CanvasItem*> *claimedItems);
    static QPointF separationOffset(const QRectF &fixedRect,
                                    const QRectF &movingRect,
                                    double epsilon = 0.5);
};
