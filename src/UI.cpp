#include "UI.h"
#include "DynamicIsland.h" // AnimatedButtonを再利用（+ボタンのみ。DynamicIsland本体のロジックは未使用）

#include <QFrame>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSvgRenderer>
#include <QHash>
#include <QEnterEvent>

namespace {

// -□×側のアイコンSVGを白でティントして描画するヘルパー。
// DynamicIsland.cpp内にも同種のtintedSvgPixmap()があるが、あちらは
// anonymous namespace内で非公開のため、ここでも同じ手法で個別に用意する
// （二重実装ではあるが、DynamicIsland側のコードには一切触れない方針のため）。
QPixmap tintedSvgPixmap(const QString &resourcePath, const QSize &size, const QColor &color)
{
    static QHash<QString, QSvgRenderer*> s_rendererCache;
    QSvgRenderer *renderer = s_rendererCache.value(resourcePath, nullptr);
    if (!renderer) {
        renderer = new QSvgRenderer(resourcePath);
        s_rendererCache.insert(resourcePath, renderer);
    }
    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    renderer->render(&p);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pixmap.rect(), color);
    p.end();
    return pixmap;
}

// 角のうち左右どちらを丸めるかを指定できる角丸矩形パス。
// セグメント調ボタンの左端/右端だけをピル全体の丸みに合わせて丸め、
// 中央のボタンは四角のまま・隣接ボタンとの境目には区切り線だけを見せる
// （DynamicIsland::paintEvent()と同じarcTo手法。角2つだけ丸める点が異なる）。
QPainterPath sideRoundedRect(const QRectF &r, qreal radius, bool roundLeft, bool roundRight)
{
    const qreal d = radius * 2.0;
    QPainterPath path;
    path.moveTo(r.left() + (roundLeft ? radius : 0), r.top());
    path.lineTo(r.right() - (roundRight ? radius : 0), r.top());
    if (roundRight) path.arcTo(r.right() - d, r.top(), d, d, 90, -90);
    path.lineTo(r.right(), r.bottom() - (roundRight ? radius : 0));
    if (roundRight) path.arcTo(r.right() - d, r.bottom() - d, d, d, 0, -90);
    path.lineTo(r.left() + (roundLeft ? radius : 0), r.bottom());
    if (roundLeft) path.arcTo(r.left(), r.bottom() - d, d, d, 270, -90);
    path.lineTo(r.left(), r.top() + (roundLeft ? radius : 0));
    if (roundLeft) path.arcTo(r.left(), r.top(), d, d, 180, -90);
    path.closeSubpath();
    return path;
}

// -□×側の1ボタン。2026-08-26: 「Apple Liquid Glass系を意識した無彩色・
// リッチな見た目」への刷新の第一段として、色付き丸背景(AnimatedButton)を
// やめ、アイコンのみを白でティントして描く。ボタン同士の境目には
// RightPillが縦の区切り線を描く（このクラス自体は線を引かない）。
// ホバー時のみ、自分の担当範囲だけをうっすら明るくする
// （左端/右端ボタンはピルの丸みに合わせて角を丸める。中央ボタンは
// 四角のまま＝区切り線の内側にぴったり収まる）。
class MonoIconButton : public QPushButton
{
public:
    enum class Edge { Middle, Left, Right };

    MonoIconButton(const QString &svgResourcePath, Edge edge, qreal pillRadius, QWidget *parent)
        : QPushButton(parent), m_svgResourcePath(svgResourcePath), m_edge(edge), m_pillRadius(pillRadius)
    {
        setFixedSize(36, 36);
        setFlat(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus); // Space/Enterでの誤クリック防止（AnimatedButtonに合わせた）
    }

protected:
    void enterEvent(QEnterEvent *) override { m_hovered = true; update(); }
    void leaveEvent(QEvent *) override { m_hovered = false; update(); }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        if (m_hovered) {
            const QPainterPath hoverPath = sideRoundedRect(
                QRectF(rect()), m_pillRadius,
                m_edge == Edge::Left, m_edge == Edge::Right);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 28)); // うっすら明るくするだけ（無彩色）
            painter.drawPath(hoverPath);
        }

        constexpr int kIconPx = 18;
        const QSize iconSize(kIconPx, kIconPx);
        const QPixmap pm = tintedSvgPixmap(m_svgResourcePath, iconSize, QColor(235, 235, 235));
        const qreal cx = width() / 2.0, cy = height() / 2.0;
        painter.drawPixmap(QRectF(cx - kIconPx / 2.0, cy - kIconPx / 2.0, kIconPx, kIconPx),
                            pm, pm.rect());
    }

private:
    QString m_svgResourcePath;
    Edge    m_edge;
    qreal   m_pillRadius;
    bool    m_hovered = false;
};

// -□×をまとめる背景ピル。2026-08-26: 無彩色・リッチ系トーンへ変更
// （見た目の細部は引き続き調整中。ボタンの丸背景を廃止し、代わりに
// このピル側でボタンの境目へ縦の区切り線を描く＝セグメントコントロール
// 的な見た目にする）。
class RightPill : public QFrame
{
public:
    explicit RightPill(QWidget *parent) : QFrame(parent) {}

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal r = height() / 2.0;
        QPainterPath path;
        path.addRoundedRect(rect(), r, r);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(28, 28, 30, 235)); // 暫定色（無彩色）
        painter.drawPath(path);

        // ボタン境目の区切り線（上下に少し余白を残す。macOSのセグメント
        // コントロール等でよく見る「短い縦線」のイメージ）。
        painter.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
        const qreal inset = 8.0;
        for (qreal x : {36.0, 72.0})
            painter.drawLine(QPointF(x, inset), QPointF(x, height() - inset));
    }
};

} // namespace

UI::UI(QWidget *host)
    : QObject(host), m_host(host)
{
    // ---- 左上: +ボタン単体 ----
    // 常時表示のため、DynamicIsland側のようなトリガー式の出現アニメは
    // 使わず、最初からvisualScale=1の状態にしてしまう。
    // （2026-08-26: 右側とは違い、こちらはまだポップな色付きのまま。
    //   無彩色化するかどうかは+ボタンのデザインを詰めるときに決める）。
    btnSettings = new AnimatedButton(QStringLiteral("#4A90E2"), QStringLiteral("+"), host);
    btnSettings->setVisualScale(1.0f);
    btnSettings->setEnabled(true);
    connect(btnSettings, &QPushButton::clicked, this, &UI::settingsRequested);

    // ---- 右上: -□× のピル(セグメント調・無彩色) ----
    m_rightPill = new RightPill(host);
    const qreal pillRadius = 36 / 2.0;
    m_rightPill->setFixedSize(36 * 3, 36); // 区切り線のみでボタン間の隙間は無し

    auto *pillMin   = new MonoIconButton(QStringLiteral(":/icons/min.svg"),        MonoIconButton::Edge::Left,   pillRadius, m_rightPill);
    auto *pillMax   = new MonoIconButton(QStringLiteral(":/icons/fullscreen.svg"), MonoIconButton::Edge::Middle, pillRadius, m_rightPill);
    auto *pillClose = new MonoIconButton(QStringLiteral(":/icons/exit.svg"),       MonoIconButton::Edge::Right,  pillRadius, m_rightPill);
    // ピル内で左から min/max/close の順に並べる（画像イメージの-□×の
    // 見た目順を踏襲。DynamicIsland側の右詰め並び順close/max/minとは
    // 左右が逆になる点に注意）。
    pillMin->move(0, 0);
    pillMax->move(36, 0);
    pillClose->move(36 * 2, 0);
    btnMin   = pillMin;
    btnMax   = pillMax;
    btnClose = pillClose;

    reposition();
}

void UI::reposition()
{
    if (!m_host)
        return;
    btnSettings->move(kMargin, kMargin);
    m_rightPill->move(m_host->width() - m_rightPill->width() - kMargin, kMargin);
}

void UI::setUiVisible(bool visible)
{
    btnSettings->setVisible(visible);
    m_rightPill->setVisible(visible);
}
