#include "IslandPromptPanel.h"

#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLinearGradient>
#include <QRadialGradient>

namespace {

// パネル左右の余白。島のピル形状の角丸（r=21）へ文字が食い込まないよう、
// 展開部の左右にはそれなりの余白を取る。
constexpr int kSidePad      = 18;
// 島の丸UI（ボタン列）の直下に空ける隙間。
constexpr int kTopGap       = 6;
// 文言ブロックとボタン行の間。
constexpr int kButtonGap    = 14;
constexpr int kButtonHeight = 40;
constexpr int kBottomPad    = 14;
// ボタン2つの間隔。
constexpr int kButtonSpace  = 12;

// ---- ShadowLabel ----
// IslandSettingsPanel.cpp と同じ理由（QGraphicsDropShadowEffect を島の
// 子ウィジェットへ載せると描画が出てこなくなる）で、影は paintEvent で
// 自前に描く。あちらは1行専用だが、こちらは複数行の本文も扱うので
// Qt::TextWordWrap を通す点だけが違う。
class ShadowLabel : public QLabel
{
public:
    explicit ShadowLabel(QWidget *parent = nullptr) : QLabel(parent)
    {
        setWordWrap(true);
        setTextInteractionFlags(Qt::NoTextInteraction);
    }

    void setTextColor(const QColor &c) { m_color = c; update(); }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        const QString t = text();
        if (t.isEmpty()) {
            QLabel::paintEvent(event);
            return;
        }
        QPainter p(this);
        p.setFont(font());
        const QRectF box = contentsRect();
        const int flags = static_cast<int>(alignment()) | Qt::TextWordWrap;
        p.setPen(QColor(0, 0, 0, 200));
        p.drawText(box.translated(1.0, 1.0), flags, t);
        p.drawText(box.translated(0.0, 1.0), flags, t);
        p.setPen(m_color);
        p.drawText(box, flags, t);
    }

private:
    QColor m_color {255, 255, 255};
};

// ---- PromptPillButton ----
// YES/NO用のピル型ボタン。見た目は AnimatedButton::paintEvent() の
// ガラス表現（フロスト＋ティント→リムグロー→左右の翳り→輪郭線）を
// 円から角丸矩形へ置き換えたもの。あちらと数字を揃えてあるので、
// 島のボタンと同じ素材に見える。
//
// ヒットテスト・ホバー・enabled はすべて QPushButton 任せ
// （paintEvent へ直接描き込まない一番の理由）。
class PromptPillButton : public QPushButton
{
public:
    PromptPillButton(const QString &color, QWidget *parent = nullptr)
        : QPushButton(parent), m_color(color)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setFlat(true);
        QFont f = font();
        f.setPixelSize(13);
        f.setBold(true);
        setFont(f);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal r = body.height() / 2.0;

        // 無効時は彩度と明度を落として「押せない」ことを見た目で示す。
        QColor baseColor(m_color);
        if (!isEnabled()) {
            const QColor hsv = baseColor.toHsv();
            baseColor = QColor::fromHsv(qMax(0, hsv.hue()),
                                        static_cast<int>(hsv.saturation() * 0.22),
                                        static_cast<int>(hsv.value() * 0.55));
        }

        const auto withAlpha = [](QColor c, int a) { c.setAlpha(a); return c; };

        const QColor shadeColor = [&] {
            const QColor hsv = baseColor.toHsv();
            return QColor::fromHsv(qMax(0, hsv.hue()),
                                   qMin(255, static_cast<int>(hsv.saturation() * 1.10)),
                                   static_cast<int>(hsv.value() * 0.52));
        }();

        constexpr int kFrostAlpha = 10;
        const int kTintAlpha = isEnabled() ? (underMouse() ? 214 : 192) : 150;

        const QColor tintColor = [&] {
            const QColor hsv = baseColor.toHsv();
            const double sparkle = isEnabled() ? 1.20 : 1.00;
            return QColor::fromHsv(qMax(0, hsv.hue()),
                                   qBound(0, static_cast<int>(hsv.saturation() * 1.04), 255),
                                   qBound(0, static_cast<int>(hsv.value() * sparkle + 10), 255));
        }();

        QPainterPath pill;
        pill.addRoundedRect(body, r, r);

        painter.save();
        painter.setClipPath(pill);
        painter.setPen(Qt::NoPen);

        painter.fillRect(body, QColor(255, 255, 255, kFrostAlpha));
        painter.fillRect(body, withAlpha(tintColor, kTintAlpha));

        // 斜めの艶（面全体へ1枚だけ）。
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
        QLinearGradient sheen(body.topLeft(), body.bottomRight());
        sheen.setColorAt(0.00, QColor(255, 255, 255, 13));
        sheen.setColorAt(0.55, QColor(255, 255, 255, 3));
        sheen.setColorAt(1.00, QColor(255, 255, 255, 0));
        painter.fillRect(body, sheen);

        // 上端から入る光。
        QLinearGradient topGlow(body.topLeft(), QPointF(body.left(), body.bottom()));
        topGlow.setColorAt(0.00, QColor(255, 255, 255, 26));
        topGlow.setColorAt(0.40, QColor(255, 255, 255, 6));
        topGlow.setColorAt(1.00, QColor(255, 255, 255, 0));
        painter.fillRect(body, topGlow);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        // 左右の翳り（乗算）。円のときと同じく、縁に中心を置いた
        // 放射グラデーションで内側へ滑らかに消す。
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        for (const int side : {-1, 1}) {
            const QPointF shadeCenter(side < 0 ? body.left() : body.right(),
                                      body.center().y());
            QRadialGradient sideShade(shadeCenter, r * 1.8);
            sideShade.setColorAt(0.00, withAlpha(shadeColor, 70));
            sideShade.setColorAt(0.45, withAlpha(shadeColor, 26));
            sideShade.setColorAt(1.00, withAlpha(shadeColor, 0));
            painter.fillRect(body, sideShade);
        }
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.restore();

        // 輪郭線（乗算）。これが無いと下地に溶けて板に見えない。
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        QLinearGradient contour(QPointF(body.center().x(), body.top()),
                                QPointF(body.center().x(), body.bottom()));
        contour.setColorAt(0.00, withAlpha(shadeColor, 120));
        contour.setColorAt(0.50, withAlpha(shadeColor, 230));
        contour.setColorAt(1.00, withAlpha(shadeColor, 190));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QBrush(contour), 0.9));
        painter.drawPath(pill);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        // 押下中はほんの少し沈める（スケールは使わない。矩形が動くと
        // 縁のアンチエイリアスがちらつくため、暗色を1枚重ねるだけ）。
        if (isDown() && isEnabled()) {
            painter.save();
            painter.setClipPath(pill);
            painter.fillRect(body, QColor(0, 0, 0, 40));
            painter.restore();
        }

        // ラベル。文字も島の設定欄と同じ「1pxずらした黒＋本体」の影付き。
        const QColor textColor = isEnabled() ? QColor(255, 255, 255)
                                             : QColor(255, 255, 255, 110);
        painter.setFont(font());
        painter.setPen(QColor(0, 0, 0, 190));
        painter.drawText(rect().translated(0, 1), Qt::AlignCenter, text());
        painter.setPen(textColor);
        painter.drawText(rect(), Qt::AlignCenter, text());
    }

private:
    QString m_color;
};

} // namespace

IslandPromptPanel::IslandPromptPanel(int islandWidth, int baseH, QWidget *parent)
    : QFrame(parent)
    , m_islandWidth(islandWidth)
    , m_baseH(baseH)
{
    // ドロップ中にパネルが出ていても、ドロップ自体はキャンバスへ
    // 通したい（島本体と同じ扱い）。
    setAcceptDrops(true);
    setStyleSheet(QStringLiteral("QFrame{background:transparent;}"));

    // 島はどんな形態（畳んだピル／設定パネル／確認プロンプト）でも、掴めば
    // ウィンドウが動く。パネルは島の全面を覆っているが、QWidgetの既定の
    // mousePressEvent() は event->ignore() するので、押下は親である島へ
    // 伝播し DynamicIsland::mousePressEvent() が受け取る（＝文言の上でも
    // 掴んで動かせる）。YES/NOボタンはイベントを受け取って処理するため、
    // そこだけが例外になる。
    //
    // 【このパネル自身に Qt::WA_TransparentForMouseEvents を付けてはいけない】
    // Qtのヒットテスト（QWidgetPrivate::childAtRecursiveHelper）は、この属性を
    // 持つウィジェットを見つけると "continue" して子孫ごと探索対象から外す。
    // パネルに付けると中のYES/NOボタンまで一緒に押せなくなる。
    // 文言側（m_content とラベル）は子を持たないので付けてよい。

    m_content = new QWidget(this);
    m_content->setAttribute(Qt::WA_TranslucentBackground);
    m_content->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *layout = new QVBoxLayout(m_content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto makeLabel = [this, layout](int pixelSize, bool bold, const QColor &color) {
        auto *label = new ShadowLabel(m_content);
        QFont f = label->font();
        f.setPixelSize(pixelSize);
        f.setBold(bold);
        label->setFont(f);
        label->setTextColor(color);
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        // 文言の上でも島を掴んでウィンドウを動かせるようにする。
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(label);
        return label;
    };

    // 各ラベルは必要な高さちょうどで積み、余った分は下端へ逃がす。
    // これが無いと、高さが上限でクランプされた場合などに QVBoxLayout が
    // 余白を行間へ均等配分し、1行1行が間延びして見える。
    // heightForWidth は必ず残すこと（これを落とすと、折り返す文言でも
    // レイアウトが1行分しか高さを確保しなくなり、2行目以降が切れる）。
    auto fixVertical = [](QLabel *label) {
        QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Fixed);
        sp.setHeightForWidth(true);
        label->setSizePolicy(sp);
    };

    m_titleLabel   = makeLabel(10, true,  QColor(255, 255, 255, 170));
    m_subjectLabel = makeLabel(15, true,  QColor(255, 255, 255));
    m_messageLabel = makeLabel(11, false, QColor(255, 255, 255, 225));
    m_detailLabel  = makeLabel(11, false, QColor(255, 255, 255, 205));
    m_warningLabel = makeLabel(10, false, QColor(255, 205, 120));

    for (QLabel *label : {m_titleLabel, m_subjectLabel, m_messageLabel,
                          m_detailLabel, m_warningLabel})
        fixVertical(label);
    layout->addStretch(1);

    // 見出しは全部大文字＋字間広めで「ラベル」らしくする。
    QFont titleFont = m_titleLabel->font();
    titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    m_titleLabel->setFont(titleFont);

    // 詳細（DLL名＋ID）は等幅寄りにして、IDの文字列が読みやすいようにする。
    QFont detailFont = m_detailLabel->font();
    detailFont.setFamily(QStringLiteral("Consolas"));
    detailFont.setPixelSize(10);
    m_detailLabel->setFont(detailFont);

    auto *accept = new PromptPillButton(QStringLiteral("#28C940"), this);
    auto *reject = new PromptPillButton(QStringLiteral("#FF5F57"), this);
    connect(accept, &QPushButton::clicked, this, &IslandPromptPanel::accepted);
    connect(reject, &QPushButton::clicked, this, &IslandPromptPanel::rejected);
    m_acceptBtn = accept;
    m_rejectBtn = reject;
}

void IslandPromptPanel::setContent(const IslandPromptContent &content)
{
    m_titleLabel->setText(content.title.toUpper());
    m_subjectLabel->setText(content.subject);
    m_messageLabel->setText(content.message);
    m_detailLabel->setText(content.details.join(QLatin1Char('\n')));
    m_warningLabel->setText(content.warning);

    // 空の項目は畳む。以降の高さ計算では isVisible() ではなく isHidden() を
    // 見ること（パネル自身がまだ show() されていない間、子の isVisible() は
    // 常に false を返し、必要高さが0になってしまう）。
    for (QLabel *label : {m_titleLabel, m_subjectLabel, m_messageLabel,
                          m_detailLabel, m_warningLabel})
        label->setHidden(label->text().isEmpty());

    m_acceptBtn->setText(content.acceptText);
    m_rejectBtn->setText(content.rejectText);
    m_acceptBtn->setEnabled(content.acceptEnabled);

    m_content->adjustSize();
    // 文言が変われば必要高さも変わる。
    m_cachedHeightForWidth = -1;
}

int IslandPromptPanel::contentHeight(int layoutWidth) const
{
    // ラベルはすべてwordWrapなので、実幅を与えてheightForWidth()で
    // 必要行数を確定させる（sizeHint()だけだと1行分しか返らない）。
    const int innerW = qMax(1, layoutWidth - kSidePad * 2);
    int h = 0;
    bool first = true;
    for (const QLabel *label : {m_titleLabel, m_subjectLabel, m_messageLabel,
                                m_detailLabel, m_warningLabel}) {
        if (label->isHidden() || label->text().isEmpty())
            continue;
        if (!first)
            h += 6; // レイアウトのspacingと合わせる
        first = false;
        h += label->heightForWidth(innerW);
    }
    return h;
}

int IslandPromptPanel::preferredPanelHeight(int layoutWidth, int maxTotalHeight) const
{
    if (m_cachedHeightForWidth != layoutWidth) {
        m_cachedUncappedHeight = m_baseH + kTopGap + contentHeight(layoutWidth)
                               + kButtonGap + kButtonHeight + kBottomPad;
        m_cachedHeightForWidth = layoutWidth;
    }
    return qMin(m_cachedUncappedHeight, maxTotalHeight);
}

void IslandPromptPanel::applyPanelGeometry(int width, int panelHeight)
{
    m_islandWidth = width;

    const int innerW     = qMax(1, width - kSidePad * 2);
    const int contentTop = m_baseH + kTopGap;
    const int buttonTop  = qMax(contentTop,
                                panelHeight - kBottomPad - kButtonHeight);

    // 文言ブロックは最終幅で確定させ、以降は動かさない。ここを幅追従に
    // すると島の展開中ずっと折り返しが計算し直され、行数が増減して
    // 文字がガタつく。左寄せなので、島に右側をクリップされて順に現れる
    // 見え方でちょうど良い。
    m_content->setGeometry(kSidePad, contentTop, innerW,
                           qMax(0, buttonTop - kButtonGap - contentTop));

    setGeometry(0, 0, width, panelHeight);
    m_visibleWidth = width;
    layoutButtons(width, buttonTop);
}

void IslandPromptPanel::setVisibleWidth(int currentWidth)
{
    const int w = qMax(0, currentWidth);
    if (w == m_visibleWidth)
        return;
    m_visibleWidth = w;

    // 高さは applyPanelGeometry() で決めたものを維持する（高さアニメは
    // 島側が別に持っており、パネルは常に最終高さのまま島にクリップされて
    // 下から現れる）。
    resize(w, height());

    const int contentTop = m_baseH + kTopGap;
    const int buttonTop  = qMax(contentTop, height() - kBottomPad - kButtonHeight);
    layoutButtons(w, buttonTop);
}

void IslandPromptPanel::layoutButtons(int width, int buttonTop)
{
    // ボタンは左右2等分。YESが左（緑）、NOが右（赤）。
    // 幅は島の現在幅から求めるので、島が広がるのに合わせて両方が
    // 同じだけ伸びる（＝島全体が1つの物体として広がって見える）。
    const int innerW = qMax(0, width - kSidePad * 2);
    const int btnW   = qMax(0, (innerW - kButtonSpace) / 2);
    m_acceptBtn->setGeometry(kSidePad, buttonTop, btnW, kButtonHeight);
    m_rejectBtn->setGeometry(kSidePad + btnW + kButtonSpace, buttonTop,
                             qMax(0, innerW - btnW - kButtonSpace), kButtonHeight);
}
