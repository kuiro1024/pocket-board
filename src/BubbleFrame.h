#pragma once

#include "AppLanguage.h"
#include <QRectF>
#include <QSet>
#include <QString>

class CanvasItem;

// ============================================================
//  BubbleFrame.h
//
//  複数のバブル（CanvasItem）を視覚的にまとめる「フレーム」。
//  Blenderのノードグループのように、フレーム矩形の中に今いる
//  バブルたちを緩やかなグループとして扱う。
//
//  操作感は通常のバブルと同じにする：本体（rect）のどこをドラッグ
//  してもフレームが動き、右下のハンドルでリサイズできる。ただし
//  バブルの「掴み上げ（ぽよん）」演出だけは行わない。
//
//  【重要な設計方針】
//  所属メンバーは固定リストとして持たない（動的判定）。
//  「今このフレームの矩形内にいるバブルがメンバー」というルールに
//  基づき、必要な瞬間（ドラッグ開始時など）に
//  CanvasWidget::frameMembers() が都度計算する。
//  そのため、バブル削除時にフレーム側の後始末は一切不要。
//
//  CanvasItem は継承しない（＝m_items には入れない）。
//  バブル同士の重なり回避システム（CanvasWidget::computeOverlapPushOffsets）
//  はフレームを一切考慮しない。フレームは「今のメンバー」と意図的に
//  重なるためのコンテナなので、メンバーとの衝突回避の対象外にする
//  必要がある。
//
//  一方で、フレーム同士や「フレームに入っていない」バブルに対しては
//  重なり回避を行う（CanvasWidget::computeFrameOverlapPush /
//  startFrameOverlapPushAnimation）。フレームをドラッグ/リサイズ/新規
//  作成した結果、他のフレームや無関係なバブルと重なった場合はそちら側を
//  押し出す。逆に、単体バブルを掴んでフレームの中へ入れる操作では
//  フレーム側は一切動かない（メンバー化は意図的な操作なので）。
// ============================================================

class BubbleFrame
{
public:
    // フレームは俯瞰（ズームアウト）した状態で操作されることが多く、
    // 通常バブルと同じサイズだとリサイズハンドルが小さすぎて見えにくい/
    // 掴みにくいため、通常バブル（CanvasItem::HANDLE_SIZE=35.0）の
    // 約3倍のサイズにしている。
    static constexpr double HANDLE_SIZE = 105.0;
    static constexpr double MIN_WIDTH   = 160.0;
    static constexpr double MIN_HEIGHT  = 120.0;
    // 標準メディアバブルの外周半径31.25に、整列時の余白16を加えた値。
    // 整列したバブルとフレームの角が同心に見える固定半径として使う。
    static constexpr double CORNER_RADIUS = 47.25;

    explicit BubbleFrame(const QRectF &r, const QString &t = AppLanguage::defaultFrameTitle())
        : rect(r), title(t) {}

    // 本体（バブルを内包する領域、かつクリック＝ドラッグ対象）。canvas座標。
    // バブルの itemRect() に相当する。
    QRectF rect;
    QString title;
    QSet<CanvasItem*> members;

    // リサイズハンドル（右下）。バブルの handleRect() と同じ考え方。
    QRectF handleRect() const {
        return QRectF(rect.right() - HANDLE_SIZE, rect.bottom() - HANDLE_SIZE,
                      HANDLE_SIZE, HANDLE_SIZE);
    }
};
