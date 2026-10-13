#include "TrashZone.h"
#include <QPainter>
#include <QPainterPath>
#include <QEasingCurve>
#include <QLineF>

namespace {
// ドラッグ中、ゴミ箱から十分離れているときに保つ「うっすら」表示の透明度。
constexpr double kDragBaseOpacity = 0.16;
// ゴミ箱に十分近づいた（当たり判定内）ときの最大透明度。
constexpr double kDragMaxOpacity  = 1.0;
// この距離（px）以上離れると透明度が kDragBaseOpacity に収束する。
constexpr double kDragFalloffPx   = 260.0;
// 距離→透明度の追従速度（1フレームあたりの補間係数）。
constexpr double kDragLerpFactor  = 0.35;
}

TrashZone::TrashZone(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(72, 72);
    setAttribute(Qt::WA_TransparentForMouseEvents);

    m_opacity = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(m_opacity);
    m_opacity->setOpacity(0.0);

    // フェードアニメーション
    m_showAnim = new QVariantAnimation(this);
    m_showAnim->setDuration(250);
    m_showAnim->setEasingCurve(QEasingCurve::OutQuad);
    connect(m_showAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        m_opacity->setOpacity(v.toDouble());
    });

    // ホバー時のスケールアニメーション
    m_scaleAnim = new QVariantAnimation(this);
    m_scaleAnim->setDuration(180);
    m_scaleAnim->setEasingCurve(QEasingCurve::OutBack);
    connect(m_scaleAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        m_scale = v.toFloat();
        update();
    });
}

bool TrashZone::isHovering(const QPointF &screenPos) const
{
    QRectF zone = QRectF(mapToParent(QPoint(0,0)), size());
    zone.adjust(-20, -20, 20, 20);
    bool nowHovering = zone.contains(screenPos);

    // ホバー状態が変わったらスケールアニメーション
    if (nowHovering != m_hovering) {
        const_cast<TrashZone*>(this)->m_hovering = nowHovering;
        const_cast<TrashZone*>(this)->m_scaleAnim->stop();
        const_cast<TrashZone*>(this)->m_scaleAnim->setStartValue(m_scale);
        const_cast<TrashZone*>(this)->m_scaleAnim->setEndValue(nowHovering ? 1.2f : 1.0f);
        const_cast<TrashZone*>(this)->m_scaleAnim->start();
    }

    return nowHovering;
}

void TrashZone::showTrash()
{
    raise();
    m_showAnim->stop();
    m_showAnim->setStartValue(m_opacity->opacity());
    m_showAnim->setEndValue(1.0);
    m_showAnim->start();
}

void TrashZone::hideTrash()
{
    m_showAnim->stop();
    m_showAnim->setStartValue(m_opacity->opacity());
    m_showAnim->setEndValue(0.0);
    m_showAnim->start();
    m_hovering = false;
}

void TrashZone::updateProximity(const QPointF &posInParent)
{
    if (!isVisible()) {
        raise();
        show();
        m_opacity->setOpacity(0.0); // 0から「うっすら」までなめらかにフェードインさせる
    }
    // 離散フェード（showTrash/hideTrash）と競合しないよう、距離追従中は止めておく。
    m_showAnim->stop();

    const QPointF center = mapToParent(rect().center());
    const double dist = QLineF(center, posInParent).length();
    const double t = qBound(0.0, 1.0 - dist / kDragFalloffPx, 1.0);
    const double eased = t * t * (3.0 - 2.0 * t); // smoothstep
    const double target = kDragBaseOpacity + (kDragMaxOpacity - kDragBaseOpacity) * eased;

    const double current = m_opacity->opacity();
    m_opacity->setOpacity(current + (target - current) * kDragLerpFactor);

    isHovering(posInParent); // 既存のホバー時スケール演出を流用
}

void TrashZone::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    float cx = width()  / 2.0f;
    float cy = height() / 2.0f;

    painter.save();
    painter.translate(cx, cy);
    painter.scale(m_scale, m_scale);
    painter.translate(-cx, -cy);

    // 背景の丸
    QColor bg = m_hovering ? QColor(220, 60, 60, 200) : QColor(60, 60, 60, 180);
    painter.setBrush(bg);
    painter.setPen(QPen(QColor(255,255,255,80), 1.5));
    painter.drawEllipse(QPointF(cx, cy), 28, 28);

    // ゴミ箱アイコン
    painter.setPen(QPen(QColor(255,255,255,230), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    float bx = cx, by = cy + 2;
    float bw = 16, bh = 14;

    // 胴体（台形）
    QPainterPath body;
    body.moveTo(bx - bw/2 + 2, by - bh/2);
    body.lineTo(bx - bw/2,     by + bh/2);
    body.lineTo(bx + bw/2,     by + bh/2);
    body.lineTo(bx + bw/2 - 2, by - bh/2);
    body.closeSubpath();
    painter.drawPath(body);

    // フタ
    painter.drawLine(QPointF(bx - bw/2 - 2, by - bh/2),
                     QPointF(bx + bw/2 + 2, by - bh/2));
    // 取っ手
    painter.drawLine(QPointF(bx - 4, by - bh/2 - 2),
                     QPointF(bx + 4, by - bh/2 - 2));

    // 内側の線（蓋の下）
    painter.setOpacity(0.5);
    painter.drawLine(QPointF(bx - 5, by - bh/2 + 4), QPointF(bx - 5, by + bh/2 - 2));
    painter.drawLine(QPointF(bx,     by - bh/2 + 4), QPointF(bx,     by + bh/2 - 2));
    painter.drawLine(QPointF(bx + 5, by - bh/2 + 4), QPointF(bx + 5, by + bh/2 - 2));

    painter.restore();
}
