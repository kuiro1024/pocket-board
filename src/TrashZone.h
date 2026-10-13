#pragma once
#include <QWidget>
#include <QVariantAnimation>
#include <QGraphicsOpacityEffect>

// 右上に出現するゴミ箱ゾーン
class TrashZone : public QWidget
{
    Q_OBJECT

public:
    explicit TrashZone(QWidget *parent = nullptr);

    // ドラッグ中にアイテムのscreen座標を渡す → 自動でホバー判定
    bool isHovering(const QPointF &screenPos) const;

    void showTrash();   // バブルをつかんだ時に呼ぶ
    void hideTrash();   // バブルを離した時に呼ぶ

    // ドラッグ中、毎フレーム呼ぶ。ゴミ箱までの距離に応じて透明度を
    // なめらかに追従させる（近づくほど濃く／離れるほど薄く、
    // ただし完全には消えない「うっすら」表示を保つ）。
    // まだ非表示なら自動で show() する。
    void updateProximity(const QPointF &posInParent);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QGraphicsOpacityEffect *m_opacity   = nullptr;
    QVariantAnimation      *m_showAnim  = nullptr;
    QVariantAnimation      *m_scaleAnim = nullptr;

    bool  m_hovering = false;
    float m_scale    = 0.8f;
};
