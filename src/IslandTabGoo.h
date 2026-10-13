#pragma once

#include <QPainterPath>
#include <QRectF>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "Blur.h"

#include <functional>

class AnimatedButton;
class QVariantAnimation;

// ============================================================
//  IslandTabGoo（2026-09-15）
//
//  設定を開くと、島の＋ボタンから液体のように雫が伸びて首がちぎれ、
//  上端の帯のタブになる。閉じると逆にタブが＋ボタンへ吸い込まれる。
//
//  数式とパラメータは試作ページ「島タブ分裂ラボ」（goo-v1）で調整したものを
//  そのまま移植している。仕組み:
//    - ＋ボタン（円）とタブ（横向きカプセル）の距離場を smin で合成する
//      → 近い間は首でつながり、離れるとちぎれる（表面張力に見える）
//    - 距離場の等高線を marching squares で拾い、中点を通る quadTo で
//      QPainterPath にする
//    - 動きはすべて時刻の関数（減衰振動の解析解）。フレーム間の状態を
//      持たないので、途中で止めても逆向きへ切り替えても破綻しない
//
//  ライティングは PocketBoard の現状（AnimatedButton のガラス表現）に合わせる:
//    - 雫と＋ボタン … 浮き出た面（上から光、左右と下を沈める、加算の縁光）
//    - 着地したタブ … 逆に島の面へ沈み込んだ面（上の内側に影、下の縁に光）
// ============================================================
namespace IslandTabGoo {

// 1フレームぶんの形と表現（島ローカル座標・論理px）
struct Frame
{
    double L = 0.0;      // 雫（タブ）の左端
    double R = 0.0;      // 雫（タブ）の右端
    double hh = 0.0;     // 雫（タブ）の高さの半分
    double bx = 0.0;     // ＋ボタンが引っ張られて横へ寄る量
    double rScale = 1.0; // ＋ボタンのふくらみ（吸い込み時）
    double sink = 0.0;   // 0=浮き出た面 / 1=沈み込んだ面
    double text = 0.0;   // タブの文字などの不透明度
    // 右端の丸みの半径 ÷ 左端の丸みの半径（1=普通のカプセル）。
    // 閉じる後半で、＋ボタン側は太いまま右へ向かってすぼまる形にする。
    double taper = 1.0;
};

// 帯の中の固定の位置関係
struct Layout
{
    double buttonCx = 21.0;  // ＋ボタンの中心
    double buttonCy = 21.0;
    double buttonR  = 12.0;  // ＋ボタンの半径（ホバーの拡大込み）
    // ＋ボタンの落ち影の基準半径（12 × 出現スケール）。AnimatedButton は影だけ
    // ホバーの拡大に追従させていないので、本物と揃えるために別に持つ。
    double buttonShadowBase = 12.0;
    double tabLeft  = 47.0;  // 開き切ったタブの左端
    double tabRight = 339.0; // 開き切ったタブの右端（島の幅に追従して毎フレーム変わる）
};

constexpr double kOpenMs  = 1100.0;
// 2026-09-15: 閉じるアニメは開くときと同じ速さに揃えた。形の計算は試作
// （goo-v1、700ms で調整）の時間軸のまま、kCloseTimeScale 倍に引き延ばして再生する。
constexpr double kCloseTimeScale = 1100.0 / 700.0;
constexpr double kCloseMs = 700.0 * kCloseTimeScale;
constexpr double kTabHeight = 26.0;

// 開く途中で首がちぎれる時刻（ms）。＋ボタンの戻り（recoil）の起点。
double openPinchMs(const Layout &layout);
Frame evalOpen(double tauMs, const Layout &layout, double pinchMs);
Frame evalClose(double tauMs, const Layout &layout);
// 開き切った状態（静止しているタブの描画用）
Frame restingOpen(const Layout &layout);

// 距離場 → 輪郭パス
QPainterPath buildPath(const Frame &frame, const Layout &layout);
// 静止しているタブ（カプセル）のパス
QPainterPath capsulePath(const QRectF &rect);

// 形に＋ボタンの円が含まれるときの円の情報。含まれるなら、円の部分は
// AnimatedButton::paintEvent と同じ放射グラデーションで描く（見た目を本物に揃える）。
struct ButtonDisc
{
    QPointF center;
    double radius = 12.0;
    double shadowBase = 12.0;
};

// 面の描画。gradX0→gradX1 の横方向で「ボタン側」から「雫側」へ移る。
// disc が nullptr なら全体を雫側（静止しているタブ）として描く。
void paintSurface(QPainter &p, const QPainterPath &path, double sink,
                  double gradX0, double gradX1, const ButtonDisc *disc = nullptr);
// paintSurface に渡すグラデーション区間（ボタンの縁 → 雫の左端）
void gradientSpan(const Frame &frame, const Layout &layout, double &x0, double &x1);
// ---- タブ1枚の中身（2026-09-18）----
// アイコン（SVG。白でティント）＋ 文字。幅が足りないときは文字を落として
// アイコンだけにする。
struct TabItem
{
    QString title;
    QString iconPath;   // 例 ":/icons/tab_general.svg"（空ならアイコンなし）
};

// タブ1枚の寸法（論理px）
constexpr double kTabIconPx  = 15.0;  // アイコンの一辺
constexpr double kTabIconGap = 6.0;   // アイコンと文字の間
constexpr double kTabTextPad = 11.0;  // 文字ありのときの左右の余白
constexpr double kTabIconPad = 9.0;   // アイコンのみのときの左右の余白
constexpr double kTabFontPx  = 11.0;

// ---- タブの並びの寸法（2026-09-18）----
//
//  幅の決め方は3段階:
//    1. 「アイコン＋文字」で必要な幅を測る。カプセルに収まるなら余りを
//       各タブへ按分して広げる（＝従来の等分割と同じ見た目になる）
//    2. 収まらなければ 全タブ一括で アイコンのみ へ落とす
//    3. アイコンのみでも収まらなければ横スクロール（scrollable=true）
//
//  測るだけで状態を持たないので、開閉アニメ中に毎フレーム呼んでも安全。
struct TabMetrics
{
    QVector<double> widths;     // 各タブの幅（合計 = total）
    double total      = 0.0;
    double available  = 0.0;    // カプセルの内幅
    bool   iconOnly   = false;
    bool   scrollable = false;

    double maxScroll() const { return qMax(0.0, total - available); }
    // index 番目のタブの左端（スクロール0基準）
    double left(int index) const;
};

// titles/icons から寸法を測る。capsuleWidth はカプセルの外形の幅。
TabMetrics measureTabs(const QVector<TabItem> &tabs, double capsuleWidth);

// カプセルローカルのx座標 → タブの番号（無ければ -1）
int tabIndexAt(const TabMetrics &m, double scrollX, double xInCapsule);

// タブの中身（選択中の塗り・区切り線・アイコン・文字）。capsule はタブの外形。
// scrollX は横スクロール量（0〜maxScroll）。
void paintTabContents(QPainter &p, const QRectF &capsule, const QVector<TabItem> &tabs,
                      const TabMetrics &m, double scrollX,
                      int current, int hover, double alpha);

// 2026-09-22: 静止しているタブ（沈み込んだカプセル）の面を、分裂アニメと同じ
// GLシェーダー（Goo）で描くための登録値。capsule は登録する矩形の左上を
// 原点とした座標（setShape に渡す矩形のローカル座標。島の座標ではない）。
// アニメの最後のフレーム（GL）から静止用のタブ（以前はQPainter）へ引き継ぐ
// 瞬間に描き手が変わり、タブの面の濃さが一瞬／恒常的に変わって見えていた
// （QPainter版とシェーダー版は近似が違ううえ、GL側の古い絵が1フレーム残ると
// 二重に重なって濃くなる）。静止中もGLで描けば描き手が変わらない。
// ＋ボタンの円は含めない（paintSurface(disc=nullptr) 相当）。
BackdropGlassPlateStyle sunkCapsuleGlass(const QRectF &capsule);

} // namespace IslandTabGoo

// ============================================================
//  IslandTabGooLayer
//
//  分裂／吸い込みのアニメ中だけ表示する、島の上端の帯ぶんのレイヤー。
//  島の子で、設定パネルより上・島のボタンより下に置く。
//  アニメ中は ＋ボタンの本体（円）を隠してアイコンだけにし、円はこちらが
//  雫と一体の形として描く。終わったら元へ戻す。
//
//  フレーム更新への配慮:
//    - 描き直すのはこのレイヤー（島の幅 × 42px）だけ。島の本体は
//      キャッシュ済みの絵が貼られるだけで描き直されない。
//    - 表示するのはアニメ中だけ。止まっている間のタブは
//      IslandSettingsTabBar が同じ関数で描く。
// ============================================================
class IslandTabGooLayer : public QWidget
{
public:
    enum class Mode { Idle, Open, Close };

    IslandTabGooLayer(AnimatedButton *button, int baseHeight, QWidget *parent);

    // タブの位置（IslandSettingsPanel::setTabBarInsets と同じ値）
    void setTabInsets(int left, int rightSpace);
    // 文字の中身（タイトル一覧と選択中）を問い合わせる
    std::function<void(QVector<IslandTabGoo::TabItem> &tabs, int &current)> tabContentsProvider;
    // アニメが最後まで進んだ（途中で stop() された場合は呼ばない）
    std::function<void(Mode finished)> onFinished;

    void startOpen();
    void startClose();
    void stop();
    Mode mode() const { return m_mode; }
    bool isRunning() const { return m_mode != Mode::Idle; }

    // 2026-09-16: 形と面（paintSurface相当）はGL側（BackdropBlur の Goo）で描く。
    // 形・島の不透明度が変わるたびに登録し直す。GL版でなければ何もしない。
    // ウィジェット側（paintEvent）はタブの文字だけを描く。
    void syncGlass();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    IslandTabGoo::Layout currentLayout() const;
    IslandTabGoo::Frame currentFrame() const;
    void start(Mode mode);
    void onTick(double tauMs);
    void finish();

    AnimatedButton    *m_button = nullptr;
    QVariantAnimation *m_anim = nullptr;
    Mode   m_mode = Mode::Idle;
    double m_tau = 0.0;
    double m_pinchMs = 0.0;
    int    m_baseH = 42;
    int    m_tabLeft = 47;
    int    m_tabRightSpace = 121;

    BackdropBlur::Surface m_glass;   // 所有者は島（親ウィジェット）
};
