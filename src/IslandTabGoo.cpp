#include "IslandTabGoo.h"
#include "PbSvgIcon.h"

#include <QFontMetricsF>

#include "DynamicIsland.h"   // AnimatedButton

#include <QFontMetrics>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QVariantAnimation>
#include <QGraphicsOpacityEffect>

#include <algorithm>
#include <cmath>
#include <vector>

namespace IslandTabGoo {

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---- パラメータ（試作ページ goo-v1 で決めた値）----------------------------
// 調整し直すときは試作ページで値を作り、ここへ写すこと。
// btnR は試作では固定14だったが、本体では＋ボタンの実際の半径
// （12 × 出現スケール × ホバーの拡大。クリック時はホバー中なので約13.8）を使う。
struct Params
{
    double k       = 11.5;   // smin の強さ（くっつき）
    double grid    = 1.5;    // 距離場の格子間隔 px
    // 開く
    double oRdelay = 0.0,   oRf = 2.6, oRz = 0.82;
    double oLdelay = 150.0, oLf = 3.1, oLz = 0.55;
    double oHdelay = 20.0,  oHf = 2.9, oHz = 1.2;
    double bulge   = 8.0;
    double recoil  = 0.0;
    // 2026-09-15（本体で追加）: 首がちぎれた後、引っ張られていた＋ボタンが
    // 定位置へ戻る動き。試作では雫の左端に引きずられてすぐ戻っていたのを、
    // ちぎれた瞬間から独立したゆっくりめのばねで戻す。
    double returnF = 2.0;    // Hz（小さいほどゆっくり）
    double returnZ = 0.72;   // 減衰（1未満でわずかに左へ行き過ぎてから落ち着く）
    // 閉じる
    double cLdelay = 0.0,   cLf = 3.4, cLz = 0.8;
    double cRdelay = 70.0,  cRf = 3.0, cRz = 0.93;
    double cHdelay = 240.0, cHf = 2.0, cHz = 1.2;
    double swell   = 0.25;
    // 2026-09-15（本体で追加）: 閉じる後半の細り。吸い込みの進み（reach）が
    // thinStart→thinEnd の間で、全体を thin だけ細くし、右端の丸みを
    // taper 倍まですぼめる（とがらせはしない。丸みは残る）。
    double thinStart = 0.30, thinEnd = 0.75;
    double thin    = 0.35;   // 左端（＋ボタン側）の太さをどれだけ減らすか
    double taper   = 0.40;   // 右端の丸みの半径 ÷ 左端の丸みの半径
    // 2026-09-15（本体で追加）: 開くときも、伸びている間は右端をすぼめる。
    // 最初から oTaper でとがった状態で伸び、oTaperRelaxStart→oTaperRelaxEnd（ms）で
    // 普通のカプセルへ戻る。とがっているのは伸び始めだけで、開くアニメ全体
    // （1100ms）の 1/4 ほどで完成形の形になる。
    double oTaper  = 0.40;
    double oTaperRelaxStart = 40.0, oTaperRelaxEnd = 280.0;
    // 表現の切り替えタイミング
    double oSinkStart = 180.0, oSinkEnd = 420.0;
    double oTextStart = 380.0, oTextEnd = 560.0;
    double cSinkEnd = 140.0, cTextEnd = 70.0;
};
constexpr Params kP {};

double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

double smoothstep(double a, double b, double x)
{
    if (b <= a)
        return x >= b ? 1.0 : 0.0;
    const double t = clamp01((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}

// 0→1 のステップ応答（減衰振動の解析解）。t は秒。
// z>=1 は臨界減衰の式で近似（試作ページと同じ式。値を揃えるため変えないこと）。
double spring(double t, double f, double z)
{
    if (t <= 0.0)
        return 0.0;
    const double w = 2.0 * kPi * f;
    if (z < 1.0) {
        const double wd = w * std::sqrt(1.0 - z * z);
        return 1.0 - std::exp(-z * w * t)
                         * (std::cos(wd * t) + (z * w / wd) * std::sin(wd * t));
    }
    return 1.0 - std::exp(-w * t) * (1.0 + w * t);
}

// 多項式 smooth min
double smin(double a, double b, double k)
{
    if (k <= 0.0)
        return std::min(a, b);
    const double h = std::max(k - std::abs(a - b), 0.0) / k;
    return std::min(a, b) - h * h * k * 0.25;
}

// 左右で丸みの半径が違う横向きカプセル（左 r1・右 r2、両端の円を接線でつなぐ）。
// 閉じる後半の「左は太く、右へすぼまる」形に使う。r2==r1 なら普通のカプセル。
double sdCapsule(double px, double py, double L, double R, double hh, double cy);
double sdTaperedCapsule(double px, double py, double L, double R,
                        double r1, double r2, double cy)
{
    if (r1 <= 0.01 || R - L <= 0.01)
        return 1e3;
    r1 = std::min(r1, (R - L) / 2.0);
    r2 = std::min(r2, r1);
    const double ax = L + r1;         // 左の円の中心
    const double bx = R - r2;         // 右の円の中心
    const double h = bx - ax;
    if (h <= (r1 - r2) + 1e-3)        // 短すぎて接線が引けない → 普通のカプセルで代用
        return sdCapsule(px, py, L, R, r1, cy);
    // 軸方向を u、垂直方向を v（上下対称なので絶対値）とした2次元の式
    const double v = std::abs(py - cy);
    const double u = px - ax;
    const double b = (r1 - r2) / h;
    const double a = std::sqrt(1.0 - b * b);
    const double kk = -b * v + a * u;
    if (kk < 0.0)
        return std::hypot(v, u) - r1;
    if (kk > a * h)
        return std::hypot(v, u - h) - r2;
    return v * a + u * b - r1;
}

double sdCapsule(double px, double py, double L, double R, double hh, double cy)
{
    if (hh <= 0.01 || R - L <= 0.01)
        return 1e3;
    const double r = std::min(hh, (R - L) / 2.0);
    const double ax = L + r, bx = R - r;
    const double qx = px < ax ? ax : (px > bx ? bx : px);
    return std::hypot(px - qx, py - cy) - r;
}

// ＋ボタンの色（AnimatedButton::paintEvent の tintColor と同じ導出）
QColor buttonTint()
{
    const QColor hsv = QColor(QStringLiteral("#4A90E2")).toHsv();
    QColor c = QColor::fromHsv(qMax(0, hsv.hue()),
                               qBound(0, static_cast<int>(hsv.saturation() * 1.04), 255),
                               qBound(0, static_cast<int>(hsv.value() * 1.20 + 10), 255));
    c.setAlpha(192);
    return c;
}
QColor buttonShade()
{
    const QColor hsv = QColor(QStringLiteral("#4A90E2")).toHsv();
    return QColor::fromHsv(qMax(0, hsv.hue()),
                           qMin(255, static_cast<int>(hsv.saturation() * 1.10)),
                           static_cast<int>(hsv.value() * 0.52));
}

QColor mixColor(const QColor &a, const QColor &b, double t)
{
    t = clamp01(t);
    const auto m = [t](int x, int y) { return qRound(x + (y - x) * t); };
    return QColor(m(a.red(), b.red()), m(a.green(), b.green()),
                  m(a.blue(), b.blue()), m(a.alpha(), b.alpha()));
}

QColor alphaScaled(QColor c, double s)
{
    c.setAlpha(qBound(0, qRound(c.alpha() * s), 255));
    return c;
}

struct OpenRaw { double L, R, hh; };
OpenRaw openRaw(double tau, const Layout &lay)
{
    const double s = tau / 1000.0;
    const double cx = lay.buttonCx;
    double R = cx + (lay.tabRight - cx) * spring(s - kP.oRdelay / 1000.0, kP.oRf, kP.oRz);
    double L = cx + (lay.tabLeft - cx) * spring(s - kP.oLdelay / 1000.0, kP.oLf, kP.oLz);
    if (L > R) { const double m = (L + R) / 2.0; L = R = m; }
    const double hh = (kTabHeight / 2.0)
                      * std::max(0.0, spring(s - kP.oHdelay / 1000.0, kP.oHf, kP.oHz));
    return { L, R, hh };
}

} // namespace

namespace {
// ちぎれるまでの「引っ張られ具合」0→1（1でちぎれる位置）
double openPull(const OpenRaw &s, const Layout &lay)
{
    return clamp01((s.L - lay.buttonCx) / (lay.buttonR + kP.k));
}
} // namespace

double openPinchMs(const Layout &layout)
{
    for (double t = 0.0; t <= kOpenMs; t += 2.0) {
        const OpenRaw s = openRaw(t, layout);
        // 雫の左端とボタンの縁の隙間が smin の効く範囲（k）を超えたら、首は切れている。
        // 行き先が近くて隙間が k に届かない場合も、引っ張られ切ったらちぎれた扱い
        // （そうしないと＋ボタンが寄ったまま戻らない）。
        if (s.hh > 0.5 && (s.L - (layout.buttonCx + layout.buttonR) >= kP.k
                           || openPull(s, layout) >= 0.98))
            return t;
    }
    return kOpenMs * 0.5;
}

Frame evalOpen(double tau, const Layout &lay, double pinchMs)
{
    const OpenRaw s = openRaw(tau, lay);
    Frame f;
    f.L = s.L; f.R = s.R; f.hh = s.hh;
    // ちぎれるまでは引っ張られて寄り続け（ちぎれる瞬間が最大）、
    // ちぎれたらその位置から独立したばねで定位置へ戻る。
    if (tau <= pinchMs) {
        f.bx = kP.bulge * std::sin(0.5 * kPi * openPull(s, lay));
    } else {
        const double atPinch = kP.bulge * std::sin(0.5 * kPi * openPull(openRaw(pinchMs, lay), lay));
        const double d = (tau - pinchMs) / 1000.0;
        f.bx = atPinch * (1.0 - spring(d, kP.returnF, kP.returnZ));
    }
    if (tau > pinchMs && kP.recoil > 0.0) {
        const double d = (tau - pinchMs) / 1000.0;
        f.bx -= kP.recoil * std::exp(-d / 0.09) * std::sin(2.0 * kPi * 7.0 * d);
    }
    f.rScale = 1.0;
    // 伸びている間は右端をすぼめ、着地に合わせて普通のカプセルへ戻す
    f.taper = kP.oTaper + (1.0 - kP.oTaper)
                          * smoothstep(kP.oTaperRelaxStart, kP.oTaperRelaxEnd, tau);
    f.sink = smoothstep(kP.oSinkStart, kP.oSinkEnd, tau);
    f.text = smoothstep(kP.oTextStart, kP.oTextEnd, tau);
    return f;
}

Frame evalClose(double tauScaled, const Layout &lay)
{
    // 形は試作の時間軸（700ms）で計算し、再生だけ引き延ばす（kCloseTimeScale）。
    const double tau = tauScaled / kCloseTimeScale;
    const double s = tau / 1000.0;
    const double cx = lay.buttonCx;
    double L = lay.tabLeft + (cx - lay.tabLeft) * spring(s - kP.cLdelay / 1000.0, kP.cLf, kP.cLz);
    double R = lay.tabRight + (cx - lay.tabRight) * spring(s - kP.cRdelay / 1000.0, kP.cRf, kP.cRz);
    if (L > R) { const double m = (L + R) / 2.0; L = R = m; }
    Frame f;
    f.L = L; f.R = R;
    f.hh = (kTabHeight / 2.0)
           * std::max(0.0, 1.0 - spring(s - kP.cHdelay / 1000.0, kP.cHf, kP.cHz));
    const double reach = clamp01(1.0 - (R - cx) / std::max(1.0, lay.tabRight - cx));
    const double bump = std::sin(kPi * clamp01((reach - 0.55) / 0.45));
    const double pull = clamp01(1.0 - (L - cx) / (lay.buttonR + kP.k));
    f.bx = kP.bulge * 0.6 * std::sin(kPi * pull) * (L > cx + 0.5 ? 1.0 : 0.0);
    f.rScale = 1.0 + kP.swell * bump;
    // 後半の細り（左は太いまま、右へすぼまる）
    const double thinQ = smoothstep(kP.thinStart, kP.thinEnd, reach);
    f.hh *= 1.0 - kP.thin * thinQ;
    f.taper = 1.0 - (1.0 - kP.taper) * thinQ;
    f.sink = 1.0 - smoothstep(0.0, kP.cSinkEnd, tau);
    f.text = 1.0 - smoothstep(0.0, kP.cTextEnd, tau);
    return f;
}

Frame restingOpen(const Layout &lay)
{
    Frame f;
    f.L = lay.tabLeft; f.R = lay.tabRight; f.hh = kTabHeight / 2.0;
    f.sink = 1.0; f.text = 1.0;
    return f;
}

// ---- 距離場 → 輪郭（marching squares）-------------------------------------
// 辺ID: 横辺 (i,j) = 2*(j*cols+i)、縦辺 (i,j) = 2*(j*cols+i)+1
QPainterPath buildPath(const Frame &fr, const Layout &lay)
{
    const double step = kP.grid;
    const double k = kP.k;
    const double cx = lay.buttonCx + fr.bx;
    const double cy = lay.buttonCy;
    const double r = lay.buttonR * fr.rScale;

    const double x0 = -4.0, y0 = -4.0;
    const double x1 = std::max(fr.R, cx + r) + k + 6.0;
    const double y1 = cy * 2.0 + 4.0;
    const int nx = static_cast<int>(std::ceil((x1 - x0) / step));
    const int ny = static_cast<int>(std::ceil((y1 - y0) / step));
    const int cols = nx + 1;
    const int rows = ny + 1;

    std::vector<float> field(static_cast<size_t>(cols) * rows);
    for (int j = 0; j <= ny; ++j) {
        const double py = y0 + j * step;
        for (int i = 0; i <= nx; ++i) {
            const double px = x0 + i * step;
            const double dc = std::hypot(px - cx, py - cy) - r;
            const double dt = (fr.taper < 0.999)
                ? sdTaperedCapsule(px, py, fr.L, fr.R, fr.hh, fr.hh * fr.taper, cy)
                : sdCapsule(px, py, fr.L, fr.R, fr.hh, cy);
            double d = smin(dc, dt, k);
            if (i == 0 || j == 0 || i == nx || j == ny)
                d = std::max(d, 0.001);   // 格子の外周は必ず外側（輪郭が閉じるように）
            field[static_cast<size_t>(j) * cols + i] = static_cast<float>(d);
        }
    }
    const auto V = [&](int i, int j) { return field[static_cast<size_t>(j) * cols + i]; };

    const size_t edgeCount = static_cast<size_t>(cols) * rows * 2;
    std::vector<int> nb(edgeCount * 2, -1);        // 各辺の隣（最大2つ）
    const auto link = [&](int a, int b) {
        auto put = [&](int e, int o) {
            if (nb[e * 2] < 0) nb[e * 2] = o; else nb[e * 2 + 1] = o;
        };
        put(a, b); put(b, a);
    };

    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const float tl = V(i, j), tr = V(i + 1, j), br = V(i + 1, j + 1), bl = V(i, j + 1);
            const int c = (tl < 0 ? 8 : 0) | (tr < 0 ? 4 : 0) | (br < 0 ? 2 : 0) | (bl < 0 ? 1 : 0);
            if (c == 0 || c == 15)
                continue;
            const int eT = 2 * (j * cols + i);
            const int eR = 2 * (j * cols + i + 1) + 1;
            const int eB = 2 * ((j + 1) * cols + i);
            const int eL = 2 * (j * cols + i) + 1;
            const bool center = (tl + tr + br + bl) / 4.0f < 0.0f;
            switch (c) {
            case 1: case 14: link(eL, eB); break;
            case 2: case 13: link(eB, eR); break;
            case 3: case 12: link(eL, eR); break;
            case 4: case 11: link(eT, eR); break;
            case 6: case 9:  link(eT, eB); break;
            case 7: case 8:  link(eL, eT); break;
            case 5:
                if (center) { link(eL, eT); link(eB, eR); } else { link(eL, eB); link(eT, eR); }
                break;
            case 10:
                if (center) { link(eT, eR); link(eL, eB); } else { link(eL, eT); link(eB, eR); }
                break;
            default: break;
            }
        }
    }

    const auto edgePoint = [&](int id) {
        const int base = id >> 1;
        const bool horiz = (id & 1) == 0;
        const int i = base % cols, j = base / cols;
        const float a = V(i, j);
        const float b = horiz ? V(i + 1, j) : V(i, j + 1);
        const double t = a / (a - b);
        return horiz ? QPointF(x0 + (i + t) * step, y0 + j * step)
                     : QPointF(x0 + i * step, y0 + (j + t) * step);
    };

    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    std::vector<char> seen(edgeCount, 0);
    std::vector<QPointF> loop;
    for (size_t start = 0; start < edgeCount; ++start) {
        if (nb[start * 2] < 0 || seen[start])
            continue;
        loop.clear();
        int prev = -1;
        int cur = static_cast<int>(start);
        while (cur >= 0 && !seen[cur]) {
            seen[cur] = 1;
            loop.push_back(edgePoint(cur));
            const int n0 = nb[cur * 2], n1 = nb[cur * 2 + 1];
            const int next = (n0 != prev) ? n0 : n1;
            prev = cur;
            cur = next;
        }
        const size_t n = loop.size();
        if (n < 3)
            continue;
        // 中点を通る2次ベジェで閉じる（格子の角ばりを消す）
        const auto mid = [&](size_t q) {
            const QPointF &a = loop[q % n], &b = loop[(q + 1) % n];
            return (a + b) * 0.5;
        };
        path.moveTo(mid(0));
        for (size_t q = 1; q <= n; ++q)
            path.quadTo(loop[q % n], mid(q));
        path.closeSubpath();
    }
    return path;
}

QPainterPath capsulePath(const QRectF &rect)
{
    QPainterPath p;
    const double r = rect.height() / 2.0;
    p.addRoundedRect(rect, r, r);
    return p;
}

// ---- 面の描画 --------------------------------------------------------------
// 浮き出た面は AnimatedButton::paintEvent の (1)〜(6) を任意の形へ置き換えたもの。
// 円用の放射グラデーションは使えないので、「縁に沿ったストローク（クリップの
// 内側に半分だけ残る）」で縁光と翳りを作る。
//
// ＋ボタンは常に浮き出た面、雫は sink に応じて沈み込む。首でつながっている間も
// 境目が段差にならないよう、光や影の濃さは横方向のグラデーション（gradX0→gradX1）
// で「ボタン側の重み」から「雫側の重み」へ移す。縦方向のグラデーションを持つ
// 細い線（輪郭・縁の光）だけは、左右で描き分けてクリップで切り替える。
void gradientSpan(const Frame &fr, const Layout &lay, double &x0, double &x1)
{
    // ボタンの縁から、雫の左端（離れていれば）まで。開き切ったタブの左端では
    // 必ず雫側の色・表現になっているように、終点を雫の左端より右へは置かない。
    const double cx = lay.buttonCx + fr.bx;
    const double r = lay.buttonR * fr.rScale;
    x0 = cx + r;
    x1 = std::max(x0 + kP.k, fr.L);
    if (fr.L > x0)
        x1 = std::max(x0 + 1.0, fr.L);
}

void paintSurface(QPainter &p, const QPainterPath &path, double sink,
                  double gradX0, double gradX1, const ButtonDisc *disc)
{
    if (path.isEmpty())
        return;
    sink = clamp01(sink);
    const double raised = 1.0 - sink;
    const QRectF bb = path.boundingRect();
    const QColor tint = buttonTint();
    const QColor shade = buttonShade();
    const double gx0 = disc ? gradX0 : bb.left() - 2000.0;
    const double gx1 = disc ? std::max(gradX0 + 1.0, gradX1) : bb.left() - 1999.0;
    const bool drawDropRaised = raised > 0.001;
    const bool drawSunk = sink > 0.001;
    const auto withA = [](QColor c, int a) { c.setAlpha(qBound(0, a, 255)); return c; };

    // 横方向に重みを変えるブラシ。左（ボタン側）で wLeft、右（雫側）で wRight 倍。
    const auto hBrush = [&](const QColor &c, double wLeft, double wRight) {
        QLinearGradient g(QPointF(gx0, 0.0), QPointF(gx1, 0.0));
        g.setColorAt(0.0, alphaScaled(c, wLeft));
        g.setColorAt(1.0, alphaScaled(c, wRight));
        return QBrush(g);
    };
    // 雫側だけに効くペン（ボタンの円は放射グラデーションで別に描くので 0）
    const auto dropPen = [&](const QColor &c, double width) {
        return QPen(hBrush(c, 0.0, raised), width);
    };
    const auto sunkPen = [&](const QColor &c, double width) {
        return QPen(hBrush(c, 0.0, sink), width);
    };

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);

    // ---- 落ち影 ----
    if (disc) {
        // AnimatedButton と同じ放射状の影（大きさは出現スケールだけに追従）
        const double shadowR = disc->shadowBase * 1.28;
        const QPointF sc(disc->center.x(), disc->center.y() + disc->shadowBase * 0.16);
        QRadialGradient shadow(sc, shadowR);
        shadow.setColorAt(0.00, QColor(0, 0, 0, 66));
        shadow.setColorAt(0.60, QColor(0, 0, 0, 28));
        shadow.setColorAt(1.00, QColor(0, 0, 0, 0));
        p.setBrush(shadow);
        p.drawEllipse(sc, shadowR, shadowR);
        p.setBrush(Qt::NoBrush);
    }
    if (drawDropRaised) {
        const QPainterPath shadowPath = path.translated(0.0, 1.9);
        p.fillPath(shadowPath, hBrush(QColor(0, 0, 0, 34), 0.0, raised));
        p.strokePath(shadowPath, dropPen(QColor(0, 0, 0, 16), 3.0));
    }

    // ---- (1) 面そのもの：フロスト＋ボタンの色 → 雫の色（沈んだ面の暗さ）----
    {
        p.fillPath(path, hBrush(QColor(255, 255, 255, 10), 1.0, raised));
        const QColor sunkFill(0, 0, 0, 58);
        QLinearGradient fill(QPointF(gx0, 0.0), QPointF(gx1, 0.0));
        fill.setColorAt(0.0, tint);
        fill.setColorAt(1.0, mixColor(tint, sunkFill, sink));
        p.fillPath(path, fill);
    }

    p.setClipPath(path);

    // ---- ＋ボタンの円：AnimatedButton::paintEvent の (2)〜(4) をそのまま ----
    // 違いは1つだけ。リムグローの放射グラデーションを円の外（首の部分）へ k だけ
    // 延ばし、そこで0へ落とす。首がつながっている間、光が円の縁で途切れず、
    // 首の中へ自然に薄れていく。
    if (disc) {
        const double cx = disc->center.x(), cy = disc->center.y(), r = disc->radius;
        const double rr = r + kP.k;
        const QRectF area(cx - r - 2.0, cy - r - 2.0, 2.0 * r + kP.k + 4.0, 2.0 * r + 4.0);

        p.setCompositionMode(QPainter::CompositionMode_Plus);
        {
            QLinearGradient sheen(QPointF(cx - r, cy - r), QPointF(cx + r, cy + r));
            sheen.setColorAt(0.00, QColor(255, 255, 255, 13));
            sheen.setColorAt(0.55, QColor(255, 255, 255, 13 / 4));
            sheen.setColorAt(1.00, QColor(255, 255, 255, 0));
            p.fillRect(area, sheen);
        }
        {
            const auto at = [&](double t) { return t * r / rr; };
            QRadialGradient ring(QPointF(cx, cy), rr);
            ring.setColorAt(0.00, QColor(255, 255, 255, 0));
            ring.setColorAt(at(0.38), QColor(255, 255, 255, 0));
            ring.setColorAt(at(0.68), QColor(255, 255, 255, 4));
            ring.setColorAt(at(0.86), QColor(255, 255, 255, 12));
            ring.setColorAt(at(0.96), QColor(255, 255, 255, 22));
            ring.setColorAt(at(1.00), QColor(255, 255, 255, 18));
            ring.setColorAt(1.00, QColor(255, 255, 255, 0));
            p.fillRect(area, ring);
        }
        {
            QRadialGradient topGlow(QPointF(cx, cy - r * 1.02), r * 0.95);
            topGlow.setColorAt(0.00, QColor(255, 255, 255, 22));
            topGlow.setColorAt(0.45, QColor(255, 255, 255, 7));
            topGlow.setColorAt(1.00, QColor(255, 255, 255, 0));
            p.fillRect(area, topGlow);
            QRadialGradient bottomGlow(QPointF(cx, cy + r * 1.06), r * 0.80);
            bottomGlow.setColorAt(0.00, QColor(255, 255, 255, 16));
            bottomGlow.setColorAt(0.45, QColor(255, 255, 255, 5));
            bottomGlow.setColorAt(1.00, QColor(255, 255, 255, 0));
            p.fillRect(area, bottomGlow);
        }
        p.setCompositionMode(QPainter::CompositionMode_Multiply);
        for (const int side : {-1, 1}) {
            QRadialGradient sideShade(QPointF(cx + side * r * 0.92, cy), r * 0.95);
            sideShade.setColorAt(0.00, withA(shade, 70));
            sideShade.setColorAt(0.45, withA(shade, 26));
            sideShade.setColorAt(1.00, withA(shade, 0));
            p.fillRect(area, sideShade);
        }
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }

    // ---- 浮き出た雫：円用の放射グラデーションが使えないので縁に沿ったストロークで ----
    if (drawDropRaised) {
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        p.strokePath(path, dropPen(QColor(255, 255, 255, 4), 8.0));
        p.strokePath(path, dropPen(QColor(255, 255, 255, 9), 4.0));
        p.strokePath(path, dropPen(QColor(255, 255, 255, 14), 1.8));
        p.strokePath(path.translated(0.0, 1.4), dropPen(QColor(255, 255, 255, 22), 2.6));
        p.strokePath(path.translated(0.0, -1.3), dropPen(QColor(255, 255, 255, 14), 2.2));
        p.setCompositionMode(QPainter::CompositionMode_Multiply);
        {
            const double s = std::min(0.45, (bb.height() * 0.5) / std::max(1.0, bb.width()));
            const QColor sc(shade.red(), shade.green(), shade.blue(), 70);
            QLinearGradient side(QPointF(bb.left(), 0.0), QPointF(bb.right(), 0.0));
            side.setColorAt(0.0, alphaScaled(sc, disc ? 0.0 : raised));
            side.setColorAt(s, alphaScaled(sc, 0.0));
            side.setColorAt(1.0 - s, alphaScaled(sc, 0.0));
            // 右端の翳りは「右端が雫側にあるとき」だけ。閉じる後半は雫が
            // 吸い込まれて形の右端が＋ボタンの縁になるので、ここで翳りを
            // 乗せるとボタンの右側だけ二重に暗くなる（ボタンの円は上で
            // 放射グラデーションの翳りを描き済み）。
            const double rightW = disc
                ? clamp01((bb.right() - gx0) / std::max(1.0, gx1 - gx0)) * raised
                : raised;
            side.setColorAt(1.0, alphaScaled(sc, rightW));
            p.strokePath(path, QPen(QBrush(side), 7.0));
        }
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }

    // ---- 沈み込んだ面：上の内側に影、下の内側に弱い光 ----
    if (drawSunk) {
        p.strokePath(path.translated(0.0, 2.3), sunkPen(QColor(0, 0, 0, 70), 4.6));
        p.strokePath(path.translated(0.0, 1.0), sunkPen(QColor(0, 0, 0, 36), 1.8));
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        p.strokePath(path.translated(0.0, -1.1), sunkPen(QColor(255, 255, 255, 16), 1.5));
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    p.setClipping(false);

    // ---- 沈み込んだ穴の下側の外に細い光（リップ）----
    if (drawSunk) {
        QPainterPath outside;
        outside.setFillRule(Qt::OddEvenFill);
        outside.addRect(bb.adjusted(-6.0, -6.0, 6.0, 6.0));
        outside.addPath(path);
        p.setClipPath(outside);
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        p.strokePath(path.translated(0.0, 0.9), sunkPen(QColor(255, 255, 255, 26), 1.2));
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setClipping(false);
    }

    // ---- (5) 輪郭線（乗算）と (6) 縁を走る細い光（加算）----
    // 縦方向のグラデーションなので横の重みは掛けられない。ボタン側と雫側で
    // 描き分け、境目（グラデーション区間の中央）でクリップを切り替える。
    // ボタン側は AnimatedButton と同じく、円の上端〜下端でグラデーションを取る。
    const auto strokeEdges = [&](double sk, double yTop, double yBottom) {
        const double rs = 1.0 - sk;
        const QColor base = mixColor(shade, QColor(20, 20, 24), sk);
        p.setCompositionMode(QPainter::CompositionMode_Multiply);
        QLinearGradient contour(QPointF(0.0, yTop), QPointF(0.0, yBottom));
        contour.setColorAt(0.0, withA(base, qRound(120 * rs + 150 * sk)));
        contour.setColorAt(0.5, withA(base, qRound(230 * rs + 90 * sk)));
        contour.setColorAt(1.0, withA(base, qRound(190 * rs + 40 * sk)));
        p.strokePath(path, QPen(QBrush(contour), 0.9));

        p.setCompositionMode(QPainter::CompositionMode_Plus);
        QLinearGradient edge(QPointF(0.0, yTop), QPointF(0.0, yBottom));
        edge.setColorAt(0.00, QColor(255, 255, 255, qRound(30 * rs + 6 * sk)));
        edge.setColorAt(0.32, QColor(255, 255, 255, qRound(11 * rs + 4 * sk)));
        edge.setColorAt(0.68, QColor(255, 255, 255, qRound(10 * rs + 8 * sk)));
        edge.setColorAt(1.00, QColor(255, 255, 255, qRound(24 * rs + 22 * sk)));
        p.strokePath(path, QPen(QBrush(edge), 0.9));
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    };
    if (disc) {
        const double midX = (gx0 + gx1) * 0.5;
        const QRectF area = bb.adjusted(-4.0, -4.0, 4.0, 4.0);
        p.setClipRect(QRectF(area.left(), area.top(), midX - area.left(), area.height()));
        strokeEdges(0.0, disc->center.y() - disc->radius, disc->center.y() + disc->radius);
        p.setClipRect(QRectF(midX, area.top(), area.right() - midX, area.height()));
        strokeEdges(sink, bb.top(), bb.bottom());
        p.setClipping(false);
    } else {
        strokeEdges(sink, bb.top(), bb.bottom());
    }

    p.restore();
}

double TabMetrics::left(int index) const
{
    double x = 0.0;
    for (int i = 0; i < index && i < widths.size(); ++i)
        x += widths[i];
    return x;
}

TabMetrics measureTabs(const QVector<TabItem> &tabs, double capsuleWidth)
{
    TabMetrics m;
    const int n = tabs.size();
    // 左右の丸み分は文字を置かない（カプセルの端に文字が食い込まないように）。
    m.available = qMax(0.0, capsuleWidth - 6.0);
    if (n == 0)
        return m;

    // 2026-09-18: QFont / QFontMetricsF を static で持たない。
    // アプリ終了時に QGuiApplication より後に破棄され、フォントエンジンの
    // 解放とぶつかることがあるため。5枚ぶんの幅を測るだけなので毎回作る。
    QFont font;
    font.setPixelSize(static_cast<int>(kTabFontPx));
    const QFontMetricsF fm(font);

    const double iconW = kTabIconPx;
    m.widths.resize(n);

    // 1. アイコン＋文字で必要な幅
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        double w = kTabTextPad * 2.0 + fm.horizontalAdvance(tabs[i].title);
        if (!tabs[i].iconPath.isEmpty())
            w += iconW + kTabIconGap;
        m.widths[i] = w;
        sum += w;
    }

    if (sum <= m.available) {
        // 収まる: 余りを按分して広げる（従来の等分割と同じ見た目）
        const double scale = m.available > 0.0 ? m.available / sum : 1.0;
        for (double &w : m.widths)
            w *= scale;
        m.total = m.available;
        return m;
    }

    // 2. 全タブ一括でアイコンのみへ
    m.iconOnly = true;
    sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double w = tabs[i].iconPath.isEmpty()
                             ? kTabIconPad * 2.0 + qMin(fm.horizontalAdvance(tabs[i].title), 40.0)
                             : kTabIconPad * 2.0 + iconW;
        m.widths[i] = w;
        sum += w;
    }
    if (sum <= m.available) {
        const double scale = m.available > 0.0 ? m.available / sum : 1.0;
        for (double &w : m.widths)
            w *= scale;
        m.total = m.available;
        return m;
    }

    // 3. それでも入らない: 横スクロール
    m.total = sum;
    m.scrollable = true;
    return m;
}

int tabIndexAt(const TabMetrics &m, double scrollX, double xInCapsule)
{
    double x = -scrollX + 3.0;   // 3.0 = available の左右マージンの片側
    for (int i = 0; i < m.widths.size(); ++i) {
        if (xInCapsule >= x && xInCapsule < x + m.widths[i])
            return i;
        x += m.widths[i];
    }
    return -1;
}

void paintTabContents(QPainter &p, const QRectF &capsule, const QVector<TabItem> &tabs,
                      const TabMetrics &m, double scrollX,
                      int current, int hover, double alpha)
{
    const int n = tabs.size();
    if (n == 0 || n != m.widths.size() || alpha <= 0.001 || capsule.width() < 24.0)
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setClipPath(capsulePath(capsule));
    p.setOpacity(p.opacity() * clamp01(alpha));

    const double top = capsule.top(), h = capsule.height();
    const double x0 = capsule.left() + 3.0 - scrollX;

    // タブ i の矩形（カプセル全体の座標）
    const auto segRect = [&](int i) {
        return QRectF(x0 + m.left(i), top, m.widths[i], h);
    };
    const auto segPill = [&](int i) {
        const QRectF r = segRect(i);
        return r.adjusted(2.0, 3.0, -2.0, -3.0);
    };

    p.setPen(Qt::NoPen);
    if (current >= 0 && current < n) {
        const QRectF r = segPill(current);
        p.setBrush(QColor(255, 255, 255, 41));
        p.drawRoundedRect(r, r.height() / 2.0, r.height() / 2.0);
    }
    if (hover >= 0 && hover < n && hover != current) {
        const QRectF r = segPill(hover);
        p.setBrush(QColor(255, 255, 255, 18));
        p.drawRoundedRect(r, r.height() / 2.0, r.height() / 2.0);
    }

    // 区切り線（選択中・ホバー中の塗りと接する側は描かない）
    p.setPen(QPen(QColor(255, 255, 255, 56), 1.0));
    for (int i = 1; i < n; ++i) {
        if (i == current || i - 1 == current || i == hover || i - 1 == hover)
            continue;
        const double x = std::round(segRect(i).left()) + 0.5;
        if (x < capsule.left() + 2.0 || x > capsule.right() - 2.0)
            continue;   // 画面外（スクロールで隠れている側）は描かない
        p.drawLine(QPointF(x, top + 7.0), QPointF(x, top + h - 7.0));
    }

    QFont f = p.font();
    f.setPixelSize(static_cast<int>(kTabFontPx));
    p.setFont(f);
    const QFontMetricsF fm(f);
    for (int i = 0; i < n; ++i) {
        const QRectF seg = segRect(i);
        // スクロールで完全に外れているタブは触らない（描画コストを出さない）
        if (seg.right() < capsule.left() - 1.0 || seg.left() > capsule.right() + 1.0)
            continue;

        const bool sel = (i == current);
        const QColor color = sel ? QColor(255, 255, 255, 242) : QColor(255, 255, 255, 153);
        const bool hasIcon = !tabs[i].iconPath.isEmpty();

        if (m.iconOnly || tabs[i].title.isEmpty()) {
            if (hasIcon) {
                const QRectF box(seg.center().x() - kTabIconPx / 2.0,
                                 seg.center().y() - kTabIconPx / 2.0,
                                 kTabIconPx, kTabIconPx);
                PbSvgIcon::draw(p, box, tabs[i].iconPath, color);
            } else {
                p.setPen(color);
                p.drawText(seg, Qt::AlignCenter | Qt::TextSingleLine,
                           fm.elidedText(tabs[i].title, Qt::ElideRight,
                                         qMax(0.0, seg.width() - 6.0)));
            }
            continue;
        }

        const double iconW = hasIcon ? kTabIconPx + kTabIconGap : 0.0;
        const double textW = qMax(0.0, seg.width() - kTabTextPad * 2.0 - iconW);
        const QString text = fm.elidedText(tabs[i].title, Qt::ElideRight, textW);
        const double contentW = iconW + fm.horizontalAdvance(text);
        double cx = seg.left() + (seg.width() - contentW) / 2.0;
        if (hasIcon) {
            PbSvgIcon::draw(p, QRectF(cx, seg.center().y() - kTabIconPx / 2.0,
                                      kTabIconPx, kTabIconPx),
                            tabs[i].iconPath, color);
            cx += iconW;
        }
        p.setPen(color);
        p.drawText(QRectF(cx, top, seg.right() - cx, h),
                   Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text);
    }
    p.restore();
}

BackdropGlassPlateStyle sunkCapsuleGlass(const QRectF &capsule)
{
    BackdropGlassPlateStyle glass;
    if (capsule.width() <= 0.5 || capsule.height() <= 0.5)
        return glass;
    glass.enabled = true;
    glass.shape = BackdropGlassPlateStyle::Shape::Goo;
    glass.body = buttonTint();
    glass.shade = buttonShade();
    const double L = capsule.left();
    auto &p = glass.params;
    // 並びは IslandTabGooLayer::syncGlass() と同じ（Blur.cpp renderGlassShape 参照）。
    p[0]  = static_cast<float>(L);                       // L
    p[1]  = static_cast<float>(capsule.right());         // R
    p[2]  = static_cast<float>(capsule.height() * 0.5);  // hh
    p[3]  = 1.0f;                                        // taper（普通のカプセル）
    // ＋ボタンの円は無し：遠くに半径0で置く（距離場にも艶の範囲にも効かない）
    p[4]  = static_cast<float>(L - 1000.0);              // cx
    p[5]  = static_cast<float>(capsule.center().y());    // cy
    p[6]  = 0.0f;                                        // r
    p[7]  = 11.5f;                                       // k
    p[8]  = 1.0f;                                        // sink（沈み込んだ面）
    // 横方向の重みの区間を形より遥か左へ＝全体が「雫側」（paintSurface の disc=nullptr と同じ）
    p[9]  = static_cast<float>(L - 2000.0);              // gx0
    p[10] = static_cast<float>(L - 1999.0);              // gx1
    p[11] = 0.0f;                                        // shadowBase（落ち影なし）
    p[12] = static_cast<float>(capsule.left());
    p[13] = static_cast<float>(capsule.top());
    p[14] = static_cast<float>(capsule.right());
    p[15] = static_cast<float>(capsule.bottom());
    p[16] = 0.0f;                                        // sideS（浮き出た面用。未使用）
    p[17] = 0.0f;                                        // sideRightW
    return glass;
}

} // namespace IslandTabGoo

// ============================================================
//  IslandTabGooLayer
// ============================================================
IslandTabGooLayer::IslandTabGooLayer(AnimatedButton *button, int baseHeight, QWidget *parent)
    : QWidget(parent), m_button(button), m_baseH(baseHeight)
{
    if (parent && islandGlassOnGpu())
        m_glass.attach(parent);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    hide();

    // 値＝経過ms（線形）。形は毎フレーム時刻から直接計算する。
    m_anim = new QVariantAnimation(this);
    m_anim->setEasingCurve(QEasingCurve::Linear);
    m_anim->setStartValue(0.0);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        onTick(v.toDouble());
    });
    connect(m_anim, &QVariantAnimation::finished, this, [this] { finish(); });
}

void IslandTabGooLayer::setTabInsets(int left, int rightSpace)
{
    m_tabLeft = left;
    m_tabRightSpace = rightSpace;
}

IslandTabGoo::Layout IslandTabGooLayer::currentLayout() const
{
    IslandTabGoo::Layout lay;
    const QWidget *island = parentWidget();
    const int islandW = island ? island->width() : width();
    if (m_button) {
        const QRectF g = m_button->geometry();
        lay.buttonCx = g.center().x();
        lay.buttonCy = g.center().y();
        lay.buttonR = m_button->bodyRadius();
        lay.buttonShadowBase = 12.0 * m_button->visualScale();
    }
    lay.tabLeft = m_tabLeft;
    lay.tabRight = std::max<double>(m_tabLeft, islandW - m_tabRightSpace);
    return lay;
}

IslandTabGoo::Frame IslandTabGooLayer::currentFrame() const
{
    const IslandTabGoo::Layout lay = currentLayout();
    if (m_mode == Mode::Open)
        return IslandTabGoo::evalOpen(m_tau, lay, m_pinchMs);
    return IslandTabGoo::evalClose(m_tau, lay);
}

void IslandTabGooLayer::startOpen() { start(Mode::Open); }
void IslandTabGooLayer::startClose() { start(Mode::Close); }

void IslandTabGooLayer::start(Mode mode)
{
    m_anim->stop();
    m_mode = mode;
    m_tau = 0.0;
    if (mode == Mode::Open)
        m_pinchMs = IslandTabGoo::openPinchMs(currentLayout());
    m_anim->setDuration(static_cast<int>(mode == Mode::Open ? IslandTabGoo::kOpenMs
                                                            : IslandTabGoo::kCloseMs));
    m_anim->setEndValue(mode == Mode::Open ? IslandTabGoo::kOpenMs : IslandTabGoo::kCloseMs);
    if (m_button)
        m_button->setBodyHidden(true);
    show();
    raise();
    if (m_button)
        stackUnder(m_button);
    onTick(0.0);
    m_anim->start();
}

void IslandTabGooLayer::stop()
{
    if (m_mode == Mode::Idle)
        return;
    m_anim->stop();
    m_mode = Mode::Idle;
    if (m_button) {
        m_button->setIconOffset(0.0);
        m_button->setBodyHidden(false);
    }
    hide();
    syncGlass();
}

void IslandTabGooLayer::syncGlass()
{
    if (!islandGlassOnGpu())
        return;
    const QWidget *island = parentWidget();
    BackdropBlur::GlassPlateStyle glass;
    qreal opacity = 0.0;
    if (island && m_mode != Mode::Idle) {
        const IslandTabGoo::Layout lay = currentLayout();
        const IslandTabGoo::Frame fr = currentFrame();
        const double cx = lay.buttonCx + fr.bx;
        const double cy = lay.buttonCy;
        const double r  = lay.buttonR * fr.rScale;
        double gx0 = 0.0, gx1 = 0.0;
        IslandTabGoo::gradientSpan(fr, lay, gx0, gx1);
        gx1 = std::max(gx0 + 1.0, gx1);   // paintSurface() と同じ補正

        // 形の外接矩形（buildPath のパスの boundingRect 相当）。
        const bool hasDrop = fr.hh > 0.01 && fr.R - fr.L > 0.01;
        const double bbL = hasDrop ? std::min(cx - r, fr.L) : cx - r;
        const double bbR = hasDrop ? std::max(cx + r, fr.R) : cx + r;
        const double bbT = cy - (hasDrop ? std::max(r, fr.hh) : r);
        const double bbB = cy + (hasDrop ? std::max(r, fr.hh) : r);
        const double raised = 1.0 - std::clamp(fr.sink, 0.0, 1.0);
        const double sideS = std::min(0.45, ((bbB - bbT) * 0.5) / std::max(1.0, bbR - bbL));
        const double rightW = std::clamp((bbR - gx0) / std::max(1.0, gx1 - gx0), 0.0, 1.0)
                              * raised;

        glass.enabled = true;
        glass.shape = BackdropBlur::GlassPlateStyle::Shape::Goo;
        // 色は paintSurface() の buttonTint() / buttonShade() と同じ（＋ボタンの色）
        {
            const QColor hsv = QColor(QStringLiteral("#4A90E2")).toHsv();
            QColor tint = QColor::fromHsv(qMax(0, hsv.hue()),
                                          qBound(0, static_cast<int>(hsv.saturation() * 1.04), 255),
                                          qBound(0, static_cast<int>(hsv.value() * 1.20 + 10), 255));
            tint.setAlpha(192);
            glass.body = tint;
            glass.shade = QColor::fromHsv(qMax(0, hsv.hue()),
                                          qMin(255, static_cast<int>(hsv.saturation() * 1.10)),
                                          static_cast<int>(hsv.value() * 0.52));
        }
        auto &p = glass.params;
        p[0]  = static_cast<float>(fr.L);
        p[1]  = static_cast<float>(fr.R);
        p[2]  = static_cast<float>(fr.hh);
        p[3]  = static_cast<float>(fr.taper);
        p[4]  = static_cast<float>(cx);
        p[5]  = static_cast<float>(cy);
        p[6]  = static_cast<float>(r);
        p[7]  = 11.5f;   // Params::k
        p[8]  = static_cast<float>(fr.sink);
        p[9]  = static_cast<float>(gx0);
        p[10] = static_cast<float>(gx1);
        p[11] = static_cast<float>(lay.buttonShadowBase);
        p[12] = static_cast<float>(bbL);
        p[13] = static_cast<float>(bbT);
        p[14] = static_cast<float>(bbR);
        p[15] = static_cast<float>(bbB);
        p[16] = static_cast<float>(sideS);
        p[17] = static_cast<float>(rightW);

        const auto *fx = qobject_cast<QGraphicsOpacityEffect *>(island->graphicsEffect());
        opacity = fx ? fx->opacity() : 1.0;
    }
    m_glass.setGlassStyle(glass);
    // レイヤーの矩形（島の上端の帯）。paintEvent と同じ範囲へ描く。
    const QRectF area(0.0, 0.0, island ? island->width() : width(), m_baseH);
    m_glass.setShape(glass.enabled ? area : QRectF(), 0.0, 0.0, opacity);
}

void IslandTabGooLayer::onTick(double tauMs)
{
    if (m_mode == Mode::Idle)
        return;
    m_tau = tauMs;
    const QWidget *island = parentWidget();
    const QSize want(island ? island->width() : width(), m_baseH);
    if (size() != want)
        setGeometry(0, 0, want.width(), want.height());
    if (m_button)
        m_button->setIconOffset(currentFrame().bx);
    update();
    syncGlass();
}

void IslandTabGooLayer::finish()
{
    if (m_mode == Mode::Idle)
        return;
    const Mode finished = m_mode;
    stop();
    if (onFinished)
        onFinished(finished);
}

void IslandTabGooLayer::paintEvent(QPaintEvent *)
{
    if (m_mode == Mode::Idle)
        return;
    const IslandTabGoo::Layout lay = currentLayout();
    const IslandTabGoo::Frame fr = currentFrame();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (islandGlassOnGpu()) {
        // 2026-09-16: 形と面はGL側（syncGlass）。距離場の計算・marching squares・
        // 約20本のストロークがアニメ中に毎フレーム走っていたのが無くなる。
        syncGlass();
    } else {
        const QPainterPath path = IslandTabGoo::buildPath(fr, lay);
        IslandTabGoo::ButtonDisc disc;
        disc.center = QPointF(lay.buttonCx + fr.bx, lay.buttonCy);
        disc.radius = lay.buttonR * fr.rScale;
        disc.shadowBase = lay.buttonShadowBase;
        double gx0 = 0.0, gx1 = 0.0;
        IslandTabGoo::gradientSpan(fr, lay, gx0, gx1);
        IslandTabGoo::paintSurface(p, path, fr.sink, gx0, gx1, &disc);
    }

    if (fr.text > 0.001 && tabContentsProvider) {
        QVector<IslandTabGoo::TabItem> tabs;
        int current = 0;
        tabContentsProvider(tabs, current);
        const QRectF capsule(fr.L, lay.buttonCy - fr.hh, fr.R - fr.L, fr.hh * 2.0);
        // 2026-09-18: 分裂／吸い込みの途中はタブが狭いので、その幅で測り直す
        // （狭ければ自動でアイコンのみになり、開き切ると文字が出る）。
        // スクロールは常に0: 途中の幅に合わせて動かすと、静止用のタブへ
        // 引き継ぐ瞬間に位置が飛ぶため。
        const IslandTabGoo::TabMetrics m = IslandTabGoo::measureTabs(tabs, capsule.width());
        IslandTabGoo::paintTabContents(p, capsule, tabs, m, 0.0, current, -1, fr.text);
    }
}
