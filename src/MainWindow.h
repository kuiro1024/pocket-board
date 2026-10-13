#pragma once
#include <QWidget>
#include <QTimer>
#include <QPropertyAnimation>
#include <QSystemTrayIcon>
#include <QStringList>
#include <QVariant>
#include <functional>
#include <windows.h>
#include "CanvasWidget.h"
#include "DynamicIsland.h"
#include "ResizeHandle.h"

// 2026-07-19: レイヤー機能を一旦切り離し（src/old/参照）。復活させる場合は
// 下のコメントを外す。
// class LayerBar;

class QCloseEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QMoveEvent;
class QShowEvent;
class QFrame;
class QLabel;
class CanvasOverlayWidget;

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    // ---- ウィンドウの最小サイズ（2026-09-16）----
    // 島を出したときに、左上のラベルが最小表示（ロゴと丸い台だけ）で島と
    // ぶつからずに並ぶ幅を下限にする。以前は動画の縦横比に合わせたリサイズで
    // 200px まで縮められ、1:1 の動画だと島（展開幅 240px）よりウィンドウが
    // 狭くなっていた。
    //   ラベルの丸い台 … 左余白 12 ＋ 直径 32（アイコン20＋上下余白6×2）→ 右端 44
    //   島と台の隙間   … 10（CanvasWidget の kIslandLabelGap と同じ）
    //   島の展開幅     … 240（中央寄せ）
    //   → (幅 − 240) / 2 ≥ 44 + 10  ⇒  幅 ≥ 348
    // 高さは極端に横長の動画でも島・ラベル・再生バーが重ならない程度。
    static constexpr int kMinWindowWidth  = 348;
    static constexpr int kMinWindowHeight = 150;
    // 2026-09-18: 上の kMinWindow* は「動画の縦横比に合わせて縮めるとき」の
    // 絶対下限。比率ロックが無い普通のリサイズでは、右下ハンドルが従来から
    // 使っていた下記の下限を使う。以前は右下ハンドルだけがこの値を持っていて
    // 「どの辺からでもリサイズ」ON のときだけ 348x150 まで縮められてしまい、
    // 操作経路によって下限が食い違っていた。
    static constexpr int kMinFreeWindowWidth  = 350;
    static constexpr int kMinFreeWindowHeight = 250;

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // 動画のアスペクト比に合わせてウィンドウをリサイズ（アニメーション付き）
    // baseSize: サイズ計算の基準にする「現在のウィンドウサイズ」の代わり。
    // 無効値(QSize())のままなら従来通り実際の現在ウィンドウサイズを使う。
    // 2026-07-17追記: 連続再生中に動画のアスペクト比が何度も変わるケースで、
    // 毎回「直前（既にアスペクト調整済み）のウィンドウサイズ」を基準に
    // フィットし直すと、調整が積み重なってウィンドウがどんどん縮んでいく
    // 問題があった。フォーカス開始時点の元のウィンドウサイズを常に
    // baseSizeとして渡すことで、常に同じ基準からの最小変化でフィットする
    // ようにできる（CanvasWidget::refitFocusedWindowForAspectChange()参照）。
    void animateToAspect(double aspectW, double aspectH, int durationMs = 400,
                          const QSize &baseSize = QSize());
    // 2026-07-15: animateToAspect()と全く同じ計算だけを行い、実際には
    // リサイズしない（アニメーションを開始しない）版。CanvasWidget側で
    // カメラのパン/ズームと同じ進行度(t)でウィンドウジオメトリも補間したい
    // 場合に、まず目標ジオメトリだけを取得するために使う。
    QRect computeAspectTargetGeometry(double aspectW, double aspectH,
                                       const QSize &baseSize = QSize()) const;
    // animateToAspect()冒頭のリサイズハンドルへのアスペクト比ロックだけを
    // 単独で呼びたい場合用（上のcomputeAspectTargetGeometry()と組み合わせて
    // 独自のアニメーションを組むときに使う）。
    void setAspectLockForFocus(double aspectW, double aspectH);
    void animateToGeometry(const QRect &geom, int durationMs = 400);
    void clampGeometryToScreen(QRect &geom) const;
    void setWindowSettingsSaveSuspended(bool suspended);
    // 【注意・ルール】このアニメーションは animateToAspect()/animateToGeometry()
    // 両方から共有される「使い回し」のQPropertyAnimationです。呼ぶたびに
    // stop()してから新しい値で再start()するため、finished()に直接
    // connect()して「完了時にやりたい処理」を仕込む場合、前回つないだ
    // 接続を切らずに放置すると、そのラムダが宙に浮いたまま残り、後で
    // 無関係なタイミング（別のフォーカス操作等）でresizeAnimが完了した際に
    // 一緒に誤発火してしまいます（stop()はfinished()を発火しないため、
    // 自然完了を待つ形の後始末が正しく動かなくなる）。
    // 2026-07-08に実際にこれが原因で「バブルを交互に全画面化すると、
    // 見た目は全画面になっていないのに内部的にはフォーカス中と誤判定される」
    // 不具合が発生した（CanvasWidget::focusItem()/unfocusItem()参照）。
    // このアニメーションのfinished()に新しくconnect()する場合は、必ず
    // 「前の接続を切ってから繋ぐ」を徹底すること
    // （CanvasWidget::m_resizeAnimFinishedConn がその実例）。
    QPropertyAnimation *resizeAnim() const { return m_resizeAnim; }
    // animateToAspectで決まる最終ウィンドウサイズを返す（実際のリサイズはしない）
    QSize calcFocusSize(double aspectW, double aspectH) const;
    void refreshDropTargets();

    // ---- ウィンドウ全画面（独自管理） ----
    // このアプリのウィンドウはQt::FramelessWindowHintの独自ウィンドウのため、
    // Qt/OS標準のisMaximized()/showMaximized()/showNormal()は挙動が不安定
    // だった（リサイズハンドルでの意図しない解除、ショートカット復帰時に
    // OS側のWINDOWPLACEMENTが古い全画面矩形を再適用してしまう、等）。
    // そのため「ウィンドウ全画面」はQtのウィンドウ状態を一切使わず、
    // 単純な「直前の通常ジオメトリを保存 → 画面いっぱいにリサイズ／その逆」
    // という自前のフラグ＋ジオメトリ操作として実装する。
    bool isWindowFullscreen() const { return m_windowFullscreenActive; }
    // durationMs > 0 ならアニメーション、0ならその場で即座に切り替える。
    void setWindowFullscreen(bool on, int durationMs = 0);
    // 2026-09-17: 右下リサイズハンドルの表示（全画面・辺リサイズ設定を反映）。
    void updateResizerVisibility();
    // 2026-07-08: ウィンドウ全画面(OS最大化)中に、バブル側の「ウィンドウ
    // サイズロック」設定によりバブル全画面解除と同時にウィンドウ全画面も
    // 解除したい場合に使う。setWindowFullscreen(false, ...)は通常
    // 「全画面化する直前のジオメトリ」(m_preFullscreenGeometry)へ戻すが、
    // それだとバブルフォーカスによってリサイズされた後の形へ戻ってしまう
    // ことがあるため、戻り先のジオメトリを明示的に指定できるようにした。
    // 既に全画面でない場合は何もしない。
    void exitFullscreenToGeometry(const QRect &geom, int durationMs = 0);
    // 全画面化する直前の通常ジオメトリ（全画面化していなければ現在のジオメトリ）。
    QRect preFullscreenGeometry() const {
        return isWindowFullscreen() ? m_preFullscreenGeometry : geometry();
    }
    // 2026-07-09: バブルがフォーカス中（バブル全画面）なら、ウィンドウを
    // そのバブルのアスペクト比へ合わせ直す（フォーカス中でなければ何もしない）。
    // DynamicIslandの島ドラッグによるウィンドウ全画面解除では、ちらつき防止の
    // ためnotifyWindowMaximizedChanged()が開始したアスペクト再フィットの
    // アニメーションをドラッグ側が即座に打ち切る（DynamicIsland.cpp参照）。
    // その場合、ドラッグ終了時にこの関数で再フィットをやり直さないと、
    // ウィンドウがロックサイズのまま余白（黒帯）付きのバブル全画面になる。
    void refitFocusedBubbleAspect();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

public:
    // 2026-10-11: 表示倍率(DPI)の違うモニター間でも追従がずれないよう、ウィンドウの
    // ドラッグ移動はQtの論理座標(QWidget::move)ではなく、Win32の物理ピクセル
    // (GetCursorPos/SetWindowPos)で行う。anchorは「掴んだ点がウィンドウ左上から
    // どれだけ離れているか」。anchorIsRatio=trueならウィンドウ幅・高さに対する
    // 割合(0〜1)、falseなら論理px(DPIに応じて物理pxへ換算)。成功でtrue。
    bool dragWindowNative(const QPointF &anchor, bool anchorIsRatio);

private:
    // 2026-10-11: ウィンドウサイズを「DPIに依存しない論理サイズ(実数)」で覚えておき、
    // モニター間移動でDPIが変わったら、そこから物理サイズを計算し直す。Qtは整数に
    // 丸めながらサイズを再計算するため、往復するたびにサイズがずれていた。
    void rememberDipSize();
    void restoreDipSizeAfterDpiChange(UINT newDpi);
    QSizeF m_dipSize;
    bool   m_dpiChanging {false};

    struct GlobalHotkey
    {
        UINT modifiers = MOD_CONTROL | MOD_SHIFT;
        UINT virtualKey = VK_SPACE;
    };

    void applyAcrylicEffect();
    void applyRoundedCorners();
    // Windowsの「透明効果」設定(EnableTransparency)がONかどうかを判定する。
    bool querySystemTransparencyEnabled() const;
    // 設定に応じてアクリル(ブラー)効果を適用するか、フォールバック（自前の
    // 半透明パネル描画）に切り替えるかを切り替える。起動時・設定変更時に呼ぶ。
    void applyAcrylicOrFallback();
    void layoutIsland();
    // 島設定パネルの本体組み込み項目（Show Grid / Grid Size /
    // Window Size Lock / Camera Pan Inertia / Shortcut Settings）を
    // IslandControlRegistry へ本体セクション（2026-09-15: 見た目/操作/その他の
    // 3つ。"host.appearance" / "host.behavior" / "host.other"）として登録する。
    // 旧実装では DynamicIsland::toggleSettingsPanel() 内でウィジェットを
    // 直接生成していたが、2026-07-13にプラグイン由来コントロールと同じ
    // 宣言（PbControlSpec）ベースの仕組みへ一本化した。コンストラクタから
    // 一度だけ呼ぶ。
    void registerIslandSettingsControls();

    // ---- プラグイン .zip ドロップの確認（2026-09-13）----
    // CanvasWidget::pluginInstallRequested を受け、島の確認プロンプト
    // （DynamicIsland::showPrompt）で承認を取ってからインストールする。
    // 確認は非モーダルなので、答えを待っている間に来た2つ目以降の .zip は
    // m_pendingPluginZips に積み、1つずつ順番に出す。
    void enqueuePluginInstallPrompt(const QString &zipFilePath);
    // キューの先頭を取り出してプロンプトを出す。処理中なら何もしない。
    void processNextPluginInstallPrompt();
    // プロンプトを1つ出す共通部分（島を出す→答えを受けて通常へ戻す）。
    void showIslandPrompt(const IslandPromptContent &content,
                          std::function<void(bool)> answered);
    // 1件ぶんが完全に終わった。次があれば続ける。
    void finishPluginInstallStep();
    // 2026-07-19: レイヤー機能を一旦切り離し（src/old/参照）。
    // void layoutLayerBar();
    bool isOnUI(const QPointF &pos) const;
    // 2026-09-15: 設定を開いている間はキャンバスへマウス操作を流さない
    // （MainWindow.cpp の mousePressEvent 手前のコメント参照）。
    bool canvasInputBlockedBySettings() const;
    void restoreWindowSettings();
    void saveWindowSettings() const;
    void scheduleWindowSettingsSave();
    void ensureWindowGeometryVisible();
    void setupTrayIcon();
    void showFromTray();
    void hideToTray();
    void quitFromTray();
    bool registerGlobalHotkey();
    bool registerGlobalHotkey(const GlobalHotkey &hotkey);
    void unregisterGlobalHotkey();
    void activateFromGlobalHotkey();
    void fadeInWindow();
    void fadeOutAndHideWindow();
    void setupHotkeyOverlay();
    void refreshNativeDropStyle();
    // 2026-07-11 🚫恒久対策: OLEはRegisterDragDrop時にIDropTargetをHWNDの
    // "OleDropTargetInterface"プロパティとして保存する。これを監視して、
    // 健全時はポインタをキャッシュ（AddRef）し、登録が消えていたら
    // キャッシュを使ってRegisterDragDropを張り直す。「誰が・どうやって
    // 登録を壊したか」に依存しない自己修復（D&Dウォッチドッグから毎回呼ぶ）。
    void ensureNativeDropRegistration();
    void *m_cachedDropTarget = nullptr;   // 実体はIDropTarget*（ヘッダを軽く保つためvoid*）
    quintptr m_cachedDropHwnd = 0;
    void makeDropTargetsRecursive(QWidget *root);
    void beginHotkeyCapture();
    void cancelHotkeyCapture();
    void commitHotkeyCapture();
    void updateHotkeyCaptureDisplay(const QString &message = QString());
    QString hotkeyToString(const GlobalHotkey &hotkey) const;
    bool isModifierVirtualKey(UINT virtualKey) const;
    UINT modifiersFromQt(Qt::KeyboardModifiers modifiers) const;
    // 2026-09-16: 島の設定「呼出しショートカットキー」の3欄（欄ごとに仮想キー1つ、空欄=0）
    void applySummonHotkeySlots(const QVariantList &values);
    QList<uint> hotkeySlotsFrom(const GlobalHotkey &hotkey) const;
    bool hotkeySlotsMatch(const QList<uint> &keySlots, const GlobalHotkey &hotkey) const;
    QString virtualKeyDisplayName(UINT vk) const;

    // 島の確認プロンプト待ちのプラグイン .zip（先頭から順に処理）。
    QStringList              m_pendingPluginZips;
    // 1件を処理中（zipの展開待ち＋プロンプト表示中＋インストール中）。
    // isPromptOpen() だけでは、展開中でまだプロンプトが出ていない時間に
    // 次のドロップが来たときに二重で走ってしまう。
    bool                     m_pluginPromptBusy = false;

    CanvasWidget            *m_canvas     = nullptr;
    DynamicIsland           *m_island     = nullptr;
    DynamicIslandController *m_islandCtrl = nullptr;
    CanvasOverlayWidget     *m_canvasOverlay = nullptr;
    ResizeHandle            *m_resizer    = nullptr;
    // 2026-07-19: レイヤー機能を一旦切り離し（src/old/参照）。
    // LayerBar                *m_layerBar   = nullptr;
    // タイマー階層: カメラ/バブルのアニメーション精度が要る処理は120Hz、
    // 再生バーのフェードやサラウンド等は60Hz、DynamicIslandのような
    // 固定UIは30Hzで十分なため分離している（動画自体のフレーム更新は
    // mpvのframeUpdated経由で完全に別経路・別タイミングなので影響しない）。
    QTimer                  *m_logicTimer  = nullptr; // 120Hz: CanvasWidget::syncLogic()
    QTimer                  *m_mediumTimer = nullptr; //  60Hz: CanvasWidget::syncLogicMedium()
    QTimer                  *m_uiTimer     = nullptr; //  30Hz: DynamicIslandController::tick()
    QPropertyAnimation      *m_resizeAnim = nullptr;  // ウィンドウリサイズアニメ
    QPropertyAnimation      *m_windowOpacityAnim = nullptr;
    QTimer                  *m_saveSettingsTimer = nullptr;
    // 2026-10-12: セッションの自動保存。変更から少し待って保存する分と、
    // 再生位置など変更通知の無いもののための定期保存。
    QTimer                  *m_sessionAutoSaveTimer = nullptr;
    QTimer                  *m_sessionPeriodicSaveTimer = nullptr;
    void autoSaveSession();
    QSystemTrayIcon         *m_trayIcon = nullptr;
    QFrame                  *m_hotkeyOverlay = nullptr;
    QLabel                  *m_hotkeyCurrentLabel = nullptr;
    QLabel                  *m_hotkeyInputLabel = nullptr;
    QLabel                  *m_hotkeyMessageLabel = nullptr;
    GlobalHotkey             m_globalHotkey;
    GlobalHotkey             m_pendingHotkey;
    int                      m_globalHotkeyId = 1;
    bool                     m_globalHotkeyRegistered = false;
    bool                     m_capturingHotkey = false;
    bool                     m_pendingHotkeyHasKey = false;
    bool                     m_allowQuit = false;
    bool                     m_restoringWindowSettings = false;
    bool                     m_hideAfterFadeOut = false;
    bool                     m_windowSettingsSaveSuspended = false;
    // Windows「透明効果」設定のON/OFF状態（OFFなら自前の半透明フォールバックを使う）
    bool                     m_systemTransparencyEnabled = true;

    // ---- ウィンドウ全画面（独自管理） ----
    bool                     m_windowFullscreenActive = false;
    QRect                    m_preFullscreenGeometry;
};
