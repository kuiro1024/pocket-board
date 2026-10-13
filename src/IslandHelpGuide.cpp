#include "IslandHelpGuide.h"
#include "AppLanguage.h"
#include "PbSvgIcon.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>

#include <cmath>

using IslandHelpGuide::Press;

namespace {

// ---- 配色 ----
const QColor kAccent(0x3a, 0xa0, 0xe8);      // 押している場所・入力の記号
const QColor kLine(255, 255, 255, 235);      // マウスの輪郭
const QColor kResult(255, 255, 255, 200);    // 結果アイコン
const QColor kText(255, 255, 255, 230);
const QColor kSubText(255, 255, 255, 160);
const QColor kRule(255, 255, 255, 34);
const QColor kWire(255, 255, 255, 76);       // 引き出し線
const QColor kBullet(255, 255, 255, 96);

// ---- 寸法（論理px）----
constexpr int kMouseW    = 46;   // マウスの図の外枠（放射の分だけ本体より広い）
constexpr int kMouseH    = 50;
constexpr int kModPx     = 17;   // 補足記号
constexpr int kResultPx  = 19;   // 結果アイコン
constexpr int kRowH      = 52;
constexpr int kCalloutH  = 56;
constexpr int kHeadH     = 28;
constexpr int kGroupGap  = 10;
constexpr int kModDx     = 52;   // labelX からの位置
constexpr int kKeysMaxW  = 182;
// 2026-09-18: アイコンは左基準（labelX から）、文字は右基準（controlRight へ）。
// ウィンドウが狭くなると右から見えなくなるのを避けるため。文字の列は
// いちばん長い項目の幅で取り、結果アイコンはその左に付く。
constexpr int kResultGap = 10;   // 結果アイコンと文字の間
constexpr int kTextMinW  = 60;

// ============================================================
//  マウスの図（QPainter で直接描く）
//
//  試作ページ（pocketboard_help_mock.html）の座標系をそのまま使う:
//  本体は 0..44 × 0..62 の中に描き、放射がはみ出すぶんを含めて
//  -16..60 × -16..66（= 76×82）を箱に収める。
// ============================================================
void drawMouse(QPainter &p, const QRectF &box, Press press, int clicks)
{
    const bool left  = (press == Press::Left  || press == Press::Both);
    const bool right = (press == Press::Right || press == Press::Both);
    const bool wheel = (press == Press::Wheel);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(box.left(), box.top());
    p.scale(box.width() / 76.0, box.width() / 76.0);
    p.translate(16.0, 16.0);   // viewBox の原点合わせ

    QPainterPath body;
    body.addRoundedRect(QRectF(6.5, 6.5, 31.0, 49.0), 15.5, 15.5);

    // 押している場所の塗り（本体の形でクリップして上半分を塗る）
    if (left || right || wheel) {
        p.save();
        p.setClipPath(body);
        p.setPen(Qt::NoPen);
        if (wheel) {
            // ホイールは面積が小さいので、うっすらハローを添えて目立たせる
            QColor halo = kAccent;
            halo.setAlphaF(0.30);
            p.setBrush(halo);
            p.drawEllipse(QPointF(22.0, 19.0), 11.0, 13.5);
        }
        p.setBrush(kAccent);
        if (left)  p.fillRect(QRectF(6.5, 6.5, 15.5, 24.5), kAccent);
        if (right) p.fillRect(QRectF(22.0, 6.5, 15.5, 24.5), kAccent);
        p.restore();
    }

    // 本体
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kLine, 2.8));
    p.drawPath(body);
    p.setPen(QPen(kLine, 2.4));
    p.drawLine(QPointF(7.0, 31.0), QPointF(37.0, 31.0));
    p.drawLine(QPointF(22.0, 7.0), QPointF(22.0, 31.0));

    // ホイール
    p.setPen(QPen(kLine, 2.2));
    p.setBrush(wheel ? QBrush(kAccent) : QBrush(QColor(0x1b, 0x1b, 0x1d)));
    p.drawRoundedRect(QRectF(18.3, 11.6, 7.4, 14.8), 3.7, 3.7);

    // ---- 放射（クリック）----
    //
    // 2026-09-18: 位置は「左斜め上」で統一する（左クリックも、左右どちらでも
    // 可のクリックも、ホイールのクリックも同じ場所）。押す場所は塗りの色で
    // 分かるので、ショックマークまで動かすと行ごとに見た目が散らかるため。
    if (clicks > 0) {
        QPointF spot(6.0, 5.0);
        double rot = -135.0;
        if (press == Press::Right) { spot = QPointF(38.0, 5.0); rot = -45.0; }   // 右専用のときだけ右上

        const double ax = std::cos(rot * M_PI / 180.0);
        const double ay = std::sin(rot * M_PI / 180.0);

        // dist = 押した点から放射の先端までの距離、perp = 軸に対する横ずれ
        const auto ray = [&](double dist, double perp, double opacity, double len, double hw) {
            p.save();
            p.setOpacity(p.opacity() * opacity);
            p.translate(spot.x() + ax * dist - ay * perp,
                        spot.y() + ay * dist + ax * perp);
            p.rotate(rot);
            QPainterPath t;
            t.moveTo(0.0, 0.0);
            t.lineTo(len, -hw);
            t.lineTo(len, hw);
            t.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(kAccent);
            p.drawPath(t);
            p.restore();
        };
        if (clicks == 1) {
            ray(3.0, 0.0, 1.0, 14.0, 5.6);
        } else if (clicks >= 3) {
            // 2026-09-20: トリプルクリック: 大1本の両脇に小2本。
            ray(3.0, 0.0, 1.0, 15.0, 6.2);
            ray(1.5, -13.0, 1.0, 8.0, 3.4);
            ray(1.5, 13.0, 1.0, 8.0, 3.4);
        } else {
            // ダブルクリック: 同じ向きの大小2本を、軸に対して横へずらして重ねる
            // （2回叩いた勢いが2つ飛んでいる形）。
            ray(3.0, 0.0, 1.0, 15.0, 6.2);
            ray(1.5, -13.0, 1.0, 8.0, 3.4);
        }
    }
    p.restore();
}

// バブル右下のリサイズマーク（マウスではないので別の絵）
void drawResizeGlyph(QPainter &p, const QRectF &box)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(box.left(), box.top());
    p.scale(box.width() / 76.0, box.width() / 76.0);
    p.translate(16.0, 16.0);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kLine, 2.4));
    p.drawRoundedRect(QRectF(7.0, 12.0, 30.0, 24.0), 5.0, 5.0);
    p.setPen(QPen(kAccent, 2.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(39.0, 36.0), QPointF(32.0, 36.0));
    p.drawLine(QPointF(39.0, 36.0), QPointF(39.0, 29.0));
    p.drawLine(QPointF(39.0, 36.0), QPointF(31.0, 28.0));
    p.restore();
}

// 影付きの文字（設定項目の ShadowLabel と同じ作り）
void drawShadowedText(QPainter &p, const QRect &box, int flags, const QString &text,
                      const QColor &color)
{
    p.setPen(QColor(0, 0, 0, 200));
    p.drawText(box.translated(1, 1), flags, text);
    p.setPen(color);
    p.drawText(box, flags, text);
}

// キーキャップ1個。描いた右端を返す。
int drawKeycap(QPainter &p, int x, int cy, const QString &label, const QFont &font)
{
    const QFontMetrics fm(font);
    const int w = fm.horizontalAdvance(label) + 10;
    const int h = 18;
    const QRectF r(x, cy - h / 2.0, w, h);
    p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    p.setBrush(QColor(255, 255, 255, 26));
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 5.0, 5.0);
    p.setFont(font);
    p.setPen(QColor(255, 255, 255, 235));
    p.drawText(r, Qt::AlignCenter, label);
    return x + w;
}

QString ic(const char *name) { return QStringLiteral(":/icons/") + QLatin1String(name); }

} // namespace

// ============================================================
//  データ
//
//  ここを足し引きするだけでヘルプの中身が変わる（描画側は触らない）。
// ============================================================
QVector<IslandHelpGuide::Entry> IslandHelpGuide::plusButtonEntries()
{
    return {
        { Press::Wheel, 0, ic("help_updown.svg"), ic("ico_Sound_3.svg"),
          AppLanguage::text("Scroll to change the volume", "ホイール上下で音量調整"), {}, {} },
        { Press::Left,  1, {},                    ic("res_gear.svg"),
          AppLanguage::text("Click to open settings", "左クリックで設定を開く"), {}, {} },
    };
}

QVector<IslandHelpGuide::Group> IslandHelpGuide::groups()
{
    using E = IslandHelpGuide::Entry;
    QVector<Group> gs;

    gs.append({ AppLanguage::text("Canvas", "キャンバス"), {
        E{ Press::Wheel, 0, ic("help_move.svg"),   ic("res_pan.svg"),
           AppLanguage::text("Drag with the wheel button to pan", "ホイールドラッグで移動"), {}, {} },
        E{ Press::Wheel, 0, ic("help_updown.svg"), ic("res_zoom.svg"),
           AppLanguage::text("Scroll to zoom in / out", "ホイール上下でズームイン / アウト"), {}, {} },
        E{ Press::Both,  1, {},                    ic("res_select.svg"),
           AppLanguage::text("Click either button to select", "左右どちらかのクリックで選択"), {}, {} },
        E{ Press::Left,  0, ic("help_drag.svg"),   ic("res_rubber.svg"),
           AppLanguage::text("Drag with the left button to select an area", "左ドラッグで範囲選択"), {}, {} },
        E{ Press::Wheel, 2, {},                    ic("res_arrange.svg"),
           AppLanguage::text("Double-click the wheel to arrange the selection", "範囲選択後にホイールダブルクリックで整列"), {}, {} },
        E{ Press::Wheel, 3, {},                    ic("res_sortname.svg"),
           AppLanguage::text("Triple-click the wheel to sort the selection by name", "範囲選択後にホイールトリプルクリックで名前順に整列"), {}, {} },
    }});

    gs.append({ AppLanguage::text("Bubble", "バブル"), {
        E{ Press::Both,  1, {},                    ic("res_select.svg"),
           AppLanguage::text("Click either button to select", "左右どちらかのクリックで選択"), {}, {} },
        E{ Press::Both,  2, {},                    ic("res_focus.svg"),
           AppLanguage::text("Double-click either button to focus", "左右どちらかのダブルクリックでフォーカス"), {}, {} },
        E{ Press::Left,  0, ic("help_drag.svg"),   ic("res_pan.svg"),
           AppLanguage::text("Drag with the left button to move", "左ドラッグで移動"), {}, {} },
        // 2026-09-20: 逆輸入（バブルをPB外のアプリへ書き出す）。Shift+ドラッグでも可。
        E{ Press::Left,  2, ic("help_drag.svg"),   ic("res_export.svg"),
           AppLanguage::text("Double-click, then drag to take it out of PocketBoard",
                             "左ダブルクリック後ドラッグでソフト外へ出す"), {}, {} },
        E{ Press::None,  0, {},                    ic("res_resize.svg"),
           AppLanguage::text("Drag the handle at the bottom right to resize", "右下のリサイズマークでリサイズ"),
           {}, QStringLiteral("resize") },
    }});

    const QVector<E> focusCommon = {
        E{ Press::Wheel, 0, ic("help_move.svg"),   ic("res_pan.svg"),
           AppLanguage::text("Drag with the wheel button to pan", "ホイールドラッグで移動"), {}, {} },
        E{ Press::Wheel, 0, ic("help_updown.svg"), ic("res_zoom.svg"),
           AppLanguage::text("Scroll to zoom in / out", "ホイール上下でズームイン / アウト"), {}, {} },
        E{ Press::Wheel, 2, {},                    ic("res_zoomreset.svg"),
           AppLanguage::text("Double-click the wheel button to reset the zoom", "ホイールダブルクリックでズームリセット"), {}, {} },
        E{ Press::Both,  2, {},                    ic("res_unfocus.svg"),
           AppLanguage::text("Double-click either button to exit", "左右どちらかのダブルクリックで解除"), {}, {} },
    };

    gs.append({ AppLanguage::text("Image focus", "画像フォーカス"), focusCommon });

    QVector<E> video = focusCommon;
    video.append(E{ Press::Left, 1, {},                  ic("res_playpause.svg"),
                    AppLanguage::text("Click to play / pause", "左クリックで再生 / 停止"), {}, {} });
    video.append(E{ Press::Left, 1, ic("help_hold.svg"), ic("res_speed.svg"),
                    AppLanguage::text("Hold the left button for 2x speed", "左クリックホールドで2倍速"), {}, {} });
    gs.append({ AppLanguage::text("Video focus", "動画フォーカス"), video });

    // ここだけ文字（キーボード）
    gs.append({ AppLanguage::text("Keyboard", "その他基本操作"), {
        E{ Press::None, 0, {}, ic("res_trash.svg"), AppLanguage::text("Delete", "削除"),
           { QStringLiteral("Ctrl+X"), QStringLiteral("Delete") }, {} },
        E{ Press::None, 0, {}, ic("res_undo.svg"),  AppLanguage::text("Undo", "巻き戻し"),
           { QStringLiteral("Ctrl+Z") }, {} },
        E{ Press::None, 0, {}, ic("res_redo.svg"),  AppLanguage::text("Redo", "先送り"),
           { QStringLiteral("Ctrl+Shift+Z"), QStringLiteral("Ctrl+Y") }, {} },
    }});

    return gs;
}

// ============================================================
//  IslandHelpGuideWidget
// ============================================================
IslandHelpGuideWidget::IslandHelpGuideWidget(QWidget *parent)
    : QWidget(parent)
    , m_plus(IslandHelpGuide::plusButtonEntries())
    , m_groups(IslandHelpGuide::groups())
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    updateHeight();
}

int IslandHelpGuideWidget::contentHeight() const
{
    int h = 6 + m_plus.size() * kCalloutH + kGroupGap * 2;
    for (const IslandHelpGuide::Group &g : m_groups)
        h += kHeadH + g.entries.size() * kRowH + kGroupGap;
    return h + 6;
}

void IslandHelpGuideWidget::updateHeight()
{
    setFixedHeight(contentHeight());
}

QSize IslandHelpGuideWidget::sizeHint() const
{
    return QSize(m_controlRight, contentHeight());
}

// 文字の列の幅 = いちばん長い項目の幅。
// 2026-09-18: leftEnd（その行で左側に描いたものの右端）を渡して、ぶつかりそうなら
// そのぶん幅を詰める（右基準のまま左へは伸びない＝アイコンにめり込まない）。
// キー表示の行はマウスの図より横に長いので、この行だけ文字が右へ寄る。
int IslandHelpGuideWidget::textColumnWidth(const QFontMetrics &fm, int leftEnd) const
{
    if (m_textColumnW <= 0) {
        int w = 0;
        for (const IslandHelpGuide::Entry &e : m_plus)
            w = qMax(w, fm.horizontalAdvance(e.text));
        for (const IslandHelpGuide::Group &g : m_groups)
            for (const IslandHelpGuide::Entry &e : g.entries)
                w = qMax(w, fm.horizontalAdvance(e.text));
        m_textColumnW = w + 2;
    }
    const int reserved = leftEnd + 14 + kResultPx + kResultGap;
    return qMax(kTextMinW, qMin(m_textColumnW, m_controlRight - reserved));
}

void IslandHelpGuideWidget::setGuides(int markerX, int labelX, int controlRight)
{
    if (m_markerX == markerX && m_labelX == labelX && m_controlRight == controlRight)
        return;
    m_markerX = markerX;
    m_labelX = labelX;
    m_controlRight = controlRight;
    update();
}

int IslandHelpGuideWidget::paintEntry(QPainter &p, const IslandHelpGuide::Entry &e, int y, int h)
{
    const bool isKeyRow = !e.keys.isEmpty();
    const int cy = y + h / 2;

    QFont base = p.font();
    base.setPixelSize(12);

    int leftEnd = m_labelX;
    if (isKeyRow) {
        // ---- キーボードの行 ----
        QFont kf = base;
        kf.setPixelSize(10);
        int x = m_labelX;
        for (int i = 0; i < e.keys.size(); ++i) {
            if (i > 0) {
                p.setFont(kf);
                p.setPen(kSubText);
                p.drawText(QRect(x, cy - 9, 12, 18), Qt::AlignCenter, QStringLiteral("/"));
                x += 12;
            }
            const QStringList parts = e.keys[i].split(QLatin1Char('+'));
            for (int k = 0; k < parts.size(); ++k) {
                if (k > 0) {
                    p.setFont(kf);
                    p.setPen(kSubText);
                    p.drawText(QRect(x, cy - 9, 10, 18), Qt::AlignCenter, QStringLiteral("+"));
                    x += 10;
                }
                x = drawKeycap(p, x, cy, parts[k], kf);
            }
            if (x > m_labelX + kKeysMaxW)
                break;   // 収まらないぶんは出さない（行が右へ突き抜けないように）
        }
        leftEnd = x;
    } else if (e.customGlyph == QLatin1String("resize")) {
        drawResizeGlyph(p, QRectF(m_labelX, cy - kMouseH / 2.0, kMouseW, kMouseH));
        leftEnd = m_labelX + kMouseW;
    } else {
        drawMouse(p, QRectF(m_labelX, cy - kMouseH / 2.0, kMouseW, kMouseH), e.press, e.clicks);
        leftEnd = m_labelX + kMouseW;
    }

    if (!e.modIcon.isEmpty()) {
        PbSvgIcon::draw(p, QRectF(m_labelX + kModDx, cy - kModPx / 2.0, kModPx, kModPx),
                        e.modIcon);
        leftEnd = qMax(leftEnd, m_labelX + kModDx + kModPx);
    }
    // ---- 右基準の列（結果アイコン＋文字）----
    p.setFont(base);
    const QFontMetrics fm(base);
    const int textW = textColumnWidth(fm, leftEnd);
    const int textX = m_controlRight - textW;
    if (!e.resultIcon.isEmpty()) {
        PbSvgIcon::draw(p, QRectF(textX - kResultGap - kResultPx, cy - kResultPx / 2.0,
                                  kResultPx, kResultPx),
                        e.resultIcon, kResult);
    }
    if (textW > 24) {
        drawShadowedText(p, QRect(textX, y, textW, h),
                         Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                         fm.elidedText(e.text, Qt::ElideRight, textW), kText);
    }
    return y + h;
}

void IslandHelpGuideWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    int y = 6;

    // ---- 特例: ＋ボタンからの引き出し線 ----
    // 縦線は＋ボタンの中心（markerX）を通る。＋ボタン自体はこの
    // ウィジェットの上（島の上端）にあるので、線は上端から始める。
    if (!m_plus.isEmpty()) {
        const int top = y;
        const int firstCy = y + kCalloutH / 2;
        const int lastCy  = y + kCalloutH * (m_plus.size() - 1) + kCalloutH / 2;
        const double lx = m_markerX + 0.5;
        p.setPen(QPen(kWire, 1.2));
        p.drawLine(QPointF(lx, 0.0), QPointF(lx, lastCy));
        for (int i = 0; i < m_plus.size(); ++i) {
            const int cyi = top + kCalloutH * i + kCalloutH / 2;
            p.drawLine(QPointF(lx, cyi), QPointF(m_labelX - 8.0, cyi));
        }
        Q_UNUSED(firstCy);
        for (const IslandHelpGuide::Entry &e : m_plus)
            y = paintEntry(p, e, y, kCalloutH);
        y += kGroupGap * 2;
    }

    // ---- ・で区切ったグループ ----
    QFont headFont = p.font();
    headFont.setPixelSize(11);

    for (const IslandHelpGuide::Group &g : m_groups) {
        // 見出し: ● は＋ボタンの中心を通る
        const int hcy = y + kHeadH / 2;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 130));
        p.drawEllipse(QPointF(m_markerX, hcy), 2.5, 2.5);
        p.setFont(headFont);
        const QFontMetrics hfm(headFont);
        p.setPen(kSubText);
        p.drawText(QRect(m_labelX, y, qMax(0, m_controlRight - m_labelX), kHeadH),
                   Qt::AlignLeft | Qt::AlignVCenter, g.title);
        {
            const int rx = m_labelX + hfm.horizontalAdvance(g.title) + 10;
            if (rx < m_controlRight) {
                p.setPen(QPen(kRule, 1.0));
                p.drawLine(QPointF(rx, hcy + 0.5), QPointF(m_controlRight, hcy + 0.5));
            }
        }
        y += kHeadH;

        for (const IslandHelpGuide::Entry &e : g.entries) {
            // 各項目の ・ は＋ボタンの中心とタブの左端のちょうど中間
            const int bx = (m_markerX + m_labelX) / 2;
            p.setPen(Qt::NoPen);
            p.setBrush(kBullet);
            p.drawEllipse(QPointF(bx, y + kRowH / 2.0), 2.5, 2.5);
            y = paintEntry(p, e, y, kRowH);
        }
        y += kGroupGap;
    }
}
