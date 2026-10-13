#pragma once

#include <QAbstractButton>
#include <QElapsedTimer>
#include <QStringList>
#include <QWidget>

#include <functional>

class QLineEdit;
class QTimer;

// ============================================================
//  IslandControls（2026-09-16）
//
//  島の設定パネル（IslandSettingsPanel）の行の右側に置く操作部品。
//  Qt標準のウィジェット＋QSSをやめ、角のない形を自前で描く。
//  寸法・動きの値は試作ページ「島コントロールラボ」（island-controls-v1）の
//  既定値をそのまま移したもの（Metrics）。
//
//  2026-09-16: 色は Windows のアクセントカラー（島と同じ）から取り、面は島・
//  ＋ボタン・着地したタブと同じガラスの層（縁の光・翳り・輪郭）で描く。
//  浮いた面＝ボタン・選択・丸ボタン、沈んだ面＝溝・入力欄・スイッチの台。
//  下地が透けるよう、＋ボタンやタブより少し透明にしてある。
//
//  動きはすべてバネ（減衰振動）。アニメ中だけタイマーが回り、落ち着いたら止まる
//  （SpringTicker）。止まっている部品は再描画しない。
//
//  【更新頻度】（2026-09-16）
//    動いていない       … 0Hz（タイマー停止）
//    ホバーのフェードだけ … 15Hz
//    メニューの選択帯の追従 … 30Hz
//    押下・切替・開閉・スライダーのドラッグなど … 60Hz
//    0/1で切り替わるだけの見た目（SpinBoxのホバー中の左右の入れ替わり等）は
//    タイマーを回さず update() を1回だけ。
//  1フレームの間に何種類動いていても、一番速いものに合わせて1回だけ描く。
// ============================================================
namespace IslandControls {

// ---- ラボの数値（論理px・ms・Hz）----
namespace Metrics {
    // 共通
    constexpr double kHoverHz        = 7.0;
    constexpr double kHoverLight     = 0.06;   // ホバーで白を重ねる量
    constexpr double kDisabledOpacity = 0.45;
    constexpr int    kCtrlFontPx     = 11;
    // 影・縁の光が部品の外形からはみ出す分。部品のウィジェットはこの分だけ外へ広く、
    // 行（IslandSettingsRow）は "pbRowOutset" でその分を外側へ広げて置く。
    constexpr int    kSurfaceMargin  = 4;
    constexpr int    kValueFontPx    = 12;     // SpinBox・Slider の数値

    // Toggle
    constexpr int    kToggleW        = 40;
    constexpr int    kToggleH        = 22;
    constexpr double kToggleInset    = 2.0;
    constexpr double kToggleStretch  = 5.0;    // 押している間つまみが伸びる量
    constexpr double kToggleOnAlpha  = 0.45;   // ONの白の濃さ（ガラスらしく少し薄め）
    constexpr double kToggleHz       = 5.5;
    constexpr double kToggleZeta     = 0.72;

    // SpinBox
    constexpr int    kSpinW          = 96;
    constexpr int    kSpinH          = 22;
    constexpr double kSpinBtnD       = 18.0;
    constexpr double kSpinBtnInset   = 2.0;
    constexpr double kSpinArrow      = 4.0;
    constexpr double kSpinPressScale = 0.86;
    constexpr double kSpinHz         = 9.0;
    constexpr double kSpinZeta       = 0.55;
    constexpr double kSpinSlideDist  = 7.0;
    constexpr int    kSpinSlideMs    = 170;
    constexpr int    kSpinRepeatDelayMs    = 400;
    constexpr int    kSpinRepeatIntervalMs = 70;

    // Slider
    constexpr int    kSliderTrackW   = 120;
    constexpr double kSliderTrackH   = 5.0;
    constexpr double kSliderKnobD    = 16.0;
    constexpr double kSliderPressGrow = 1.12;
    constexpr double kSliderFillAlpha = 0.42;
    constexpr int    kSliderValueW   = 32;
    constexpr int    kSliderValueGap = 8;
    constexpr double kSliderHz       = 6.0;
    constexpr double kSliderZeta     = 0.8;

    // Choice
    constexpr int    kChoiceH        = 22;
    constexpr int    kChoiceMinW     = 120;
    constexpr int    kChoiceMaxW     = 180;
    constexpr double kChoicePadL     = 12.0;
    constexpr double kChoiceChevron  = 3.0;
    constexpr double kMenuRadius     = 11.0;
    constexpr int    kMenuItemH      = 24;
    constexpr double kMenuPad        = 4.0;
    constexpr double kMenuDark       = 0.38;
    constexpr int    kMenuOpenMs     = 180;
    constexpr int    kMenuCloseMs    = 120;
    constexpr double kMenuOpenScale  = 0.92;
    constexpr double kMenuHighlightHz   = 10.0;
    constexpr double kMenuHighlightZeta = 0.85;

    // Button
    constexpr int    kButtonH        = 22;
    constexpr int    kButtonMinW     = 120;
    constexpr int    kButtonMaxW     = 180;
    constexpr double kButtonPadX     = 16.0;
    constexpr double kButtonPressScale = 0.95;
    constexpr double kButtonPressDark  = 0.12;
    constexpr double kButtonHz       = 7.0;
    constexpr double kButtonZeta     = 0.6;

    // TextInput
    constexpr int    kTextW          = 160;
    constexpr int    kTextH          = 22;
    constexpr double kTextPadX       = 10.0;
    constexpr double kTextRingW      = 1.5;
    constexpr double kTextRingAlpha  = 0.6;
    constexpr double kTextRingGap    = 1.0;
    constexpr double kTextFocusHz    = 6.0;
    // 光の輪が欄の外にはみ出す分。行（IslandSettingsRow）はこの分だけ部品を外へ広げて置く。
    constexpr int    kTextOutset     = kSurfaceMargin;

    // KeyInput（見た目は TextInput と同じ）
    constexpr int    kKeyW           = 58;     // 1欄の幅
    constexpr int    kKeyColumnGap   = 6;      // 欄を並べるときの間隔
}

// 仮想キーコードの既定の表示名（Ctrl / Shift / A / 5 / F9 / Space など）。
// 本体は Windows のキー名を使う keyName を別に渡す（IslandControlEntry::keyName）。
QString defaultKeyName(int virtualKey);

// 島の色（Windows のアクセントカラー）。変わると生きている部品をすべて描き直す。
// DynamicIsland が色を読んだとき・色のアニメ中に呼ぶ。
void setAccentColor(const QColor &color);

// 減衰振動のバネ。step() は動いている間 true を返す。
struct Spring
{
    double x = 0.0;
    double v = 0.0;
    bool step(double target, double hz, double zeta, double dt);
    void snap(double value) { x = value; v = 0.0; }
};

// 1フレームで必要な更新頻度。大きいほど速い（合成は std::max）。
enum class Rate { Idle = 0, Hover = 1, Half = 2, Full = 3 };
inline Rate operator|(Rate a, Rate b) { return static_cast<int>(a) > static_cast<int>(b) ? a : b; }
inline Rate &operator|=(Rate &a, Rate b) { a = a | b; return a; }
// バネが動いていれば r、止まっていれば Idle
inline Rate rateIf(bool moving, Rate r) { return moving ? r : Rate::Idle; }

// アニメ中だけ回るタイマー。step(dt秒) が返した頻度で回り、Idle で止まる。
class SpringTicker
{
public:
    SpringTicker(QWidget *owner, std::function<Rate(double)> step);
    ~SpringTicker();
    SpringTicker(const SpringTicker &) = delete;
    SpringTicker &operator=(const SpringTicker &) = delete;
    // 動き出すきっかけ。止まっていれば今すぐ1回描いてタイマーを起こす。
    // 回っていれば、want の方が速いときだけ間隔を詰める（描くのは次の tick）。
    void kick(Rate want = Rate::Full);
private:
    static int intervalFor(Rate r);
    QWidget *m_owner;
    QTimer *m_timer;
    QElapsedTimer m_clock;
    std::function<Rate(double)> m_step;
};

// ---------------- Toggle（スライドスイッチ）----------------
class Toggle : public QAbstractButton
{
public:
    explicit Toggle(QWidget *parent = nullptr);
    // アニメなしで状態を合わせる（構築時）
    void setCheckedImmediate(bool on);
    // 通知しない（外からの連動用）。つまみはバネで動く。
    void setCheckedSilently(bool on);
    QSize sizeHint() const override { return { Metrics::kToggleW, Metrics::kToggleH }; }
protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    Rate stepAnim(double dt);
    Spring m_on, m_press, m_hover;
    bool m_hovered = false;
    SpringTicker m_ticker;
};

// ---------------- Button（細長い丸ボタン）----------------
class Button : public QAbstractButton
{
public:
    explicit Button(const QString &text, QWidget *parent = nullptr);
    // 文字が収まる幅（上下限あり）
    int preferredWidth() const;
protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    Rate stepAnim(double dt);
    Spring m_press, m_hover;
    bool m_hovered = false;
    SpringTicker m_ticker;
};

// ---------------- SpinBox（中央に値・左右に丸ボタン）----------------
class SpinBox : public QWidget
{
public:
    SpinBox(int minValue, int maxValue, int step, int value, QWidget *parent = nullptr);
    int value() const { return m_value; }
    void setValue(int value);   // 通知しない（外からの連動用）
    // ボタン操作で値が変わったとき
    std::function<void(int)> onValueChanged;
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    enum Side { None, Left, Right };
    Side sideAt(const QPointF &pos) const;
    void stepBy(int dir);
    void stopRepeat();
    Rate stepAnim(double dt);

    int m_min, m_max, m_step, m_value, m_prev;
    int m_slideDir = 1;
    QElapsedTimer m_slideClock;
    Side m_pressed = None;
    Side m_hoverSide = None;
    Spring m_pressL, m_pressR, m_hover;
    QTimer *m_repeat = nullptr;
    SpringTicker m_ticker;
};

// ---------------- Slider ----------------
class Slider : public QWidget
{
public:
    Slider(int minValue, int maxValue, int step, int value, QWidget *parent = nullptr);
    int value() const { return m_value; }
    // 通知しない（外からの連動用）。つまみはバネで移動、操作中なら何もしない。
    void setValue(int value);
    // 値の確定。ドラッグ中は呼ばず、離したときに1回（設定ファイルへの過剰な書き込みを防ぐ）。
    std::function<void(int)> onValueChanged;
    static int totalWidth() { return Metrics::kSliderValueW + Metrics::kSliderValueGap + Metrics::kSliderTrackW; }
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    double knobX0() const;
    double knobX1() const;
    double fractionOf(int value) const;
    int valueAtX(double x) const;
    Rate stepAnim(double dt);

    int m_min, m_max, m_step, m_value;
    int m_valueAtPress = 0;
    bool m_pressed = false;
    bool m_dragging = false;
    bool m_dragDirty = false;   // ドラッグでつまみが動いた（次の tick で描く）
    double m_dragOffset = 0.0;
    Spring m_knob, m_press, m_hover;
    bool m_hovered = false;
    SpringTicker m_ticker;
};

// ---------------- Choice（プルダウン）----------------
class ChoicePopup;
class Choice : public QWidget
{
public:
    explicit Choice(QWidget *parent = nullptr);
    void addItem(const QString &label, const QString &id);
    int count() const { return m_labels.size(); }
    int findData(const QString &id) const { return m_ids.indexOf(id); }
    QString itemData(int index) const { return m_ids.value(index); }
    int currentIndex() const { return m_current; }
    void setCurrentIndex(int index);          // 通知しない
    int preferredWidth() const;
    // メニューで選び直したとき
    std::function<void(int)> onCurrentIndexChanged;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    friend class ChoicePopup;
    Rate stepAnim(double dt);
    void choose(int index);

    QStringList m_labels;
    QStringList m_ids;
    int m_current = -1;
    bool m_pressed = false;
    bool m_hovered = false;
    Spring m_press, m_hover;
    ChoicePopup *m_popup = nullptr;
    SpringTicker m_ticker;
};

// ---------------- TextInput ----------------
// 中の入力は QLineEdit（日本語入力・選択・コピーをそのまま使う）。見た目だけ自前。
class TextField : public QWidget
{
public:
    explicit TextField(QWidget *parent = nullptr);
    QLineEdit *edit() const { return m_edit; }
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
private:
    Rate stepAnim(double dt);
    QLineEdit *m_edit = nullptr;
    Spring m_focus, m_hover;
    bool m_hovered = false;
    SpringTicker m_ticker;
};

// ---------------- KeyInput ----------------
// 見た目は TextField と同じ。欄を押してからキーを1つ押すと、そのキーが入る
// （文字は打てない・日本語入力も効かない）。キーを1つ入れた時点で選択が外れる。
// Backspace で空にして選択解除、Esc で何も変えずに選択解除
// （2026-09-17: Delete はキーとして入る。以前は Delete でも空になっていた）。
// 値は Windows の仮想キーコード（空欄=0）。左右のある修飾キーは区別しない。
class KeyInput : public TextField
{
public:
    explicit KeyInput(QWidget *parent = nullptr);
    int key() const { return m_key; }
    void setKey(int virtualKey);                        // 通知しない
    void setKeyNameFunction(std::function<QString(int)> fn);
    // キーを押して値が変わったとき
    std::function<void(int)> onKeyChanged;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    int m_key = 0;
    std::function<QString(int)> m_keyName;
};

} // namespace IslandControls
