#include "MainWindow.h"
#include <QRegularExpression>
#include <algorithm>
#include <memory>
#include "ResizeHandle.h"
#include "CanvasVideo.h"
#include "CanvasOverlayWidget.h"
#include "AppSettings.h"
#include "AppLanguage.h"

#include <QTimer>
#include <cmath>
#include "IslandControlRegistry.h"
#include "plugin/PluginManager.h"
// 2026-07-19: レイヤー機能を一旦切り離し（src/old/参照）。
// #include "LayerBar.h"
// #include "LayerManifest.h"
#include "SessionManager.h"
#include "StartupRegistration.h"
#include "WebImageCache.h"
#include "WebVideoCache.h"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QResizeEvent>
#include <QMoveEvent>
#include <QShowEvent>
#include <QCloseEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QApplication>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QFrame>
#include <QAction>
#include <QMenu>
#include <QSettings>
#include <QFileInfo>
#include <QPointer>
#include <QDebug>
#include <QKeySequence>
#include <QStringList>
#include <windows.h>
#include <dwmapi.h>
#include <cwchar>

namespace {
bool isCanvasOriginatedDrag(const QDropEvent *event, CanvasWidget *canvas)
{
    return event && canvas && event->source() == canvas;
}

void forceAcceptDropEvent(QDropEvent *event)
{
    if (!event)
        return;

    Qt::DropAction action = event->proposedAction();
    const Qt::DropActions possible = event->possibleActions();
    if (action == Qt::IgnoreAction) {
        if (possible.testFlag(Qt::CopyAction))
            action = Qt::CopyAction;
        else if (possible.testFlag(Qt::LinkAction))
            action = Qt::LinkAction;
        else if (possible.testFlag(Qt::MoveAction))
            action = Qt::MoveAction;
    }

    if (action != Qt::IgnoreAction)
        event->setDropAction(action);
    event->accept();
}

// 2026-07-19: レイヤー機能を一旦切り離した際の暫定対応。SessionManagerは
// 元々「レイヤー番号Nを受け取ってsession_N.jsonを読み書きする」設計のため、
// レイヤー機能無効化後もこの番号だけは決め打ちで必要になる。常に0番
// （最初から存在する既定レイヤー）に固定する。
constexpr int kSingleLayerNumber = 0;
}

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    // 2026-09-18: 表示言語は設定（全般 → 言語）で決まる。AppSettings の
    // 最初の参照で保存値が AppLanguage へ流し込まれるので、先に触っておく。
    AppSettings::instance();
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setWindowOpacity(0.0);
    resize(900, 700);
    // 2026-09-16: どの経路（リサイズハンドル・縦横比合わせ・設定の復元）でも
    // この大きさより小さくしない（kMinWindowWidth のコメント参照）。
    setMinimumSize(kMinWindowWidth, kMinWindowHeight);
    setAcceptDrops(true);
    setMouseTracking(true);
    qApp->installEventFilter(this);

    refreshNativeDropStyle();

    // キャンバス（QOpenGLWidget）
    m_canvas = new CanvasWidget(this);
    m_canvas->setGeometry(rect());
    m_canvasOverlay = new CanvasOverlayWidget(m_canvas, this);
    m_canvasOverlay->setGeometry(rect());
    connect(m_canvas, &CanvasWidget::playbackOverlayUpdateRequested,
            m_canvasOverlay, &CanvasOverlayWidget::refreshPlaybackUi);
    // 2026-09-12: イコライザだけが動いているフレームは、オーバーレイ全体では
    // なくその矩形だけを無効化する（部分更新）。
    connect(m_canvas, &CanvasWidget::playbackOverlayEqualizerUpdateRequested,
            m_canvasOverlay, &CanvasOverlayWidget::refreshPlaybackEqualizer);
    connect(m_canvas, &CanvasWidget::focusOverlayUpdateRequested,
            m_canvasOverlay, &CanvasOverlayWidget::refreshFocusUi);
    // 2026-09-17: 長押し2倍速の間は動画UI（ラベル・再生バー等）をフェードで隠す。
    connect(m_canvas, &CanvasWidget::speedHoldChanged,
            m_canvasOverlay, &CanvasOverlayWidget::setSpeedHoldSuppressed);
    connect(m_canvas, &CanvasWidget::borderOverlayUpdateRequested,
            m_canvasOverlay, &CanvasOverlayWidget::refreshBorderUi);
    connect(m_canvas, &CanvasWidget::launcherTooltipOverlayUpdateRequested,
            m_canvasOverlay, &CanvasOverlayWidget::refreshLauncherTooltipUi);
    m_canvas->lower(); // 島・リサイズハンドルより下

    // Dynamic Island
    m_island = new DynamicIsland(this);
    m_island->setAcceptDrops(true);
    connect(m_island->btnClose, &QPushButton::clicked, this, &MainWindow::close);
    connect(m_island->btnMax,   &QPushButton::clicked, this, [this]{
        setWindowFullscreen(!isWindowFullscreen(), 320);
    });
    connect(m_island->btnMin, &QPushButton::clicked, this, &MainWindow::showMinimized);
    connect(m_island->btnSettings, &QPushButton::clicked, this, [this]{
        m_island->toggleSettingsPanel(m_canvas);
        refreshDropTargets();
    });
    connect(m_island, &DynamicIsland::shortcutSettingsRequested,
            this, &MainWindow::beginHotkeyCapture);
    // 2026-09-15: 設定パネル展開中はキャンバス上のUIを隠す
    // （CanvasOverlayWidget::setUiSuppressed() のコメント参照）。
    connect(m_island, &DynamicIsland::settingsOpenChanged,
            m_canvasOverlay, &CanvasOverlayWidget::setUiSuppressed);
    // 2026-09-16: アクセントカラーが変わったら（設定・Windows側）、島以外で
    // その色を使っている所も描き直す。動画UIの絵のキャッシュは色をキーに
    // 含んでいるので、次の描画で自動的に作り直される。ここではその描画の
    // きっかけだけを出す（静止画だけのシーンでは他に再描画の契機が無いため）。
    connect(m_island, &DynamicIsland::accentColorChanged, this, [this]{
        if (m_canvas)
            m_canvas->update();
        if (m_canvasOverlay)
            m_canvasOverlay->update();
    });

    // 2026-09-13: プラグイン .zip ドロップの確認を島のプロンプトで行う。
    // この接続が存在することが、CanvasWidget側でQMessageBox版を使わない
    // 条件にもなっている（CanvasWidget::promptAndInstallPluginZip参照）。
    connect(m_canvas, &CanvasWidget::pluginInstallRequested,
            this, &MainWindow::enqueuePluginInstallPrompt);

    m_islandCtrl = new DynamicIslandController(m_island, this, m_canvas);
    layoutIsland();
    // 島設定パネルの本体項目をIslandControlRegistryへ登録する
    // （パネルUI自体はIslandSettingsPanelが登録内容から自動構築する）。
    registerIslandSettingsControls();

    // リサイズハンドル（右下）
    m_resizer = new ResizeHandle(this);
    m_resizer->setAcceptDrops(true);
    m_resizer->move(width() - 40, height() - 40);
    m_resizer->raise();
    // 2026-09-17: 「どの辺からでもリサイズ」をON/OFFしたらハンドルの表示を追従させる。
    connect(&AppSettings::instance(), &AppSettings::changed,
            this, &MainWindow::updateResizerVisibility);

    // 2026-07-19: レイヤー機能を一旦切り離し（src/old/参照）。誤操作事故の
    // 懸念があったため、UI・切り替えロジックともに無効化している
    // （完全削除ではなく凍結。復活させる場合は下記コメントを外す）。
    // m_layerBar = new LayerBar(m_canvas, this);
    // m_layerBar->raise();
    // connect(m_layerBar, &LayerBar::layoutChanged, this, [this]{
    //     layoutLayerBar();
    // });
    // layoutLayerBar();

    // ロジックタイマー階層。動画自体の描画更新はmpvのframeUpdated経由で
    // 完全に別経路（フレーム到着ごとにイベント駆動でupdate()）なので、
    // ここで扱うのはカメラアニメーションやUIロジックのみ。
    //    60Hz: カメラのイーズアニメーション（ズーム/パン）と、画面外判定・
    //          サムネイル常駐管理（syncLogic）
    //    60Hz: 再生バーのフェード・ボリュームスライダー・
    //          再生バーのフェード等（syncLogicMedium）
    //    30Hz: DynamicIsland（固定UI）のトリガー判定・カーソルグロー
    //
    // 2026-09-14 Phase4: syncLogic を 8ms(120Hz) から 16ms(60Hz) へ変更した。
    // カメラのフォーカス演出は0.3〜0.5秒の決め打ちアニメーションで、120Hzで
    // なければ破綻するような速度は出ない。60Hzで足りる。
    // この変更で、syncLogic の中で毎tick走っていた全動画の画面矩形計算・
    // notifyScreenRect()・サムネイル常駐のソートも、そのまま半分になる。
    //
    // 【重要】以前は BubbleController が画面外猶予の残り時間を
    // 「notifyScreenRect()が呼ばれた回数 × SYNC_INTERVAL_MS(=8)」で数えて
    // いたため、この間隔を変えると猶予が3秒→6秒に化けるという隠れた結合が
    // あった。同じ Phase4 で BubbleController 側を経過時間ベースへ直したので、
    // ここの周期をこの先どう変えても猶予時間は影響を受けない。
    //
    // なお現在 kFast と kMedium は同じ 16ms だが、タイマーは分けたままに
    // してある（責務が違い、片方だけ周期を変えたくなったときに戻しやすい）。
    constexpr int kFastIntervalMs   = 16;  //  60Hz相当
    constexpr int kMediumIntervalMs = 16;  //  60Hz相当
    constexpr int kUiIntervalMs     = 33;  //  30Hz相当

    m_logicTimer = new QTimer(this);
    // 2026-07-15: QTimer既定のCoarseTimerは精度が保証されず(特にWindowsでは
    // OSタイマー分解能の影響を受ける)、指定間隔が実際には粗く刻まれ得る。
    // カメラのフォーカス演出はこのタイマーで駆動している
    // (CanvasWidget::tickCameraAnimation)ため、PreciseTimerを明示する。
    m_logicTimer->setTimerType(Qt::PreciseTimer);
    m_logicTimer->setInterval(kFastIntervalMs);
    connect(m_logicTimer, &QTimer::timeout, this, [this]{
        m_canvas->syncLogic();
    });
    m_logicTimer->start();

    // 2026-09-14 C-3: ラベルのリストのスクロールだけを120Hzで進める専用
    // タイマー。syncLogic は Phase4 で60Hzへ落としてある（全動画の画面矩形
    // 計算やサムネイル常駐のソートがぶら下がっているため）ので、そこへ
    // 相乗りさせず、また8msへ戻しもしない。スクロールは指で送っている間
    // ずっと目で追う動きなので、ここだけ倍の細かさで進める。
    // 進めた結果の再描画は「板の矩形だけ」の部分更新で済ませている
    // （CanvasWidget::syncLabelScroll 参照）。
    auto *scrollTimer = new QTimer(this);
    scrollTimer->setTimerType(Qt::PreciseTimer);
    scrollTimer->setInterval(8);   // 120Hz相当
    connect(scrollTimer, &QTimer::timeout, this, [this]{
        m_canvas->syncLabelScroll();
    });
    scrollTimer->start();

    m_mediumTimer = new QTimer(this);
    m_mediumTimer->setInterval(kMediumIntervalMs);
    connect(m_mediumTimer, &QTimer::timeout, this, [this]{
        m_canvas->syncLogicMedium(kMediumIntervalMs);
    });
    m_mediumTimer->start();

    m_uiTimer = new QTimer(this);
    m_uiTimer->setInterval(kUiIntervalMs);
    connect(m_uiTimer, &QTimer::timeout, this, [this]{
        m_islandCtrl->tick();
    });
    m_uiTimer->start();

    // アクセントカラー定期更新
    QTimer *accentTimer = new QTimer(this);
    accentTimer->setInterval(2000);
    connect(accentTimer, &QTimer::timeout, m_island, &DynamicIsland::refreshAccentColor);
    accentTimer->start();

    // 2026-07-09 🚫再発対策: D&Dウォッチドッグ。mpv/WASAPIのCOM絡みの
    // 破棄処理がメインスレッドのOLEアパートメントを巻き添えにすると、
    // どの操作が引き金かによらずウィンドウ全体へのD&Dが🚫固定になる
    // （ゴミ箱削除経路には診断があるが、Sleep/FocusSleep遷移等のmpv破棄
    // 経路には無かった）。5秒ごとにOLEの生存を確認し、死んでいたら
    // 再初期化＋ドロップターゲットの強制再登録で自動復旧する。
    // 健全時はOleInitializeのプローブ分をOleUninitializeで戻すだけなので
    // 実質ゼロコスト。
    QTimer *dndWatchdog = new QTimer(this);
    dndWatchdog->setInterval(5000);
    connect(dndWatchdog, &QTimer::timeout, this, [this]{
        // (1) OLEアパートメントの生存確認（死んでいたら再初期化して全体refresh）
        const HRESULT hr = OleInitialize(nullptr);
        if (hr == S_OK) {
            qWarning() << "[DND][watchdog] OLE apartment died unexpectedly! "
                          "re-initializing and re-registering drop targets";
            refreshDropTargets();
        } else if (hr == S_FALSE) {
            OleUninitialize();
        }
        // (2) 拡張スタイル（WS_EX_TRANSPARENT等）が何かに書き換えられて
        //     いないか（変更があった時だけ修復・ログされる）
        refreshNativeDropStyle();
        // (3) RegisterDragDrop登録そのものの生死を直接監視・修復
        //     （2026-07-11追加。OLEが生きていても登録だけ失われるケース、
        //      外部フック等による破壊を含めた最後の砦）
        ensureNativeDropRegistration();
    });
    dndWatchdog->start();

    m_systemTransparencyEnabled = querySystemTransparencyEnabled();
    applyAcrylicOrFallback();
    applyRoundedCorners();

    // ウィンドウリサイズアニメーション
    m_resizeAnim = new QPropertyAnimation(this, "geometry", this);
    m_resizeAnim->setEasingCurve(QEasingCurve::OutCubic);

    m_windowOpacityAnim = new QPropertyAnimation(this, "windowOpacity", this);
    m_windowOpacityAnim->setDuration(180);
    m_windowOpacityAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_windowOpacityAnim, &QPropertyAnimation::finished, this, [this]{
        if (!m_hideAfterFadeOut) {
            refreshDropTargets();
            return;
        }
        m_hideAfterFadeOut = false;
        hide();
        setWindowOpacity(0.0);
    });

    setupHotkeyOverlay();

    m_saveSettingsTimer = new QTimer(this);
    m_saveSettingsTimer->setSingleShot(true);
    m_saveSettingsTimer->setInterval(300);
    connect(m_saveSettingsTimer, &QTimer::timeout, this, &MainWindow::saveWindowSettings);

    // 2026-10-12: セッションの自動保存。以前はトレイ格納・終了時にしか
    // 保存しておらず、表示中に落ちると出してから後の変更が全部消えていた。
    //   - 変更（Undoの1ステップ確定・Undo/Redo）から3秒後。操作が続く間は延長。
    //   - 再生位置・音量など変更通知の無いもののため、表示中は60秒ごと。
    // 書き込みはワーカーで行い、中身が同じなら書かない（SessionManager::saveAsync）。
    m_sessionAutoSaveTimer = new QTimer(this);
    m_sessionAutoSaveTimer->setSingleShot(true);
    m_sessionAutoSaveTimer->setInterval(3000);
    connect(m_sessionAutoSaveTimer, &QTimer::timeout, this, &MainWindow::autoSaveSession);
    connect(m_canvas, &CanvasWidget::sessionContentChanged,
            m_sessionAutoSaveTimer, qOverload<>(&QTimer::start));

    m_sessionPeriodicSaveTimer = new QTimer(this);
    m_sessionPeriodicSaveTimer->setInterval(60000);
    connect(m_sessionPeriodicSaveTimer, &QTimer::timeout, this, [this] {
        if (isVisible())
            autoSaveSession();
    });
    m_sessionPeriodicSaveTimer->start();

    setupTrayIcon();
    restoreWindowSettings();
    // 起動時はウィンドウ全画面状態を復元しないため、リサイズハンドルは
    // 常に表示された状態で開始する（isWindowFullscreen()は常にfalse）。
    updateResizerVisibility();
    // ---- セッション復元 ----
    // 2026-09-23: 初回起動チュートリアル（サンプル動画の合成セッション含む）は
    // 機能ごと削除した。操作方法は Help（IslandHelpGuide）を参照。
    // 2026-07-19: レイヤー機能を一旦切り離したため、常に固定の1レイヤー
    // （session_0.json）だけを扱う。
    const int activeLayerNumber = kSingleLayerNumber;
    {
        QList<int> allLayerNumbers{ kSingleLayerNumber };
        SessionManager::preloadAllLayers(allLayerNumbers);

        // 2026-07-12: webimageキャッシュの起動時ガベージコレクション。
        // 全レイヤーを先読みし終えた直後（追加のディスクI/Oなし）に、
        // どのレイヤーからも参照されていないキャッシュファイルを掃除する。
        // クラッシュ等でバブル削除時の削除処理を経由できなかった孤児対策。
        WebImageCache::sweepOrphans(SessionManager::collectAllWebImageUrls(allLayerNumbers));
        // 2026-10-12: webvideoキャッシュも同じ考え方で掃除する
        // （途中で終わったダウンロードの .part もここで消える）。
        WebVideoCache::sweepOrphans(SessionManager::collectAllWebVideoUrls(allLayerNumbers));
    }
    SessionManager::restore(m_canvas, activeLayerNumber);
    refreshDropTargets();
    auto &cfg = AppSettings::instance();
    m_globalHotkey.modifiers = cfg.hotkeyModifiers();
    m_globalHotkey.virtualKey = cfg.hotkeyVirtualKey();
    registerGlobalHotkey();
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
    unregisterGlobalHotkey();
    saveWindowSettings();
}

// 2026-07-15: animateToAspect()から純粋な計算部分だけを切り出したもの。
// CanvasWidget::focusItem()（フォーカスイン時、カメラのパン/ズームと
// ウィンドウのアスペクト比合わせリサイズが別々のアニメーションで走って
// タイミングがズレる不具合の根本修正）が、ウィンドウリサイズとカメラを
// 同じ進行度(t)で同期させるために、実際にアニメーションを開始せず
// 「目標ジオメトリ」だけを先に知りたくて追加した。ロジック・数値は
// animateToAspect()から一切変更していない。
QRect MainWindow::computeAspectTargetGeometry(double aspectW, double aspectH,
                                               const QSize &baseSize) const
{
    if (aspectW <= 0 || aspectH <= 0) return geometry();

    // 現在のウィンドウ中心点を保持（位置はbaseSize指定時も動かさない）。
    //
    // 2026-07-18 バグ修正（ウィンドウが毎回1px左上へずれていく）:
    // 従来はQRect::center()（整数座標）を使っていたが、QRectのcenter()は
    // x + (w-1)/2 、つまり幅/高さが偶数のとき「本当の中心より0.5小さい値」を
    // 切り捨てで返す。そこから left = center - newW/2 で左上を逆算すると、
    // サイズが全く変わらない場合でも毎回 x-1 になり、この関数を通るたび
    // （フォーカスイン・連続再生の対比リフィットのたび）にウィンドウが
    // 1pxずつ左上へ流れて蓄積していた。QRectFのcenter()（x + w/2.0、正確な
    // 実数中心）で計算し、サイズ不変なら位置も厳密に不変になるようにする。
    const QPointF center = QRectF(geometry()).center();

    // 現在のウィンドウサイズからアスペクトに合わせたサイズを計算
    // 小さい方に合わせる（画質倒帪防止）
    // baseSizeが有効な場合はそちらを基準サイズとして使う（上のヘッダ
    // コメント・animateToAspect()のbaseSize引数コメント参照）。
    double curW = baseSize.isValid() ? baseSize.width()  : width();
    double curH = baseSize.isValid() ? baseSize.height() : height();
    double targetAR = aspectW / aspectH;
    double curAR    = curW / curH;

    double newW, newH;
    if (targetAR > curAR) {
        newW = curW;
        newH = curW / targetAR;
    } else {
        newH = curH;
        newW = curH * targetAR;
    }

    // 全画面時の最小ウィンドウサイズを保証（480x270以上）
    // 現在のウィンドウが小さすぎる場合は拡大
    const double MIN_FOCUS_W = 480.0;
    const double MIN_FOCUS_H = 270.0;
    if (newW < MIN_FOCUS_W) { newW = MIN_FOCUS_W; newH = newW / targetAR; }
    if (newH < MIN_FOCUS_H) { newH = MIN_FOCUS_H; newW = newH * targetAR; }

    // 画面内に収まるようにクランプ
    QScreen *screen = QGuiApplication::screenAt(center.toPoint());
    if (!screen) screen = QGuiApplication::primaryScreen();
    QRect avail = screen->availableGeometry();
    newW = qBound(static_cast<double>(kMinWindowWidth), newW, (double)avail.width());
    newH = qBound(static_cast<double>(kMinWindowHeight), newH, (double)avail.height());

    // 中心位置は変えずにリサイズ（上記2026-07-18修正コメント参照。
    // 実数中心から整数サイズの半分を引いてqRoundするため、サイズ不変なら
    // 左上座標も厳密に元のまま）。
    const int intW = (int)newW;
    const int intH = (int)newH;
    QRect targetGeom(
        qRound(center.x() - intW / 2.0),
        qRound(center.y() - intH / 2.0),
        intW, intH
    );
    // 画面の左上からはみ出さないように調整
    // 注意: QRect::right()/bottom()は右端・下端の座標（inclusive）を返すため、
    // targetGeomの幅/高さがavailと同じ（＝ウィンドウが既に画面いっぱい）の場合、
    // avail.right() - targetGeom.width() が avail.left() より1小さくなり、
    // qBound()の引数がmin>maxになってQ_ASSERTでクラッシュしていた。
    // avail.left()/avail.top() 起点で最大値を計算し、qMaxでmin<=maxを保証する。
    const int maxLeft = qMax(avail.left(), avail.left() + avail.width()  - targetGeom.width());
    const int maxTop  = qMax(avail.top(),  avail.top()  + avail.height() - targetGeom.height());
    targetGeom.moveLeft(qBound(avail.left(), targetGeom.left(), maxLeft));
    targetGeom.moveTop(qBound(avail.top(), targetGeom.top(), maxTop));
    return targetGeom;
}

// 2026-09-17: 右下ハンドルは「ウィンドウ全画面でない」かつ「辺リサイズがOFF」のときだけ出す。
void MainWindow::updateResizerVisibility()
{
    if (!m_resizer) return;
    m_resizer->setVisible(!m_windowFullscreenActive
                          && !AppSettings::instance().edgeResizeEnabled());
}

void MainWindow::setAspectLockForFocus(double aspectW, double aspectH)
{
    if (m_resizer) m_resizer->setAspectLock(true, aspectW, aspectH);
}

void MainWindow::animateToAspect(double aspectW, double aspectH, int durationMs,
                                  const QSize &baseSize)
{
    // 2026-07-18: 「連続再生で対比が変わってもリサイズされない」調査用ログを
    // 撤去。原因は本関数ではなく、(1)セッション復元経路のバブルに
    // videoDimensionsReadyが接続されていなかったこと
    // （CanvasWidget::connectVideoDimensionSignals()参照）、(2)ウィンドウ
    // 全画面中にカメラの再フィットが無かったこと
    // （CanvasWidget::refitFocusedWindowForAspectChange()参照）。
    if (aspectW <= 0 || aspectH <= 0)
        return;

    setAspectLockForFocus(aspectW, aspectH);
    const QRect targetGeom = computeAspectTargetGeometry(aspectW, aspectH, baseSize);

    m_resizeAnim->stop();
    m_resizeAnim->setDuration(durationMs);
    // 2026-07-15: カメラのフォーカス演出(CanvasWidget::m_camAnimDriver)と同じ
    // InOutCubicへ明示的に揃える。ウィンドウサイズの変化とカメラの寄りが
    // 同じ速度カーブで同時に進行・同時に完了するようにするため。
    // (以前はここでイージングを設定しておらず、直前にanimateToGeometry()が
    //  設定したOutBackが漏れて使われる潜在バグもあった)
    m_resizeAnim->setEasingCurve(QEasingCurve::InOutCubic);
    m_resizeAnim->setStartValue(geometry());
    m_resizeAnim->setEndValue(targetGeom);
    m_resizeAnim->start();
}

QSize MainWindow::calcFocusSize(double aspectW, double aspectH) const
{
    if (aspectW <= 0 || aspectH <= 0) return size();
    double curW = width(), curH = height();
    double targetAR = aspectW / aspectH;
    double curAR    = curW / curH;
    double newW, newH;
    if (targetAR > curAR) { newW = curW; newH = curW / targetAR; }
    else                  { newH = curH; newW = curH * targetAR; }
    // 最小サイズ保証（animateToAspectと同じロジック）
    const double MIN_W = 480.0, MIN_H = 270.0;
    if (newW < MIN_W) { newW = MIN_W; newH = newW / targetAR; }
    if (newH < MIN_H) { newH = MIN_H; newW = newH * targetAR; }
    return QSize(qRound(newW), qRound(newH));
}

void MainWindow::clampGeometryToScreen(QRect &geom) const
{
    QScreen *screen = QGuiApplication::screenAt(geom.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    QRect avail = screen->availableGeometry();

    // ウィンドウの上端が画面内に必ず入るように（店の辺が埋もれないよう）
    const int MIN_VISIBLE_TOP = 40; // 少なくとも上ぃ04pxは画面内
    if (geom.top() < avail.top()) geom.moveTop(avail.top());
    if (geom.top() > avail.bottom() - MIN_VISIBLE_TOP)
        geom.moveTop(avail.bottom() - MIN_VISIBLE_TOP);
    if (geom.right() < avail.left() + 100) geom.moveRight(avail.left() + 100);
    if (geom.left() > avail.right() - 100) geom.moveLeft(avail.right() - 100);
}

void MainWindow::setWindowFullscreen(bool on, int durationMs)
{
    if (on == m_windowFullscreenActive)
        return;

    QRect target;
    if (on) {
        // 全画面化する直前の通常ジオメトリを保存しておく（解除時にここへ戻す）。
        m_preFullscreenGeometry = geometry();
        QScreen *screen = QGuiApplication::screenAt(geometry().center());
        if (!screen) screen = QGuiApplication::primaryScreen();
        target = screen->availableGeometry();
    } else {
        target = m_preFullscreenGeometry.isValid() ? m_preFullscreenGeometry : normalGeometry();
        clampGeometryToScreen(target);
    }

    m_windowFullscreenActive = on;
    if (m_resizer) {
        m_resizer->setAspectLock(false);
    }
    updateResizerVisibility();

    // ウィンドウのジオメトリを実際に変更する処理（アニメーションまたは即時）。
    // 2026-07-08: notifyWindowMaximizedChanged()（バブルフォーカス中に全画面
    // 解除された場合、フォーカス中バブルのアスペクト比にウィンドウを合わせ
    // 直す処理）を、以前はこの関数の最後で「リサイズ開始と同時」に呼んで
    // いた。しかしアニメーション経路(durationMs>0)ではリサイズ自体が
    // フェードアウト完了後まで始まっておらず、そこへ割り込む形で
    // notifyWindowMaximizedChanged()がm_resizeAnimを触ると、後から本来の
    // リサイズが同じm_resizeAnimを奪い合ってしまう。リサイズが実際に
    // 完了した後に呼ぶよう、両経路(アニメ/即時)の完了地点まで移動した。
    auto *mwSelf = this;
    const bool nowFullscreen = m_windowFullscreenActive;
    auto notifyMaximizedChanged = [mwSelf, nowFullscreen]() {
        if (mwSelf->m_canvas)
            mwSelf->m_canvas->notifyWindowMaximizedChanged(nowFullscreen);
    };
    auto applyGeometry = [this, target, durationMs, notifyMaximizedChanged]() {
        if (durationMs > 0) {
            m_resizeAnim->stop();
            m_resizeAnim->setDuration(durationMs);
            m_resizeAnim->setEasingCurve(QEasingCurve::OutCubic);
            m_resizeAnim->setStartValue(geometry());
            m_resizeAnim->setEndValue(target);
            auto *conn = new QMetaObject::Connection;
            *conn = connect(m_resizeAnim, &QPropertyAnimation::finished, this,
                            [this, conn, notifyMaximizedChanged]() {
                // ウィンドウのリサイズが終わってから、キャンバス内容は
                // フェードインで、島は即座に再表示する。
                if (m_canvas)
                    m_canvas->fadeContentOpacityTo(1.0, 220);
                if (m_island)
                    m_island->setVisible(true);
                notifyMaximizedChanged();
                QObject::disconnect(*conn);
                delete conn;
            });
            m_resizeAnim->start();
        } else {
            setGeometry(target);
            if (m_canvas)
                m_canvas->setContentOpacity(1.0);
            if (m_island)
                m_island->setVisible(true);
            notifyMaximizedChanged();
        }
    };

    if (durationMs > 0 && m_canvas) {
        // ウィンドウのリサイズアニメーション中は毎フレームカメラ・レイアウトが
        // 再計算されてグリッドやバブルがガクつく（がくがくする）ため、
        // アニメーション開始前にキャンバス内容を素早くフェードアウトして隠し、
        // リサイズ完了後にフェードインで見せる。
        // 島（Dynamic Island）は中央寄せのため毎フレーム位置が動いてしまう。
        // こちらはフェードではなく即座に非表示にし、リサイズが終わったら
        // 即座に再表示する。
        if (m_island)
            m_island->setVisible(false);
        m_canvas->fadeContentOpacityTo(0.0, qMin(120, durationMs / 2), applyGeometry);
    } else {
        applyGeometry();
    }

    refreshDropTargets();
    scheduleWindowSettingsSave();
}

void MainWindow::exitFullscreenToGeometry(const QRect &geom, int durationMs)
{
    if (!m_windowFullscreenActive)
        return;
    // setWindowFullscreen(false, ...)が使う「戻り先」を、通常の
    // m_preFullscreenGeometryではなく指定されたgeomに差し替えてから
    // 呼び出す。setWindowFullscreen()自体のロジック(m_windowFullscreenActive
    // の更新、リサイザーの表示、notifyWindowMaximizedChanged()呼び出し等)は
    // そのまま流用する。
    QRect target = geom;
    clampGeometryToScreen(target);
    m_preFullscreenGeometry = target;
    setWindowFullscreen(false, durationMs);
}

void MainWindow::refitFocusedBubbleAspect()
{
    // notifyWindowMaximizedChanged(false)は「ウィンドウ全画面から復帰した」
    // 通知として、フォーカス中バブルがあればanimateToAspect()での再フィットと
    // ウィンドウサイズロックの帳簿更新を行う（フォーカス中バブルが無ければ
    // 何もしない）。ロックの復元先(preWindowGeometry)はセッション中の
    // 再呼び出しでは上書きされないようガード済みのため、ここから何度
    // 呼んでも安全（CanvasWidget::notifyWindowMaximizedChanged()参照）。
    if (m_canvas && !m_windowFullscreenActive)
        m_canvas->notifyWindowMaximizedChanged(false);
}

void MainWindow::restoreWindowSettings()
{
    m_restoringWindowSettings = true;

    // Qt標準のsaveGeometry()/restoreGeometry()（不透明なバイナリblob）は
    // ウィンドウの最大化状態などQt/OS側の状態も一緒に埋め込んで復元して
    // しまうことがあり、フレームレスウィンドウでは信頼できなかった。
    // そのため x/y/width/height を自前でQSettingsに保存・復元し、
    // ウィンドウ全画面（独自管理）は起動時には一切復元しない
    // （常に全画面化する前の通常サイズで起動する）。
    QSettings settings;
    if (settings.contains("window/x")) {
        QRect geom(
            settings.value("window/x").toInt(),
            settings.value("window/y").toInt(),
            settings.value("window/w", 900).toInt(),
            settings.value("window/h", 700).toInt()
        );
        setGeometry(geom);
        ensureWindowGeometryVisible();
    }

    m_restoringWindowSettings = false;
}

void MainWindow::saveWindowSettings() const
{
    if (m_windowSettingsSaveSuspended)
        return;
    QSettings settings;
    // ウィンドウ全画面（独自管理）中に保存する場合は、現在の（画面いっぱいの）
    // ジオメトリではなく、全画面化する前の通常ジオメトリを保存する。
    // ウィンドウの実際のジオメトリ自体はここでは一切変更しない
    // （バックグラウンドへ隠す直前にウィンドウが一瞬元サイズへ縮んでから
    //   消える、という見た目の不自然さを避けるため）。
    const QRect geom = preFullscreenGeometry();
    settings.setValue("window/x", geom.x());
    settings.setValue("window/y", geom.y());
    settings.setValue("window/w", geom.width());
    settings.setValue("window/h", geom.height());
    // 参考情報として保存しておく（起動時の復元には使わない）。
    settings.setValue("window/state", isWindowFullscreen());
}

void MainWindow::scheduleWindowSettingsSave()
{
    if (m_restoringWindowSettings || m_windowSettingsSaveSuspended || !m_saveSettingsTimer)
        return;
    m_saveSettingsTimer->start();
}

void MainWindow::setWindowSettingsSaveSuspended(bool suspended)
{
    if (m_windowSettingsSaveSuspended == suspended)
        return;
    m_windowSettingsSaveSuspended = suspended;
    if (!suspended)
        scheduleWindowSettingsSave();
}

void MainWindow::ensureWindowGeometryVisible()
{
    QRect geom = geometry();

    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen->availableGeometry().intersects(geom))
            return;
    }

    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect avail = screen->availableGeometry();
    const QSize boundedSize(
        qBound(kMinWindowWidth, geom.width(), avail.width()),
        qBound(kMinWindowHeight, geom.height(), avail.height())
    );
    QRect safeGeom(QPoint(0, 0), boundedSize);
    safeGeom.moveCenter(avail.center());
    setGeometry(safeGeom);
}

void MainWindow::setupTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    // main.cpp で設定したアプリアイコンをそのまま使う（SP_ComputerIcon フォールバックなし）
    QIcon icon = QApplication::windowIcon();
    setWindowIcon(icon);

    QMenu *trayMenu = new QMenu(this);
    QAction *showAction = trayMenu->addAction(AppLanguage::text("Show", "表示"));
    QAction *hideAction = trayMenu->addAction(AppLanguage::text("Hide", "隠す"));
    trayMenu->addSeparator();
    QAction *quitAction = trayMenu->addAction(AppLanguage::text("Quit", "終了"));

    connect(showAction, &QAction::triggered, this, &MainWindow::showFromTray);
    connect(hideAction, &QAction::triggered, this, &MainWindow::hideToTray);
    connect(quitAction, &QAction::triggered, this, &MainWindow::quitFromTray);

    m_trayIcon = new QSystemTrayIcon(icon, this);
    m_trayIcon->setToolTip(QStringLiteral("PocketBoard"));
    m_trayIcon->setContextMenu(trayMenu);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    showFromTray();
                }
            });
    m_trayIcon->show();
}

void MainWindow::showFromTray()
{
    refreshDropTargets();
    if (!isVisible())
        setWindowOpacity(0.0);
    // ウィンドウ全画面（独自管理）の解除・最小化からの復元は、ウィンドウが
    // まだ非表示（invisible）のうちに済ませておく。show()した後で解除すると、
    // 一瞬全画面サイズのまま表示されてから縮む様子が見えてしまい見た目が
    // 良くないため、必ずshow()より前に行う。
    if (isWindowFullscreen())
        setWindowFullscreen(false);
    if (isMinimized())
        showNormal();
    show();
    raise();
    activateWindow();
    fadeInWindow();
}

void MainWindow::hideToTray()
{
    if (m_sessionAutoSaveTimer)
        m_sessionAutoSaveTimer->stop();   // この後で同期保存する
    // ウィンドウ全画面（独自管理）を隠す前に解除しない。解除してから隠すと
    // 一瞬通常サイズへ縮んでから消える様子が見えてしまい見た目が良くない
    // ため、全画面のままフェードアウトさせる（見えなくなった後の状態は
    // ユーザーからは分からないので問題ない）。次回showFromTray()で表示する
    // 直前に非表示のまま解除する。保存するジオメトリはsaveWindowSettings()
    // 内でpreFullscreenGeometry()を使って常に正しい値（全画面化前のサイズ）
    // にしているので、ここで解除しなくても保存内容には影響しない。
    saveWindowSettings();
    {
        // 2026-07-19: レイヤー機能を一旦切り離したため、固定の1レイヤー
        // （session_0.json）にのみ保存する。
        SessionManager::save(m_canvas, kSingleLayerNumber, QString());
    }
    if (m_canvas)
        m_canvas->stopAllVideosForBackground();
    fadeOutAndHideWindow();
}

void MainWindow::autoSaveSession()
{
    if (!m_canvas || m_allowQuit)
        return;
    SessionManager::saveAsync(m_canvas, kSingleLayerNumber, QString());
}

void MainWindow::quitFromTray()
{
    if (m_sessionAutoSaveTimer)
        m_sessionAutoSaveTimer->stop();   // この後で同期保存する
    m_allowQuit = true;
    saveWindowSettings();
    {
        // 2026-07-19: レイヤー機能を一旦切り離したため、固定の1レイヤー
        // （session_0.json）にのみ保存する。
        SessionManager::save(m_canvas, kSingleLayerNumber, QString());
    }
    if (m_canvas)
        m_canvas->stopAllVideosForBackground();
    unregisterGlobalHotkey();
    if (m_trayIcon)
        m_trayIcon->hide();
    close();
    QApplication::quit();
}

bool MainWindow::registerGlobalHotkey()
{
    return registerGlobalHotkey(m_globalHotkey);
}

bool MainWindow::registerGlobalHotkey(const GlobalHotkey &hotkey)
{
    unregisterGlobalHotkey();

    // 2026-09-16: 設定で欄を全部空にすると「ショートカットなし」（キー0）になる
    if (hotkey.virtualKey == 0) {
        m_globalHotkey = hotkey;
        return true;
    }

    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!RegisterHotKey(hwnd, m_globalHotkeyId,
                        hotkey.modifiers,
                        hotkey.virtualKey)) {
        const DWORD error = GetLastError();
        qWarning() << "RegisterHotKey failed for" << hotkeyToString(hotkey)
                   << "GetLastError =" << error;
        return false;
    }

    m_globalHotkey = hotkey;
    m_globalHotkeyRegistered = true;
    qInfo() << "Registered global hotkey:" << hotkeyToString(m_globalHotkey);
    return true;
}

void MainWindow::unregisterGlobalHotkey()
{
    if (!m_globalHotkeyRegistered)
        return;

    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!UnregisterHotKey(hwnd, m_globalHotkeyId)) {
        qWarning() << "UnregisterHotKey failed. GetLastError =" << GetLastError();
        return;
    }

    m_globalHotkeyRegistered = false;
}

void MainWindow::activateFromGlobalHotkey()
{
    showFromTray();

    // ウィンドウの最小化からの復元、およびウィンドウ全画面（独自管理）の
    // 解除はshowFromTray()側で既に完結している。
    // 以前はここで追加にネイティブAPIのShowWindow(SW_RESTORE/SW_SHOWNORMAL)を
    // 呼んでおり、Qt標準のisMaximized()/showMaximized()/showNormal()に
    // 依存していた頃はこれがOS側に残ったWINDOWPLACEMENT（全画面時の矩形）を
    // 再適用してしまい、「ソフト再起動では非全画面で起動するのに、ショート
    // カット呼び出しだと全画面のまま復帰する」食い違いの原因になっていた。
    // 現在はウィンドウ全画面をQtのウィンドウ状態を使わない自前実装
    // （MainWindow::setWindowFullscreen()）に置き換えたため、そもそもOSの
    // ネイティブ最大化状態やWINDOWPLACEMENTには一切触れない。ここでは
    // フォアグラウンドへの昇格のみ行う。
    HWND hwnd = reinterpret_cast<HWND>(winId());
    SetForegroundWindow(hwnd);
}

void MainWindow::fadeInWindow()
{
    refreshDropTargets();
    if (!m_windowOpacityAnim)
        return;

    m_hideAfterFadeOut = false;
    m_windowOpacityAnim->stop();
    m_windowOpacityAnim->setDuration(180);
    m_windowOpacityAnim->setEasingCurve(QEasingCurve::OutCubic);
    m_windowOpacityAnim->setStartValue(windowOpacity());
    m_windowOpacityAnim->setEndValue(1.0);
    m_windowOpacityAnim->start();
}

void MainWindow::fadeOutAndHideWindow()
{
    if (!m_windowOpacityAnim || !isVisible()) {
        hide();
        setWindowOpacity(0.0);
        return;
    }

    m_hideAfterFadeOut = true;
    m_windowOpacityAnim->stop();
    m_windowOpacityAnim->setDuration(150);
    m_windowOpacityAnim->setEasingCurve(QEasingCurve::InCubic);
    m_windowOpacityAnim->setStartValue(windowOpacity());
    m_windowOpacityAnim->setEndValue(0.0);
    m_windowOpacityAnim->start();
}

void MainWindow::setupHotkeyOverlay()
{
    m_hotkeyOverlay = new QFrame(this);
    m_hotkeyOverlay->setGeometry(rect());
    m_hotkeyOverlay->setAcceptDrops(true);
    m_hotkeyOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_hotkeyOverlay->setStyleSheet(
        "QFrame{background:rgba(0,0,0,175);}"
        "QLabel{background:transparent;color:white;}");
    m_hotkeyOverlay->hide();
    m_hotkeyOverlay->raise();

    auto *layout = new QVBoxLayout(m_hotkeyOverlay);
    layout->setContentsMargins(40, 40, 40, 40);
    layout->setSpacing(14);
    layout->addStretch(1);

    auto *title = new QLabel(AppLanguage::text("Press Keys...", "キーを押してください…"), m_hotkeyOverlay);
    title->setAlignment(Qt::AlignCenter);
    QFont titleFont = title->font();
    titleFont.setPointSize(34);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    m_hotkeyInputLabel = new QLabel(m_hotkeyOverlay);
    m_hotkeyInputLabel->setAlignment(Qt::AlignCenter);
    QFont inputFont = m_hotkeyInputLabel->font();
    inputFont.setPointSize(22);
    inputFont.setBold(true);
    m_hotkeyInputLabel->setFont(inputFont);
    layout->addWidget(m_hotkeyInputLabel);

    m_hotkeyCurrentLabel = new QLabel(m_hotkeyOverlay);
    m_hotkeyCurrentLabel->setAlignment(Qt::AlignCenter);
    QFont currentFont = m_hotkeyCurrentLabel->font();
    currentFont.setPointSize(11);
    m_hotkeyCurrentLabel->setFont(currentFont);
    layout->addWidget(m_hotkeyCurrentLabel);

    m_hotkeyMessageLabel = new QLabel(m_hotkeyOverlay);
    m_hotkeyMessageLabel->setAlignment(Qt::AlignCenter);
    QFont messageFont = m_hotkeyMessageLabel->font();
    messageFont.setPointSize(10);
    m_hotkeyMessageLabel->setFont(messageFont);
    layout->addWidget(m_hotkeyMessageLabel);

    layout->addStretch(1);
}

void MainWindow::refreshDropTargets()
{
    refreshNativeDropStyle();

    // 2026-07-09 🚫再発調査: メインスレッドのOLEアパートメントの生存確認。
    // mpv/WASAPI等のCOM絡みの破棄処理（ゴミ箱削除だけでなくSleep/FocusSleep
    // 遷移でのMpvPlayer破棄でも起きうる）がOLEを巻き添えにすると、
    // RegisterDragDropの登録ごと失われ、フォーカス仕様とは無関係に
    // ウィンドウ全体へのD&Dが🚫固定になる（この場合Qtにドラッグイベントが
    // 一切届かないため[DND]ログも出ない）。S_OK=「未初期化だった」なら
    // 異常検出なので再初期化したまま維持し、S_FALSE=「健全」なら
    // プローブで増えた参照カウントを直ちに戻す。
    {
        const HRESULT oleHr = OleInitialize(nullptr);
        if (oleHr == S_OK) {
            qWarning() << "[DND][MainWindow] OLE apartment was dead! re-initialized "
                          "(D&D breakage detected; forcing drop-site re-registration)";
        } else if (oleHr == S_FALSE) {
            OleUninitialize(); // プローブ分の参照カウントを戻す（健全時）
        } else {
            qWarning() << "[DND][MainWindow] OleInitialize probe failed hr=0x"
                       << Qt::hex << static_cast<unsigned long>(oleHr) << Qt::dec;
        }
    }

    setAcceptDrops(true);
    if (m_canvas)
        m_canvas->setAcceptDrops(true);
    if (m_island)
        m_island->setAcceptDrops(true);
    if (m_resizer)
        m_resizer->setAcceptDrops(true);
    if (m_hotkeyOverlay)
        m_hotkeyOverlay->setAcceptDrops(true);
    makeDropTargetsRecursive(this);

    // 2026-07-09 🚫再発調査（重要）: setAcceptDrops(true)は既にtrueの
    // ウィジェットには完全なno-opで、OLE側のRegisterDragDrop登録が失われて
    // いても再登録されない（＝従来のrefreshDropTargets()は「壊れた後の復旧」
    // としては機能していなかった）。WA_DropSiteRegisteredのOFF→ONは
    // プラットフォームウィンドウの RevokeDragDrop→RegisterDragDrop を直接
    // 駆動するため、ネイティブ登録を確実に張り直せる。
    if (windowHandle()) {
        setAttribute(Qt::WA_DropSiteRegistered, false);
        setAttribute(Qt::WA_DropSiteRegistered, true);
    }

    // ネイティブ登録の直接検査・修復（2026-07-11。詳細は関数コメント参照）。
    ensureNativeDropRegistration();

    qDebug() << "[DND][MainWindow] drop targets refreshed"
             << "main=" << acceptDrops()
             << "canvas=" << (m_canvas && m_canvas->acceptDrops())
             << "island=" << (m_island && m_island->acceptDrops())
             << "resizer=" << (m_resizer && m_resizer->acceptDrops())
             << "hotkeyOverlay=" << (m_hotkeyOverlay && m_hotkeyOverlay->acceptDrops());
}

void MainWindow::refreshNativeDropStyle()
{
    HWND hwnd = reinterpret_cast<HWND>(winId());
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const LONG_PTR before = exStyle;
    exStyle &= ~WS_EX_TRANSPARENT;
    exStyle &= ~WS_EX_NOACTIVATE;
    if (exStyle != before) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        qDebug() << "[DND][MainWindow] native drop style refreshed"
                 << "before=0x" << Qt::hex << before
                 << "after=0x" << exStyle << Qt::dec;
    }
}

// 2026-07-11 🚫（D&D禁止固定）恒久対策の本命:
// これまでの調査で、🚫はフォーカスフラグ（Qt層）ではなくネイティブ層＝
// 「HWNDに対するRegisterDragDrop登録の喪失」で起きていることが濃厚と
// なった（🚫発生時に[DND]ログが一切出ない＝Qtへドラッグイベント自体が
// 届いていない）。壊す犯人の候補はmpv/WASAPIのCOM絡み・シェルAPI・
// OBSのgraphics-hook注入など複数あり、コードレビューだけでは特定
// しきれないため、「壊れたことを直接検出して張り直す」方式で恒久対応する。
//
// 仕組み: OLE(ole32.dll)はRegisterDragDrop(hwnd, target)の際、targetを
// そのHWNDの "OleDropTargetInterface" ウィンドウプロパティに保存する
// （数十年変わっていない実装で、登録状態を外部から検査する定石）。
// - プロパティが取れる間は健全 → ポインタをAddRefしてキャッシュ
// - 取れなくなったら登録喪失 → OLE再初期化＋キャッシュした
//   Qt製IDropTargetでRegisterDragDropを張り直す
void MainWindow::ensureNativeDropRegistration()
{
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd)
        return;

    IDropTarget *current = reinterpret_cast<IDropTarget*>(
        GetPropW(hwnd, L"OleDropTargetInterface"));
    if (current) {
        // 健全。初回・HWND再生成時はキャッシュを取り直す。
        if (m_cachedDropTarget != current
            || m_cachedDropHwnd != reinterpret_cast<quintptr>(hwnd)) {
            if (m_cachedDropTarget)
                reinterpret_cast<IDropTarget*>(m_cachedDropTarget)->Release();
            current->AddRef();
            m_cachedDropTarget = current;
            m_cachedDropHwnd = reinterpret_cast<quintptr>(hwnd);
            qDebug() << "[DND][watchdog] IDropTarget cached for hwnd" << hwnd;
        }
        return;
    }

    // 一度も登録を観測できていない場合は何もしない（プロパティ名が将来の
    // Windowsで変わった場合等に、5秒ごとの誤修復ループへ陥らないための保険）。
    if (!m_cachedDropTarget || m_cachedDropHwnd != reinterpret_cast<quintptr>(hwnd))
        return;

    qWarning() << "[DND][watchdog] RegisterDragDrop registration LOST on hwnd" << hwnd
               << "- repairing (OleInitialize + RegisterDragDrop)";
    // OLEアパートメントごと死んでいた場合に備えて再初期化（生きていれば
    // 参照カウント+1になるだけ。修理経路なのでバランスは取らず維持する）。
    OleInitialize(nullptr);
    const HRESULT hr = RegisterDragDrop(
        hwnd, reinterpret_cast<IDropTarget*>(m_cachedDropTarget));
    qWarning() << "[DND][watchdog] RegisterDragDrop repair result hr=0x"
               << Qt::hex << static_cast<unsigned long>(hr) << Qt::dec
               << (hr == S_OK ? "(repaired!)"
                   : hr == DRAGDROP_E_ALREADYREGISTERED ? "(already registered?)"
                   : "(failed)");
}

void MainWindow::makeDropTargetsRecursive(QWidget *root)
{
    if (!root)
        return;

    root->setAcceptDrops(true);
    const auto children = root->findChildren<QWidget*>();
    for (QWidget *child : children) {
        if (!child)
            continue;
        child->setAcceptDrops(true);
    }
}

void MainWindow::beginHotkeyCapture()
{
    if (!m_hotkeyOverlay)
        setupHotkeyOverlay();

    m_capturingHotkey = true;
    m_pendingHotkey = GlobalHotkey{};
    m_pendingHotkey.modifiers = 0;
    m_pendingHotkey.virtualKey = 0;
    m_pendingHotkeyHasKey = false;
    updateHotkeyCaptureDisplay();
    m_hotkeyOverlay->setGeometry(rect());
    m_hotkeyOverlay->show();
    m_hotkeyOverlay->raise();
    setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::cancelHotkeyCapture()
{
    m_capturingHotkey = false;
    m_pendingHotkeyHasKey = false;
    if (m_hotkeyOverlay)
        m_hotkeyOverlay->hide();
}

void MainWindow::commitHotkeyCapture()
{
    if (!m_pendingHotkeyHasKey || isModifierVirtualKey(m_pendingHotkey.virtualKey)) {
        updateHotkeyCaptureDisplay(AppLanguage::text(
            "Modifier keys alone cannot be registered", "修飾キーだけは登録できません"));
        return;
    }

    const GlobalHotkey previous = m_globalHotkey;
    if (registerGlobalHotkey(m_pendingHotkey)) {
        AppSettings::instance().setHotkey(m_globalHotkey.modifiers, m_globalHotkey.virtualKey);
        cancelHotkeyCapture();
        return;
    }

    registerGlobalHotkey(previous);
    updateHotkeyCaptureDisplay(AppLanguage::text(
        "Registration failed. Keeping the previous shortcut", "登録に失敗しました。以前のショートカットを維持します"));
}

void MainWindow::updateHotkeyCaptureDisplay(const QString &message)
{
    if (!m_hotkeyOverlay)
        return;

    const QString input = (m_pendingHotkey.modifiers || m_pendingHotkeyHasKey)
        ? hotkeyToString(m_pendingHotkey)
        : QStringLiteral("...");
    m_hotkeyInputLabel->setText(input);
    m_hotkeyCurrentLabel->setText(AppLanguage::text("Current: %1", "現在: %1")
        .arg(hotkeyToString(m_globalHotkey)));
    m_hotkeyMessageLabel->setText(message.isEmpty()
        ? AppLanguage::text("Enter = Confirm / Esc = Cancel", "Enter = 決定 / Esc = キャンセル")
        : message);
}

QString MainWindow::hotkeyToString(const GlobalHotkey &hotkey) const
{
    QStringList parts;
    if (hotkey.modifiers & MOD_CONTROL) parts << QStringLiteral("Ctrl");
    if (hotkey.modifiers & MOD_SHIFT) parts << QStringLiteral("Shift");
    if (hotkey.modifiers & MOD_ALT) parts << QStringLiteral("Alt");
    if (hotkey.modifiers & MOD_WIN) parts << QStringLiteral("Win");

    if (hotkey.virtualKey != 0 && !isModifierVirtualKey(hotkey.virtualKey)) {
        const int qtKey = static_cast<int>(hotkey.virtualKey);
        QString keyName = QKeySequence(qtKey).toString(QKeySequence::NativeText);
        if (keyName.isEmpty()) {
            if (hotkey.virtualKey == VK_SPACE)
                keyName = QStringLiteral("Space");
            else
                keyName = QStringLiteral("VK_%1").arg(hotkey.virtualKey);
        }
        parts << keyName;
    }

    return parts.isEmpty() ? QStringLiteral("None") : parts.join(QStringLiteral(" + "));
}

bool MainWindow::isModifierVirtualKey(UINT virtualKey) const
{
    return virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL ||
           virtualKey == VK_SHIFT   || virtualKey == VK_LSHIFT   || virtualKey == VK_RSHIFT ||
           virtualKey == VK_MENU    || virtualKey == VK_LMENU    || virtualKey == VK_RMENU ||
           virtualKey == VK_LWIN    || virtualKey == VK_RWIN;
}

UINT MainWindow::modifiersFromQt(Qt::KeyboardModifiers modifiers) const
{
    UINT result = 0;
    if (modifiers & Qt::ControlModifier) result |= MOD_CONTROL;
    if (modifiers & Qt::ShiftModifier) result |= MOD_SHIFT;
    if (modifiers & Qt::AltModifier) result |= MOD_ALT;
    if (modifiers & Qt::MetaModifier) result |= MOD_WIN;
    return result;
}

void MainWindow::animateToGeometry(const QRect &geom, int durationMs)
{
    if (!geom.isValid()) return;
    if (m_resizer) m_resizer->setAspectLock(false);
    m_resizeAnim->stop();
    m_resizeAnim->setDuration(durationMs);
    // 2026-07-15: OutBack(微小オーバーシュート)→InOutCubicへ変更。
    // フォーカス解除の「引く」カメラ演出と同じカーブ・同じ長さで
    // ウィンドウ復元が同時進行するように揃えるため(オーバーシュートは
    // カメラ側と動きがずれて見えるのでやめた)。
    m_resizeAnim->setEasingCurve(QEasingCurve::InOutCubic);
    m_resizeAnim->setStartValue(geometry());
    m_resizeAnim->setEndValue(geom);
    m_resizeAnim->start();
}

void MainWindow::layoutIsland()
{
    int iw = m_island->islandWidth();
    int ih = m_island->height();
    m_island->move((width() - iw) / 2, 15);
    // islandWidthが変わるたびに位置を更新するためsetIslandWidthをフック
    connect(m_island, &DynamicIsland::islandWidthChanged, this, [this]{
        m_island->move((width() - m_island->islandWidth()) / 2, 15);
    });
}

// 2026-07-19: レイヤー機能を一旦切り離し（src/old/参照）。
// void MainWindow::layoutLayerBar()
// {
//     if (!m_layerBar)
//         return;
//     const int margin = 16;
//     const int x = width() - m_layerBar->width() - margin;
//     const int y = (height() - m_layerBar->height()) / 2;
//     m_layerBar->move(x, y);
// }

// ============================================================
//  プラグイン .zip ドロップの確認（2026-09-13）
//
//  従来は CanvasWidget が QMessageBox::question() をモーダルで出していた。
//  島（DynamicIsland + IslandPromptPanel）へ移したことで非モーダルになり、
//  「答えが返る前に次の .zip が来る」ケースが発生しうるためキューを持つ。
//  DLLのメタデータ読み取り（previewPluginZip）はプラグインを
//  インスタンス化しないので、プロンプトを出す時点ではまだ
//  プラグイン作者のコードは一切動いていない。
// ============================================================
//  プラグイン .zip ドロップの確認（2026-09-13）
//
//  従来は CanvasWidget が QMessageBox::question() をモーダルで出していた。
//  島（DynamicIsland + IslandPromptPanel）へ移したことで非モーダルになり、
//  「答えが返る前に次の .zip が来る」ケースが発生しうるためキューを持つ。
//
//  zipの展開（PowerShellのExpand-Archive）は非同期版を使う。同期版は
//  PowerShellの起動だけで数百ms〜1秒以上UIスレッドを止めるため、
//  ドロップ直後に画面が固まり、承認後は島の閉じアニメーションが
//  途中で止まって見えていた（PluginManager::previewPluginZipAsync 参照）。
//  DLLのメタデータ読み取りも、承認後の読み込みも、コールバックが
//  メインスレッドで呼ばれるのでスレッド安全性の心配は要らない。
// ============================================================
void MainWindow::enqueuePluginInstallPrompt(const QString &zipFilePath)
{
    if (zipFilePath.isEmpty())
        return;
    // 同じファイルを二重に積まない（複数ファイルドロップで同一パスが
    // 重複して届いた場合の保険）。
    if (!m_pendingPluginZips.contains(zipFilePath))
        m_pendingPluginZips.append(zipFilePath);
    processNextPluginInstallPrompt();
}

// プロンプトを1つ出す共通部分。島を確実に出し（openPromptView）、答えが
// 返ったら通常の島の振る舞いへ戻す（closePromptView）。
void MainWindow::showIslandPrompt(const IslandPromptContent &content,
                                  std::function<void(bool)> answered)
{
    if (!m_island || !m_islandCtrl)
        return;
    m_islandCtrl->openPromptView();
    m_island->showPrompt(content, [this, answered](bool accepted) {
        m_islandCtrl->closePromptView();
        refreshDropTargets();
        if (answered)
            answered(accepted);
    });
    // プロンプト内のボタンもドロップを素通しできるようにしておく
    // （島の子ウィジェットが増えたときの決まり事。refreshDropTargets参照）。
    refreshDropTargets();
}

// 1件ぶんの処理が完全に終わった。次があれば続ける。
void MainWindow::finishPluginInstallStep()
{
    m_pluginPromptBusy = false;
    if (m_pendingPluginZips.isEmpty())
        return;
    // 島の閉じアニメーション（250ms）が終わってから次を出す。
    QPointer<MainWindow> selfGuard(this);
    QTimer::singleShot(300, this, [selfGuard]() {
        if (selfGuard)
            selfGuard->processNextPluginInstallPrompt();
    });
}

void MainWindow::processNextPluginInstallPrompt()
{
    // m_pluginPromptBusy は「展開待ちを含めて1件を処理中」の印。
    // isPromptOpen() だけでは、展開中（プロンプトがまだ出ていない時間）に
    // 次のドロップが来たときに二重で走ってしまう。
    if (!m_island || m_pluginPromptBusy || m_island->isPromptOpen()
        || m_pendingPluginZips.isEmpty())
        return;

    m_pluginPromptBusy = true;
    const QString zipFilePath = m_pendingPluginZips.takeFirst();
    const QString fileName    = QFileInfo(zipFilePath).fileName();

    QPointer<MainWindow> selfGuard(this);
    PluginManager::previewPluginZipAsync(zipFilePath, this,
        [selfGuard, zipFilePath, fileName](const PluginManager::PluginZipPreview &preview) {
        if (!selfGuard)
            return;

        IslandPromptContent content;
        content.title   = QStringLiteral("Install Plugin");
        content.subject = fileName;

        if (!preview.ok) {
            qWarning() << "[Plugin][MainWindow] zip preview failed:"
                       << zipFilePath << preview.errorMessage;
            content.message       = QStringLiteral("This file could not be recognized as a plugin.");
            content.details       = QStringList{preview.errorMessage};
            content.acceptEnabled = false;
            content.rejectText    = QStringLiteral("CLOSE");
        } else {
            bool anyCompatible = false;
            QStringList details;
            for (const PluginManager::PluginZipEntry &entry : preview.entries) {
                const QString ids = entry.keys.isEmpty()
                    ? QStringLiteral("(unknown ID)")
                    : entry.keys.join(QStringLiteral(", "));
                details << QStringLiteral("%1  [%2]%3")
                               .arg(entry.fileName, ids,
                                    entry.iidMatches
                                        ? QString()
                                        : QStringLiteral("  * incompatible"));
                anyCompatible = anyCompatible || entry.iidMatches;
            }
            content.message = QStringLiteral("Install as a PocketBoard plugin?");
            content.details = details;
            content.warning = QStringLiteral(
                "Plugins run as native code. "
                "Only install plugins from sources you trust.");
            content.acceptEnabled = anyCompatible;
            if (!anyCompatible) {
                content.message    = QStringLiteral("No plugin in this file is compatible with this build.");
                content.warning.clear();
                content.rejectText = QStringLiteral("CLOSE");
            }
        }

        selfGuard->showIslandPrompt(content, [selfGuard, zipFilePath](bool accepted) {
            if (!selfGuard)
                return;
            if (!accepted) {
                qDebug() << "[Plugin][MainWindow] plugin install cancelled by user:"
                         << zipFilePath;
                selfGuard->finishPluginInstallStep();
                return;
            }

            PluginManager::instance().installPluginZipAsync(zipFilePath, selfGuard,
                [selfGuard, zipFilePath](bool ok, const QString &err) {
                if (!selfGuard)
                    return;
                if (ok) {
                    qDebug() << "[Plugin][MainWindow] plugin package installed:" << zipFilePath;
                    selfGuard->finishPluginInstallStep();
                    return;
                }
                qWarning() << "[Plugin][MainWindow] plugin package install failed:"
                           << zipFilePath << err;
                IslandPromptContent failure;
                failure.title         = QStringLiteral("Install Plugin");
                failure.subject       = QStringLiteral("Installation failed");
                failure.details       = QStringList{err};
                failure.acceptEnabled = false;
                failure.rejectText    = QStringLiteral("CLOSE");
                selfGuard->showIslandPrompt(failure, [selfGuard](bool) {
                    if (selfGuard)
                        selfGuard->finishPluginInstallStep();
                });
            });
        });
    });
}

void MainWindow::registerIslandSettingsControls()
{
    AppSettings *cfg = &AppSettings::instance();
    CanvasWidget *canvas = m_canvas;
    const bool japanese = AppLanguage::usesJapanese();

    // 2026-09-16: 項目の並びを作り直した（「見た目」タブは「全般」に改名）。
    //   全般  : グリッドを表示 / グリッドサイズ / ガラス表現 /
    //           Windowsのカラー設定を使用 / カラー設定（↑がオフのときだけ出す）
    //   操作  : ズームの感度 / カメラ移動の慣性 / 呼出しショートカットキー（3欄）/
    //           A・B・C（操作設定の仮置き。中身は未実装）
    //   その他: ウィンドウサイズを固定 / 配信: 画面外でリソースを節約
    // セクションIDは変えていない（host.appearance のまま。表示名だけ変更）。
    IslandControlSection general;
    general.id = IslandControlRegistry::hostSectionId(QStringLiteral("appearance"));
    general.title = AppLanguage::text("General", "全般");
    general.category = IslandControlCategory::Appearance;

    IslandControlSection behavior;
    behavior.id = IslandControlRegistry::hostSectionId(QStringLiteral("behavior"));
    behavior.title = AppLanguage::text("Controls", "操作");
    behavior.category = IslandControlCategory::Behavior;

    IslandControlSection other;
    other.id = IslandControlRegistry::hostSectionId(QStringLiteral("other"));
    other.title = AppLanguage::text("Other", "その他");
    other.category = IslandControlCategory::Other;

    // ================= 全般 =================
    // 2026-10-13: Windows起動時に自動で起動（スタートアップ）。全般の一番上。
    // 状態はレジストリ（HKCU\...\Run の "PocketBoard"）を毎回読む。
    // インストーラの「Windows起動時に自動で起動」も同じ値を書く。
    {
        IslandControlEntry e;
        e.spec.id = QStringLiteral("launchAtStartup");
        e.spec.label = AppLanguage::text("Launch at Windows startup", "Windows起動時に自動で起動");
        e.spec.kind = PbControlKind::Toggle;
        e.spec.tooltip = AppLanguage::text(
            "Starts in the tray when Windows starts. Open it from the tray icon or the shortcut key.",
            "Windowsの起動時にトレイに常駐した状態で起動します。トレイアイコンかショートカットキーで開けます。");
        e.currentValue = [] { return QVariant(StartupRegistration::isEnabled()); };
        e.onValueChanged = [](const QVariant &v) { StartupRegistration::setEnabled(v.toBool()); };
        general.entries.append(e);
    }
    // 2026-09-18: 表示言語。Windows の表示言語から自動で決める実装は廃止し、
    // ここで明示的に選ぶ。変えたら本体セクションを登録し直して文言を作り直す
    // （この場で作り直すと、今クリックしている部品ごと壊すので次のイベントループで）。
    {
        IslandControlEntry e;
        e.spec.id = QStringLiteral("appLanguage");
        e.spec.label = AppLanguage::text("Language", "言語");
        e.spec.kind = PbControlKind::Choice;
        e.spec.options.append({ QStringLiteral("ja"), QStringLiteral("日本語") });
        e.spec.options.append({ QStringLiteral("en"), QStringLiteral("English") });
        e.spec.tooltip = AppLanguage::text(
            "Some text is only replaced after a restart.",
            "一部の文言は再起動後に入れ替わります。");
        e.currentValue = [cfg] {
            return QVariant(cfg->language() == AppLanguage::Language::English
                                ? QStringLiteral("en") : QStringLiteral("ja"));
        };
        e.onValueChanged = [this, cfg](const QVariant &v) {
            const bool english = (v.toString() == QLatin1String("en"));
            cfg->setLanguage(english ? AppLanguage::Language::English
                                     : AppLanguage::Language::Japanese);
            QTimer::singleShot(0, this, [this] { registerIslandSettingsControls(); });
        };
        general.entries.append(e);
    }
    {
        IslandControlEntry e;
        e.spec.id = QStringLiteral("showGrid");
        e.spec.label = AppLanguage::text("Show Grid", "グリッドを表示");
        e.spec.kind = PbControlKind::Toggle;
        e.currentValue = [canvas] { return QVariant(canvas->showGrid()); };
        e.onValueChanged = [canvas](const QVariant &v) { canvas->setShowGrid(v.toBool()); };
        general.entries.append(e);
    }
    {
        IslandControlEntry e;
        e.spec.id = QStringLiteral("gridSize");
        e.spec.label = AppLanguage::text("Grid Size", "グリッドサイズ");
        e.spec.kind = PbControlKind::SpinBox;
        e.spec.minValue = 10;
        e.spec.maxValue = 400;
        e.spec.step = 10;
        e.currentValue = [canvas] { return QVariant(canvas->gridSize()); };
        e.onValueChanged = [canvas](const QVariant &v) { canvas->setGridSize(v.toInt()); };
        general.entries.append(e);
    }
    {
        // 2026-07-19: バブルの疑似ガラスハイライトの品質3択。
        //   performance: ガラス描画なし（表示名「シンプル」。設定値のidは互換のため据え置き） / medium: ライティングあり（既定）
        //   quality: ライティング＋疑似屈折（屈折は未実装。当面mediumと同じ）
        struct GlassOption { const char *id; const char *english; const char *japanese; AppSettings::GlassEffectQuality mode; };
        static const GlassOption kGlassOptions[] = {
            { "performance", "Simple",      "シンプル",           AppSettings::GlassEffectQuality::Performance },
            { "medium",      "Balanced",    "中間",               AppSettings::GlassEffectQuality::Medium },
            { "quality",     "Quality",     "品質優先",           AppSettings::GlassEffectQuality::Quality },
        };
        IslandControlEntry e;
        e.spec.id = QStringLiteral("glassEffectQuality");
        e.spec.label = AppLanguage::text("Glass Effect", "ガラス表現");
        e.spec.kind = PbControlKind::Choice;
        for (const GlassOption &opt : kGlassOptions)
            e.spec.options.append({ QString::fromLatin1(opt.id),
                                    QString::fromUtf8(japanese ? opt.japanese : opt.english) });
        e.currentValue = [cfg] {
            for (const GlassOption &opt : kGlassOptions) {
                if (opt.mode == cfg->glassEffectQuality())
                    return QVariant(QString::fromLatin1(opt.id));
            }
            return QVariant(QStringLiteral("medium"));
        };
        e.onValueChanged = [cfg, canvas, generalId = general.id](const QVariant &v) {
            const QString id = v.toString();
            for (const GlassOption &opt : kGlassOptions) {
                if (id == QLatin1String(opt.id)) {
                    cfg->setGlassEffectQuality(opt.mode);
                    // 2026-09-16: ガラススタイルは「中間」以上のときだけ出す
                    // （シンプル〈旧: パフォーマンス優先〉ではガラス自体を描かないため）。
                    IslandControlRegistry::instance().setControlVisible(
                        generalId, QStringLiteral("glassStyle"),
                        opt.mode != AppSettings::GlassEffectQuality::Performance);
                    // 静止画だけのシーンでは再描画契機が無いため明示的に更新
                    canvas->update();
                    return;
                }
            }
        };
        general.entries.append(e);
    }
    {
        // 2026-09-16: ガラススタイル（標準 / 削り出し）。2026-10-11: グミを追加。
        // 表示条件は上の「ガラス表現」が中間以上のとき（登録後に初期状態を設定）。
        IslandControlEntry e;
        e.spec.id = QStringLiteral("glassStyle");
        e.spec.label = AppLanguage::text("Glass Style", "ガラススタイル");
        e.spec.kind = PbControlKind::Choice;
        e.spec.options.append({ QStringLiteral("standard"),
                                AppLanguage::text("Standard", "標準") });
        e.spec.options.append({ QStringLiteral("carved"),
                                AppLanguage::text("Carved", "削り出し") });
        e.spec.options.append({ QStringLiteral("gummy"),
                                AppLanguage::text("Gummy", "グミ") });
        e.currentValue = [cfg] {
            return QVariant(AppSettings::glassStyleToString(cfg->glassStyle()));
        };
        e.onValueChanged = [cfg, canvas](const QVariant &v) {
            cfg->setGlassStyle(AppSettings::glassStyleFromString(v.toString()));
            canvas->update();
        };
        general.entries.append(e);
    }
    const QString generalId = general.id;
    const QString kAccentColorCodeId = QStringLiteral("accentColorCode");
    const QString kAccentHueId        = QStringLiteral("accentHue");
    const QString kAccentSaturationId = QStringLiteral("accentSaturation");
    const QString kAccentValueId      = QStringLiteral("accentValue");
    // 「Windowsのカラー設定を使用」がオフのときだけ出す行
    const QStringList customColorRows = { kAccentColorCodeId, kAccentHueId,
                                          kAccentSaturationId, kAccentValueId };

    // 2026-09-16: カラーコードと H/S/V スライダーの連動。
    // H/S/V は整数（H 0〜359、S・V 0〜100%）で持ち、スライダーを動かしたときは
    // 「持っている3つの値」から色を作る（コード→HSV→コードの丸めで他の値が
    // じわじわずれないように）。コードを入れたときだけコードから3つを作り直す。
    // 無彩色（S=0）のコードでは色相が決まらないので、H は前の値のまま。
    struct AccentHsv { int h = 0; int s = 0; int v = 0; };
    auto accentHsv = std::make_shared<AccentHsv>();
    const auto hsvFromCode = [](const QString &code, AccentHsv &out) {
        const QColor c(code);
        if (!c.isValid())
            return;
        float h = 0, s = 0, v = 0;
        c.getHsvF(&h, &s, &v);
        if (h >= 0)
            out.h = qBound(0, qRound(h * 360.0f), 359);
        out.s = qBound(0, qRound(s * 100.0f), 100);
        out.v = qBound(0, qRound(v * 100.0f), 100);
    };
    hsvFromCode(cfg->customAccentColor(), *accentHsv);
    // 色を保存して島へ反映し、連動する他の部品へ値を渡す（変わった瞬間に1回だけ）。
    const auto applyAccent = [this, cfg, generalId = general.id, accentHsv,
                              kAccentColorCodeId, kAccentHueId, kAccentSaturationId, kAccentValueId]
        (const QString &code, bool pushCode, bool pushHsv) {
        if (code != cfg->customAccentColor()) {
            cfg->setCustomAccentColor(code);
            if (m_island)
                m_island->refreshAccentColor();
        }
        auto &reg = IslandControlRegistry::instance();
        if (pushCode)
            reg.pushControlValue(generalId, kAccentColorCodeId, code);
        if (pushHsv) {
            reg.pushControlValue(generalId, kAccentHueId, accentHsv->h);
            reg.pushControlValue(generalId, kAccentSaturationId, accentHsv->s);
            reg.pushControlValue(generalId, kAccentValueId, accentHsv->v);
        }
    };
    const auto codeFromHsv = [accentHsv] {
        return QColor::fromHsvF(accentHsv->h / 360.0f, accentHsv->s / 100.0f, accentHsv->v / 100.0f)
                   .name(QColor::HexRgb).toUpper();
    };
    {
        // 2026-09-16: 島（と設定の部品）の色を Windows のアクセントカラーから取るか。
        // オフにすると下の「カラー設定」が出てきて、カラーコードで指定できる。
        IslandControlEntry e;
        e.spec.id = QStringLiteral("useWindowsAccentColor");
        e.spec.label = AppLanguage::text("Use Windows Accent Color", "Windowsのカラー設定を使用");
        e.spec.kind = PbControlKind::Toggle;
        e.currentValue = [cfg] { return QVariant(cfg->useWindowsAccentColor()); };
        e.onValueChanged = [this, cfg, generalId, customColorRows](const QVariant &v) {
            cfg->setUseWindowsAccentColor(v.toBool());
            for (const QString &id : customColorRows)
                IslandControlRegistry::instance().setControlVisible(generalId, id, !v.toBool());
            if (m_island)
                m_island->refreshAccentColor();
        };
        general.entries.append(e);
    }
    {
        IslandControlEntry e;
        e.spec.id = kAccentColorCodeId;
        e.spec.label = AppLanguage::text("Accent Color", "カラー設定");
        e.spec.kind = PbControlKind::TextInput;
        e.spec.placeholder = QStringLiteral("#4A90E2");
        e.spec.maxLength = 7;
        e.currentValue = [cfg] { return QVariant(cfg->customAccentColor()); };
        e.onValueChanged = [cfg, accentHsv, applyAccent, hsvFromCode](const QVariant &v) {
            // "#RRGGBB" / "RRGGBB" を受け付ける。それ以外は元のコードに戻す（色は変えない）。
            QString text = v.toString().trimmed();
            if (!text.startsWith(QLatin1Char('#')))
                text.prepend(QLatin1Char('#'));
            static const QRegularExpression kHex(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
            if (!kHex.match(text).hasMatch()) {
                applyAccent(cfg->customAccentColor(), true, false);
                return;
            }
            const QString code = text.toUpper();
            hsvFromCode(code, *accentHsv);
            // 欄も "#RRGGBB" の形に揃えて戻す（小文字・# なしで入れた場合）
            applyAccent(code, code != v.toString(), true);
        };
        general.entries.append(e);
    }
    {
        // H/S/V。スライダーは離したとき（溝のクリックも）に1回だけ確定が来るので、
        // 色の保存・島の色の切り替え・コード欄の書き換えもその1回だけ。
        struct HsvRow { const QString *id; const char *english; const char *japanese; int max; int AccentHsv::*field; };
        const HsvRow rows[] = {
            { &kAccentHueId,        "Hue",        "H（色相）", 359, &AccentHsv::h },
            { &kAccentSaturationId, "Saturation", "S（彩度）", 100, &AccentHsv::s },
            { &kAccentValueId,      "Value",      "V（明度）", 100, &AccentHsv::v },
        };
        for (const HsvRow &row : rows) {
            IslandControlEntry e;
            e.spec.id = *row.id;
            e.spec.label = AppLanguage::text(row.english, row.japanese);
            e.spec.kind = PbControlKind::Slider;
            e.spec.minValue = 0;
            e.spec.maxValue = row.max;
            e.spec.step = 1;
            const auto field = row.field;
            e.currentValue = [accentHsv, field] { return QVariant((*accentHsv).*field); };
            e.onValueChanged = [accentHsv, field, applyAccent, codeFromHsv](const QVariant &v) {
                (*accentHsv).*field = v.toInt();
                applyAccent(codeFromHsv(), true, false);
            };
            general.entries.append(e);
        }
    }

    // ================= 操作 =================
    // Camera Pan Inertia の Choice option id ⇔ enum の対応表。
    // 島UIとAppSettingsの間でのみ使う値なので、enumの数値ではなく
    // 文字列IDで安定させる（enumの並び替え事故の影響を受けない）。
    struct InertiaOption { const char *id; const char *english; const char *japanese; AppSettings::CameraPanInertiaMode mode; };
    static const InertiaOption kInertiaOptions[] = {
        { "always",     "Always",      "常に有効",                 AppSettings::CameraPanInertiaMode::Always },
        { "canvasOnly", "Canvas Only", "キャンバスのみ",           AppSettings::CameraPanInertiaMode::CanvasOnly },
        { "focusOnly",  "Focus Only",  "フォーカス時のみ",         AppSettings::CameraPanInertiaMode::FocusOnly },
        { "off",        "Off",         "無効",                     AppSettings::CameraPanInertiaMode::Off },
    };
    // 2026-09-17: ウィンドウ操作の2項目（仮置きの「B」「C」を置き換え）。
    // 「操作」セクションの先頭（ズームの感度より上）に置く。
    {
        // 右ボタンを押したまま動かす → ウィンドウ移動（CanvasWidget::handleMouseMove）。
        // 動かさずに離した右クリックは従来どおりアクティブ化。
        IslandControlEntry e;
        e.spec.id = QStringLiteral("rightDragWindowMove");
        e.spec.label = AppLanguage::text("Right-drag to Move the Window", "右ドラッグでウィンドウを移動");
        e.spec.kind = PbControlKind::Toggle;
        e.currentValue = [cfg] { return QVariant(cfg->rightDragWindowMoveEnabled()); };
        e.onValueChanged = [cfg](const QVariant &v) { cfg->setRightDragWindowMoveEnabled(v.toBool()); };
        behavior.entries.append(e);
    }
    {
        // ON: 右下のハンドルを消して、どの辺・角からでもリサイズ（nativeEvent の WM_NCHITTEST）。
        IslandControlEntry e;
        e.spec.id = QStringLiteral("edgeResize");
        e.spec.label = AppLanguage::text("Resize from Any Edge", "どの辺からでもリサイズ");
        e.spec.kind = PbControlKind::Toggle;
        e.currentValue = [cfg] { return QVariant(cfg->edgeResizeEnabled()); };
        e.onValueChanged = [cfg](const QVariant &v) { cfg->setEdgeResizeEnabled(v.toBool()); };
        behavior.entries.append(e);
    }
    {
        // 2026-07-17: ホイール1コリあたりのズーム量（感度、%）。
        // CanvasWidget::handleWheel()がAppSettings::zoomWheelFactorStep()経由で読む。
        IslandControlEntry e;
        e.spec.id = QStringLiteral("zoomWheelSensitivity");
        e.spec.label = AppLanguage::text("Zoom Sensitivity", "ズームの感度");
        e.spec.kind = PbControlKind::Slider;
        e.spec.minValue = AppSettings::kZoomWheelSensitivityMin;
        e.spec.maxValue = AppSettings::kZoomWheelSensitivityMax;
        e.spec.step = 1;
        e.currentValue = [cfg] { return QVariant(cfg->zoomWheelSensitivityPercent()); };
        e.onValueChanged = [cfg](const QVariant &v) { cfg->setZoomWheelSensitivityPercent(v.toInt()); };
        behavior.entries.append(e);
    }
    {
        IslandControlEntry e;
        e.spec.id = QStringLiteral("cameraPanInertia");
        e.spec.label = AppLanguage::text("Pan Inertia", "カメラ移動の慣性");
        e.spec.kind = PbControlKind::Choice;
        for (const InertiaOption &opt : kInertiaOptions)
            e.spec.options.append({ QString::fromLatin1(opt.id),
                                    QString::fromUtf8(japanese ? opt.japanese : opt.english) });
        e.currentValue = [cfg] {
            for (const InertiaOption &opt : kInertiaOptions) {
                if (opt.mode == cfg->cameraPanInertiaMode())
                    return QVariant(QString::fromLatin1(opt.id));
            }
            return QVariant(QStringLiteral("always"));
        };
        e.onValueChanged = [cfg](const QVariant &v) {
            const QString id = v.toString();
            for (const InertiaOption &opt : kInertiaOptions) {
                if (id == QLatin1String(opt.id)) {
                    cfg->setCameraPanInertiaMode(opt.mode);
                    return;
                }
            }
        };
        behavior.entries.append(e);
    }
    {
        // 2026-09-16: ボタン（全画面の入力オーバーレイ）から、3つの欄へ変更。
        // 欄を押してキーを1つ押すと、その欄にそのキーが入る。1〜3個、どの欄に
        // 入れてもよい。修飾キー（Ctrl/Shift/Alt/Win）と普通のキー1つの組み合わせが
        // そろった時点で登録する（applySummonHotkeySlots）。
        IslandControlEntry e;
        e.spec.id = QStringLiteral("summonShortcut");
        e.spec.label = AppLanguage::text("Summon Shortcut Key", "呼出しショートカットキー");
        e.spec.kind = PbControlKind::KeyInput;
        e.columns = 3;
        e.keyName = [this](int vk) { return virtualKeyDisplayName(static_cast<UINT>(vk)); };
        e.currentValue = [this, cfg] {
            QList<uint> keySlots = cfg->hotkeySlots();
            // 覚えている並びが今の登録内容と食い違う（旧オーバーレイで変えた等）なら作り直す
            if (!hotkeySlotsMatch(keySlots, m_globalHotkey))
                keySlots = hotkeySlotsFrom(m_globalHotkey);
            QVariantList values;
            for (uint vk : keySlots)
                values.append(static_cast<int>(vk));
            return QVariant(values);
        };
        e.onValueChanged = [this](const QVariant &v) { applySummonHotkeySlots(v.toList()); };
        behavior.entries.append(e);
    }
    {
        // 2026-09-17: 元に戻す/やり直しで遡れる最大回数（1〜64）。
        // 仮置きの「A」をこれに置き換えた。CanvasWidget側は AppSettings::changed
        // を受けて、減らした分の古い履歴をすぐ捨てる。
        IslandControlEntry e;
        e.spec.id = QStringLiteral("undoHistoryLimit");
        e.spec.label = AppLanguage::text("Undo History (steps)", "元に戻せる回数");
        e.spec.kind = PbControlKind::SpinBox;
        e.spec.minValue = AppSettings::kUndoHistoryLimitMin;
        e.spec.maxValue = AppSettings::kUndoHistoryLimitMax;
        e.spec.step = 1;
        e.currentValue = [cfg] { return QVariant(cfg->undoHistoryLimit()); };
        e.onValueChanged = [cfg](const QVariant &v) { cfg->setUndoHistoryLimit(v.toInt()); };
        behavior.entries.append(e);
    }
    // 2026-09-22: キャンバス操作のショートカット（元に戻す／やり直し／バブルを削除と
    // 各サブ）の欄は設定画面から外した。変えたい人が少ないため。
    // 割り当て自体は AppSettings::canvasShortcut() の既定値／保存値のまま有効。
    // ================= その他 =================
    {
        IslandControlEntry e;
        // 2026-10-11: 旧「ウィンドウサイズを固定」(ON/OFF)を3択にした。
        e.spec.id = QStringLiteral("focusWindowMode");
        e.spec.label = AppLanguage::text("Bubble Fullscreen Fixing", "バブル全画面時の固定");
        e.spec.kind = PbControlKind::Choice;
        e.spec.options.append({ QStringLiteral("canvas"),
                                AppLanguage::text("Fix Canvas", "キャンバス固定") });
        e.spec.options.append({ QStringLiteral("window"),
                                AppLanguage::text("Fix Window", "ウィンドウ固定") });
        e.spec.options.append({ QStringLiteral("off"),
                                AppLanguage::text("Off", "オフ") });
        e.currentValue = [cfg] {
            switch (cfg->focusWindowMode()) {
            case AppSettings::FocusWindowMode::CanvasFixed: return QVariant(QStringLiteral("canvas"));
            case AppSettings::FocusWindowMode::WindowFixed: return QVariant(QStringLiteral("window"));
            default:                                        return QVariant(QStringLiteral("off"));
            }
        };
        e.onValueChanged = [cfg](const QVariant &v) {
            const QString id = v.toString();
            cfg->setFocusWindowMode(id == QLatin1String("canvas") ? AppSettings::FocusWindowMode::CanvasFixed
                                  : id == QLatin1String("window") ? AppSettings::FocusWindowMode::WindowFixed
                                  : AppSettings::FocusWindowMode::Off);
        };
        other.entries.append(e);
    }
    {
        // 2026-10-11: キャンバス固定でバブル全画面を解除するとき、サイズだけでなく
        // 位置も全画面前に戻す。
        IslandControlEntry e;
        e.spec.id = QStringLiteral("rememberWindowPosition");
        e.spec.label = AppLanguage::text("Remember Window Position", "ウィンドウ位置を記憶");
        e.spec.kind = PbControlKind::Toggle;
        e.currentValue = [cfg] { return QVariant(cfg->rememberWindowPosition()); };
        e.onValueChanged = [cfg](const QVariant &v) { cfg->setRememberWindowPosition(v.toBool()); };
        other.entries.append(e);
    }
    {
        IslandControlEntry e;
        e.spec.id = QStringLiteral("streamOffscreenResourceSaving");
        e.spec.label = AppLanguage::text("Streaming: Save Resources When Off-screen", "配信: 画面外でリソースを節約");
        e.spec.kind = PbControlKind::Toggle;
        e.currentValue = [cfg] {
            return QVariant(cfg->streamOffscreenResourceSavingEnabled());
        };
        e.onValueChanged = [cfg](const QVariant &v) {
            cfg->setStreamOffscreenResourceSavingEnabled(v.toBool());
        };
        other.entries.append(e);
    }

    // 登録順＝表示順（本体セクション同士は登録順に並ぶ）。
    IslandControlRegistry::instance().registerSection(general);
    IslandControlRegistry::instance().registerSection(behavior);
    IslandControlRegistry::instance().registerSection(other);

    // 登録後でないと状態を設定できない
    IslandControlRegistry::instance().setControlVisible(
        generalId, QStringLiteral("glassStyle"),
        cfg->glassEffectQuality() != AppSettings::GlassEffectQuality::Performance);
    for (const QString &id : customColorRows)
        IslandControlRegistry::instance().setControlVisible(generalId, id,
                                                            !cfg->useWindowsAccentColor());
}

// ---- 呼出しショートカットキーの3欄（2026-09-16）----
QList<uint> MainWindow::hotkeySlotsFrom(const GlobalHotkey &hotkey) const
{
    QList<uint> keySlots;
    if (hotkey.modifiers & MOD_CONTROL) keySlots << VK_CONTROL;
    if (hotkey.modifiers & MOD_SHIFT)   keySlots << VK_SHIFT;
    if (hotkey.modifiers & MOD_ALT)     keySlots << VK_MENU;
    if (hotkey.modifiers & MOD_WIN)     keySlots << VK_LWIN;
    if (hotkey.virtualKey != 0 && !isModifierVirtualKey(hotkey.virtualKey))
        keySlots << hotkey.virtualKey;
    // 欄は3つ。修飾キー3つ＋キーの4つ組（旧オーバーレイで登録できた）は先頭3つだけ出す
    while (keySlots.size() > 3)
        keySlots.removeAt(keySlots.size() - 2);
    while (keySlots.size() < 3)
        keySlots << 0;
    return keySlots;
}

bool MainWindow::hotkeySlotsMatch(const QList<uint> &keySlots, const GlobalHotkey &hotkey) const
{
    if (keySlots.size() != 3)
        return false;
    UINT modifiers = 0;
    UINT key = 0;
    for (uint vk : keySlots) {
        switch (vk) {
        case 0: break;
        case VK_CONTROL: modifiers |= MOD_CONTROL; break;
        case VK_SHIFT:   modifiers |= MOD_SHIFT; break;
        case VK_MENU:    modifiers |= MOD_ALT; break;
        case VK_LWIN:    modifiers |= MOD_WIN; break;
        default:         key = vk; break;
        }
    }
    return modifiers == hotkey.modifiers && key == hotkey.virtualKey;
}

void MainWindow::applySummonHotkeySlots(const QVariantList &values)
{
    QList<uint> keySlots;
    UINT modifiers = 0;
    QList<UINT> keys;
    bool duplicate = false;
    for (int i = 0; i < 3; ++i) {
        const uint vk = i < values.size() ? static_cast<uint>(values[i].toInt()) : 0;
        if (vk != 0 && keySlots.contains(vk))
            duplicate = true;
        keySlots << vk;
        switch (vk) {
        case 0: break;
        case VK_CONTROL: modifiers |= MOD_CONTROL; break;
        case VK_SHIFT:   modifiers |= MOD_SHIFT; break;
        case VK_MENU:    modifiers |= MOD_ALT; break;
        case VK_LWIN:    modifiers |= MOD_WIN; break;
        default:         keys << vk; break;
        }
    }
    // 欄の並びは入れたとおりに覚えておく（未完成の途中でも、開き直して続きを入れられる）
    AppSettings::instance().setHotkeySlots(keySlots);

    const bool allEmpty = std::all_of(keySlots.begin(), keySlots.end(), [](uint vk) { return vk == 0; });
    if (allEmpty) {
        // 全部空にしたら、呼出しショートカットなし
        unregisterGlobalHotkey();
        m_globalHotkey.modifiers = 0;
        m_globalHotkey.virtualKey = 0;
        AppSettings::instance().setHotkey(0, 0);
        qInfo() << "Global hotkey cleared";
        return;
    }
    // 使える組み合わせ＝普通のキーがちょうど1つ（修飾キーは0〜2個）。
    // 修飾キーだけ・普通のキーが2つ以上・同じキーの重複は、まだ入力途中として
    // 今の登録を変えない。
    if (duplicate || keys.size() != 1)
        return;

    GlobalHotkey next;
    next.modifiers = modifiers;
    next.virtualKey = keys.first();
    if (next.modifiers == m_globalHotkey.modifiers && next.virtualKey == m_globalHotkey.virtualKey
        && m_globalHotkeyRegistered)
        return;

    const GlobalHotkey previous = m_globalHotkey;
    if (registerGlobalHotkey(next)) {
        AppSettings::instance().setHotkey(m_globalHotkey.modifiers, m_globalHotkey.virtualKey);
        return;
    }
    // 他のアプリが使っている等で登録できなければ、前のショートカットに戻す
    qWarning() << "Summon hotkey registration failed; keeping" << hotkeyToString(previous);
    if (previous.virtualKey != 0)
        registerGlobalHotkey(previous);
}

QString MainWindow::virtualKeyDisplayName(UINT vk) const
{
    switch (vk) {
    case VK_CONTROL: return QStringLiteral("Ctrl");
    case VK_SHIFT:   return QStringLiteral("Shift");
    case VK_MENU:    return QStringLiteral("Alt");
    case VK_LWIN:
    case VK_RWIN:    return QStringLiteral("Win");
    case VK_SPACE:   return QStringLiteral("Space");
    case VK_RETURN:  return QStringLiteral("Enter");
    case VK_TAB:     return QStringLiteral("Tab");
    case VK_ESCAPE:  return QStringLiteral("Esc");
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24)
        return QStringLiteral("F%1").arg(vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return QString(QChar(static_cast<char16_t>(vk)));
    // それ以外はキーボード配列に合わせた名前を Windows から取る
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {   // 拡張キーは拡張ビットを立てないとテンキー側の名前になる
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        scan |= 0x100;
        break;
    default:
        break;
    }
    wchar_t name[64] = {};
    if (scan != 0 && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0)
        return QString::fromWCharArray(name);
    return QStringLiteral("VK_%1").arg(vk);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (isCanvasOriginatedDrag(event, m_canvas)) {
        event->ignore();
        return;
    }

    if (m_canvas && m_canvas->isFocusDropBlocked()) {
        m_canvas->logDropMimeData("MainWindow::dragEnterEvent", event->mimeData(), false,
            QStringLiteral("a bubble is focused/fullscreen; show forbidden cursor"));
        event->ignore();
        return;
    }
    if (m_canvas) {
        QString reason;
        const bool supported = m_canvas->canAcceptDropMimeData(event->mimeData(), &reason);
        if (!supported)
            reason = QStringLiteral("MainWindow override permissive drag-stage accept; original reason: %1").arg(reason);
        m_canvas->logDropMimeData("MainWindow::dragEnterEvent", event->mimeData(), true, reason);
    }
    forceAcceptDropEvent(event);
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (isCanvasOriginatedDrag(event, m_canvas)) {
        event->ignore();
        return;
    }

    if (m_canvas && m_canvas->isFocusDropBlocked()) {
        m_canvas->logDropMimeData("MainWindow::dragMoveEvent", event->mimeData(), false,
            QStringLiteral("a bubble is focused/fullscreen; show forbidden cursor"));
        event->ignore();
        return;
    }
    if (m_canvas) {
        QString reason;
        const bool supported = m_canvas->canAcceptDropMimeData(event->mimeData(), &reason);
        if (!supported)
            reason = QStringLiteral("MainWindow override permissive drag-stage accept; original reason: %1").arg(reason);
        m_canvas->logDropMimeData("MainWindow::dragMoveEvent", event->mimeData(), true, reason);
    }
    forceAcceptDropEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (isCanvasOriginatedDrag(event, m_canvas)) {
        event->ignore();
        return;
    }

    bool dropped = false;
    if (m_canvas) {
        dropped = m_canvas->handleDropMimeData(
            event->mimeData(),
            QPointF(m_canvas->mapFrom(this, event->position().toPoint())),
            QStringLiteral("MainWindow::dropEvent"));
        if (!dropped)
            qDebug() << "[DND][MainWindow] drop accepted by override, but content is unsupported";
    }
    forceAcceptDropEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_canvas)
        return QWidget::eventFilter(watched, event);

    QWidget *watchedWidget = qobject_cast<QWidget*>(watched);
    const bool belongsToThisWindow =
        watched == this ||
        (watchedWidget && (watchedWidget == this || isAncestorOf(watchedWidget)));
    if (!belongsToThisWindow)
        return QWidget::eventFilter(watched, event);

    auto mapEventPositionToCanvas = [this, watchedWidget](const QPointF &pos) {
        QPoint windowPos = pos.toPoint();
        if (watchedWidget && watchedWidget != this)
            windowPos = watchedWidget->mapTo(this, windowPos);
        return QPointF(m_canvas->mapFrom(this, windowPos));
    };

    if (event->type() == QEvent::DragEnter) {
        auto *dragEvent = static_cast<QDragEnterEvent*>(event);
        if (isCanvasOriginatedDrag(dragEvent, m_canvas)) {
            dragEvent->ignore();
            return true;
        }
        const QString widgetName = QString::fromLatin1(
            watchedWidget ? watchedWidget->metaObject()->className()
                          : watched->metaObject()->className());
        if (m_canvas->isFocusDropBlocked()) {
            m_canvas->logDropMimeData("MainWindow::eventFilter.dragEnter",
                dragEvent->mimeData(), false,
                QStringLiteral("a bubble is focused/fullscreen on %1; show forbidden cursor").arg(widgetName));
            dragEvent->ignore();
            return true;
        }
        QString reason;
        const bool supported = m_canvas->canAcceptDropMimeData(dragEvent->mimeData(), &reason);
        reason = supported
            ? QStringLiteral("accepted on %1; %2").arg(widgetName, reason)
            : QStringLiteral("permissive drag-stage accept on %1; original reason: %2")
                  .arg(widgetName, reason);
        m_canvas->logDropMimeData("MainWindow::eventFilter.dragEnter",
                                  dragEvent->mimeData(), true, reason);
        forceAcceptDropEvent(dragEvent);
        return true;
    }

    if (event->type() == QEvent::DragMove) {
        auto *dragEvent = static_cast<QDragMoveEvent*>(event);
        if (isCanvasOriginatedDrag(dragEvent, m_canvas)) {
            dragEvent->ignore();
            return true;
        }
        const QString widgetName = QString::fromLatin1(
            watchedWidget ? watchedWidget->metaObject()->className()
                          : watched->metaObject()->className());
        if (m_canvas->isFocusDropBlocked()) {
            m_canvas->logDropMimeData("MainWindow::eventFilter.dragMove",
                dragEvent->mimeData(), false,
                QStringLiteral("a bubble is focused/fullscreen on %1; show forbidden cursor").arg(widgetName));
            dragEvent->ignore();
            return true;
        }
        QString reason;
        const bool supported = m_canvas->canAcceptDropMimeData(dragEvent->mimeData(), &reason);
        reason = supported
            ? QStringLiteral("accepted on %1; %2").arg(widgetName, reason)
            : QStringLiteral("permissive drag-stage accept on %1; original reason: %2")
                  .arg(widgetName, reason);
        m_canvas->logDropMimeData("MainWindow::eventFilter.dragMove",
                                  dragEvent->mimeData(), true, reason);
        forceAcceptDropEvent(dragEvent);
        return true;
    }

    if (event->type() == QEvent::Drop) {
        auto *dropEvent = static_cast<QDropEvent*>(event);
        if (isCanvasOriginatedDrag(dropEvent, m_canvas)) {
            dropEvent->ignore();
            return true;
        }
        const QPointF canvasPos = mapEventPositionToCanvas(dropEvent->position());
        const bool dropped = m_canvas->handleDropMimeData(
            dropEvent->mimeData(), canvasPos,
            QStringLiteral("MainWindow::eventFilter:%1")
                .arg(watchedWidget ? watchedWidget->metaObject()->className()
                                   : watched->metaObject()->className()));
        if (!dropped)
            qDebug() << "[DND][MainWindow] drop accepted by eventFilter, but content is unsupported"
                     << "widget=" << (watchedWidget ? watchedWidget->metaObject()->className()
                                                    : watched->metaObject()->className());
        forceAcceptDropEvent(dropEvent);
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

void MainWindow::paintEvent(QPaintEvent *)
{
    // 枠線はCanvasWidget::paintEvent内で最後に描画するのでここは何も描かない
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // DPI変更に伴うQtの自動リサイズ中は、覚えている論理サイズを上書きしない。
    if (!m_dpiChanging)
        rememberDipSize();
    if (m_canvas)  m_canvas->setGeometry(rect());
    if (m_canvasOverlay) m_canvasOverlay->setGeometry(rect());
    if (m_hotkeyOverlay) m_hotkeyOverlay->setGeometry(rect());
    if (m_island)  m_island->move((width() - m_island->islandWidth()) / 2, 15);
    // 設定パネル展開中はパネル高さ（ウィンドウ縦サイズ上限）を追従させる。
    if (m_island)  m_island->notifyHostWindowResized();
    if (m_resizer) m_resizer->move(width() - 40, height() - 40);
    // layoutLayerBar(); // 2026-07-19: レイヤー機能を一旦切り離し（src/old/参照）。
    scheduleWindowSettingsSave();
}

bool MainWindow::dragWindowNative(const QPointF &anchor, bool anchorIsRatio)
{
    HWND hwnd = reinterpret_cast<HWND>(winId());
    POINT cur;
    RECT wr;
    if (!hwnd || !GetCursorPos(&cur) || !GetWindowRect(hwnd, &wr))
        return false;
    const double w = wr.right - wr.left;
    const double h = wr.bottom - wr.top;
    const UINT dpi = GetDpiForWindow(hwnd);
    const double scale = dpi ? dpi / 96.0 : 1.0;
    const double ox = anchorIsRatio ? anchor.x() * w : anchor.x() * scale;
    const double oy = anchorIsRatio ? anchor.y() * h : anchor.y() * scale;
    return SetWindowPos(hwnd, nullptr,
                        qRound(cur.x - ox), qRound(cur.y - oy), 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

void MainWindow::rememberDipSize()
{
    if (!isVisible() || m_windowFullscreenActive)
        return;
    HWND hwnd = reinterpret_cast<HWND>(winId());
    RECT wr;
    const UINT dpi = GetDpiForWindow(hwnd);
    if (!hwnd || !dpi || !GetWindowRect(hwnd, &wr))
        return;
    m_dipSize = QSizeF((wr.right - wr.left) * 96.0 / dpi,
                       (wr.bottom - wr.top) * 96.0 / dpi);
}

// WM_DPICHANGED（Qt自身の処理）が終わった後に呼ぶ。覚えておいた論理サイズから
// 新しいDPIでの物理サイズを計算し直し、ずれていれば直す（左上は動かさない）。
void MainWindow::restoreDipSizeAfterDpiChange(UINT newDpi)
{
    HWND hwnd = reinterpret_cast<HWND>(winId());
    RECT wr;
    const bool resizeAnimRunning =
        m_resizeAnim && m_resizeAnim->state() == QAbstractAnimation::Running;
    if (hwnd && newDpi && m_dipSize.isValid() && !m_dipSize.isEmpty()
        && !m_windowFullscreenActive && !resizeAnimRunning
        && GetWindowRect(hwnd, &wr)) {
        const double scale = newDpi / 96.0;
        const int nw = qRound(m_dipSize.width()  * scale);
        const int nh = qRound(m_dipSize.height() * scale);
        if (nw != wr.right - wr.left || nh != wr.bottom - wr.top) {
            qDebug("[Dpi] size corrected %dx%d -> %dx%d (dpi=%u dip=%.2fx%.2f)",
                   int(wr.right - wr.left), int(wr.bottom - wr.top), nw, nh,
                   newDpi, m_dipSize.width(), m_dipSize.height());
            SetWindowPos(hwnd, nullptr, 0, 0, nw, nh,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    // 補正のリサイズ(WM_SIZE→resizeEvent)が終わってから解除する（その間は
    // 論理サイズを上書きしない）。
    m_dpiChanging = false;
}

void MainWindow::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    scheduleWindowSettingsSave();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    rememberDipSize();
    refreshDropTargets();
    if (!m_allowQuit)
        fadeInWindow();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // ウィンドウ全画面（独自管理）はここでは解除しない（hideToTray()と同じ
    // 理由。見た目の不自然さを避けるため、次回showFromTray()で表示する
    // 直前に非表示のまま解除する）。保存するジオメトリはsaveWindowSettings()
    // 内でpreFullscreenGeometry()を使うため、ここで解除しなくても正しい値が
    // 保存される。
    saveWindowSettings();
    if (m_allowQuit) {
        QWidget::closeEvent(event);
        return;
    }

    event->ignore();
    if (m_sessionAutoSaveTimer)
        m_sessionAutoSaveTimer->stop();   // この後で同期保存する
    {
        // 2026-07-19: レイヤー機能を一旦切り離したため、固定の1レイヤー
        // （session_0.json）にのみ保存する。
        SessionManager::save(m_canvas, kSingleLayerNumber, QString());
    }
    if (m_canvas)
        m_canvas->stopAllVideosForBackground();
    fadeOutAndHideWindow();
}

void MainWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // 2026-09-12: ウィンドウが非アクティブになった瞬間に、ラベル／行の
    // ドラッグを取り消す。掴んだまま他のアプリへ移るとマウスのグラブが
    // 外れて移動が届かなくなり、行が最後の位置で固まったまま「継続中」に
    // なる（戻ると再開する）。中途半端なのでここで畳む。並べ替えの確定も
    // 固定トグルもしない。離すイベントの取りこぼしそのものへの保険は
    // CanvasWidget::cancelStaleLabelDragModes()（毎tick）が別に持っている。
    if (event->type() == QEvent::ActivationChange && !isActiveWindow()) {
        if (m_canvas)
            m_canvas->cancelLabelDragModes();
    }
    if (event->type() == QEvent::WindowStateChange) {
        // ウィンドウ全画面（独自管理）に関わる副作用（リサイズハンドルの
        // 表示/非表示、フォーカス中動画のアスペクト再フィット等）は
        // setWindowFullscreen()側で直接処理しているため、ここでは行わない
        // （ウィンドウ全画面はQtのウィンドウ状態を使っていないため、この
        //   イベントはそもそも発生しない）。ここでは最小化などの純粋な
        //   Qtウィンドウ状態変化に対する一般的な後処理のみ行う。
        refreshDropTargets();
        scheduleWindowSettingsSave();
    }
}

// UI領域（島・再生バー）のヒット判定ヘルパー
bool MainWindow::isOnUI(const QPointF &pos) const
{
    // 島の矩形
    if (m_island) {
        QRect ir = m_island->geometry();
        if (ir.contains(pos.toPoint())) return true;
    }
    // 再生バー領域（ウィンドウ下部bH=56ピクセル + バッファー20px）
    if (m_canvas && m_canvas->isPlaybackBarVisibleForActiveVideo()
        && pos.y() >= height() - 56 - 20) {
        return true;
    }
    // 音量スライダー表示中はスライダー領域もUI
    if (m_canvas) {
        auto *av = m_canvas->activeVideo();
        if (av && av->isVolSliderVisible()) {
            if (av->volSliderScreenRect(height()).contains(pos))
                return true;
        }
    }
    // 2026-07-19: レイヤー機能を一旦切り離し（src/old/参照）。
    // if (m_layerBar && m_layerBar->isRevealed()) {
    //     QRect lr = m_layerBar->geometry();
    //     if (lr.contains(pos.toPoint())) return true;
    // }
    return false;
}

// 2026-09-15: 設定を開いている間は、キャンバスへマウス操作を一切流さない。
// 設定中はキャンバス上のUI（ラベル・再生バー等）を CanvasOverlayWidget が
// 隠しているが、当たり判定は CanvasWidget 側に残っているため、見えない
// ボタンが押せてしまう。島がウィンドウ全体を覆うので大半のイベントはそもそも
// 島が受けるが、端の余白（DynamicIsland::kSettingsInset）はここへ来る。
// 閉じ始めた時点（isSettingsOpen()==false）で通常へ戻す。
bool MainWindow::canvasInputBlockedBySettings() const
{
    return m_island && m_island->isSettingsOpen();
}

void MainWindow::mousePressEvent(QMouseEvent *e)
{
    if (canvasInputBlockedBySettings()) { e->accept(); return; }
    if (m_canvas) m_canvas->handleMousePress(e, isOnUI(e->position()));
}
void MainWindow::mouseMoveEvent(QMouseEvent *e)
{
    // ホバー（再生バーの出現やラベルのハイライト）も隠れたUIのためには動かさない。
    if (canvasInputBlockedBySettings()) { e->accept(); return; }
    if (m_canvas) {
        // ムーブは常に転送（ドラッグ中に値位置が外に出ることがあるため）
        m_canvas->handleMouseMove(e);
    }
}
void MainWindow::mouseReleaseEvent(QMouseEvent *e)
{
    if (canvasInputBlockedBySettings()) { e->accept(); return; }
    if (m_canvas) m_canvas->handleMouseRelease(e);
}
void MainWindow::wheelEvent(QWheelEvent *e)
{
    // 島の+ボタン上でホイール → マスターボリューム調整（1クレンジ=1%）。
    // 個別動画の音量スライダーの判定より先に見る（+ボタンは常に島の中に
    // あるため、判定順はどちらでも実害はないが、島自体の操作を優先する
    // という意味でここに置く）。
    if (m_canvas && m_island) {
        const QRect btnLocal = m_island->settingsButtonRect();
        const QRect btnInWindow(m_island->pos() + btnLocal.topLeft(), btnLocal.size());
        if (btnInWindow.contains(e->position().toPoint())) {
            const int steps = (e->angleDelta().y() > 0) ? 1 : -1;
            const int newValue = m_canvas->applyMasterVolumeWheelSteps(steps);
            m_island->notifyMasterVolumeChanged(newValue);
            e->accept();
            return;
        }
    }
    // 2026-09-15: 設定中はズーム・音量スライダーへも流さない（+ボタン上の
    // マスターボリュームだけは上で処理済み）。canvasInputBlockedBySettings()参照。
    if (canvasInputBlockedBySettings()) { e->accept(); return; }
    // 音量スライダー表示中にスライダー上でホイール → 音量調整
    if (m_canvas) {
        auto *av = m_canvas->activeVideo();
        if (av && av->isVolSliderVisible()) {
            QPointF pos = e->position();
            if (av->volSliderScreenRect(height()).contains(pos) ||
                (m_canvas->isPlaybackBarVisibleForActiveVideo()
                 && pos.y() >= height() - 56 - 20)) {
                float delta = (e->angleDelta().y() > 0) ? 0.05f : -0.05f;
                av->adjustVolume(delta);
                e->accept();
                return;
            }
        }
    }
    // 2026-07-19: レイヤー機能を一旦切り離し（src/old/参照）。
    // if (m_layerBar && m_layerBar->isRevealed()) {
    //     const QRect lr = m_layerBar->geometry();
    //     if (lr.contains(e->position().toPoint())) {
    //         const int direction = (e->angleDelta().y() > 0) ? -1 : 1; // 上スクロール=前のレイヤーへ
    //         m_layerBar->stepLayer(direction);
    //         e->accept();
    //         return;
    //     }
    // }
    if (m_canvas && !isOnUI(e->position())) m_canvas->handleWheel(e);
    else e->accept();
}
void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (m_capturingHotkey) {
        if (e->isAutoRepeat()) {
            e->accept();
            return;
        }

        if (e->key() == Qt::Key_Escape) {
            cancelHotkeyCapture();
            e->accept();
            return;
        }

        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            commitHotkeyCapture();
            e->accept();
            return;
        }

        UINT virtualKey = static_cast<UINT>(e->nativeVirtualKey());
        if (virtualKey == 0)
            virtualKey = static_cast<UINT>(e->key());

        m_pendingHotkey.modifiers = modifiersFromQt(e->modifiers());
        if (!isModifierVirtualKey(virtualKey)) {
            m_pendingHotkey.virtualKey = virtualKey;
            m_pendingHotkeyHasKey = true;
        }

        updateHotkeyCaptureDisplay();
        e->accept();
        return;
    }

    if(m_canvas) m_canvas->handleKeyPress(e);
}

void MainWindow::applyAcrylicEffect()
{
    struct ACCENT_POLICY { int AccentState, AccentFlags, GradientColor, AnimationId; };
    struct WCAD { int Attribute; PVOID Data; SIZE_T SizeOfData; };
    typedef BOOL(WINAPI*Fn)(HWND,WCAD*);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                   "SetWindowCompositionAttribute"));
    if (!fn) return;
    ACCENT_POLICY p{3, 0x20, 0x00FFFFFF, 0};
    WCAD d{19, &p, sizeof(p)};
    fn(reinterpret_cast<HWND>(winId()), &d);
}

bool MainWindow::querySystemTransparencyEnabled() const
{
    // HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize
    // EnableTransparency (DWORD, 0=OFF/1=ON)。値が無い場合はWindowsの既定(ON)とみなす。
    QSettings personalize(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        QSettings::NativeFormat);
    return personalize.value("EnableTransparency", 1).toInt() != 0;
}

void MainWindow::applyAcrylicOrFallback()
{
    if (m_systemTransparencyEnabled) {
        applyAcrylicEffect();
        if (m_canvas) m_canvas->setDimBackgroundEnabled(false);
        return;
    }

    // 「透明効果」がOFFの環境ではDWMのブラー合成自体が働かず、
    // SetWindowCompositionAttributeによるアクリル効果が期待通りに描画されない
    // （塗りが全く入らずUIだけ浮いたような見た目になる）。
    // そのためブラー効果の指定を明示的に無効化しつつ、CanvasWidget側で
    // ウィンドウ全面に薄暗い半透明の塗りを自前描画するフォールバックに切り替える。
    struct ACCENT_POLICY { int AccentState, AccentFlags, GradientColor, AnimationId; };
    struct WCAD { int Attribute; PVOID Data; SIZE_T SizeOfData; };
    typedef BOOL(WINAPI*Fn)(HWND,WCAD*);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                   "SetWindowCompositionAttribute"));
    if (fn) {
        ACCENT_POLICY p{0, 0, 0, 0}; // AccentState=0: ACCENT_DISABLED
        WCAD d{19, &p, sizeof(p)};
        fn(reinterpret_cast<HWND>(winId()), &d);
    }

    if (m_canvas) m_canvas->setDimBackgroundEnabled(true);
}

void MainWindow::applyRoundedCorners()
{
    HWND hwnd = reinterpret_cast<HWND>(winId());
    int pref = 2;
    DwmSetWindowAttribute(hwnd, 33, &pref, sizeof(pref));
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    MSG *msg = static_cast<MSG*>(message);
    if (msg->message == WM_HOTKEY &&
        static_cast<int>(msg->wParam) == m_globalHotkeyId) {
        activateFromGlobalHotkey();
        *result = 0;
        return true;
    }

    if (msg->message == WM_NCHITTEST) {
        // 2026-09-17: 「どの辺からでもリサイズ」ON のときだけ、外周の細い帯を
        // リサイズ用の非クライアント領域として返す。カーソル形状の変更と
        // ドラッグでのサイズ変更は Windows 標準の処理に任せる。
        // それ以外（OFF・ウィンドウ全画面中）は従来どおり全面クライアント。
        *result = HTCLIENT;
        if (AppSettings::instance().edgeResizeEnabled() && !m_windowFullscreenActive) {
            RECT wr;
            if (GetWindowRect(msg->hwnd, &wr)) {
                const int x = static_cast<short>(LOWORD(msg->lParam));
                const int y = static_cast<short>(HIWORD(msg->lParam));
                const UINT dpi = GetDpiForWindow(msg->hwnd);
                const int band   = MulDiv(6,  dpi ? int(dpi) : 96, 96);  // 辺の掴める幅
                const int corner = MulDiv(16, dpi ? int(dpi) : 96, 96);  // 角として扱う範囲
                const bool left   = x <  wr.left   + band;
                const bool right  = x >= wr.right  - band;
                const bool top    = y <  wr.top    + band;
                const bool bottom = y >= wr.bottom - band;
                // 角は帯より少し広めに取り、斜めリサイズを掴みやすくする。
                const bool nearL = x <  wr.left   + corner, nearR = x >= wr.right  - corner;
                const bool nearT = y <  wr.top    + corner, nearB = y >= wr.bottom - corner;
                if      ((top && nearL)    || (left && nearT))  *result = HTTOPLEFT;
                else if ((top && nearR)    || (right && nearT)) *result = HTTOPRIGHT;
                else if ((bottom && nearL) || (left && nearB))  *result = HTBOTTOMLEFT;
                else if ((bottom && nearR) || (right && nearB)) *result = HTBOTTOMRIGHT;
                else if (left)   *result = HTLEFT;
                else if (right)  *result = HTRIGHT;
                else if (top)    *result = HTTOP;
                else if (bottom) *result = HTBOTTOM;
            }
        }
        return true;
    }

    // 2026-09-17: 辺リサイズ中も、バブル全画面（フォーカス）中はハンドルと同じく
    // 動画の比率を保つ。
    // 2026-09-18 バグ修正: 比率ロックが無いときは WM_SIZING を素通ししていたため、
    // 下限が Qt の setMinimumSize()（= kMinWindow*、比率合わせ用の絶対下限）だけに
    // なり、「どの辺からでもリサイズ」ON のとき右下ハンドル（kMinFreeWindow*）より
    // 小さくできてしまっていた。比率ロックの有無にかかわらずここで下限を掛ける。
    if (msg->message == WM_SIZING) {
        RECT *r = reinterpret_cast<RECT*>(msg->lParam);
        const double dpr = GetDpiForWindow(msg->hwnd) / 96.0;
        const WPARAM edge = msg->wParam;
        double w = r->right - r->left;
        double h = r->bottom - r->top;

        const bool aspect = m_resizer && m_resizer->aspectLocked()
                            && m_resizer->aspectW() > 0 && m_resizer->aspectH() > 0;
        if (aspect) {
            const double ar = m_resizer->aspectW() / m_resizer->aspectH();
            // 上下の辺だけを動かしているときは高さ基準、それ以外は幅基準。
            if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) w = h * ar;
            else                                          h = w / ar;
            // 最小サイズは片側だけクランプすると比率が崩れるので、両辺を同じ
            // 倍率で引き上げる（ResizeHandle と同じ考え方）。
            const double minW = kMinWindowWidth * dpr, minH = kMinWindowHeight * dpr;
            const double scale = qMax(1.0, qMax(minW / w, minH / h));
            w *= scale;
            h *= scale;
        } else {
            // 自由リサイズ。右下ハンドルと同じ下限にそろえる。
            w = qMax(w, kMinFreeWindowWidth  * dpr);
            h = qMax(h, kMinFreeWindowHeight * dpr);
        }

        const LONG nw = LONG(std::lround(w));
        const LONG nh = LONG(std::lround(h));
        // 掴んでいる側を動かし、反対側は固定する。
        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
            r->left = r->right - nw;
        else
            r->right = r->left + nw;
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
            r->top = r->bottom - nh;
        else
            r->bottom = r->top + nh;
        *result = TRUE;
        return true;
    }

    // ユーザーが設定アプリで「透明効果」をON/OFFした際、Windowsは
    // WM_SETTINGCHANGE(lParam="ImmersiveColorSet")をブロードキャストする。
    // これを検知して、アクリル/半透明フォールバックをその場で切り替える。
    if (msg->message == WM_SETTINGCHANGE && msg->lParam) {
        const wchar_t *section = reinterpret_cast<const wchar_t*>(msg->lParam);
        if (wcscmp(section, L"ImmersiveColorSet") == 0) {
            const bool enabled = querySystemTransparencyEnabled();
            if (enabled != m_systemTransparencyEnabled) {
                m_systemTransparencyEnabled = enabled;
                applyAcrylicOrFallback();
            }
        }
    }

    // 2026-10-11: モニター間移動でDPIが変わった。Qtが先に自分のリサイズを行うので、
    // 終わった後（イベントループに戻ってから）論理サイズを基準に物理サイズを直す。
    if (msg->message == WM_DPICHANGED) {
        m_dpiChanging = true;
        const UINT newDpi = LOWORD(msg->wParam);
        QTimer::singleShot(0, this, [this, newDpi]() {
            restoreDipSizeAfterDpiChange(newDpi);
        });
    }

    return QWidget::nativeEvent(eventType, message, result);
}
