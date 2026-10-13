#include "IslandControls.h"

#include <QApplication>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QTimer>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace IslandControls {

using namespace Metrics;

namespace {

double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }
double outCubic(double t) { t = clamp01(t); const double u = 1.0 - t; return 1.0 - u * u * u; }

QColor mixColor(const QColor &a, const QColor &b, double t)
{
    t = clamp01(t);
    return QColor::fromRgbF(float(lerp(a.redF(), b.redF(), t)), float(lerp(a.greenF(), b.greenF(), t)),
                            float(lerp(a.blueF(), b.blueF(), t)), float(lerp(a.alphaF(), b.alphaF(), t)));
}
QColor withAlphaF(QColor c, double a) { c.setAlphaF(float(clamp01(a))); return c; }

// ---- 色（2026-09-16: Windows のアクセントカラーから取る）----
// DynamicIsland が getWindowsAccentColor() の値（色が変わるときのアニメ中も）を
// setAccentColor() で渡してくる。島の本体と同じ色から、ボタン類の色を導く。
QColor s_accent(0x4A, 0x90, 0xE2);

// 浮いた面の色。AnimatedButton の「きらめかせた発色」と同じ式（明度×1.20+10、彩度×1.04）。
QColor tintColor()
{
    const QColor hsv = s_accent.toHsv();
    return QColor::fromHsv(qMax(0, hsv.hue()),
                           qBound(0, static_cast<int>(hsv.saturation() * 1.04), 255),
                           qBound(0, static_cast<int>(hsv.value() * 1.20 + 10), 255));
}
// 暗部（乗算）の色。AnimatedButton と同じ（明度×0.52、彩度×1.10）。
QColor shadeColor()
{
    const QColor hsv = s_accent.toHsv();
    return QColor::fromHsv(qMax(0, hsv.hue()),
                           qMin(255, static_cast<int>(hsv.saturation() * 1.10)),
                           static_cast<int>(hsv.value() * 0.52));
}

// ---- 透明度 ----
// ガラスとして下地が透けるよう、＋ボタン(192)・着地したタブ(58)より少し薄くしてある。
constexpr int kRaisedTintAlpha = 150;  // 浮いた面のボタン色
constexpr int kMenuTintAlpha   = 215;  // メニュー（別窓で下地のぼかしが無いので濃いめ）
constexpr int kSunkFillAlpha   = 46;   // 沈んだ面の暗さ
constexpr int kKnobAlpha       = 240;  // 白いつまみ（下の溝の塗りが透けすぎない程度）

// 生きている部品（色が変わったら描き直す）
QSet<QWidget *> &liveControls()
{
    static QSet<QWidget *> set;
    return set;
}

QPainterPath capsule(const QRectF &r)
{
    QPainterPath p;
    const double rad = std::min(r.width(), r.height()) / 2.0;
    p.addRoundedRect(r, rad, rad);
    return p;
}

// ホバーは加算で白を足す（浮いた面の光が強まる見え方）
void fillHover(QPainter &p, const QPainterPath &path, double hover, double k = 1.0)
{
    if (hover <= 0.001)
        return;
    p.save();
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    p.fillPath(path, withAlphaF(Qt::white, kHoverLight * hover * k));
    p.restore();
}
// 押し込みは黒を重ねて暗くする
void fillPressDark(QPainter &p, const QPainterPath &path, double amount)
{
    if (amount <= 0.001)
        return;
    p.fillPath(path, QColor(0, 0, 0, qRound(255 * clamp01(amount))));
}

// ============================================================
//  面の描画（島・＋ボタン・着地したタブと同じガラスの層構成）
//
//  層は IslandTabGoo::paintSurface() の「浮き出た雫」「沈み込んだ面」を
//  そのまま移したもの（色だけアクセントカラー由来に差し替え）。
//  細い溝（スライダー）では線の太さ・ずらし量を高さに合わせて縮める（k）。
//
//  【フレーム更新への配慮】ストロークを十数本重ねるので、毎フレーム描くと重い。
//  形・大きさ・DPR・色が同じなら絵は同じなので、ピクセルマップにして全部品で
//  共有する（surfacePixmap）。アニメで変わるのは「縮小（押し込み）」
//  「ホバーの加算」「押し込みの暗さ」だけで、どれも貼った絵の上で安く済む。
//  色が変わると（setAccentColor）キャッシュを捨てる。
// ============================================================
enum class Surface { RaisedCapsule, RaisedCircle, SunkCapsule, Menu };

QPainterPath surfacePath(Surface kind, const QRectF &r)
{
    QPainterPath path;
    switch (kind) {
    case Surface::RaisedCircle: path.addEllipse(r); break;
    case Surface::Menu:         path.addRoundedRect(r, kMenuRadius, kMenuRadius); break;
    default:                    return capsule(r);
    }
    return path;
}

void paintRaised(QPainter &p, const QPainterPath &path, const QRectF &bb, bool menu)
{
    const QColor tint = tintColor();
    const QColor shade = shadeColor();
    const auto withA = [](QColor c, int a) { c.setAlpha(qBound(0, a, 255)); return c; };

    p.setPen(Qt::NoPen);
    // 落ち影
    {
        const QPainterPath shadowPath = path.translated(0.0, 1.9);
        p.fillPath(shadowPath, QColor(0, 0, 0, 34));
        p.strokePath(shadowPath, QPen(QColor(0, 0, 0, 16), 3.0));
    }
    // 面：フロスト＋ボタン色
    p.fillPath(path, QColor(255, 255, 255, 10));
    if (menu)
        p.fillPath(path, withA(mixColor(tint, QColor(10, 14, 24), kMenuDark), kMenuTintAlpha));
    else
        p.fillPath(path, withA(tint, kRaisedTintAlpha));

    p.save();
    p.setClipPath(path);
    // 縁の光（加算）
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    p.strokePath(path, QPen(QColor(255, 255, 255, 4), 8.0));
    p.strokePath(path, QPen(QColor(255, 255, 255, 9), 4.0));
    p.strokePath(path, QPen(QColor(255, 255, 255, 14), 1.8));
    p.strokePath(path.translated(0.0, 1.4), QPen(QColor(255, 255, 255, 22), 2.6));
    p.strokePath(path.translated(0.0, -1.3), QPen(QColor(255, 255, 255, 14), 2.2));
    // 左右の翳り（乗算）
    p.setCompositionMode(QPainter::CompositionMode_Multiply);
    {
        const double s = std::min(0.45, (bb.height() * 0.5) / std::max(1.0, bb.width()));
        QLinearGradient side(QPointF(bb.left(), 0.0), QPointF(bb.right(), 0.0));
        side.setColorAt(0.0, withA(shade, 70));
        side.setColorAt(s, withA(shade, 0));
        side.setColorAt(1.0 - s, withA(shade, 0));
        side.setColorAt(1.0, withA(shade, 70));
        p.strokePath(path, QPen(QBrush(side), 7.0));
    }
    p.restore();

    // 輪郭線（乗算）と縁を走る細い光（加算）
    p.save();
    p.setCompositionMode(QPainter::CompositionMode_Multiply);
    QLinearGradient contour(QPointF(0.0, bb.top()), QPointF(0.0, bb.bottom()));
    contour.setColorAt(0.0, withA(shade, 120));
    contour.setColorAt(0.5, withA(shade, 230));
    contour.setColorAt(1.0, withA(shade, 190));
    p.strokePath(path, QPen(QBrush(contour), 0.9));
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    QLinearGradient edge(QPointF(0.0, bb.top()), QPointF(0.0, bb.bottom()));
    edge.setColorAt(0.00, QColor(255, 255, 255, 30));
    edge.setColorAt(0.32, QColor(255, 255, 255, 11));
    edge.setColorAt(0.68, QColor(255, 255, 255, 10));
    edge.setColorAt(1.00, QColor(255, 255, 255, 24));
    p.strokePath(path, QPen(QBrush(edge), 0.9));
    p.restore();
}

void paintSunk(QPainter &p, const QPainterPath &path, const QRectF &bb)
{
    // 着地したタブ基準の線を、部品の高さに合わせて縮める
    const double k = std::clamp(bb.height() / 22.0, 0.35, 1.0);
    p.setPen(Qt::NoPen);
    p.fillPath(path, QColor(0, 0, 0, kSunkFillAlpha));

    // 上の内側に影、下の内側に弱い光
    p.save();
    p.setClipPath(path);
    // 2026-09-16: へこみすぎて見えたので影を細く（2.3/4.6 → 1.3/2.6、1.0/1.8 → 0.6/1.2）
    p.strokePath(path.translated(0.0, 1.3 * k), QPen(QColor(0, 0, 0, 56), 2.6 * k));
    p.strokePath(path.translated(0.0, 0.6 * k), QPen(QColor(0, 0, 0, 30), 1.2 * k));
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    p.strokePath(path.translated(0.0, -1.1 * k), QPen(QColor(255, 255, 255, 16), 1.5 * k));
    p.restore();

    // 穴の下側の外に細い光（リップ）
    p.save();
    QPainterPath outside;
    outside.setFillRule(Qt::OddEvenFill);
    outside.addRect(bb.adjusted(-kSurfaceMargin, -kSurfaceMargin, kSurfaceMargin, kSurfaceMargin));
    outside.addPath(path);
    p.setClipPath(outside);
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    p.strokePath(path.translated(0.0, 0.9), QPen(QColor(255, 255, 255, 26), 1.2));
    p.restore();

    // 輪郭（乗算）と縁の光（加算）。沈んだ面は色を持たない暗色
    p.save();
    const auto withA = [](QColor c, int a) { c.setAlpha(a); return c; };
    const QColor base(20, 20, 24);
    p.setCompositionMode(QPainter::CompositionMode_Multiply);
    QLinearGradient contour(QPointF(0.0, bb.top()), QPointF(0.0, bb.bottom()));
    contour.setColorAt(0.0, withA(base, 150));
    contour.setColorAt(0.5, withA(base, 90));
    contour.setColorAt(1.0, withA(base, 40));
    p.strokePath(path, QPen(QBrush(contour), 0.9));
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    QLinearGradient edge(QPointF(0.0, bb.top()), QPointF(0.0, bb.bottom()));
    edge.setColorAt(0.00, QColor(255, 255, 255, 6));
    edge.setColorAt(0.32, QColor(255, 255, 255, 4));
    edge.setColorAt(0.68, QColor(255, 255, 255, 8));
    edge.setColorAt(1.00, QColor(255, 255, 255, 22));
    p.strokePath(path, QPen(QBrush(edge), 0.9));
    p.restore();
}

QHash<QString, QPixmap> &surfaceCache()
{
    static QHash<QString, QPixmap> cache;
    return cache;
}

QPixmap surfacePixmap(Surface kind, const QSizeF &size, qreal dpr)
{
    // 0.25px単位で丸めて共有する（スライダーの溝など小数の大きさがあるため）
    const auto q = [](double v) { return qRound(v * 4.0); };
    const QString key = QStringLiteral("%1_%2_%3_%4")
                            .arg(int(kind)).arg(q(size.width())).arg(q(size.height())).arg(q(dpr));
    QHash<QString, QPixmap> &cache = surfaceCache();
    const auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return it.value();
    if (cache.size() > 96)
        cache.clear();

    const QSizeF logical(size.width() + kSurfaceMargin * 2.0, size.height() + kSurfaceMargin * 2.0);
    QPixmap pm(qCeil(logical.width() * dpr), qCeil(logical.height() * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r(kSurfaceMargin, kSurfaceMargin, size.width(), size.height());
        const QPainterPath path = surfacePath(kind, r);
        if (kind == Surface::SunkCapsule)
            paintSunk(p, path, r);
        else
            paintRaised(p, path, r, kind == Surface::Menu);
    }
    cache.insert(key, pm);
    return pm;
}

// 面を r に貼る。scale を渡すと r の中心を基準に縮める（押し込み）。
void drawSurface(QPainter &p, const QWidget *w, Surface kind, const QRectF &r, double scale = 1.0)
{
    const QPixmap pm = surfacePixmap(kind, r.size(), w->devicePixelRatioF());
    p.save();
    if (std::abs(scale - 1.0) > 0.0005) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QPointF c = r.center();
        p.translate(c);
        p.scale(scale, scale);
        p.translate(-c);
    }
    p.drawPixmap(QPointF(r.left() - kSurfaceMargin, r.top() - kSurfaceMargin), pm);
    p.restore();
}

// 白いガラスのつまみ。大きさが連続的に変わる（伸び・拡大）のでキャッシュせず
// 直接描く。描く物は3つだけなので軽い。
void drawKnob(QPainter &p, const QRectF &r)
{
    const QPainterPath path = capsule(r);
    // 落ち影（AnimatedButton と同じ放射グラデーション）
    {
        const double base = r.height() / 2.0;
        const QPointF sc(r.center().x(), r.center().y() + base * 0.16);
        const double rx = r.width() / 2.0 + base * 0.28, ry = base * 1.28;
        QRadialGradient shadow(QPointF(0, 0), 1.0);
        shadow.setColorAt(0.00, QColor(0, 0, 0, 66));
        shadow.setColorAt(0.60, QColor(0, 0, 0, 28));
        shadow.setColorAt(1.00, QColor(0, 0, 0, 0));
        p.save();
        p.translate(sc);
        p.scale(rx, ry);
        p.setPen(Qt::NoPen);
        p.setBrush(shadow);
        p.drawEllipse(QPointF(0, 0), 1.0, 1.0);
        p.restore();
    }
    p.fillPath(path, QColor(255, 255, 255, kKnobAlpha));
    // 輪郭（乗算・下ほど濃い）
    p.save();
    p.setCompositionMode(QPainter::CompositionMode_Multiply);
    QLinearGradient contour(QPointF(0.0, r.top()), QPointF(0.0, r.bottom()));
    contour.setColorAt(0.0, QColor(150, 158, 170, 90));
    contour.setColorAt(1.0, QColor(110, 118, 132, 200));
    p.strokePath(path, QPen(QBrush(contour), 0.9));
    p.restore();
}

// 設定欄のラベルと同じ影付きの文字（ShadowLabel と同じ作り）
void drawShadowText(QPainter &p, const QRectF &box, int flags, const QString &text, double alpha = 0.95)
{
    p.setPen(QColor(0, 0, 0, qRound(200 * clamp01(alpha))));
    p.drawText(box.translated(1.0, 1.0), flags, text);
    p.drawText(box.translated(0.0, 1.0), flags, text);
    p.setPen(withAlphaF(Qt::white, alpha));
    p.drawText(box, flags, text);
}

QFont pixelFont(const QFont &base, int px)
{
    QFont f = base;
    f.setPixelSize(px);
    return f;
}

void beginPaint(QPainter &p, const QWidget *w)
{
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    if (!w->isEnabled())
        p.setOpacity(kDisabledOpacity);
}

} // namespace

// ============================================================
//  Spring / SpringTicker
// ============================================================
bool Spring::step(double target, double hz, double zeta, double dt)
{
    constexpr double kPi = 3.14159265358979323846;
    const double w = 2.0 * kPi * hz;
    double t = dt;
    while (t > 0.0) {
        const double h = std::min(t, 1.0 / 240.0);
        v += (-w * w * (x - target) - 2.0 * zeta * w * v) * h;
        x += v * h;
        t -= h;
    }
    if (std::abs(x - target) < 0.0005 && std::abs(v) < 0.005) {
        x = target;
        v = 0.0;
        return false;
    }
    return true;
}

int SpringTicker::intervalFor(Rate r)
{
    switch (r) {
    case Rate::Full:  return 16;   // 60Hz
    case Rate::Half:  return 33;   // 30Hz
    case Rate::Hover: return 66;   // 15Hz
    case Rate::Idle:  break;
    }
    return 0;
}

SpringTicker::SpringTicker(QWidget *owner, std::function<Rate(double)> step)
    : m_owner(owner), m_timer(new QTimer(owner)), m_step(std::move(step))
{
    liveControls().insert(owner);
    m_timer->setTimerType(Qt::PreciseTimer);
    QObject::connect(m_timer, &QTimer::timeout, owner, [this] {
        // 15Hz で回っている間もバネは実時間で進める（dt は間隔に依存しない）。
        // 上限は 15Hz の1周期より少し長め。
        const double dt = std::min(0.1, m_clock.restart() / 1000.0);
        const Rate rate = m_step(dt);
        m_owner->update();
        if (rate == Rate::Idle) {
            m_timer->stop();
            return;
        }
        const int interval = intervalFor(rate);
        if (m_timer->interval() != interval)
            m_timer->setInterval(interval);
    });
}

SpringTicker::~SpringTicker()
{
    liveControls().remove(m_owner);
}

void setAccentColor(const QColor &color)
{
    QColor opaque = color;
    opaque.setAlpha(255);
    if (opaque == s_accent)
        return;
    s_accent = opaque;
    surfaceCache().clear();
    for (QWidget *w : std::as_const(liveControls()))
        w->update();
}

void SpringTicker::kick(Rate want)
{
    if (want == Rate::Idle)
        want = Rate::Hover;
    const int interval = intervalFor(want);
    if (!m_timer->isActive()) {
        m_clock.restart();
        m_owner->update();          // 押した瞬間の見た目はすぐ出す
        m_timer->start(interval);
    } else if (m_timer->interval() > interval) {
        m_timer->start(interval);   // 遅い間隔で待っていたら、次の tick を早める
    }
}

// ============================================================
//  Toggle
// ============================================================
Toggle::Toggle(QWidget *parent)
    : QAbstractButton(parent)
    , m_ticker(this, [this](double dt) { return stepAnim(dt); })
{
    setCheckable(true);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_TranslucentBackground, true);
    connect(this, &QAbstractButton::toggled, this, [this] { m_ticker.kick(); });
    connect(this, &QAbstractButton::pressed, this, [this] { m_ticker.kick(); });
    connect(this, &QAbstractButton::released, this, [this] { m_ticker.kick(); });
}

void Toggle::setCheckedImmediate(bool on)
{
    const QSignalBlocker block(this);
    setChecked(on);
    m_on.snap(on ? 1.0 : 0.0);
    update();
}

void Toggle::setCheckedSilently(bool on)
{
    if (isChecked() == on)
        return;
    {
        const QSignalBlocker block(this);
        setChecked(on);
    }
    m_ticker.kick();
}

Rate Toggle::stepAnim(double dt)
{
    Rate r = rateIf(m_on.step(isChecked() ? 1.0 : 0.0, kToggleHz, kToggleZeta, dt), Rate::Full);
    r |= rateIf(m_press.step(isDown() ? 1.0 : 0.0, kToggleHz * 1.4, 0.9, dt), Rate::Full);
    r |= rateIf(m_hover.step(m_hovered ? 1.0 : 0.0, kHoverHz, 1.0, dt), Rate::Hover);
    return r;
}

void Toggle::enterEvent(QEnterEvent *e) { m_hovered = true; m_ticker.kick(Rate::Hover); QAbstractButton::enterEvent(e); }
void Toggle::leaveEvent(QEvent *e) { m_hovered = false; m_ticker.kick(Rate::Hover); QAbstractButton::leaveEvent(e); }

void Toggle::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    beginPaint(p, this);
    const QRectF track = QRectF(rect()).adjusted(kSurfaceMargin, kSurfaceMargin, -kSurfaceMargin, -kSurfaceMargin);
    const QPainterPath path = capsule(track);
    drawSurface(p, this, Surface::SunkCapsule, track);
    const double t = m_on.x;
    if (t > 0.001)
        p.fillPath(path, withAlphaF(Qt::white, kToggleOnAlpha * clamp01(t)));
    fillHover(p, path, m_hover.x);

    const double d = track.height() - kToggleInset * 2.0;
    const double kw = d + kToggleStretch * clamp01(m_press.x);
    const double kx = track.left() + kToggleInset + (track.width() - kToggleInset * 2.0 - kw) * t;
    drawKnob(p, QRectF(kx, track.top() + kToggleInset, kw, d));
}

// ============================================================
//  Button
// ============================================================
Button::Button(const QString &text, QWidget *parent)
    : QAbstractButton(parent)
    , m_ticker(this, [this](double dt) { return stepAnim(dt); })
{
    setText(text);
    setFont(pixelFont(font(), kCtrlFontPx));
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_TranslucentBackground, true);
    connect(this, &QAbstractButton::pressed, this, [this] { m_ticker.kick(); });
    connect(this, &QAbstractButton::released, this, [this] { m_ticker.kick(); });
}

int Button::preferredWidth() const
{
    const double tw = QFontMetricsF(font()).horizontalAdvance(text());
    return qBound(kButtonMinW, static_cast<int>(std::ceil(tw + kButtonPadX * 2.0)), kButtonMaxW);
}

Rate Button::stepAnim(double dt)
{
    Rate r = rateIf(m_press.step(isDown() ? 1.0 : 0.0, kButtonHz, kButtonZeta, dt), Rate::Full);
    r |= rateIf(m_hover.step(m_hovered ? 1.0 : 0.0, kHoverHz, 1.0, dt), Rate::Hover);
    return r;
}

void Button::enterEvent(QEnterEvent *e) { m_hovered = true; m_ticker.kick(Rate::Hover); QAbstractButton::enterEvent(e); }
void Button::leaveEvent(QEvent *e) { m_hovered = false; m_ticker.kick(Rate::Hover); QAbstractButton::leaveEvent(e); }

void Button::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    beginPaint(p, this);
    const double sc = lerp(1.0, kButtonPressScale, m_press.x);
    const QRectF full = QRectF(rect()).adjusted(kSurfaceMargin, kSurfaceMargin, -kSurfaceMargin, -kSurfaceMargin);
    const QPointF c = full.center();
    const QRectF r(c.x() - full.width() * sc / 2.0, c.y() - full.height() * sc / 2.0,
                   full.width() * sc, full.height() * sc);
    const QPainterPath path = capsule(r);
    drawSurface(p, this, Surface::RaisedCapsule, full, sc);
    fillHover(p, path, m_hover.x);
    fillPressDark(p, path, kButtonPressDark * clamp01(m_press.x));

    p.setFont(font());
    const QString t = QFontMetrics(font()).elidedText(text(), Qt::ElideRight,
                                                 qMax(0, static_cast<int>(r.width() - kButtonPadX * 2.0 + 8.0)));
    drawShadowText(p, full, Qt::AlignCenter | Qt::TextSingleLine, t);
}

// ============================================================
//  SpinBox
// ============================================================
SpinBox::SpinBox(int minValue, int maxValue, int step, int value, QWidget *parent)
    : QWidget(parent)
    , m_min(minValue), m_max(maxValue), m_step(qMax(1, step))
    , m_value(qBound(minValue, value, maxValue)), m_prev(m_value)
    , m_ticker(this, [this](double dt) { return stepAnim(dt); })
{
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    m_repeat = new QTimer(this);
    connect(m_repeat, &QTimer::timeout, this, [this] {
        if (m_pressed == None) { stopRepeat(); return; }
        stepBy(m_pressed == Left ? -1 : 1);
        m_repeat->setInterval(kSpinRepeatIntervalMs);
    });
}

SpinBox::Side SpinBox::sideAt(const QPointF &pos) const
{
    if (!rect().contains(pos.toPoint()))
        return None;
    return pos.x() < width() / 2.0 ? Left : Right;
}

void SpinBox::stepBy(int dir)
{
    const int nv = qBound(m_min, m_value + dir * m_step, m_max);
    if (nv == m_value)
        return;
    m_prev = m_value;
    m_value = nv;
    m_slideDir = dir;
    m_slideClock.restart();
    m_ticker.kick();
    if (onValueChanged)
        onValueChanged(m_value);
}

void SpinBox::stopRepeat() { m_repeat->stop(); }

void SpinBox::setValue(int value)
{
    const int nv = qBound(m_min, value, m_max);
    if (nv == m_value || m_pressed != None)
        return;
    m_prev = m_value;
    m_slideDir = nv > m_value ? 1 : -1;
    m_value = nv;
    m_slideClock.restart();
    m_ticker.kick();
}

void SpinBox::mousePressEvent(QMouseEvent *e)
{
    e->accept();
    if (e->button() != Qt::LeftButton)
        return;
    m_pressed = sideAt(e->position());
    if (m_pressed == None)
        return;
    stepBy(m_pressed == Left ? -1 : 1);
    m_repeat->setInterval(kSpinRepeatDelayMs);
    m_repeat->start();
    m_ticker.kick();
}

void SpinBox::mouseReleaseEvent(QMouseEvent *e)
{
    e->accept();
    stopRepeat();
    m_pressed = None;
    m_ticker.kick();
}

void SpinBox::mouseMoveEvent(QMouseEvent *e)
{
    const Side s = sideAt(e->position());
    if (s != m_hoverSide) {
        m_hoverSide = s;
        setCursor(s == None ? Qt::ArrowCursor : Qt::PointingHandCursor);
        // 左右どちらが明るいかの入れ替わりは0/1なので、1回描くだけ
        // （入ってきた／出ていったときのフェードは enter/leave 側で15Hz）
        if (s != None && m_hover.x > 0.999)
            update();
        else
            m_ticker.kick(Rate::Hover);
    }
    QWidget::mouseMoveEvent(e);
}

void SpinBox::enterEvent(QEnterEvent *e) { m_ticker.kick(Rate::Hover); QWidget::enterEvent(e); }
void SpinBox::leaveEvent(QEvent *e) { m_hoverSide = None; m_ticker.kick(Rate::Hover); QWidget::leaveEvent(e); }

Rate SpinBox::stepAnim(double dt)
{
    Rate r = rateIf(m_pressL.step(m_pressed == Left ? 1.0 : 0.0, kSpinHz, kSpinZeta, dt), Rate::Full);
    r |= rateIf(m_pressR.step(m_pressed == Right ? 1.0 : 0.0, kSpinHz, kSpinZeta, dt), Rate::Full);
    r |= rateIf(m_hover.step(m_hoverSide != None ? 1.0 : 0.0, kHoverHz, 1.0, dt), Rate::Hover);
    // 値が流れて入れ替わる途中（最後の1枚を描くため、終わった直後の1回も含める）
    if (m_slideClock.isValid() && m_slideClock.elapsed() < kSpinSlideMs + 16)
        r |= Rate::Full;
    return r;
}

void SpinBox::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    beginPaint(p, this);
    const QRectF r = QRectF(rect()).adjusted(kSurfaceMargin, kSurfaceMargin, -kSurfaceMargin, -kSurfaceMargin);
    drawSurface(p, this, Surface::SunkCapsule, r);
    const double cy = r.center().y();

    // 値（押した向きへ流れて入れ替わる）
    p.setFont(pixelFont(font(), kValueFontPx));
    const double side = kSpinBtnInset + kSpinBtnD;
    const QRectF valueBox(r.left() + side, r.top(), r.width() - side * 2.0, r.height());
    p.save();
    p.setClipRect(valueBox);
    const int flags = Qt::AlignCenter | Qt::TextSingleLine;
    const double e = m_slideClock.isValid() ? outCubic(m_slideClock.elapsed() / double(kSpinSlideMs)) : 1.0;
    if (e < 1.0) {
        drawShadowText(p, valueBox.translated(0.0, -m_slideDir * kSpinSlideDist * e), flags,
                       QString::number(m_prev), 0.95 * (1.0 - e));
        drawShadowText(p, valueBox.translated(0.0, m_slideDir * kSpinSlideDist * (1.0 - e)), flags,
                       QString::number(m_value), 0.95 * e);
    } else {
        drawShadowText(p, valueBox, flags, QString::number(m_value));
    }
    p.restore();

    // 左右の丸ボタン（左＝▼ 減らす / 右＝▲ 増やす）
    const auto drawStepButton = [&](const Spring &press, double bx, int dir, bool hovered) {
        const double sc = lerp(1.0, kSpinPressScale, clamp01(press.x));
        const double rad = kSpinBtnD / 2.0 * sc;
        QPainterPath circle;
        circle.addEllipse(QPointF(bx, cy), rad, rad);
        const bool atLimit = (dir < 0) ? (m_value <= m_min) : (m_value >= m_max);
        p.save();
        if (atLimit)
            p.setOpacity(p.opacity() * 0.5);
        drawSurface(p, this, Surface::RaisedCircle,
                    QRectF(bx - kSpinBtnD / 2.0, cy - kSpinBtnD / 2.0, kSpinBtnD, kSpinBtnD), sc);
        if (hovered)
            fillHover(p, circle, m_hover.x);
        fillPressDark(p, circle, 0.12 * clamp01(press.x));
        const double a = kSpinArrow * sc;
        QPainterPath tri;
        if (dir < 0) {
            tri.moveTo(bx - a, cy - a * 0.5);
            tri.lineTo(bx + a, cy - a * 0.5);
            tri.lineTo(bx, cy + a * 0.7);
        } else {
            tri.moveTo(bx - a, cy + a * 0.5);
            tri.lineTo(bx + a, cy + a * 0.5);
            tri.lineTo(bx, cy - a * 0.7);
        }
        tri.closeSubpath();
        p.fillPath(tri, withAlphaF(Qt::white, 0.95));
        p.restore();
    };
    drawStepButton(m_pressL, r.left() + kSpinBtnInset + kSpinBtnD / 2.0, -1, m_hoverSide == Left);
    drawStepButton(m_pressR, r.right() - kSpinBtnInset - kSpinBtnD / 2.0, 1, m_hoverSide == Right);
}

// ============================================================
//  Slider
// ============================================================
Slider::Slider(int minValue, int maxValue, int step, int value, QWidget *parent)
    : QWidget(parent)
    , m_min(minValue), m_max(qMax(minValue, maxValue)), m_step(qMax(1, step))
    , m_value(qBound(minValue, value, maxValue))
    , m_ticker(this, [this](double dt) { return stepAnim(dt); })
{
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    m_knob.snap(fractionOf(m_value));
}

// 2026-09-16: 影がはみ出す分（kSurfaceMargin）だけ部品は外へ広く置かれている
double Slider::knobX0() const { return kSurfaceMargin + kSliderValueW + kSliderValueGap + kSliderKnobD / 2.0; }
double Slider::knobX1() const { return kSurfaceMargin + kSliderValueW + kSliderValueGap + kSliderTrackW - kSliderKnobD / 2.0; }
void Slider::setValue(int value)
{
    const int nv = qBound(m_min, value, m_max);
    if (nv == m_value || m_pressed)
        return;
    m_value = nv;
    m_ticker.kick();   // つまみがバネで動く間だけ60Hz、止まったら0Hz
}

double Slider::fractionOf(int value) const
{
    return m_max > m_min ? double(value - m_min) / double(m_max - m_min) : 0.0;
}
int Slider::valueAtX(double x) const
{
    const double frac = clamp01((x - knobX0()) / std::max(1.0, knobX1() - knobX0()));
    const double raw = m_min + frac * (m_max - m_min);
    const int stepped = m_min + qRound((raw - m_min) / m_step) * m_step;
    return qBound(m_min, stepped, m_max);
}

void Slider::mousePressEvent(QMouseEvent *e)
{
    e->accept();
    if (e->button() != Qt::LeftButton)
        return;
    const double x = e->position().x();
    if (x < kSurfaceMargin + kSliderValueW + kSliderValueGap - 4)
        return; // 数値の上は反応しない
    m_pressed = true;
    m_valueAtPress = m_value;
    const double kx = lerp(knobX0(), knobX1(), m_knob.x);
    if (std::abs(x - kx) <= kSliderKnobD * 0.7) {
        // つまみを掴んだ：指に吸い付く
        m_dragging = true;
        m_dragOffset = x - kx;
    } else {
        // 溝をクリック：値だけ先に変え、つまみはバネで飛ぶ
        m_value = valueAtX(x);
    }
    m_ticker.kick();
}

void Slider::mouseMoveEvent(QMouseEvent *e)
{
    const double x = e->position().x();
    if (m_pressed) {
        if (!m_dragging) {
            m_dragging = true;
            m_dragOffset = 0.0;
        }
        const double pos = x - m_dragOffset;
        m_value = valueAtX(pos);
        m_knob.snap(clamp01((pos - knobX0()) / std::max(1.0, knobX1() - knobX0())));
        // マウスの移動イベントは60Hzより細かく来るので、ここでは描かずに
        // 印だけ付けて60Hzの tick に任せる（止まっていれば描かない）
        m_dragDirty = true;
        m_ticker.kick(Rate::Full);
    } else {
        const bool over = x >= kSurfaceMargin + kSliderValueW + kSliderValueGap - 4;
        if (over != m_hovered) {
            m_hovered = over;
            setCursor(over ? Qt::PointingHandCursor : Qt::ArrowCursor);
            m_ticker.kick(Rate::Hover);
        }
    }
    e->accept();
}

void Slider::mouseReleaseEvent(QMouseEvent *e)
{
    e->accept();
    if (!m_pressed)
        return;
    m_pressed = false;
    m_dragging = false;
    m_ticker.kick(); // つまみを値の位置へ戻す（ステップに吸着）
    if (m_value != m_valueAtPress && onValueChanged)
        onValueChanged(m_value);
}

void Slider::enterEvent(QEnterEvent *e) { QWidget::enterEvent(e); }
void Slider::leaveEvent(QEvent *e) { m_hovered = false; m_ticker.kick(Rate::Hover); QWidget::leaveEvent(e); }

Rate Slider::stepAnim(double dt)
{
    Rate r = Rate::Idle;
    if (m_dragDirty) {
        m_dragDirty = false;
        r |= Rate::Full;
    }
    if (!m_dragging)
        r |= rateIf(m_knob.step(fractionOf(m_value), kSliderHz, kSliderZeta, dt), Rate::Full);
    r |= rateIf(m_press.step(m_pressed ? 1.0 : 0.0, kSliderHz * 1.3, 0.8, dt), Rate::Full);
    r |= rateIf(m_hover.step(m_hovered ? 1.0 : 0.0, kHoverHz, 1.0, dt), Rate::Hover);
    return r;
}

void Slider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    beginPaint(p, this);
    const double cy = height() / 2.0;

    p.setFont(pixelFont(font(), kValueFontPx));
    drawShadowText(p, QRectF(kSurfaceMargin, 0, kSliderValueW, height()), Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine,
                   QString::number(m_value));

    const double tx = kSurfaceMargin + kSliderValueW + kSliderValueGap;
    const QRectF trackRect(tx, cy - kSliderTrackH / 2.0, kSliderTrackW, kSliderTrackH);
    const QPainterPath track = capsule(trackRect);
    drawSurface(p, this, Surface::SunkCapsule, trackRect);
    const double kx = lerp(knobX0(), knobX1(), m_knob.x);
    p.save();
    p.setClipPath(track);
    p.fillRect(QRectF(tx, trackRect.top(), kx - tx, trackRect.height()), withAlphaF(Qt::white, kSliderFillAlpha));
    p.restore();

    const double d = kSliderKnobD * lerp(1.0, kSliderPressGrow, clamp01(m_press.x)) * (1.0 + 0.04 * m_hover.x);
    drawKnob(p, QRectF(kx - d / 2.0, cy - d / 2.0, d, d));
}

// ============================================================
//  Choice のメニュー（島の外へはみ出せるようにポップアップ窓で出す）
// ============================================================
class ChoicePopup : public QWidget
{
public:
    explicit ChoicePopup(Choice *owner)
        : QWidget(owner, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
        , m_owner(owner)
        , m_ticker(this, [this](double dt) { return stepAnim(dt); })
    {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoMouseReplay, true);   // 閉じたクリックを下へ再送しない
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);
        setFont(pixelFont(owner->font(), kCtrlFontPx));
    }

    void openBelow()
    {
        const QFontMetricsF fm(font());
        double tw = 0.0;
        for (const QString &s : m_owner->m_labels)
            tw = std::max(tw, fm.horizontalAdvance(s));
        const int w = std::max(m_owner->width() - kSurfaceMargin * 2, static_cast<int>(std::ceil(tw + 22.0 + 18.0 + kMenuPad * 2.0)));
        const int h = static_cast<int>(m_owner->m_labels.size() * kMenuItemH + kMenuPad * 2.0);

        // 部品・窓とも影の分（kSurfaceMargin）外へ広い。見た目の外形で位置を合わせる。
        const int m = kSurfaceMargin;
        const QPoint ownerTopRight = m_owner->mapToGlobal(QPoint(m_owner->width() - m, m));
        const int ownerH = m_owner->height() - m * 2;
        QRect g(ownerTopRight.x() - w - m, ownerTopRight.y() + ownerH + 4 - m, w + m * 2, h + m * 2);
        m_up = false;
        if (QScreen *screen = m_owner->screen()) {
            const QRect avail = screen->availableGeometry();
            if (g.bottom() > avail.bottom()) {
                g.moveTop(ownerTopRight.y() - 4 - h - m);
                m_up = true;
            }
            if (g.left() < avail.left())
                g.moveLeft(avail.left());
        }
        setGeometry(g);

        m_hl = m_owner->m_current;
        m_hlSpring.snap(std::max(0, m_hl));
        m_closing = false;
        m_pressedInside = false;
        m_clock.restart();
        show();
        m_ticker.kick();
    }

    void closeAnimated()
    {
        if (m_closing || !isVisible())
            return;
        m_closing = true;
        // 開き途中で閉じたら、今の開き具合から縮める
        m_closeFrom = openProgress();
        m_clock.restart();
        m_ticker.kick();
    }

protected:
    double openProgress() const
    {
        if (m_closing)
            return m_closeFrom * (1.0 - outCubic(m_clock.elapsed() / double(kMenuCloseMs)));
        return outCubic(m_clock.elapsed() / double(kMenuOpenMs));
    }

    Rate stepAnim(double dt)
    {
        // 選択帯の追従は30Hz、開閉は60Hz。出たまま止まっていれば0Hz。
        Rate r = rateIf(m_hlSpring.step(std::max(0, m_hl), kMenuHighlightHz, kMenuHighlightZeta, dt), Rate::Half);
        if (m_closing) {
            if (m_clock.elapsed() >= kMenuCloseMs) {
                hide();
                return Rate::Idle;
            }
            return Rate::Full;
        }
        if (m_clock.elapsed() < kMenuOpenMs + 16)
            r |= Rate::Full;
        return r;
    }

    int itemAt(const QPointF &pos) const
    {
        if (!rect().contains(pos.toPoint()))
            return -1;
        const int i = static_cast<int>(std::floor((pos.y() - kSurfaceMargin - kMenuPad) / kMenuItemH));
        return qBound(0, i, int(m_owner->m_labels.size()) - 1);
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_closing)
            return;
        const int i = itemAt(e->position());
        if (i >= 0 && i != m_hl) {
            m_hl = i;
            m_ticker.kick(Rate::Half);
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (!rect().contains(e->position().toPoint())) {
            // 外側を押したら、閉じるアニメをしてから隠す。
            // 【2026-09-16】以前は QWidget::mousePressEvent（＝即座に閉じる）へ
            // 流していた。Qt はポップアップを閉じたクリックを下のウィジェットへ
            // 再送するので、開いた元の Choice を押すと「閉じる→その押下で
            // すぐ開き直す」になっていた。ここで受け取って閉じれば、閉じ切るまで
            // ポップアップがマウスを掴んだままなので、押下も解放も Choice へ届かない。
            e->accept();
            closeAnimated();
            return;
        }
        m_pressedInside = true;
        e->accept();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        e->accept();
        if (m_closing)
            return;
        const int i = itemAt(e->position());
        // 押したまま開いてメニュー上で離した場合も選べる（Choice 側で開いた直後の解放は無視）
        if (i >= 0 && (m_pressedInside || m_clock.elapsed() > 250)) {
            m_owner->choose(i);
            closeAnimated();
        }
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape) {
            closeAnimated();
            return;
        }
        QWidget::keyPressEvent(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        const double e = openProgress();
        if (e <= 0.001)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setOpacity(e);
        // 右上（上に開いたときは右下）を起点に拡大
        const double sc = lerp(kMenuOpenScale, 1.0, e);
        const QPointF origin(width() - kSurfaceMargin, m_up ? height() - kSurfaceMargin : kSurfaceMargin);
        p.translate(origin);
        p.scale(sc, sc);
        p.translate(-origin);

        const QRectF r = QRectF(rect()).adjusted(kSurfaceMargin, kSurfaceMargin, -kSurfaceMargin, -kSurfaceMargin);
        drawSurface(p, this, Surface::Menu, r);
        p.translate(kSurfaceMargin, kSurfaceMargin); // 以降はメニュー本体の座標
        const double menuW = r.width();

        if (m_hl >= 0) {
            const QRectF hr(kMenuPad, kMenuPad + m_hlSpring.x * kMenuItemH, menuW - kMenuPad * 2.0, kMenuItemH);
            const double rad = std::min(kMenuItemH / 2.0, std::max(2.0, kMenuRadius - kMenuPad));
            QPainterPath hp;
            hp.addRoundedRect(hr, rad, rad);
            p.fillPath(hp, withAlphaF(Qt::white, 0.2));
        }

        p.setFont(font());
        const QStringList &labels = m_owner->m_labels;
        for (int i = 0; i < labels.size(); ++i) {
            const double top = kMenuPad + i * kMenuItemH;
            const double cy = top + kMenuItemH / 2.0;
            if (i == m_owner->m_current) {
                const double cx = kMenuPad + 11.0;
                QPainterPath check;
                check.moveTo(cx - 3.5, cy + 0.5);
                check.lineTo(cx - 1.0, cy + 3.0);
                check.lineTo(cx + 3.5, cy - 3.0);
                QPen pen(withAlphaF(Qt::white, 0.95), 1.5);
                pen.setCapStyle(Qt::RoundCap);
                pen.setJoinStyle(Qt::RoundJoin);
                p.strokePath(check, pen);
            }
            const QRectF box(kMenuPad + 22.0, top, menuW - kMenuPad * 2.0 - 26.0, kMenuItemH);
            drawShadowText(p, box, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, labels[i],
                           i == m_owner->m_current ? 0.98 : 0.85);
        }
    }

private:
    Choice *m_owner;
    QElapsedTimer m_clock;
    Spring m_hlSpring;
    int m_hl = -1;
    bool m_up = false;
    bool m_closing = false;
    bool m_pressedInside = false;
    double m_closeFrom = 1.0;
    SpringTicker m_ticker;
};

// ============================================================
//  Choice
// ============================================================
Choice::Choice(QWidget *parent)
    : QWidget(parent)
    , m_ticker(this, [this](double dt) { return stepAnim(dt); })
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFont(pixelFont(font(), kCtrlFontPx));
}

void Choice::addItem(const QString &label, const QString &id)
{
    m_labels << label;
    m_ids << id;
    if (m_current < 0)
        m_current = 0;
    update();
}

void Choice::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_labels.size() || index == m_current)
        return;
    m_current = index;
    update();
}

void Choice::choose(int index)
{
    if (index < 0 || index >= m_labels.size() || index == m_current)
        return;
    m_current = index;
    update();
    if (onCurrentIndexChanged)
        onCurrentIndexChanged(index);
}

int Choice::preferredWidth() const
{
    const QFontMetricsF fm(font());
    double tw = 0.0;
    for (const QString &s : m_labels)
        tw = std::max(tw, fm.horizontalAdvance(s));
    return qBound(kChoiceMinW, static_cast<int>(std::ceil(tw + kChoicePadL + 26.0)), kChoiceMaxW);
}

void Choice::mousePressEvent(QMouseEvent *e)
{
    e->accept();
    if (e->button() != Qt::LeftButton)
        return;
    m_pressed = true;
    m_ticker.kick();
}

void Choice::mouseReleaseEvent(QMouseEvent *e)
{
    e->accept();
    if (!m_pressed)
        return;
    m_pressed = false;
    m_ticker.kick();
    if (rect().adjusted(-6, -6, 6, 6).contains(e->position().toPoint()) && !m_labels.isEmpty()) {
        if (!m_popup)
            m_popup = new ChoicePopup(this);
        m_popup->openBelow();
    }
}

void Choice::enterEvent(QEnterEvent *e) { m_hovered = true; m_ticker.kick(Rate::Hover); QWidget::enterEvent(e); }
void Choice::leaveEvent(QEvent *e) { m_hovered = false; m_ticker.kick(Rate::Hover); QWidget::leaveEvent(e); }

Rate Choice::stepAnim(double dt)
{
    Rate r = rateIf(m_press.step(m_pressed ? 1.0 : 0.0, kButtonHz, kButtonZeta, dt), Rate::Full);
    r |= rateIf(m_hover.step(m_hovered ? 1.0 : 0.0, kHoverHz, 1.0, dt), Rate::Hover);
    return r;
}

void Choice::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    beginPaint(p, this);
    const QRectF r = QRectF(rect()).adjusted(kSurfaceMargin, kSurfaceMargin, -kSurfaceMargin, -kSurfaceMargin);
    const QPainterPath path = capsule(r);
    drawSurface(p, this, Surface::RaisedCapsule, r);
    fillHover(p, path, m_hover.x);
    fillPressDark(p, path, 0.10 * clamp01(m_press.x));

    p.setFont(font());
    const QString label = m_labels.value(m_current);
    const QRectF textBox(r.left() + kChoicePadL, r.top(), r.width() - kChoicePadL - 26.0, r.height());
    const QString elided = QFontMetrics(font()).elidedText(label, Qt::ElideRight, qMax(0, int(textBox.width())));
    drawShadowText(p, textBox, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, elided);

    // ⌃⌄
    const double cx = r.right() - 13.0, cy = r.center().y(), c = kChoiceChevron;
    QPainterPath chev;
    chev.moveTo(cx - c, cy - 1.5);
    chev.lineTo(cx, cy - 1.5 - c);
    chev.lineTo(cx + c, cy - 1.5);
    chev.moveTo(cx - c, cy + 1.5);
    chev.lineTo(cx, cy + 1.5 + c);
    chev.lineTo(cx + c, cy + 1.5);
    QPen pen(withAlphaF(Qt::white, 0.9), 1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.strokePath(chev, pen);
}

// ============================================================
//  TextField
// ============================================================
TextField::TextField(QWidget *parent)
    : QWidget(parent)
    , m_ticker(this, [this](double dt) { return stepAnim(dt); })
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setCursor(Qt::IBeamCursor);
    m_edit = new QLineEdit(this);
    m_edit->setFrame(false);
    m_edit->setAttribute(Qt::WA_TranslucentBackground, true);
    // パネルのQSSより優先させるため、この部品だけに直接指定する
    m_edit->setStyleSheet(QStringLiteral(
        "QLineEdit{background:transparent;border:none;padding:0;margin:0;color:white;"
        "selection-background-color:rgba(255,255,255,90);selection-color:white;}"));
    m_edit->setFont(pixelFont(font(), kCtrlFontPx));
    QPalette pal = m_edit->palette();
    pal.setColor(QPalette::PlaceholderText, QColor(255, 255, 255, 115));
    pal.setColor(QPalette::Text, Qt::white);
    m_edit->setPalette(pal);
    m_edit->installEventFilter(this);
}

void TextField::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    const int o = kTextOutset;
    const int padX = static_cast<int>(kTextPadX);
    m_edit->setGeometry(o + padX, o, qMax(0, width() - (o + padX) * 2), qMax(0, height() - o * 2));
}

bool TextField::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit) {
        switch (event->type()) {
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            m_ticker.kick();
            break;
        case QEvent::Enter:
            m_hovered = true;
            m_ticker.kick(Rate::Hover);
            break;
        case QEvent::EnabledChange:
            update();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TextField::enterEvent(QEnterEvent *e) { m_hovered = true; m_ticker.kick(Rate::Hover); QWidget::enterEvent(e); }
void TextField::leaveEvent(QEvent *e)
{
    // 子の QLineEdit へ入ったときも Leave が来るので、実際に外へ出たかを見る
    m_hovered = rect().contains(mapFromGlobal(QCursor::pos()));
    m_ticker.kick(Rate::Hover);
    QWidget::leaveEvent(e);
}

void TextField::mousePressEvent(QMouseEvent *e)
{
    // 余白（左右の丸い端）を押しても入力できるように
    m_edit->setFocus(Qt::MouseFocusReason);
    m_edit->setCursorPosition(e->position().x() < width() / 2.0 ? 0 : m_edit->text().size());
    e->accept();
}

Rate TextField::stepAnim(double dt)
{
    Rate r = rateIf(m_focus.step(m_edit->hasFocus() ? 1.0 : 0.0, kTextFocusHz, 0.85, dt), Rate::Full);
    r |= rateIf(m_hover.step(m_hovered ? 1.0 : 0.0, kHoverHz, 1.0, dt), Rate::Hover);
    return r;
}

void TextField::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    beginPaint(p, this);
    const double o = kTextOutset;
    const QRectF field = QRectF(rect()).adjusted(o, o, -o, -o);
    const QPainterPath path = capsule(field);
    drawSurface(p, this, Surface::SunkCapsule, field);
    fillHover(p, path, m_hover.x, 0.6);

    const double f = clamp01(m_focus.x);
    if (f > 0.002) {
        const double gap = (kTextRingGap + kTextRingW / 2.0) * f;
        QPen pen(withAlphaF(QColor(0xbf, 0xe3, 0xff), kTextRingAlpha * f), kTextRingW * lerp(0.4, 1.0, f));
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(capsule(field.adjusted(-gap, -gap, gap, gap)));
    }
}

// ============================================================
//  KeyInput
// ============================================================
QString defaultKeyName(int vk)
{
    switch (vk) {
    case 0x10: return QStringLiteral("Shift");
    case 0x11: return QStringLiteral("Ctrl");
    case 0x12: return QStringLiteral("Alt");
    case 0x5B: return QStringLiteral("Win");
    case 0x20: return QStringLiteral("Space");
    case 0x0D: return QStringLiteral("Enter");
    case 0x09: return QStringLiteral("Tab");
    case 0x25: return QStringLiteral("←");
    case 0x26: return QStringLiteral("↑");
    case 0x27: return QStringLiteral("→");
    case 0x28: return QStringLiteral("↓");
    default: break;
    }
    if (vk >= 0x70 && vk <= 0x87)
        return QStringLiteral("F%1").arg(vk - 0x70 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return QString(QChar(static_cast<char16_t>(vk)));
    return QStringLiteral("0x%1").arg(vk, 2, 16, QLatin1Char('0')).toUpper();
}

KeyInput::KeyInput(QWidget *parent)
    : TextField(parent)
{
    setCursor(Qt::PointingHandCursor);
    edit()->setAlignment(Qt::AlignCenter);
    edit()->setAttribute(Qt::WA_InputMethodEnabled, false);
    edit()->setContextMenuPolicy(Qt::NoContextMenu);
    edit()->setFocusPolicy(Qt::ClickFocus);
    edit()->setCursor(Qt::PointingHandCursor);
    edit()->setPlaceholderText(QStringLiteral("—"));
    // 入力中の目印はフォーカスの輪が出すので、文字カーソルは出さない
    edit()->setReadOnly(true);
}

void KeyInput::setKeyNameFunction(std::function<QString(int)> fn)
{
    m_keyName = std::move(fn);
    setKey(m_key);
}

void KeyInput::setKey(int virtualKey)
{
    m_key = qMax(0, virtualKey);
    edit()->setText(m_key == 0 ? QString() : (m_keyName ? m_keyName(m_key) : defaultKeyName(m_key)));
    edit()->setCursorPosition(0);
}

bool KeyInput::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == edit()) {
        switch (event->type()) {
        case QEvent::ShortcutOverride:
            // キャンバス等のショートカットに奪われないよう、全部この欄が受け取る
            event->accept();
            return true;
        case QEvent::InputMethod:
            return true;
        case QEvent::KeyPress: {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (ke->isAutoRepeat())
                return true;
            if (ke->key() == Qt::Key_Escape) {
                edit()->clearFocus();   // 何もせずに選択解除
                return true;
            }
            int vk = 0;
            // 2026-09-17: Delete はバブル削除の既定キーなので割り当てられるようにし、
            // 空にするのは Backspace だけにした。
            if (ke->key() != Qt::Key_Backspace) {
                vk = static_cast<int>(ke->nativeVirtualKey());
                if (vk == 0)
                    vk = ke->key();
                switch (vk) {   // 左右のある修飾キーは区別しない（RegisterHotKey も区別しない）
                case 0xA0: case 0xA1: vk = 0x10; break;   // L/R Shift
                case 0xA2: case 0xA3: vk = 0x11; break;   // L/R Ctrl
                case 0xA4: case 0xA5: vk = 0x12; break;   // L/R Alt
                case 0x5C:            vk = 0x5B; break;   // R Win
                default: break;
                }
            }
            // 2026-09-16: キーを1つ入れたら（Backspace で空にした場合も）選択を
            // 解除する。何も変えずに抜けるときは Esc（上）。
            // 通知の前に解除しておく（通知先でパネルが作り直されても安全なように）。
            edit()->clearFocus();
            if (vk != m_key) {
                setKey(vk);
                if (onKeyChanged)
                    onKeyChanged(vk);
            }
            return true;
        }
        default:
            break;
        }
    }
    return TextField::eventFilter(watched, event);
}

} // namespace IslandControls
