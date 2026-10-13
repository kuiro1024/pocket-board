#pragma once
#include "Blur.h"
#include <QFrame>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <QGraphicsOpacityEffect>
#include <QColor>
#include <QPushButton>
#include <QTimer>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QLabel>
#include <QPixmap>
#include <QComboBox>
#include <QCheckBox>
#include <cmath>
#include <functional>

#include "IslandPromptPanel.h"

class CanvasWidget;
class IslandSettingsPanel;
class IslandTabGooLayer;

QColor getWindowsAccentColor();
// 2026-09-16: アプリ全体のアクセントカラー。設定「Windowsのカラー設定を使用」が
// オンなら Windows のアクセントカラー、オフなら設定のカラーコード。
// 島だけでなく動画UI・フローモードなど、アクセントカラーを
// 使う所はすべてこれを通すこと（getWindowsAccentColor() を直接呼ぶと設定が
// 効かない）。レジストリ参照を伴うので、毎フレーム呼ぶ所ではキャッシュする。
QColor appAccentColor();

// 2026-09-16: 島のガラス（本体・丸ボタン・設定タブの液体アニメ）をGL側で描くか。
// 既定はGL。PB_ISLAND_GL_GLASS=0 で従来のQPainter描画へ戻せる（見比べ用）。
bool islandGlassOnGpu();

// ---- AnimatedButton ----
class AnimatedButton : public QPushButton
{
    Q_OBJECT
    Q_PROPERTY(float visualScale READ visualScale WRITE setVisualScale)
    Q_PROPERTY(float bounceScale READ bounceScale WRITE setBounceScale)

public:
    explicit AnimatedButton(const QString &color, const QString &iconType, QWidget *parent = nullptr);

    float visualScale() const { return m_visualScale; }
    void  setVisualScale(float v) { m_visualScale = v; update(); syncGlass(); }
    float bounceScale() const { return m_bounceScale; }
    void  setBounceScale(float v) { m_bounceScale = v; update(); syncGlass(); }

    // 2026-09-16: 円のガラス（落ち影・本体・縁の光）をGL側（BackdropBlur）へ
    // 登録し直す。見た目に関わる値が変わるたびに呼ぶ。島の不透明度も
    // 反映するので、島の paintEvent からも呼ばれる。GL版でなければ何もしない。
    void syncGlass();

    // アイコン種別を動的に変更できる。"+" のみ自前パス描画。
    // "x"→image/svg/exit.svg（閉じる）、"square"→fullscreen.svg（全画面）、
    // "-"→min.svg（最小化）、"sound0"〜"sound3"→ico_Sound_0〜3.svg
    // （マスターボリューム操作中の+ボタン表示用）はいずれも白でティントして描画。
    void setIconType(const QString &t) { m_iconType = t; update(); }
    QString iconType() const { return m_iconType; }
    void startAppear();
    void resetState();

    // 2026-09-13: 確認プロンプト表示中など「今は押させたくない」間、
    // 彩度と明度を落として押せないことを見た目で示す。
    // paintEvent() は isEnabled() を見ていない（島が畳まれている間は
    // そもそも見えないので必要が無かった）ため、専用のフラグを持つ。
    // setEnabled(false) と併用すること（こちらは見た目だけ）。
    void setDimmed(bool dimmed);
    bool isDimmed() const { return m_dimmed; }

    // 2026-09-15: 設定タブの分裂アニメ（IslandTabGooLayer）の間だけ、円の本体と
    // 落ち影を描かずアイコンだけにする。円は雫と一体の形としてレイヤー側が描く。
    void setBodyHidden(bool hidden) { if (m_bodyHidden != hidden) { m_bodyHidden = hidden; update(); syncGlass(); } }
    // アイコンを横へずらす量（雫に引っ張られて円が寄るのに合わせる）。
    void setIconOffset(qreal x) { if (!qFuzzyCompare(m_iconOffsetX + 1.0, x + 1.0)) { m_iconOffsetX = x; update(); } }
    // 画面上の円の半径（出現スケールとホバーの拡大を含む）。
    qreal bodyRadius() const { return 12.0 * m_visualScale * m_bounceScale; }

signals:
    // マスターボリューム操作(+ボタン ホイール)用。カーソルが実際に
    // ボタンへ入った/出たタイミングをDynamicIslandへ伝える
    // （ホバーバウンスアニメの発火可否=m_fullyAppearedに関わらず常に発火）。
    void hoverEntered();
    void hoverLeft();

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    static qreal physicsEasing(qreal t);
    // 円の色（グレーアウトを反映済み）。paintEvent と syncGlass で共有する。
    void glassColors(QColor &tint, QColor &shade) const;

    // 円のガラスのGL登録（所有者は島＝親ウィジェット）
    BackdropBlur::Surface m_glass;

    QString m_color;
    QString m_iconType;   // "+", "x", "square", "-"
    float   m_visualScale = 0.0f;
    float   m_bounceScale = 1.0f;
    bool    m_fullyAppeared = false;
    bool    m_dimmed = false;
    bool    m_bodyHidden = false;
    qreal   m_iconOffsetX = 0.0;

    QPropertyAnimation *m_appearAnim   = nullptr;
    QPropertyAnimation *m_hoverInAnim  = nullptr;
    QVariantAnimation  *m_hoverOutAnim = nullptr;
};

// ---- DynamicIsland ----
class DynamicIsland : public QFrame
{
    Q_OBJECT
    Q_PROPERTY(int islandWidth READ islandWidth WRITE setIslandWidth NOTIFY islandWidthChanged)

public:
    explicit DynamicIsland(QWidget *parent = nullptr);

    int  islandWidth() const { return m_islandWidth; }
    void setIslandWidth(int w);
    void prepareForFirstAnimation();
    void refreshAccentColor();
    void toggleSettingsPanel(CanvasWidget *canvas);
    bool isSettingsOpen() const { return m_settingsOpen; }

    // ---- 確認プロンプト（2026-09-13）----
    // 島から生える YES/NO の確認表示。プラグインzipドロップ時に出していた
    // QMessageBox::question() の置き換えで、以下が従来と異なる:
    //   - 非モーダル。答えは answered コールバックで返る（true=承認）
    //   - 表示中は設定パネルを閉じ、ウィンドウ操作3ボタンをグレーアウト
    //   - 表示中は DynamicIslandController が島を畳まない／消さない
    // 既にプロンプトが出ている間に呼ぶと、前のプロンプトは false 扱いで
    // 打ち切られる（キューイングは呼び出し側=MainWindowの責任）。
    void showPrompt(const IslandPromptContent &content,
                    std::function<void(bool)> answered);
    bool isPromptOpen() const { return m_promptOpen; }
    // 畳んだ状態の高さ（＝ピルの直径）。DynamicIslandController が島を
    // 畳むときの目標幅に使う。height() をそのまま使うと、パネルの閉じ
    // アニメーションがまだ終わっていない瞬間に畳み始めた場合、
    // 「幅＝途中の高さ」の大きな四角へ縮む事故になる。
    int baseHeight() const { return m_baseH; }
    // プロンプト表示中の島の幅。通常の展開幅(240)では文言が窮屈なため広げる。
    static int promptIslandWidth() { return 360; }

    // ---- 設定画面（2026-09-15: ウィンドウ全体へ展開）----
    // 設定を開くと島が幅・高さともウィンドウいっぱい（端に kSettingsInset
    // ずつ余白を残す）まで広がる。上端はもともとの島の位置(15px)のまま。
    static constexpr int kSettingsInset = 15;
    // 2026-09-16: 展開の最大サイズ。大きいウィンドウ（全画面など）で設定を
    // 開くと、島の本体の描画・キャッシュ・背後のブラーがウィンドウ大になり、
    // 30fps前後まで落ちていた。これより大きいウィンドウでは、島はこの大きさで
    // 上端中央に留まる（小さいウィンドウでは従来どおり端の余白まで広がる）。
    static constexpr int kSettingsMaxWidth  = 680;
    static constexpr int kSettingsMaxHeight = 480;
    // 設定の開閉アニメーション（幅＋高さ）が走っているか。
    // DynamicIslandController::tick() はこの間、島の幅に手を出さない
    // （通常の出し入れの幅アニメとぶつかるため）。
    bool isSettingsResizing() const;

    // 2026-09-12: 島が「引っ込む方向」に入っているか。畳み始めた瞬間に
    // true になり、出るときに false へ戻る。
    // 左上のラベル／リストは島に場所を譲って畳んでいるが、島が完全に
    // 消えるまで待ってから開き直すと、譲る必要が無くなってからもしばらく
    // 畳んだままに見える。畳み始めた時点で戻し始めたいので、不透明度が
    // 0になるのを待たずに判定できるこのフラグを見る
    // （CanvasWidget::topOverlayObstacleLeft()）。
    bool isRetracting() const { return m_retracting; }
    void setRetracting(bool retracting) { m_retracting = retracting; }

    // 2026-07-13: 設定パネルの高さはウィンドウの縦サイズに収まる範囲で
    // 自動計算される（収まらない分はパネル内スクロール）ため、パネル
    // 展開中にウィンドウがリサイズされたら高さを追従させる必要がある。
    // MainWindow::resizeEvent() から呼ばれる（アニメなしの即時追従。
    // リサイズ中にアニメを張り直すとがくつくため）。
    void notifyHostWindowResized();

    // ---- マスターボリューム(+ボタン ホイール操作) ----
    // +ボタンの当たり判定矩形（島ローカル座標）。MainWindow::wheelEventが
    // 島の位置と合わせてウィンドウ座標へ変換し、ホバー判定に使う。
    QRect settingsButtonRect() const { return QRect(btnSettings->pos(), btnSettings->size()); }
    // MainWindow::wheelEventから、+ボタン上でホイールした直後に呼ぶ。
    // value(0-100)に応じて島全体を一時的にシークバー状表示に切り替え、
    // +ボタンのアイコンを音量段階アイコンへ差し替える。操作が止まって
    // 一定時間経つと自動的に通常表示へ戻る。
    void notifyMasterVolumeChanged(int value);

    // ボタンへのアクセス
    AnimatedButton *btnSettings = nullptr;
    AnimatedButton *btnClose    = nullptr;
    AnimatedButton *btnMax      = nullptr;
    AnimatedButton *btnMin      = nullptr;

signals:
    void islandWidthChanged();
    // 2026-09-16: アクセントカラーが変わった（設定の変更・Windows側の変更）。
    // 島以外の部品（動画UI等）を描き直すきっかけに使う。
    void accentColorChanged(const QColor &color);
    void shortcutSettingsRequested();
    // 2026-09-15: 設定パネルの開閉（isSettingsOpen() が変わった瞬間）。
    // 開き始め／閉じ始めで発火する（アニメ完了は待たない）。MainWindow が
    // キャンバス上のUI（CanvasOverlayWidget）を隠す／戻すのに使う。
    void settingsOpenChanged(bool open);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    // 2026-09-22: 背後のぼかし＋GLのガラス板（島の本体）の登録。paintEvent と
    // resizeEvent から呼ぶ（DynamicIsland.cpp のコメント参照）。
    void syncBodyGlass();
    void updateMasterVolumeIcon(int value);
    void onMasterVolumeButtonHoverEntered();
    void onMasterVolumeButtonHoverLeft();

    // ---- 設定パネル（IslandSettingsPanel）関連 ----
    // 初回のみパネルを生成し、IslandControlRegistry::sectionsChanged への
    // 再構築接続とウォームアップ描画を行う。
    void ensureSettingsPanel();
    // パネル全体高さの上限（ウィンドウ縦サイズ - 島上マージン - 下マージン）。
    // 2026-09-15以降は確認プロンプトの高さ上限としてのみ使う。
    int settingsPanelMaxHeight() const;
    // 2026-09-15: 設定を開き切ったときの島のサイズ（ウィンドウ - 余白）。
    QSize settingsExpandedSize() const;
    // 島の幅と高さを同時に適用し、パネルの列を中央へ置き直す。
    void applySettingsIslandSize(const QSize &size);
    // 開いているパネルのサイズをウィンドウに合わせ直す（項目の増減・
    // ウィンドウのリサイズ時。アニメ中なら行き先だけ差し替える）。
    void refreshSettingsPanelGeometry();

    // ---- 確認プロンプト（2026-09-13）----
    // プロンプトを畳んで閉じ、保持しているコールバックを answer で呼ぶ。
    // コールバックは呼ぶ直前にメンバから取り出して空にする（コールバックの
    // 中から次のプロンプトが開かれても二重発火しないようにするため）。
    void finishPrompt(bool answer);
    // ウィンドウ操作3ボタン（閉じる/全画面/最小化）の無効化＋グレーアウト。
    // +ボタン（設定）も対象。プロンプト中は島ドラッグも止める。
    void setControlsSuppressed(bool suppressed);

    QColor m_accentColor;
    int    m_islandWidth;
    int    m_baseH = 42;

    // 島の背後(キャンバス)をぼかすための登録ハンドル。実際のぼかしは
    // CanvasWidget::paintGL()側のGLパスで行われる(設計はBlur.h参照)。
    BackdropBlur::Surface m_backdropBlur;

    // ---- マスターボリューム(+ボタン ホイール操作) ----
    // シークバー状オーバーレイの不透明度(0.0〜1.0)。0より大きい間、
    // paintEvent()がオーバーレイを描画する。ホイール操作中は1.0へ
    // フェードインし、+ボタンからカーソルが外れるまではそのまま
    // 維持し続ける（タイムアウトでは消さない）。外れたら0.0へ
    // フェードアウトする（btnSettings::hoverLeftシグナル経由）。
    // 型はdouble固定（QVariantAnimationはfloat型のQVariantに対する
    // 標準補間器を持たないため、floatだと値が全く補間されず、開始値の
    // ままアニメーションが進まない不具合になる。double/qrealなら
    // 標準対応している）。
    double  m_masterVolumeGestureAlpha {0.0};
    int     m_masterVolumeValue {50};
    QVariantAnimation      *m_masterVolumeFadeAnim = nullptr;
    // フェードアウト完了時に+ボタンのアイコンを戻すための単発接続。
    // 途中で再度ホバー/ホイールが入ってフェードインへ差し戻された場合、
    // 前回分の接続を確実に切ってから新しく繋ぎ直す（重複発火防止）。
    QMetaObject::Connection m_masterVolumeFadeFinishedConn;

    QVariantAnimation      *m_colorAnim   = nullptr;
    QGraphicsOpacityEffect *m_opacity     = nullptr;
    QVariantAnimation      *m_heightAnim  = nullptr; // 高さアニメ
    // 閉じアニメ完了時に設定パネルをhideするための単発接続。
    // stop()は途中停止ではfinishedを発火しないため、閉じアニメ中に
    // +ボタンが再度押され開き直されるとこの接続が残存し、「開きアニメの
    // 完了」でパネルがhideされてしまう（＋連打で設定項目が消えるバグ）。
    // 開く側では必ずこの接続を切ってからアニメを開始する。
    QMetaObject::Connection m_heightAnimFinishedConn;
    IslandSettingsPanel    *m_settingsPanel = nullptr; // 設定パネル（自動レイアウト版）
    bool                    m_settingsOpen  = false;
    // 2026-09-15: 設定の開閉アニメ（幅＋高さ）。値は進み具合 0.0〜1.0 で、
    // from→to のサイズを補間する（2つのアニメを別々に回すと、片方だけ先に
    // 終わって角が四角く見える瞬間ができるため1本にまとめる）。
    // 型はdouble（m_masterVolumeGestureAlpha のコメント参照）。
    QVariantAnimation      *m_settingsSizeAnim = nullptr;

    // 2026-09-16: 設定の開閉アニメ（幅＋高さ）の間だけ、設定パネルの代わりに
    // 「パネルを1枚に描いた画像」を置く（beginPanelSnapshot 参照）。
    // 島は半透明なので、サイズが変わるたびに島の範囲の子ウィジェット＝
    // 見えている設定の行が全部描き直されていた（文字の影つき描画×行数など）。
    // 画像なら毎フレーム1枚貼るだけで済む。
    QLabel *m_panelSnapshot = nullptr;
    void beginPanelSnapshot();
    void endPanelSnapshot();
    QSize                   m_settingsAnimFrom;
    QSize                   m_settingsAnimTo;
    // 2026-09-15: ＋ボタンからタブが液体のように分裂する演出（IslandTabGoo.h）。
    // 設定パネルと同時に初めて作る。
    IslandTabGooLayer      *m_tabGoo = nullptr;
    void ensureTabGoo();

    // ---- 確認プロンプト（2026-09-13）----
    IslandPromptPanel      *m_promptPanel = nullptr;
    bool                    m_promptOpen  = false;
    std::function<void(bool)> m_promptCallback;
    // 閉じアニメ完了時にプロンプトをhideするための単発接続
    // （m_heightAnimFinishedConn と同じ流儀。開き直しで確実に切る）。
    QMetaObject::Connection m_promptAnimFinishedConn;
    // 島が畳まれる方向に入っているか（isRetracting()参照）。
    bool m_retracting = true;
    bool                    m_buttonsInteractive = false;
    // 確認プロンプト表示中など、ボタンを押させない状態。setIslandWidth()の
    // 幅による自動有効化より優先される（幅が閾値をまたいでも復帰させない）。
    bool                    m_controlsSuppressed = false;

    bool   m_dragging     = false;
    // 2026-07-09 バグ修正: 島ドラッグでウィンドウ全画面を解除した際、
    // ちらつき防止のためフォーカス中バブルへのアスペクト再フィット
    // アニメーションを打ち切っている（mouseMoveEvent参照）。そのままだと
    // ウィンドウがロックサイズのまま余白付きのバブル全画面になってしまう
    // ため、ドラッグ終了時(mouseReleaseEvent)に再フィットをやり直す必要が
    // あることを示すフラグ。
    bool   m_pendingFocusAspectRefit = false;
    // ドラッグ開始時に「島の中のどこを掴んだか」を島ローカル座標で保持する。
    // ウィンドウが最大化⇔通常サイズどちらへ変化しても、この掴んだ点が
    // カーソルの真下にあり続けるようにウィンドウ位置を逆算するための基準点。
    QPoint m_dragStartPos;

    // ---- 計測（2026-09-14追加・[Island][stat]）------------------------
    //
    // 【なぜ要るか】DynamicIslandController::tick() は30Hzで回っており、
    // 末尾に「カーソル追従グローの位置が変わったら update()」という分岐が
    // ある。ところが現在の paintEvent() はカーソル位置を一切読んでいない
    // （追従グローはガラスの縁の固定表現へ置き換え済み。paintEvent()冒頭の
    // コメント参照）。つまりその update() は同じ絵を描き直しているだけの
    // 可能性が高い。しかも島は hide() されず不透明度0で隠れるだけなので、
    // 見えていない間もその領域が dirty になり、下のQOpenGLWidgetごと
    // 再合成されうる。
    //
    // 【2026-09-14 実測で分かったこと】
    //   - 島が畳まれている間: glowReq=561 に対して paints=3。つまり不透明度0の
    //     間は Qt(QGraphicsOpacityEffect) が描画自体を打ち切っており、
    //     update() は空振りしている。
    //   - 島が出ている間: paints=100 に対して afterGlow=38。グロー分岐由来は
    //     3分の1で、残りは別の要因（下のQOpenGLWidgetのpresentに連動した
    //     兄弟ウィジェットの再合成と思われる。CanvasOverlayのframesとほぼ一致）。
    //   - 設定パネル展開中: paint=6〜8ms/フレーム × 約50回/秒。
    //     同じ時間帯の [RenderSchedule] は avgPaint=0.02〜0.14ms なので、
    //     GLではなくこのQPainter描画が支配的。
    // → グロー分岐は「同じ絵を描き直すだけ」と確定したので削除した。
    //   次の狙いは paintEvent() の結果そのもののキャッシュなので、
    //   「見た目を決める値が前フレームと同じだった回数」を数えておく。
    //
    // 2秒ごとに次を出す:
    //   paints    paintEvent() が走った回数
    //   sameKey   そのうち「見た目を決める値が前回と完全に同じ」だった回数
    //             （= QPixmapキャッシュで丸ごと省けるはずの回数）
    //   hidden    不透明度がほぼ0のまま描いた回数
    // ---- 島の本体の描画結果のキャッシュ（2026-09-14）-------------------
    // paintEvent() の中身は幅・高さ・アクセント色・マスターボリューム表示
    // だけで決まる。外からの再合成（子ボタンの再描画／下のQOpenGLWidgetの
    // present）で毎秒25〜50回呼ばれても、絵が同じなら貼るだけで済ませる。
    // 詳しい経緯は DynamicIsland.cpp の paintEvent() のコメント。
    QPixmap m_bodyCache;
    quint64 m_bodyCacheKey = 0;
    qreal   m_bodyCacheDpr = 0.0;
    // 上のキャッシュのキー。paintEvent() の本体が新しく参照する状態を
    // 増やしたときは、必ずこちらにも足すこと。
    quint64 islandBodyCacheKey() const;

#ifndef NDEBUG
public:
    // 2秒窓を実時間で閉じるため、30Hzのtick()から呼ぶ。paintEvent()の中だけで
    // 判定すると、描画が止まっている間は窓が閉じず数字の分母が狂う。
    void noteIslandTick() { reportIslandStat(); }

private:
    QElapsedTimer m_statWindow;
    int     m_statPaints       = 0;   // paintEvent()が走った回数
    int     m_statPaintsSame   = 0;   // うち、絵が前回と同一だった回数
    int     m_statPaintsHidden = 0;   // 不透明度がほぼ0のまま描いた回数
    int     m_statBuilds       = 0;   // キャッシュを作り直した回数
    double  m_statBuildMsSum   = 0.0;
    // 1ビルドの内訳（[Island][build]）。どこを削るかを数字で決めるため。
    double  m_statBuildBaseMs    = 0.0;  // 下地＋音量シークバー
    double  m_statBuildFrostMs   = 0.0;  // フロストの全面fillRect
    double  m_statBuildPlusMs    = 0.0;  // 加算の縁光 4本 = 48 drawPath
    double  m_statBuildShadeMs   = 0.0;  // 乗算の側面翳り 8 drawPath
    double  m_statBuildContourMs = 0.0;  // 輪郭線 1 drawPath
    double  m_statBuildEndMs     = 0.0;  // QPainter::end()
    double  m_statBuildOutlineMs = 0.0;  // 帯の多角形12枚の生成
    int     m_statBuildStrokes   = 0;    // 実際に走った drawPath の本数
    double  m_statPaintMsSum   = 0.0;
    double  m_statPaintMsMax   = 0.0;
    quint64 m_statContentKey     = 0;
    bool    m_statContentKeyInit = false;
    void    reportIslandStat();
#endif
};

// ---- DynamicIslandController ----
class DynamicIslandController : public QObject
{
    Q_OBJECT
public:
    explicit DynamicIslandController(DynamicIsland *island, QWidget *window, CanvasWidget *canvas = nullptr);
    void tick();

    // ---- 確認プロンプト（2026-09-13）----
    // カーソルがトリガー領域(中央上部300x80)の外でも島を出し、幅を
    // DynamicIsland::promptIslandWidth() まで広げる。プロンプトは
    // ドロップをきっかけに出るため、ホバー由来の通常経路では出せない。
    void openPromptView();
    // 通常幅へ戻し、以降はホバー由来の通常制御へ返す。カーソルが
    // トリガー領域外なら、そのまま畳んで消える。
    void closePromptView();

private:
    void showIsland();
    void hideIsland();
    void startFadeOut();

    DynamicIsland      *m_island    = nullptr;
    QWidget            *m_window    = nullptr;
    // 2026-07-11: 右島(LayerBar)と同様、キャンバス移動中(カメラパン/
    // バブル・フレームのつかみ移動)は島を出さないようにするための参照。
    CanvasWidget        *m_canvas    = nullptr;
    bool                m_showing   = false;
    bool                m_animLock  = false;
    // 2026-07-12: 設定パネルを開いたままキャンバス操作（カメラパン等）を
    // した場合に、島を畳む(hideIsland())のではなくオパシティだけを
    // 落として一時的に隠すためのフラグ。畳んでしまうと設定パネルの
    // レイアウトが島の幅アニメーションに巻き込まれて崩れて見える不具合が
    // あったため、この間は m_showing/m_settingsOpen 等の状態を一切
    // 変えずにフェードだけで隠す。
    bool                m_manipulationFadeActive = false;
    // 2026-07-12: 上のフェード復帰を「操作が完全に途切れてから0.5秒後」に
    // 遅らせるためのシングルショットタイマー。慣性で画面がまだ動いている
    // 最中でも、ドラッグそのもの（isCanvasBeingManipulated()＝
    // m_activeModeがcamera_pan等である間）が終わった時点でこのタイマーを
    // 開始する（＝慣性の終了は待たない）。0.5秒以内に操作が再開したら
    // stop()して仕切り直す。
    QTimer             *m_manipulationFadeInTimer = nullptr;

    // 2026-09-14: ここにあった m_lastGlowCursorPos（カーソル追従グロー用の
    // 前回位置）は、追従グローの廃止に伴い tick() の分岐ごと削除した。
    // 経緯は DynamicIsland.cpp の tick() 末尾のコメントを参照。

    QPropertyAnimation *m_islandAnim = nullptr;
    QPropertyAnimation *m_fadeAnim   = nullptr;
    QTimer             *m_lockTimer  = nullptr;
};
