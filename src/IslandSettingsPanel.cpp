#include "IslandSettingsPanel.h"
#include "IslandControlRegistry.h"
#include "AppLanguage.h"
#include "IslandTabGoo.h"
#include "IslandControls.h"
#include "IslandHelpGuide.h"
#include "DynamicIsland.h"   // islandGlassOnGpu()

#include <QApplication>
#include <QAbstractButton>
#include <QDebug>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QGraphicsOpacityEffect>
#include <QPointer>
#include <QLinearGradient>
#include <QTimer>
#include <QVector>

#include <functional>
#include <memory>

namespace {


// 2026-09-12: 島のガラス／アクセント色の上に文字が乗るため、設定欄の文字は
// 白＋影にする。
//
// 【QGraphicsDropShadowEffect は使わない】最初そちらで実装したところ、
// ラベルが丸ごと表示されなくなった。QSSで背景を透過にしたQScrollArea配下の
// ウィジェットにグラフィックスエフェクトを載せると、描画がオフスクリーンへ
// 回された時点で中身が出てこなくなる。そこで QLabel を継承して paintEvent で
// 自前に「1pxずらした黒を2回＋本体」を描く（CanvasVideoのdrawShadowedText()と
// 同じ影の作り方）。色はQSS任せにせずコード側で持つ（QSSのcolorは
// palette()へ必ずしも反映されないため）。
class ShadowLabel : public QLabel
{
public:
    explicit ShadowLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(text, parent) {}

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
        const int flags = static_cast<int>(alignment()) | Qt::TextSingleLine;
        p.setPen(QColor(0, 0, 0, 200));
        p.drawText(box.translated(1.0, 1.0), flags, t);
        p.drawText(box.translated(0.0, 1.0), flags, t);
        p.setPen(m_color);
        p.drawText(box, flags, t);
    }

private:
    QColor m_color {255, 255, 255};
};

QString categoryTitle(IslandControlCategory category)
{
    switch (category) {
    // 2026-09-16: 「見た目」→「全般」（色・グリッドなど全体に関わる設定を置く）
    case IslandControlCategory::Appearance: return AppLanguage::text("General", "全般");
    case IslandControlCategory::Behavior:   return AppLanguage::text("Controls", "操作");
    case IslandControlCategory::Other:      return AppLanguage::text("Other", "その他");
    // 2026-09-18: 設定項目を持たない、操作ガイド専用のタブ。
    case IslandControlCategory::Help:       return AppLanguage::text("Help", "ヘルプ");
    case IslandControlCategory::Plugins:    return AppLanguage::text("Plugins", "プラグイン");
    }
    return QString();
}

// 2026-09-18: タブのアイコン（:/icons/…）。幅が足りないときは文字が消えて
// これだけになるので、カテゴリの区別が付く絵にしてある。
QString categoryIcon(IslandControlCategory category)
{
    switch (category) {
    case IslandControlCategory::Appearance: return QStringLiteral(":/icons/tab_general.svg");
    case IslandControlCategory::Behavior:   return QStringLiteral(":/icons/tab_controls.svg");
    case IslandControlCategory::Other:      return QStringLiteral(":/icons/tab_other.svg");
    case IslandControlCategory::Help:       return QStringLiteral(":/icons/tab_help.svg");
    case IslandControlCategory::Plugins:    return QStringLiteral(":/icons/tab_plugins.svg");
    }
    return QString();
}

QVariant entryCurrentValue(const IslandControlEntry &entry)
{
    if (entry.currentValue) {
        const QVariant v = entry.currentValue();
        if (v.isValid())
            return v;
    }
    return entry.spec.defaultValue;
}

} // namespace

// ============================================================
//  IslandSettingsTabBar（2026-09-15）
//
//  島の上端の帯に置くカテゴリのタブ。ピル型の枠の中を等分割し、区切り線で
//  分ける。見た目は仮（ガラス表現は後で詰める）。
//
//  自前描画にしている理由: 設定の開閉アニメ中は島の幅に合わせて毎フレーム
//  リサイズされる。QTabBar やボタン＋レイアウトだと、そのたびにレイアウトと
//  スタイルの再計算が走る。これは矩形を割って文字を置くだけなので軽い。
// ============================================================
class IslandSettingsTabBar : public QWidget
{
public:
    std::function<void(int)> onSelected;

    explicit IslandSettingsTabBar(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_TranslucentBackground, true);

        // ---- 横スクロールの慣性（2026-09-18）----
        // 【フレーム更新の方針】
        //   止まっている間はタイマーを完全に止める（0Hz）。動かす必要が
        //   できたときだけ 60Hz で回し、目標へ着いたら即 stop() する。
        //   描き直すのはこのウィジェット（タブの帯 = 島幅×30px 程度）だけで、
        //   島本体・設定パネル・キャンバスへは update() を出さない。
        //   ドラッグ中はイベント駆動（マウス移動のたびに1回描く）なので
        //   タイマーは使わない。
        m_scrollTimer.setTimerType(Qt::PreciseTimer);
        m_scrollTimer.setInterval(16);   // 60Hz
        QObject::connect(&m_scrollTimer, &QTimer::timeout, this, [this] { stepScroll(); });
    }

    void setTabs(const QVector<IslandTabGoo::TabItem> &tabs, int current)
    {
        m_tabs = tabs;
        m_current = current;
        m_hover = -1;
        syncGlass();
        // 2026-09-18: 言語を切り替えるとタブの枚数は同じまま文字だけ変わる。
        // remeasure() は幅と枚数が同じなら何もしないので、ここで必ず測り直させる。
        m_measuredWidth = -1.0;
        remeasure();
        ensureVisible(current, /*animate=*/false);
        update();
    }
    void setCurrent(int index)
    {
        if (m_current == index)
            return;
        m_current = index;
        ensureVisible(index, /*animate=*/true);
        update();
    }

    // 幅が変わったら測り直す（開閉アニメ中もここを通る）。
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        remeasure();
        syncGlass();
    }

    // ---- タブの面のGL描画（2026-09-22）----
    // 分裂アニメ（IslandTabGooLayer）はタブの面をGL側で描いているのに、静止用の
    // このウィジェットはQPainterで描いていたため、アニメ完了で引き継ぐ瞬間に
    // 面の濃さが変わって見えた（IslandTabGoo::sunkCapsuleGlass のコメント）。
    // GL版では面をGL側（島を所有者にした登録）で描き、ここは中身だけを描く。
    // island: 島（タブの座標を島ローカルへ直す基準。不透明度もここから取る）
    void attachGlass(QWidget *island)
    {
        if (!island || !islandGlassOnGpu())
            return;
        m_island = island;
        m_glass.attach(island);
        syncGlass();
    }

    // 形・表示状態・島の不透明度が変わるたびに呼ぶ（値が同じなら何も起きない）。
    void syncGlass()
    {
        if (!m_island)
            return;
        BackdropBlur::GlassPlateStyle glass;
        QRectF area;
        qreal opacity = 0.0;
        // isVisible() は親（パネル）が隠れている間も false になる。
        if (isVisible() && !m_tabs.isEmpty() && width() >= 8
            && m_island->isAncestorOf(this)) {
            const QPoint origin = mapTo(m_island, QPoint(0, 0));
            area = QRectF(rect()).translated(origin);
            // 形の座標は「登録した矩形（area）の左上が原点」（Blur.cpp の
            // kGlassVertexSrc の vLocal）。島の座標ではなくタブバーの座標で渡す。
            glass = IslandTabGoo::sunkCapsuleGlass(capsuleRect());
            const auto *fx = qobject_cast<QGraphicsOpacityEffect *>(m_island->graphicsEffect());
            opacity = fx ? fx->opacity() : 1.0;
        }
        // 隠れている間に「空」を登録し直し続けると、そのたびにキャンバスの
        // 全面再描画が要求される（空の登録は描かれた記録が残らないため、
        // setShape() が毎回「変化あり」と判定する）。空→空は送らない。
        if (!glass.enabled && m_glassEmptySent)
            return;
        m_glassEmptySent = !glass.enabled;
        m_glass.setGlassStyle(glass);
        m_glass.setShape(glass.enabled ? area : QRectF(), 0.0, 0.0, opacity);
    }

protected:
    QRectF capsuleRect() const
    {
        return QRectF(0.0, kTabPad, width(), height() - kTabPad * 2.0);
    }

    void remeasure()
    {
        const double w = capsuleRect().width();
        if (qFuzzyCompare(w + 1.0, m_measuredWidth + 1.0) && m_metrics.widths.size() == m_tabs.size())
            return;
        m_measuredWidth = w;
        m_metrics = IslandTabGoo::measureTabs(m_tabs, w);
        clampScroll();
    }

    void clampScroll()
    {
        const double maxS = m_metrics.maxScroll();
        m_scroll = qBound(0.0, m_scroll, maxS);
        m_scrollTarget = qBound(0.0, m_scrollTarget, maxS);
        if (qAbs(m_scrollTarget - m_scroll) < 0.25) {
            m_scroll = m_scrollTarget;
            m_scrollTimer.stop();
        }
    }

    // 目標を決めてタイマーを回す（既に着いているなら回さない）。
    void setScrollTarget(double target)
    {
        target = qBound(0.0, target, m_metrics.maxScroll());
        if (qFuzzyCompare(target + 1.0, m_scrollTarget + 1.0))
            return;
        m_scrollTarget = target;
        if (qAbs(m_scrollTarget - m_scroll) < 0.25) {
            m_scroll = m_scrollTarget;
            m_scrollTimer.stop();
            update();
            return;
        }
        if (!m_scrollTimer.isActive())
            m_scrollTimer.start();
    }

    // 60Hz。指数イーズで寄せ、着いたら止める。
    void stepScroll()
    {
        const double d = m_scrollTarget - m_scroll;
        if (qAbs(d) < 0.4) {
            m_scroll = m_scrollTarget;
            m_scrollTimer.stop();
            update();
            return;
        }
        m_scroll += d * 0.28;
        update();
    }

    // index 番目のタブが帯の中に入るまでスクロールする。
    void ensureVisible(int index, bool animate)
    {
        if (index < 0 || index >= m_metrics.widths.size() || !m_metrics.scrollable)
            return;
        const double left  = m_metrics.left(index);
        const double right = left + m_metrics.widths[index];
        const double view  = m_metrics.available;
        double target = m_scrollTarget;
        if (left < target + 8.0)
            target = left - 8.0;
        else if (right > target + view - 8.0)
            target = right - view + 8.0;
        target = qBound(0.0, target, m_metrics.maxScroll());
        if (animate) {
            setScrollTarget(target);
        } else {
            m_scrollTimer.stop();
            m_scroll = m_scrollTarget = target;
        }
    }

    int indexAt(const QPointF &pos) const
    {
        if (m_tabs.isEmpty() || width() <= 0)
            return -1;
        return IslandTabGoo::tabIndexAt(m_metrics, m_scroll, pos.x());
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (!m_metrics.scrollable) {
            event->ignore();
            return;
        }
        // 縦ホイールも横スクロールとして扱う（タブの上ではこれ以外に
        // スクロールさせたいものが無いため）。120 ≒ 60px。
        const QPoint d = event->angleDelta();
        const double delta = (d.y() != 0 ? d.y() : d.x()) * 0.5;
        setScrollTarget(m_scrollTarget - delta);
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // 島のドラッグ（ウィンドウ移動）へは流さない。
        event->accept();
        if (event->button() != Qt::LeftButton)
            return;
        m_pressX = event->position().x();
        m_pressed = true;
        m_dragged = false;
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        event->accept();
        const bool wasDrag = m_dragged;
        m_pressed = false;
        m_dragged = false;
        if (event->button() != Qt::LeftButton || wasDrag)
            return;   // 掴んで動かしただけならタブは切り替えない
        const int i = indexAt(event->position());
        if (i >= 0 && i != m_current && onSelected)
            onSelected(i);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        event->accept();
        if (m_pressed && m_metrics.scrollable) {
            const double dx = event->position().x() - m_pressX;
            if (!m_dragged && qAbs(dx) > 4.0)
                m_dragged = true;
            if (m_dragged) {
                // ドラッグ中は指に追従させる（タイマーは使わない＝
                // マウス移動イベント1回につき1描画）。
                m_scrollTimer.stop();
                m_scroll = qBound(0.0, m_scroll - dx, m_metrics.maxScroll());
                m_scrollTarget = m_scroll;
                m_pressX = event->position().x();
                if (m_hover != -1) m_hover = -1;
                update();
                return;
            }
        }
        const int i = indexAt(event->position());
        if (i != m_hover) {
            m_hover = i;
            update();
        }
    }
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        syncGlass();
    }
    void hideEvent(QHideEvent *event) override
    {
        QWidget::hideEvent(event);
        syncGlass();
    }
    void moveEvent(QMoveEvent *event) override
    {
        QWidget::moveEvent(event);
        syncGlass();
    }

    void leaveEvent(QEvent *) override
    {
        if (m_hover != -1) {
            m_hover = -1;
            update();
        }
    }

    void paintEvent(QPaintEvent *) override
    {
        if (m_tabs.isEmpty() || width() < 8)
            return;
        // 2026-09-15: 沈み込んだタブの面と中身は IslandTabGoo の関数で描く。
        // 分裂アニメの最後のフレームと同じ関数なので、アニメが終わってこの
        // ウィジェットへ切り替わる瞬間に見た目が飛ばない。
        // 上下に kTabPad ずつ余白を取っているのは、沈み込みの下縁の光（リップ）が
        // 形の外側 1px に出るため。
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF capsule = capsuleRect();
        const QPainterPath path = IslandTabGoo::capsulePath(capsule);
        if (m_island) {
            // 2026-09-22: 面はGL側（syncGlass）。ここは中身だけ。
            syncGlass();
        } else {
            // ＋ボタンの円を含まない（disc=nullptr）ので、全体が雫側として描かれる。
            IslandTabGoo::paintSurface(p, path, 1.0, 0.0, 0.0, nullptr);
        }
        IslandTabGoo::paintTabContents(p, capsule, m_tabs, m_metrics, m_scroll,
                                       m_current, m_hover, 1.0);
        // 続きがあることが分かるよう、両端に薄い影を落とす（スクロール可能時のみ）。
        if (m_metrics.scrollable) {
            p.setClipPath(path);
            const double fadeW = 14.0;
            if (m_scroll > 0.5) {
                QLinearGradient g(capsule.left(), 0, capsule.left() + fadeW, 0);
                g.setColorAt(0.0, QColor(0, 0, 0, 90));
                g.setColorAt(1.0, QColor(0, 0, 0, 0));
                p.fillRect(QRectF(capsule.left(), capsule.top(), fadeW, capsule.height()), g);
            }
            if (m_scroll < m_metrics.maxScroll() - 0.5) {
                QLinearGradient g(capsule.right() - fadeW, 0, capsule.right(), 0);
                g.setColorAt(0.0, QColor(0, 0, 0, 0));
                g.setColorAt(1.0, QColor(0, 0, 0, 90));
                p.fillRect(QRectF(capsule.right() - fadeW, capsule.top(), fadeW, capsule.height()), g);
            }
        }
    }

public:
    static constexpr int kTabPad = 2;

private:
    QVector<IslandTabGoo::TabItem> m_tabs;
    IslandTabGoo::TabMetrics m_metrics;
    double m_measuredWidth = -1.0;
    double m_scroll = 0.0;
    double m_scrollTarget = 0.0;
    QTimer m_scrollTimer;
    bool   m_pressed = false;
    bool   m_dragged = false;
    double m_pressX = 0.0;
    int m_current = 0;
    int m_hover = -1;
    // GL版のタブの面（attachGlass 参照）。m_island が null ならQPainterで描く。
    QPointer<QWidget> m_island;
    BackdropBlur::Surface m_glass;
    bool m_glassEmptySent = false;
};

// ============================================================
//  IslandSettingsRow（2026-09-16）
//
//  設定項目1件＝1行。どの種類の項目も同じ規則で並べる:
//
//    ●  項目名 ....................................  [操作部品]
//    ↑  ↑                                                    ↑
//  markerX labelX                                     controlRight
//  （＋ボタン中心）（タブの左端）                     （×ボタン中心）
//
//  - 3本の縦線は IslandSettingsPanel::setRowGuides() で島ローカル座標として
//    渡され、パネルが行の座標へ直して配る。
//  - 行の高さは一定（kRowHeight）で、点・項目名・部品は行の上下中央に揃う。
//  - 部品の大きさは作る側（createControlWidget）がプロパティで指定する
//    （"pbRowWidth" / "pbRowHeight"）。行は右端揃えで置くだけ。
//  - 注記（Label）は点も部品も持たず、項目名の線から右端の線まで折り返す。
//  - 見出し（プラグイン名）は点の線から始まる小さめの文字。
//
//  子の配置は resizeEvent / relayout で直接 setGeometry する（レイアウト
//  オブジェクトを入れ子にしない）。行が数十あっても再計算が軽い。
// ============================================================
class IslandSettingsRow : public QWidget
{
public:
    enum class Kind { Control, Note, Heading };

    static constexpr int kRowHeight = 30;
    static constexpr int kControlHeight = 22;
    static constexpr int kLabelGap = 12;     // 項目名と部品の最小の隙間
    static constexpr int kMarkerRadius = 2;

    IslandSettingsRow(Kind kind, const QString &text, QWidget *control, QWidget *parent)
        : QWidget(parent), m_kind(kind), m_text(text), m_control(control)
    {
        setAcceptDrops(true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        if (kind == Kind::Note) {
            m_note = new QLabel(text, this);
            m_note->setWordWrap(true);
            m_note->setStyleSheet(QStringLiteral("color:rgba(255,255,255,165);font-size:11px;"));
        } else {
            m_label = new ShadowLabel(text, this);
            QFont f = m_label->font();
            f.setPixelSize(kind == Kind::Heading ? 10 : 12);
            m_label->setFont(f);
            m_label->setTextColor(kind == Kind::Heading ? QColor(255, 255, 255, 150)
                                                        : QColor(255, 255, 255, 235));
            m_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        }
        if (m_control)
            m_control->setParent(this);
        setFixedHeight(kRowHeight);
    }

    QWidget *control() const { return m_control; }
    QLabel *noteLabel() const { return m_note; }

    // 揃え線（行ローカル座標）
    void setGuides(int markerX, int labelX, int controlRight)
    {
        if (m_markerX == markerX && m_labelX == labelX && m_controlRight == controlRight)
            return;
        m_markerX = markerX;
        m_labelX = labelX;
        m_controlRight = controlRight;
        relayout();
    }

    void relayout()
    {
        const int h = height();
        const int cy = h / 2;
        if (m_kind == Kind::Note) {
            const int w = qMax(0, m_controlRight - m_labelX);
            const int textH = m_note->heightForWidth(w);
            const int rowH = qMax(kControlHeight, textH) + 8;
            if (height() != rowH)
                setFixedHeight(rowH);       // 折り返し行数で行の高さが変わる
            m_note->setGeometry(m_labelX, 4, w, qMax(0, rowH - 8));
            return;
        }

        int controlLeft = m_controlRight;
        if (m_control) {
            const int cw = m_control->property("pbRowWidth").toInt();
            const int ch = m_control->property("pbRowHeight").isValid()
                               ? m_control->property("pbRowHeight").toInt()
                               : kControlHeight;
            controlLeft = m_controlRight - cw;
            // 2026-09-16: 部品の外に描く飾り（入力欄のフォーカスの輪など）の分だけ、
            // 揃え線はそのままで外側へ広げて置く（"pbRowOutset"）。
            const int out = m_control->property("pbRowOutset").toInt();
            m_control->setGeometry(controlLeft - out, cy - ch / 2 - out, cw + out * 2, ch + out * 2);
        }

        const int textX = (m_kind == Kind::Heading) ? m_markerX - kMarkerRadius : m_labelX;
        const int textW = qMax(0, (m_control ? controlLeft - kLabelGap : m_controlRight) - textX);
        const QString elided = QFontMetrics(m_label->font()).elidedText(m_text, Qt::ElideRight, textW);
        if (m_label->text() != elided)
            m_label->setText(elided);
        m_label->setToolTip(elided != m_text ? m_text : QString());
        m_label->setGeometry(textX, 0, textW, h);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        relayout();
    }

    void paintEvent(QPaintEvent *) override
    {
        if (m_kind != Kind::Control)
            return;
        // 行頭の点。無効な行は薄くする。
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, isEnabled() ? 70 : 30));
        p.drawEllipse(QPointF(m_markerX, height() / 2.0), kMarkerRadius, kMarkerRadius);
    }

    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::EnabledChange && m_label)
            m_label->setTextColor(isEnabled() ? QColor(255, 255, 255, 235)
                                              : QColor(255, 255, 255, 110));
    }

private:
    Kind m_kind;
    QString m_text;
    QWidget *m_control = nullptr;
    ShadowLabel *m_label = nullptr;
    QLabel *m_note = nullptr;
    int m_markerX = 21;
    int m_labelX = 47;
    int m_controlRight = 200;
};

IslandSettingsPanel::IslandSettingsPanel(int islandWidth, int baseH, QWidget *parent)
    : QFrame(parent)
    , m_islandWidth(islandWidth)
    , m_baseH(baseH)
{
    setAcceptDrops(true);

    // QSSは旧実装（DynamicIsland::toggleSettingsPanel()内のベタ書き文字列）
    // からの移設＋Slider/ScrollBar/見出し分の追加。
    setStyleSheet(
        "QFrame{background:transparent;}"
        "QLabel{color:#ffffff;font-size:11px;}"
        "QLabel#pbSectionHeading{color:rgba(255,255,255,215);font-size:10px;}"
        // 2026-09-16: 操作部品は IslandControls の自前描画になったので、
        // 旧 QComboBox/QSpinBox/QCheckBox/QPushButton/QSlider 等の指定は削除した。
        // 【重要】QSSを使うとQAbstractScrollAreaはviewportをパレットの
        // Base色でベタ塗りする（島のアクセント色背景の上に暗い矩形が
        // 出てデザインが崩れる）。QScrollArea本体だけでなく、viewport
        // （> QWidget）とその中のcontentウィジェット（> QWidget > QWidget）
        // まで明示的に透過指定する必要がある。
        "QScrollArea{background:transparent;border:none;}"
        "QScrollArea > QWidget > QWidget{background:transparent;}"
        "QScrollArea > QWidget{background:transparent;}"
        "QScrollBar:vertical{width:6px;background:transparent;margin:0;}"
        "QScrollBar::handle:vertical{background:rgba(255,255,255,60);"
        "border-radius:3px;min-height:20px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"
    );

    // 中身が上限高さに収まらない場合のみ縦スクロールバーが出る。
    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setAcceptDrops(true);
    // QSS側の透過指定（上記コメント参照）に加えて、パレット経由の
    // 背景塗りも保険で無効化しておく（スタイル分岐によってはQSSの
    // 透過だけでは効かないケースがあるため二重に潰す）。
    m_scroll->setAutoFillBackground(false);
    m_scroll->viewport()->setAutoFillBackground(false);
    {
        QPalette pal = m_scroll->palette();
        pal.setColor(QPalette::Window, Qt::transparent);
        pal.setColor(QPalette::Base, Qt::transparent);
        m_scroll->setPalette(pal);
        m_scroll->viewport()->setPalette(pal);
    }
    m_scroll->viewport()->setAcceptDrops(true);

    m_content = new QWidget(m_scroll);
    m_content->setObjectName(QStringLiteral("pbSettingsContent"));
    m_content->setAutoFillBackground(false);
    m_content->setAcceptDrops(true);
    m_contentLayout = new QVBoxLayout(m_content);
    // 左右16pxは旧実装の px=16 に合わせる。
    // 2026-09-16: 行が揃え線を自分で持つので、左右の余白は0。上だけ少し空ける。
    m_contentLayout->setContentsMargins(0, 6, 0, 8);
    m_contentLayout->setSpacing(0);   // 行の高さが間隔を兼ねる
    m_scroll->setWidget(m_content);
    // 2026-09-15: スクロール領域は島の幅いっぱい（スクロールバーは島の右端）、
    // 項目の列は構築時の幅のまま左に寄せる。
    // 2026-09-16: 行は島の幅いっぱい（右端の揃え線が×ボタンの中心）に広がる。
    m_scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    m_tabBar = new IslandSettingsTabBar(this);
    // 2026-09-22: タブの面をGL側で描く（島＝このパネルの親を基準にする）。
    m_tabBar->attachGlass(parentWidget());
    m_tabBar->onSelected = [this](int index) {
        if (index >= 0 && index < m_categories.size())
            selectCategory(m_categories[index]);
    };

    // 実行時の状態上書き（PbControlHost経由）を、パネルが開いている間は
    // 該当ウィジェットだけその場更新で反映する（2026-07-13追加）。
    connect(&IslandControlRegistry::instance(),
            &IslandControlRegistry::controlStateChanged,
            this, &IslandSettingsPanel::onControlStateChanged);
    connect(&IslandControlRegistry::instance(),
            &IslandControlRegistry::controlValuePushed,
            this, &IslandSettingsPanel::onControlValuePushed);
}

void IslandSettingsPanel::applyControlStateToWidget(QWidget *w,
                                                    const IslandControlState &state)
{
    if (!w)
        return;
    if (state.hasEnabled) {
        // 2026-09-16: 無効化は行ごと（項目名と点も一緒に薄くなる）
        QWidget *target = w;
        for (QWidget *p = w; p; p = p->parentWidget()) {
            if (dynamic_cast<IslandSettingsRow *>(p)) { target = p; break; }
        }
        target->setEnabled(state.enabled);
    }
    if (state.hasVisible) {
        // 2026-09-16: 行ごと出し入れする（レイアウトから外れるので下の行が詰まる）
        for (QWidget *p = w; p; p = p->parentWidget()) {
            if (auto *row = dynamic_cast<IslandSettingsRow *>(p)) {
                row->setVisible(state.visible);
                break;
            }
        }
    }
    if (state.hasLabel) {
        // 文言変更対象は「単体ウィジェットとして生成されるkind」のみ:
        //   Label  → QLabel（createControlWidget()のLabel分岐）
        //   Button → IslandControls::Button（QAbstractButton。同Button分岐）
        // Slider/TextInput等のコンテナ（QWidget+内部QLabel）は対象外
        // （どのQLabelを指すか曖昧なため。Registryが警告済みの契約）。
        if (auto *label = qobject_cast<QLabel *>(w))
            label->setText(state.label);
        else if (auto *btn = dynamic_cast<IslandControls::Button *>(w))
            btn->setText(state.label);
    }
}

void IslandSettingsPanel::onControlValuePushed(const QString &sectionId,
                                               const QString &controlId,
                                               const QVariant &value)
{
    QWidget *w = m_controlWidgets.value(controlWidgetKey(sectionId, controlId));
    if (!w)
        return; // パネル未構築・別タブ（次に作るときは currentValue から読む）
    if (auto *slider = dynamic_cast<IslandControls::Slider *>(w)) {
        slider->setValue(value.toInt());
    } else if (auto *spin = dynamic_cast<IslandControls::SpinBox *>(w)) {
        spin->setValue(value.toInt());
    } else if (auto *toggle = dynamic_cast<IslandControls::Toggle *>(w)) {
        toggle->setCheckedSilently(value.toBool());   // バネで切り替わる（toggled は出さない）
    } else if (auto *choice = dynamic_cast<IslandControls::Choice *>(w)) {
        const int idx = choice->findData(value.toString());
        if (idx >= 0)
            choice->setCurrentIndex(idx);
    } else if (auto *field = dynamic_cast<IslandControls::TextField *>(w)) {
        // 入力中の欄は書き換えない（打っている途中の文字を消さない）
        QLineEdit *edit = field->edit();
        const QString text = value.toString();
        if (!edit->hasFocus() && edit->text() != text) {
            edit->setText(text);
            edit->setProperty("pbLastCommitted", text);
        }
    }
}

void IslandSettingsPanel::onControlStateChanged(const QString &sectionId,
                                                const QString &controlId)
{
    QWidget *w = m_controlWidgets.value(controlWidgetKey(sectionId, controlId));
    if (!w)
        return; // パネル未構築 or 対象セクションが未表示（rebuild時に反映される）
    applyControlStateToWidget(
        w, IslandControlRegistry::instance().controlState(sectionId, controlId));
    // Labelの文言変更で行の折り返し高さが変わり得るため、行を並べ直す。
    for (QWidget *p = w; p; p = p->parentWidget()) {
        if (auto *row = dynamic_cast<IslandSettingsRow *>(p)) {
            row->relayout();
            break;
        }
    }
    m_content->updateGeometry();
}

void IslandSettingsPanel::installWheelGuard(QWidget *w)
{
    w->installEventFilter(this);
}

bool IslandSettingsPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Wheel) {
        // 値系ウィジェット上でのホイールによる値変更（QSpinBox/QComboBox/
        // QSliderの既定挙動）は誤操作事故のもとなので無効化する。
        // イベントは握りつぶさずスクロールエリアのviewportへ転送し、
        // ウィジェット上でもパネルのスクロールだけは効くようにする
        // （フィルタはviewportにはインストールしていないため再帰しない）。
        auto *src = static_cast<QWheelEvent *>(event);
        auto *watchedWidget = qobject_cast<QWidget *>(watched);
        if (watchedWidget && m_scroll && m_scroll->viewport()) {
            const QPointF globalPos = src->globalPosition();
            const QPointF viewportPos =
                m_scroll->viewport()->mapFromGlobal(globalPos.toPoint());
            QWheelEvent forwarded(viewportPos, globalPos,
                                  src->pixelDelta(), src->angleDelta(),
                                  src->buttons(), src->modifiers(),
                                  src->phase(), src->inverted(), src->source());
            QApplication::sendEvent(m_scroll->viewport(), &forwarded);
        }
        return true; // ウィジェット本体には渡さない（値を変えさせない）
    }
    return QFrame::eventFilter(watched, event);
}

void IslandSettingsPanel::clearContent()
{
    m_controlWidgets.clear();
    m_rows.clear();
    m_helpWidget = nullptr;   // 下の takeAt ループで deleteLater される
    while (QLayoutItem *item = m_contentLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
}

void IslandSettingsPanel::addSectionHeading(const QString &title)
{
    // 2026-09-16: 見出しも行の1種類。先頭以外は上に区切り線を引く。
    // 長い表示名（プラグインの displayName）は行が揃え線の幅で省略する。
    if (m_contentLayout->count() > 0) {
        auto *line = new QFrame(m_content);
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background:rgba(255,255,255,22);"));
        m_contentLayout->addSpacing(6);
        m_contentLayout->addWidget(line);
    }
    auto *row = new IslandSettingsRow(IslandSettingsRow::Kind::Heading, title, nullptr, m_content);
    m_rows.append(row);
    m_contentLayout->addWidget(row);
}

void IslandSettingsPanel::relayoutRows()
{
    // パネル（島）ローカル座標の揃え線を、行の座標（スクロール領域の中）へ直す。
    const int offsetX = m_scroll ? m_scroll->x() : 0;
    const int panelW = m_panelWidth > 0 ? m_panelWidth : width();
    const int markerX = m_markerX - offsetX;
    const int labelX = m_labelX - offsetX;
    const int controlRight = panelW - m_controlRightInset - offsetX;
    for (IslandSettingsRow *row : std::as_const(m_rows))
        row->setGuides(markerX, labelX, controlRight);
    if (m_helpWidget)
        m_helpWidget->setGuides(markerX, labelX, controlRight);
}

void IslandSettingsPanel::setRowGuides(int markerX, int labelX, int controlRightInset)
{
    m_markerX = markerX;
    m_labelX = labelX;
    m_controlRightInset = controlRightInset;
    relayoutRows();
}

void IslandSettingsPanel::rebuild()
{
    clearContent();

    // ---- タブの一覧（2026-09-15）----
    // 選択中のカテゴリが消えていたら（プラグインが外れた等）先頭へ戻す。
    m_categories = availableCategories();
    if (!m_categories.isEmpty() && !m_categories.contains(m_currentCategory))
        m_currentCategory = m_categories.first();
    m_tabBar->setTabs(tabItems(), m_categories.indexOf(m_currentCategory));

    // ---- ヘルプ（2026-09-18）----
    // 設定項目ではなく操作ガイドを出すタブ。Registry は見ない。
    if (m_currentCategory == IslandControlCategory::Help) {
        m_helpWidget = new IslandHelpGuideWidget(m_content);
        m_contentLayout->addWidget(m_helpWidget);
        relayoutRows();
        m_contentLayout->addStretch(1);
        m_contentLayout->activate();
        return;
    }

    const QList<IslandControlSection> sections =
        IslandControlRegistry::instance().sections();

    for (const IslandControlSection &section : sections) {
        if (section.category != m_currentCategory)
            continue;
        // 本体のカテゴリはタブ名がそのまま見出しなので出さない。
        // プラグインのタブはプラグインごとに区切る必要があるので出す。
        if (section.category == IslandControlCategory::Plugins && !section.title.isEmpty())
            addSectionHeading(section.title);

        // 2026-09-16: 1項目＝1行。sameRowAsPrevious（同じ行に横並び）は、
        // 項目名を左・部品を右に揃える規則と両立しないため、今は無視する
        // （宣言は残るのでプラグイン側の変更は不要）。
        for (const IslandControlEntry &entry : section.entries) {
            QWidget *control = createControlWidget(entry);
            const bool isNote = (entry.spec.kind == PbControlKind::Label);
            auto *row = new IslandSettingsRow(
                isNote ? IslandSettingsRow::Kind::Note : IslandSettingsRow::Kind::Control,
                entry.spec.label, isNote ? nullptr : control, m_content);
            if (isNote) {
                // 注記は行が持つ QLabel そのものを索引する（文言の上書き対象）
                delete control;
                control = row->noteLabel();
            }
            if (!entry.spec.tooltip.isEmpty())
                row->setToolTip(entry.spec.tooltip);

            // 生成したウィジェットを sectionId/controlId で索引し、実行時の
            // 状態上書き（有効/無効・文言）があれば構築時点で反映する
            // （パネルを閉じて開き直しても最後の状態が復元されるように）。
            m_controlWidgets.insert(controlWidgetKey(section.id, entry.spec.id), control);
            applyControlStateToWidget(
                control, IslandControlRegistry::instance().controlState(section.id, entry.spec.id));

            m_rows.append(row);
            m_contentLayout->addWidget(row);
        }
    }

    relayoutRows();
    m_contentLayout->addStretch(1);
    m_contentLayout->activate();
}

QList<IslandControlCategory> IslandSettingsPanel::availableCategories() const
{
    QList<IslandControlCategory> result;
    const QList<IslandControlSection> sections = IslandControlRegistry::instance().sections();
    for (IslandControlCategory c : { IslandControlCategory::Appearance,
                                     IslandControlCategory::Behavior,
                                     IslandControlCategory::Other,
                                     IslandControlCategory::Help,
                                     IslandControlCategory::Plugins }) {
        // 2026-09-18: ヘルプは登録された設定項目ではなく操作ガイドを出すので、
        // Registry に無くても常に並べる。
        if (c == IslandControlCategory::Help) {
            result.append(c);
            continue;
        }
        for (const IslandControlSection &s : sections) {
            if (s.category == c) {
                result.append(c);
                break;
            }
        }
    }
    return result;
}

void IslandSettingsPanel::selectCategory(IslandControlCategory category)
{
    if (category == m_currentCategory)
        return;
    m_currentCategory = category;
    rebuild();
    // 前のタブのスクロール位置を持ち越さない。
    m_scroll->verticalScrollBar()->setValue(0);
}

void IslandSettingsPanel::syncTabGlass()
{
    if (m_tabBar)
        m_tabBar->syncGlass();
}

void IslandSettingsPanel::setTabBarSuppressed(bool suppressed)
{
    if (m_tabBarSuppressed == suppressed)
        return;
    m_tabBarSuppressed = suppressed;
    setVisibleWidth(width());
}

QVector<IslandTabGoo::TabItem> IslandSettingsPanel::tabItems() const
{
    QVector<IslandTabGoo::TabItem> items;
    items.reserve(m_categories.size());
    for (IslandControlCategory c : m_categories)
        items.append({ categoryTitle(c), categoryIcon(c) });
    return items;
}

void IslandSettingsPanel::setTabBarInsets(int left, int rightSpace)
{
    m_tabLeft = left;
    m_tabRightSpace = rightSpace;
    setVisibleWidth(width());
}

// 2026-09-16: 行の右側に置く「部品だけ」を作る。項目名は行（IslandSettingsRow）が
// 左に描く。部品の大きさは "pbRowWidth" / "pbRowHeight" プロパティで行へ伝える。
// 部品の中身は IslandControls（自前描画・アニメ付き）。
QWidget *IslandSettingsPanel::createControlWidget(const IslandControlEntry &entry)
{
    const PbControlSpec &spec = entry.spec;
    // std::functionをラムダへ値コピーして接続する（entry自体はrebuild()の
    // ローカルコピーのQList要素なので、参照を掴んではいけない）。
    const auto onValueChanged = entry.onValueChanged;
    const auto onTriggered    = entry.onTriggered;

    // 2026-09-16: 部品は IslandControls の自前描画（寸法・動きは島コントロールラボの値）。
    namespace M = IslandControls::Metrics;
    const auto sized = [](QWidget *w, int width, int height) {
        w->setProperty("pbRowWidth", width);
        w->setProperty("pbRowHeight", height);
        return w;
    };

    QWidget *result = nullptr;

    switch (spec.kind) {
    case PbControlKind::Toggle: {
        auto *toggle = new IslandControls::Toggle(m_content);
        toggle->setCheckedImmediate(entryCurrentValue(entry).toBool());
        if (onValueChanged)
            connect(toggle, &QAbstractButton::toggled, this,
                    [onValueChanged](bool v) { onValueChanged(v); });
        result = sized(toggle, M::kToggleW, M::kToggleH);
        break;
    }
    case PbControlKind::SpinBox: {
        auto *spin = new IslandControls::SpinBox(
            spec.minValue, spec.maxValue, spec.step,
            entryCurrentValue(entry).toInt(), m_content);
        if (onValueChanged)
            spin->onValueChanged = [onValueChanged](int v) { onValueChanged(v); };
        result = sized(spin, M::kSpinW, M::kSpinH);
        break;
    }
    case PbControlKind::Slider: {
        // [数値][溝] を右端揃えで。数値は溝の左（部品が自分で描く）。
        // 確定は「溝のクリック・ドラッグとも離したとき」（plugin_config.jsonへの過剰書き込み防止）。
        auto *slider = new IslandControls::Slider(
            spec.minValue, spec.maxValue, spec.step,
            entryCurrentValue(entry).toInt(), m_content);
        if (onValueChanged)
            slider->onValueChanged = [onValueChanged](int v) { onValueChanged(v); };
        result = sized(slider, IslandControls::Slider::totalWidth(), IslandSettingsRow::kControlHeight);
        break;
    }
    case PbControlKind::Choice: {
        auto *choice = new IslandControls::Choice(m_content);
        for (const PbControlOption &opt : spec.options)
            choice->addItem(opt.label, opt.id);
        const int idx = choice->findData(entryCurrentValue(entry).toString());
        choice->setCurrentIndex(idx >= 0 ? idx : 0);
        if (onValueChanged)
            choice->onCurrentIndexChanged = [onValueChanged, choice](int i) {
                onValueChanged(choice->itemData(i));
            };
        // 一番長い選択肢が収まる幅（上下限あり）
        result = sized(choice, choice->preferredWidth(), M::kChoiceH);
        break;
    }
    case PbControlKind::Button: {
        const QString text = entry.buttonText.isEmpty()
            ? AppLanguage::text("Open…", "設定…")
            : entry.buttonText;
        auto *btn = new IslandControls::Button(text, m_content);
        if (onTriggered)
            connect(btn, &QAbstractButton::clicked, this,
                    [onTriggered]() { onTriggered(); });
        result = sized(btn, btn->preferredWidth(), M::kButtonH);
        break;
    }
    case PbControlKind::Label: {
        // 注記は行（Kind::Note）が自分で文字を持つ。ここでは作らない。
        result = new QWidget(m_content);
        break;
    }
    case PbControlKind::KeyInput: {
        // 2026-09-16: キー入力欄。columns が2以上なら横に並べ、値は欄ごとの
        // 仮想キーコードの並び（QVariantList）。1欄なら値は int。
        const int columns = qMax(1, entry.columns);
        const int gap = M::kKeyColumnGap;
        const int fieldW = M::kKeyW;
        const int m = M::kSurfaceMargin;
        const QVariant current = entryCurrentValue(entry);
        const QVariantList values = columns > 1 ? current.toList() : QVariantList{ current };

        auto *box = new QWidget(m_content);
        box->setAcceptDrops(true);
        auto inputs = std::make_shared<QList<IslandControls::KeyInput *>>();
        for (int i = 0; i < columns; ++i) {
            auto *input = new IslandControls::KeyInput(box);
            input->setGeometry(i * (fieldW + gap), 0, fieldW + m * 2, M::kTextH + m * 2);
            if (entry.keyName)
                input->setKeyNameFunction(entry.keyName);
            input->setKey(i < values.size() ? values[i].toInt() : 0);
            inputs->append(input);
        }
        if (onValueChanged) {
            for (IslandControls::KeyInput *input : std::as_const(*inputs)) {
                input->onKeyChanged = [onValueChanged, inputs, columns](int vk) {
                    if (columns == 1) {
                        onValueChanged(vk);
                        return;
                    }
                    QVariantList all;
                    for (IslandControls::KeyInput *k : std::as_const(*inputs))
                        all.append(k->key());
                    onValueChanged(all);
                };
            }
        }
        const int totalW = columns * fieldW + (columns - 1) * gap;
        box->setFixedSize(totalW + m * 2, M::kTextH + m * 2);
        result = sized(box, totalW, M::kTextH);
        break;
    }
    case PbControlKind::TextInput: {
        // 2026-09-16: 複数の欄を横に並べる（本体側だけ）。値は欄ごとの QString の並び。
        if (entry.columns > 1) {
            const int columns = entry.columns;
            const int gap = M::kKeyColumnGap;
            const int fieldW = (M::kTextW - gap * (columns - 1)) / columns;
            const int m = M::kSurfaceMargin;
            auto *box = new QWidget(m_content);
            box->setAcceptDrops(true);
            QList<QLineEdit *> edits;
            const QVariantList values = entryCurrentValue(entry).toList();
            for (int i = 0; i < columns; ++i) {
                auto *field = new IslandControls::TextField(box);
                field->setGeometry(i * (fieldW + gap), 0, fieldW + m * 2, M::kTextH + m * 2);
                QLineEdit *edit = field->edit();
                edit->setFocusPolicy(Qt::ClickFocus);
                edit->setText(i < values.size() ? values[i].toString() : QString());
                if (!spec.placeholder.isEmpty())
                    edit->setPlaceholderText(spec.placeholder);
                if (spec.maxLength > 0)
                    edit->setMaxLength(spec.maxLength);
                edits.append(edit);
            }
            if (onValueChanged) {
                for (QLineEdit *edit : std::as_const(edits)) {
                    connect(edit, &QLineEdit::editingFinished, this, [onValueChanged, edits]() {
                        QVariantList all;
                        for (QLineEdit *e : edits)
                            all.append(e->text());
                        onValueChanged(all);
                    });
                }
            }
            const int totalW = columns * fieldW + (columns - 1) * gap;
            box->setFixedSize(totalW + m * 2, M::kTextH + m * 2);
            result = sized(box, totalW, M::kTextH);
            break;
        }
        auto *field = new IslandControls::TextField(m_content);
        QLineEdit *edit = field->edit();
        edit->setText(entryCurrentValue(entry).toString());
        if (!spec.placeholder.isEmpty())
            edit->setPlaceholderText(spec.placeholder);
        if (spec.maxLength > 0)
            edit->setMaxLength(spec.maxLength);
        // 他のコントロールはNoFocusだが、テキスト入力にはフォーカスが必須。
        // ClickFocusにして「クリックした時だけ」キーボードフォーカスを取る
        // （Tab移動等で意図せずフォーカスを奪い、キャンバスのホットキーを
        // 妨げないように）。
        edit->setFocusPolicy(Qt::ClickFocus);

        if (onValueChanged) {
            // 確定はEnterまたはフォーカスアウト時（editingFinished）。
            // 1文字ごとの通知・保存はうるさいのでやらない。同じ値の
            // 再確定で二重通知しないよう、最後に確定した値をプロパティに
            // 覚えて比較する。
            edit->setProperty("pbLastCommitted", edit->text());
            connect(edit, &QLineEdit::editingFinished, this,
                    [onValueChanged, edit]() {
                        const QString text = edit->text();
                        if (edit->property("pbLastCommitted").toString() == text)
                            return;
                        edit->setProperty("pbLastCommitted", text);
                        onValueChanged(text);
                    });
        }
        result = sized(field, M::kTextW, M::kTextH);
        break;
    }
    }

    // 影・縁の光・フォーカスの輪が外形の外へ出る分だけ、行が外へ広げて置く
    if (result && spec.kind != PbControlKind::Label)
        result->setProperty("pbRowOutset", M::kSurfaceMargin);

    if (result && !spec.tooltip.isEmpty())
        result->setToolTip(spec.tooltip);
    return result;
}

int IslandSettingsPanel::preferredPanelHeight(int maxTotalHeight) const
{
    // 島上部の丸UI（ボタン列）分 + 上の隙間。
    const int scrollTop  = m_baseH + 8;
    const int bottomPad  = 10;
    const int contentH   = m_content->sizeHint().height();
    const int uncapped   = scrollTop + contentH + bottomPad;
    return qMin(uncapped, maxTotalHeight);
}

void IslandSettingsPanel::applyPanelGeometry(int width, int panelHeight)
{
    // 2026-09-16: 行は島の幅いっぱいに広がり、揃え線（setRowGuides）で並ぶ。
    setGeometry(0, 0, width, panelHeight);
    const int scrollTop = m_baseH + 8;
    const int bottomPad = 10;
    // 2026-09-15: 列は左寄せ。スクロール領域は島の幅いっぱいに取り、
    // スクロールバーを島の右端に沿わせる（ウィンドウのリサイズ時だけ変わる）。
    constexpr int kScrollLeft  = 8;
    constexpr int kScrollRight = 8;
    const QRect scrollRect(kScrollLeft, scrollTop,
                           qMax(0, width - kScrollLeft - kScrollRight),
                           qMax(0, panelHeight - scrollTop - bottomPad));
    if (m_scroll->geometry() != scrollRect)
        m_scroll->setGeometry(scrollRect);
    // 2026-09-16: 右端の揃え線は島の幅で決まるので、幅が変わったときだけ並べ直す
    // （開閉アニメ中は目標サイズで一度呼ばれるだけ）。
    if (m_panelWidth != width) {
        m_panelWidth = width;
        relayoutRows();
    }
    setVisibleWidth(width);
}

void IslandSettingsPanel::setVisibleWidth(int islandWidth)
{
    if (!m_tabBar)
        return;
    const int kTabHeight = static_cast<int>(IslandTabGoo::kTabHeight);
    const int w = islandWidth - m_tabLeft - m_tabRightSpace;
    // 畳んでいる途中など、タブが読めないほど狭いときは出さない。
    // 2026-09-18: 狭いときはアイコンのみ＋横スクロールになるので、
    // 「アイコン2枚ぶん」まで出しておく（それ以下は潰れて意味が無い）。
    constexpr int kMinTabBarWidth = 48;
    if (w < kMinTabBarWidth || m_categories.isEmpty() || m_tabBarSuppressed) {
        // isVisible() はパネル自体が隠れている間ずっと false なので、
        // 明示的な表示状態（isHidden）で見る（毎フレーム show/hide しない）。
        if (!m_tabBar->isHidden())
            m_tabBar->hide();
        return;
    }
    const int pad = IslandSettingsTabBar::kTabPad;
    const QRect r(m_tabLeft, (m_baseH - kTabHeight) / 2 - pad, w, kTabHeight + pad * 2);
    if (m_tabBar->geometry() != r)
        m_tabBar->setGeometry(r);
    if (m_tabBar->isHidden())
        m_tabBar->show();
}
