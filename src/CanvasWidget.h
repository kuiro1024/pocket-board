#pragma once
#include "Blur.h"

#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QPointF>
#include <QPainterPath>
#include <QRect>
#include <QRegion>
#include <QSize>
#include <QList>
#include <QSet>
#include <QHash>
#include <QString>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QPointer>
#include <QVarLengthArray>
#include <QVector>
#include <QPair>
#include "FocusSessionController.h"
#include "MasterVolumeController.h"
#include "CanvasItem.h"
#include "CanvasVideo.h"
#include "Renderer.h"
#include "CanvasRenderScheduler.h"
#include "CanvasStaticLayerCache.h"
#include "GlassHighlightEffect.h"
#include "GpuProfiler.h"
#include "BubbleLayerCache.h"
#include "BubbleFusion.h"
#include "engine/VideoLayerHost.h"
#include <memory>
#include <functional>

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;
class QDragMoveEvent;
class QMimeData;
class QPixmap;
class QPainter;
class CanvasImage;
class CanvasLauncher;
class BubbleDragExporter;
class TrashZone;
class CameraPanInertia;
class CameraZoomEase;

class CanvasWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT

public:
    explicit CanvasWidget(QWidget *parent = nullptr);
    ~CanvasWidget() override;

    void handleMousePress(QMouseEvent *event, bool onUI = false);
    void handleMouseMove(QMouseEvent *event);
    void handleMouseRelease(QMouseEvent *event);
    // 2026-09-17: 動かさずに離した右クリックの処理（アクティブ化／ダブルで全画面）。
    void performRightClick(const QPointF &pos);
    void handleWheel(QWheelEvent *event);
    void handleKeyPress(QKeyEvent *event);
    // 2026-09-20: Ctrl+V。クリップボードの中身をバブルとして貼り付ける
    // （カーソルがPB上ならその位置、それ以外は画面中央）。
    bool pasteFromClipboard();

    // 座標変換（canvas ↔ screen）
    QPointF screenToCanvas(const QPointF &screenPos) const;
    QPointF canvasToScreen(const QPointF &canvasPos) const;

    // アイテムのscreen矩形を計算（lift変換込み）
    QRectF itemScreenRect(CanvasItem *item) const;
    // 描画スケール（canvasScale * liftScale）
    double itemScreenScale(CanvasItem *item) const;

    bool showGrid() const    { return m_showGrid; }
    int gridSize() const { return m_gridSize; }
    void setShowGrid(bool v);
    void setGridSize(int v);
    CanvasVideo* activeVideo() const { return canvasItemAsVideo(m_activeItem); }
    bool isPlaybackBarVisibleForActiveVideo() const { return activeVideo() && m_playbarVisible; }
    // 2026-09-14 D-3: 下の帯を無効化する必要があるか。再生バーが出ている／
    // 消えていく途中／直前までアクティブだった動画のフェードが残っている間。
    // ここが false のフレームでは、オーバーレイの再描画要求に下の帯を
    // 足さない（足すとバウンディング矩形がウィンドウ全体になってしまう）。
    bool playbackBarNeedsDamage() const {
        if (m_playbarOutgoingVideo)
            return true;
        auto *av = activeVideo();
        if (!av)
            return false;
        return m_playbarVisible || av->playbackBarAnimActive();
    }
    // blurRegions: 非nullなら、描いたUI部品の矩形をここへ登録して背後を
    // ぼかしてもらう(Blur.h参照)。描画そのものには影響しない。
    void paintPlaybackOverlay(QPainter &painter,
                              BackdropBlur::Regions *blurRegions = nullptr);
    void paintFocusOverlay(QPainter &painter,
                           BackdropBlur::Regions *blurRegions = nullptr);
    // 2026-09-14 D-2: ダメージ領域を受け取り、外周の帯に掛かっていない
    // フレームでは何もしない。
    void paintBorderOverlay(QPainter &painter, const QRegion &damage);
    void paintLauncherOverlay(QPainter &painter);
    QRect launcherTooltipOverlayRect() const;
    // 2026-07-15: カーソルがシークバー/音量スライダー領域（＝MainWindow::
    // isOnUI()がキャンバス操作をブロックするのと同じ判定基準）の上に
    // あるかどうか。再生バーの自動非表示タイマーを止めるために使う。
    bool isCursorOverPlaybackBarArea(const QPoint &local) const;

    // 2026-09-12: 画面上部にある島（DynamicIsland）の左端（このウィジェット
    // 座標）。左上から右へ伸びるUI（ラベルのコントローラ）が島と重ならない
    // よう、伸ばしてよい限界として使う。島が無い/隠れている場合は width()。
    int topOverlayObstacleLeft() const;
    // 2026-09-13: テクスチャの色を上下左右に分けて読む（アンビエント発光用）。
    // 粗いミップ段を数十バイトだけ読み戻す。GPU→CPUの読み戻しなので毎
    // フレーム呼ばないこと（実装側のコメント参照）。
    //
    // 【2026-09-14 非同期化】この読み戻しは以前 glGetTexImage() を直接
    // 呼んでいた。読み戻し先がクライアントメモリだと、GLはそこまでに
    // 積まれた全コマンドの完了を待ってから返る＝CPUがGPUに追いつかれる
    // まで完全に止まる（パイプラインストール）。150msに1回・再生中の動画
    // 1本につき1回なので、秒6〜7回、動画が複数あればその本数ぶん、
    // 描画スレッドがミリ秒単位で固まっていた。止まる長さはGPUに積まれて
    // いる仕事の量に比例するため、ウィンドウが大きいほど長い＝
    // 「全画面にすると島のアニメやカメラ移動が一定間隔でカクつく」
    // という形で出る。
    // 読み戻し先をPBO（GPU側のバッファ）にすると glGetTexImage() は
    // 命令を積むだけで即座に返る。実際に中身を見るのはフェンスが通った
    // 後の別フレーム（collectAmbientSample）なので、待ち時間はゼロになる。
    struct AmbientReadback {
        GLuint  pbo     = 0;
        GLsync  fence   = nullptr;
        QPointer<CanvasVideo> owner;
        int     width   = 0;
        int     height  = 0;
        bool    pending = false;
    };
    AmbientReadback m_ambientReadback;
    // 読み戻しを1件だけ発行する（既に飛んでいるものがあれば何もしない）。
    void issueAmbientSample(CanvasVideo *video, unsigned int texId);
    // GPUが書き終わっていれば結果を取り込む。まだなら何もしない（次フレーム）。
    void collectAmbientSample();
    void releaseAmbientReadback();
    // バブルが全画面（フォーカス）表示中かどうか。ドラッグ中のカーソルを
    // 禁止マーク（🚫）にするかどうかの判定に使う（MainWindow からも参照する）。
    // 2026-07-09 フェイルセーフ: 何らかの残留バグで m_focusedItem が
    // 「もう存在しないアイテム」を指したままになっても🚫が固定されないよう、
    // 現存アイテム(m_items)に含まれることも条件にする（誤発火の安全網）。
    bool isFocusDropBlocked() const { return m_focusedItem != nullptr && m_items.contains(m_focusedItem); }
    // 2026-07-11 レイヤー機能(Layer.md Phase C): 右島(LayerBar)向け。
    // カメラのホイール押し込みドラッグ中・バブル移動中・フレーム移動中は
    // trueを返す（m_activeModeの値はhandleMousePress/Move内で設定されるもの
    // と同じ文字列。playbar表示条件(m_activeMode != "panning")と同じ流儀）。
    // 2026-09-17: バブル全画面中の中ボタンドラッグ（focus_image_pan）も含める。
    // 以前は通常キャンバスのパンだけが対象で、全画面でズームして動かして
    // いるときはカーソルが上へ行くと島が出ていた。
    bool isCanvasBeingManipulated() const {
        return m_activeMode == QStringLiteral("camera_pan")
            || m_activeMode == QStringLiteral("panning")
            || m_activeMode == QStringLiteral("focus_image_pan");
    }
    // 2026-09-17: 長押しで2倍速になっている間（一般的な動画プレイヤーと同じく他のUIを隠す）。
    bool isSpeedHoldActive() const { return m_longPressActive; }
    // 島を隠すべきキャンバス側の状態（島の DynamicIslandController::tick が見る共通判定）。
    bool shouldHideIslandForCanvas() const {
        return isCanvasBeingManipulated() || isSpeedHoldActive();
    }
    bool canAcceptDropMimeData(const QMimeData *mime, QString *reason = nullptr) const;
    bool handleDropMimeData(const QMimeData *mime, const QPointF &widgetPos, const QString &source);
    void logDropMimeData(const char *eventName, const QMimeData *mime,
                         bool accepted, const QString &reason) const;
    QJsonObject sessionToJson() const;
    void restoreSessionFromJson(const QJsonObject &canvas);

    // ---- 元に戻す / やり直し（2026-09-17、CanvasUndoHistory.cpp）----
    // バブルの移動・リサイズ・追加・削除を「操作1回＝1ステップ」で記録する。最大回数は
    // AppSettings::undoHistoryLimit()（1〜64）。Ctrl+Z / Ctrl+Y(Ctrl+Shift+Z)。
    bool canUndo() const { return !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }
    bool undo();
    bool redo();
    // レイヤー切り替え（restoreSessionFromJson）など、キャンバスの中身が
    // まるごと入れ替わったときに呼ぶ。
    void clearUndoHistory();
    // 2026-09-17: 選択中のバブル（複数選択が無ければアクティブなバブル）を削除する。
    // ゴミ箱へのドラッグと同じ後始末で、元に戻すで戻せる。ショートカット用。
    bool deleteSelectedBubbles();
    void stopAllVideosForBackground();
    // 2026-07-10 レイヤー機能(Layer.md Phase C): レイヤー切り替え直後など、
    // restoreSessionFromJson()で一括で出現した既存アイテム・フレームすべてを
    // 既存の削除フェードイン機構（シェイク削除の復帰演出）と同じ仕組みで
    // フェードインさせる。LayerSwitcherから呼ぶ想定。
    void fadeInAllItems();

    // ---- マスターボリューム ----
    // 島の+ボタン上でのホイール操作(1クレンジ=1%)を反映する。
    // MainWindow::wheelEventから呼ぶ。呼び出し後の値(0-100)を返す。
    int applyMasterVolumeWheelSteps(int steps);

    // MainWindowの最大化状態が変化した際に呼ばれる。
    // フォーカス中の動画があり、かつ「ウィンドウ全画面中だったのでウィンドウ
    // リサイズを保留していた」場合、全画面が解除された瞬間に本来のフォーカス
    // 挙動（ウィンドウを動画のアスペクト比にぴったり合わせる）を今ここで適用する。
    void notifyWindowMaximizedChanged(bool isMaximizedNow);

    // ウィンドウ全画面のアニメーション中、キャンバス内容（グリッド/バブル等）を
    // 一時的にフェードアウト/インさせる。ウィンドウリサイズ中は毎フレーム
    // カメラ・レイアウトが再計算されてガクつく（がくがくする）ため、
    // アニメーション前に非表示にしてアニメーション後にフェードで見せる。
    void setContentOpacity(double opacity);
    void fadeContentOpacityTo(double targetOpacity, int durationMs,
                              std::function<void()> onFinished = nullptr);

    // Windows「透明効果」がOFFでアクリル(ブラー)が使えない環境向けのフォールバック。
    // trueの間、paintGL()の先頭でウィンドウ全面に薄暗い半透明の塗りを描画する。
    void setDimBackgroundEnabled(bool enabled);

signals:
    void playbackOverlayUpdateRequested();
    // 2026-09-12: イコライザだけが動いているフレーム用の部分更新。
    // 引数はウィジェット座標の矩形（棒＋音符の飛翔範囲）。
    void playbackOverlayEqualizerUpdateRequested(const QRect &damage);
    void focusOverlayUpdateRequested();
    // 2026-09-17: 長押し2倍速の開始/終了。オーバーレイの動画UIをフェードで隠す/戻す
    // （CanvasOverlayWidget::setSpeedHoldSuppressed）。島は tick 側で判定する。
    void speedHoldChanged(bool active);
    void borderOverlayUpdateRequested();
    void launcherTooltipOverlayUpdateRequested();
    // 2026-10-12: 保存すべき形が変わった（Undoの1ステップ確定・Undo/Redo）。
    // MainWindow がこれを受けて数秒後に自動保存する。
    void sessionContentChanged();

    // 2026-09-13: プラグイン .zip がドロップされた。確認UIは島
    // （DynamicIsland + IslandPromptPanel）で出すため、キャンバスは
    // 「来た」ことだけを伝えて判断は MainWindow へ委ねる。
    // 受け手がいない場合は従来どおり QMessageBox にフォールバックする
    // （CanvasWidget::promptAndInstallPluginZip 参照）。
    void pluginInstallRequested(const QString &zipFilePath);

public slots:
    // QWidget::update()を隠して、既存の更新要求も必ずDirty管理へ通す。
    // 実描画のスケジュール自体は基底QOpenGLWidgetへそのまま委譲する。
    void update();
    void update(const QRect &rect);
    // 120Hz: カメラのイーズアニメーション追従など、精度が必要な処理のみ。
    void syncLogic();
    // 60Hz相当: 再生バーのフェード・ボリュームスライダーなど。dtMsは呼び出し間隔の実測値(ms)で、
    // フェード時間やアニメーション速度の計算に使う。
    void syncLogicMedium(int dtMs);
    // 120Hz: ラベルのリストのスクロールだけ（2026-09-14 C-3）。
    // MainWindow の専用タイマーから呼ばれる。
    void syncLabelScroll();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void requestFullRender(CanvasRenderReason reason);
    // 2026-09-15 D-5: BackdropBlur の形状変化を、全面ではなくその矩形だけの
    // 再描画にする（実装側のコメントに経緯と安全性の根拠）。
    void requestBlurRectRender(QWidget *owner, const QRectF &ownerRect);
    void requestItemRender(CanvasItem *item, CanvasRenderReason reason);
    void updateCanvasFromCamera();
    // 画像バブル全画面中に、ビューポートが画像の外へはみ出さない範囲へ
    // m_cameraPosを収める（ホイールズーム・ドラッグパン共通のクランプ処理）。
    void clampCameraToFocusedImage(const QRectF &itemRect);
    // 2026-07-12: フォーカス中ドラッグパン（focus_image_pan）の慣性は
    // CameraPanInertiaクラスへ切り出した（実体はm_focusPanInertia）。
    // stopFocusPanInertia()は既存の呼び出し箇所（unfocusItem系、新規パン
    // 開始時のリセット、ホイールズームでの打ち切り等）を変えずに済むよう
    // 残してある薄い転送用ヘルパー。
    void stopFocusPanInertia();
    // 2026-07-12: 中ボタンのダブルクリックで、フォーカス中にズーム/パンで
    // ずらした表示状態をネイティブ（fit）状態へアニメーションで戻す。
    void resetFocusCameraToNative(CanvasItem *focusTarget);
    void startLiftUp();
    void startLiftDown();

    // ---- 2026-09-22: バブル融合（BubbleFusion.h）----
    // 毎フレーム（syncLogic）呼ぶ。ドラッグ中の重なり判定・吸い寄せ・
    // 吸い込みアニメ・受け側の膨らみを進める。
    void tickBubbleFusion();
    // ドラッグ中のバブル dragged と十分に重なっている融合相手を探す。
    // draggedRect は吸い寄せ前の（マウスそのままの）位置での矩形。
    // overlapRatio: 見つかった相手との中身の重なり（小さい方の面積に対する割合）。
    // boardOverlapRatio: 板（ベゼル込み）どうしの重なり。首の太さに使う。
    // 2026-09-23: 相手は「板が少しでも重なっていれば」見つける（中身の重なりでは
    // 板どうしが先に重なっているのに融合が始まらず／先に終わり、2枚の板が
    // 境目のある状態で重なって見えていた）。
    CanvasItem *findFusionCandidate(CanvasItem *dragged, const QRectF &draggedRect,
                                    double *overlapRatio = nullptr,
                                    double *boardOverlapRatio = nullptr) const;
    // この2つを融合してよいか。今は「どちらも動画バブル」だけを見る。
    // 【将来】プラグインが拒否できるようにする場所（例: 上限のあるMix）。
    bool bubblesCanFuse(CanvasItem *dragged, CanvasItem *target) const;
    // 離した瞬間に、結合できる状態なら吸い込みを始める（始めたら true）。
    bool beginFusionAbsorbOnRelease();
    // 吸い込み完了: 中身を移して吸われた側を消す。
    void finishFusionAbsorb(CanvasItem *absorbed, CanvasItem *target);
    // 2026-09-23: 一覧の行を板の外へ引っ張り出した → その場にバブルを出して、
    // そのまま掴んだ状態（panning）へ移る。
    void extractListRowToBubble(CanvasVideo *owner, const QPointF &screenPos);
    // 2026-09-23: ラベルの長押しがたまりきった（PbLabelActionApi.h）。
    // 中身は本体（ローカル動画 → 同じフォルダで List）か、プラグイン
    // （例: 単発の動画 → 関連動画のリスト）。結果は video->finishLabelHold() で返す。
    void runLabelHoldAction(CanvasVideo *video);

    // ---- バブル重なり解消（ドロップ/リサイズ確定時にアニメーションで押し出す）----
    // fixedItems（ドラッグ/リサイズ/グループ移動を終えたアイテム群）は動かさず、
    // それ以外のアイテムが重ならなくなる位置までの移動量を計算するだけ
    // （実際の setPos は行わない）。単一アイテム版は1要素のQSetに委譲する。
    QHash<CanvasItem*, QPointF> computeOverlapPushOffsets(const QSet<CanvasItem*> &fixedItems) const;
    QHash<CanvasItem*, QPointF> computeOverlapPushOffsets(CanvasItem *fixedItem) const;
    // 上記の移動量ぶん、対象アイテムを「もにゅっ」としたイージングでアニメーションさせる。
    void startOverlapPushAnimation(const QSet<CanvasItem*> &fixedItems);
    void startOverlapPushAnimation(CanvasItem *fixedItem);
    // 計算済みのオフセット（キー＝アイテム）ぶんを実際にアニメーションさせる共通処理。
    // startOverlapPushAnimation() とフレーム由来の押し出し（下記）の両方から使う。
    void animateItemOffsets(const QHash<CanvasItem*, QPointF> &offsets);
    // 指定アイテムに紐づく進行中の押し出しアニメーションを止めて破棄する
    // （再度掴んだ時・削除される時に呼ぶ）。
    void cancelOverlapAnim(CanvasItem *item);
    void cancelAllOverlapAnims();

    // ---- 範囲選択（複数バブル選択） ----
    // 現在の選択範囲（矩形）に少しでも触れている（boardRectが交差する）アイテムを返す。
    QList<CanvasItem*> itemsInCanvasRect(const QRectF &canvasRect) const;
    void clearSelection();
    // 選択中バブルをまとめてゴミ箱へ（trashZone にドロップされた時に呼ぶ）。
    void deleteSelectedItems();
    // アイテム破棄の共通処理（Phase 10 Step 4）。tooltip/focus・click系
    // 参照/anim系/pause系の後始末、m_itemsからの除去、（動画/音声なら）
    // releaseGlResources()、delete item を1つにまとめたもの。
    // 【重要】呼び出し元がGLコンテキストをcurrentにしてから呼ぶこと
    // （makeCurrent()/doneCurrent()はこの関数の外側の責務。複数件をまとめて
    // 削除する場合にGLコンテキスト切り替えを1回にまとめる既存の最適化を
    // 壊さないため、あえてこの関数の内側には入れていない）。
    // OleInitialize診断/refreshDropTargets() の呼び出しも、複数件削除時に
    // 1回にまとめる必要があるため、この関数の外（各呼び出し元）で行うこと。
    void destroyItem(CanvasItem *item);

    // ---- フェードイン演出 ----
    void startFadeInAnim(CanvasItem *item);
    void cancelFadeInAnim(CanvasItem *item);
    void cancelAllFadeInAnims();

    void focusItem(CanvasItem *item);
    void unfocusItem();
    // 2026-07-17追加: フォーカス中のバブル自体の映像アスペクト比が
    // 途中で変わった場合（再生リスト連続再生でnextUrlの動画へ切り替わり、
    // 前の動画と縦横比が異なるケース等）に、ウィンドウを改めてフィットし
    // 直す。CanvasVideo::videoDimensionsReady から、対象がまさに
    // フォーカス中の場合にのみ呼ばれる。
    void refitFocusedWindowForAspectChange(CanvasItem *item);
    void beginFocusPlaybackPolicy(CanvasItem *focusItem);
    void endFocusPlaybackPolicy();
    // アイテム削除時に呼ぶ。m_focusSleepBookkeeping.videosAsleep /
    // resumeAfterFocus から対象videoの記録を両方まとめて取り除く
    // （削除後に破棄済みポインタへの参照が残るのを防ぐ）。
    // videoがnullptrの場合は何もしない。
    void forgetFocusPausedVideo(CanvasVideo *video);
    // カメラアニメの「アンカー」= 画面上を直線的・進行度比例で動かす注目点。
    //   Target: 着地先(寄る演出: これから寄るバブルの中心)を画面中心へ
    //           直線的に引き込む。
    //   Start : 出発点(引く演出: いま画面中心にあるバブル)を、着地後の
    //           画面位置へ直線的に送り出す。
    //   Auto  : ズームイン(targetZoom >= 現在ズーム)ならTarget、
    //           ズームアウトならStart。
    // どちらのアンカーでも開始・着地のカメラ位置/ズーム自体は同一で、
    // 途中の軌道だけが変わる。
    enum class CameraAnimAnchor { Auto, Target, Start };
    void animateCamera(QPointF targetPos, double targetZoom, int duration = 600,
                       CameraAnimAnchor anchor = CameraAnimAnchor::Auto);
    // 進行度f(イージング適用済み0→1)からm_cameraPos/m_cameraZoomを
    // 計算して適用する（animateCamera()のコメント参照）。
    void applyCameraAnimationProgress(double f);
    // カメラ演出の1tick。120HzのsyncLogic()から毎回呼ばれ、経過時間から
    // 進行度を計算してapplyCameraAnimationProgress()する（m_camAnimDriverの
    // コメント参照）。
    void tickCameraAnimation();
    // カメラアニメ実行中に着地先だけを差し替える（ウィンドウリサイズ追従用。
    // 旧実装のsetEndValue()差し替えに相当）。カメラの現在値が連続になるよう
    // 補間パラメータを張り直す。非実行時は即座に着地値を適用する。
    void retargetCameraAnimation(QPointF targetPos, double targetZoom);
    // フォーカスイン/アウト演出のカメラアニメ（m_camAnimDriver）が
    // 現在再生中かどうか。animateCamera()はフォーカスイン/アウト演出以外の
    // 目的では呼ばれないため、このアニメが走っている間は「カメラが目的の
    // バブルへ寄っている/引いている最中」であることを意味する。
    // この間にホイールドラッグ（中ボタンパン）やホイールズームでカメラを
    // 直接動かすと、アニメーションと手動操作が同じ m_cameraPos/m_cameraZoom を
    // 奪い合ってガクつく・着地位置がずれる不具合になるため、この間は
    // カメラ操作を受け付けないようにする（mousePressEvent/handleWheel参照）。
    // （引く演出中だけ手動操作を許可する案も試したが、割り込み時に
    // unfocusItem()が同時に走らせるウィンドウリサイズアニメが宙に浮いてしまい
    // 次の寄る演出に悪影響が出る不具合があったため、シンプルに
    // 「アニメ中は常にブロック」へ戻した。）
    bool isCameraFocusAnimationRunning() const;

    // 描画ヘルパー
    void paintGrid();
    // 背景画素の反転合成が必要なため、原寸リセット時の強グローだけは
    // QPainter overlayではなくCanvasのGLパスに残す。
    void paintStrongBorderGlow();
    // ホイール中クリックのダブルクリックによる原寸リセット時に、通常より
    // 強い枠グローを即座に発生させ、約0.5秒保持したのちフェードアウトさせる
    // （resetFocusCameraToNative()から呼ばれる。実装はpaintStrongBorderGlow()参照）。
    void triggerStrongBorderGlow();
    void executeSingleClick(CanvasItem *item, const QPointF &screenPos);
    // 動画全画面中の「長押しで2倍速」の状態を解除する（release時・
    // フォーカス解除時などに呼ぶ）。長押し確定中だった場合は再生速度を
    // 1.0へ戻す。
    void cancelLongPress();
    // m_longPressActive の変更は必ずここを通す（変わったときだけ speedHoldChanged を出す）。
    void setLongPressActive(bool active);
    // 2026-09-20: 逆輸入（PB外へ書き出すドラッグ）の視覚ヒント（背景の暗転）。
    void paintExportHintBackground();
    bool exportHintWanted() const;
    void updateExportHint();
    bool cursorOverCanvas(QPoint *localPos = nullptr) const;
    void startBubbleExportDrag(CanvasItem *dragItem);
    // 左ダブルクリックの確定（2回目を動かさずに離したとき）。
    void performLeftDoubleClick(CanvasItem *target);
    // 2026-09-20: 選択中のバブルを整列する（範囲選択→ホイールダブルクリック）。
    // 旧フレーム機能の「フレームを掴んでSpace」と同じ並べ方を、フレーム無しで
    // 選択範囲の外接矩形に対して行う。
    enum class ArrangeMode { BySize, ByName };
    bool arrangeSelectedBubbles(ArrangeMode mode = ArrangeMode::BySize);
    // 範囲選択中のマーキー矩形と、選択中バブルのハイライト枠を描画する。
    // バブルより後（上）に描く。
    void paintSelectionOverlay();
    void updateLauncherTooltipHover(const QPoint &cursorPos, int dtMs);
    void clearLauncherTooltip();

    // 2026-09-14 Phase3: 1アイテムが汚す画面矩形（ベゼル＋影/グロー/発光ぶんの
    // パディング込み）。requestItemRender() と paintGL() の
    // dynamicDamageRegion の両方が必ずこれを使うこと。式が食い違うと
    // 静的レイヤーキャッシュが再利用されなくなる（実装側のコメント参照）。
    QRectF itemDamageRect(CanvasItem *item) const;

    // 2026-09-15 D-4: アンビエント発光の「十字の筋」は明るさ(streak)に比例して
    // 伸びるのに、itemDamageRect() は streak を見ずに常に最大長ぶん
    // （minSide * 1.30）を四方へ足していた。そのため、筋が全く出ていない
    // 動画でも damage が minSide の2.6倍角に膨らみ、全画面では 1.36Mpx
    // （= ウィンドウの57%）になっていた。半透明の CanvasOverlayWidget は
    // GLキャンバスの兄弟なので、この damage がそのまま overlay の
    // paintEvent 領域として降ってくる（[CanvasOverlay][stat] の
    // area/frame が [RenderSchedule] の avgDamage に追従するのはこのため）。
    // streak を damage にも反映させる。縮むフレームで筋の先端が消し残ら
    // ないよう、前フレームの値との max を使う（キーはポインタの同一性
    // だけに使い、参照はしない）。
    mutable QHash<const CanvasItem *, double> m_ambientStreakDamage;

    // 2026-09-14: サラウンドモードを廃止。updateSurroundPans() /
    // queueSurroundVolumeApply() / flushSurroundVolumeQueue() を削除した
    // （syncLogicMedium()から60Hzで全バブルを走査していた）。

    // ---- マスターボリューム ----
    // 現在のm_masterVolume.gain()を、キャンバス上の全CanvasVideoへ即座に
    // 反映する。値変更時（ホイール操作・セッション復元）に呼ぶ。
    void applyMasterGainToAllVideos();
    // 2026-07-18追加: videoDimensionsReady（実解像度・対比の判明/変化）への
    // 接続の共通ヘルパー。addMediaSource()（新規追加）と
    // restoreSessionFromJson()（セッション復元。CanvasSessionSerializer.cpp）
    // の両経路から必ず呼ぶ。復元経路で接続し忘れていたのが「復元バブルは
    // 連続再生で対比が変わってもフォーカス中のウィンドウが追従しない」原因。
    void connectVideoDimensionSignals(CanvasVideo *video);
    void connectVideoRenderSignals(CanvasVideo *video);
    CanvasVideo *addMediaSource(const MediaSource &source, const QPointF &canvasPos);
    CanvasVideo *addVideoUrl(const QString &mediaUrl, const QPointF &canvasPos);
    // Phase 8: Plugin API — url に対応するプラグインが見つかれば
    // MediaKind::PluginMedia として addMediaSource() 経由で追加する。
    // 対応プラグインが無ければ何もせず false を返す。
    bool tryAddPluginMedia(const QString &url, const QPointF &canvasPos);
    // 2026-10-12: 動画ファイルの直リンク（.mp4 等）を MediaKind::WebVideo として追加する。
    CanvasVideo *addWebVideoUrl(const QString &url, const QPointF &canvasPos);
    // 2026-10-12: Webリンク候補のURL。中身が動画か問い合わせ、動画なら動画バブル、
    // そうでなければWebリンクバブルにする（非同期）。
    void addWebLinkOrProbedVideo(const QString &url, const QPointF &canvasPos);

    // Phase 8: Plugin API — .zip の中身（.dllのIID/Keysのみ。実行はしない）を
    // 事前確認するダイアログを出し、ユーザーが承認した場合のみ
    // PluginManager::installPluginZip() を呼ぶ。
    // dropEvent() の完了後（QTimer::singleShot(0,...)経由）に
    // showPluginInstallPrompt() を実際に呼び出す。
    void promptAndInstallPluginZip(const QString &zipFilePath);
    void showPluginInstallPrompt(const QString &zipFilePath);
    CanvasImage *addImagePixmap(const QPixmap &pixmap, const QPointF &canvasPos,
                                double scale = 0.5,
                                bool centerOnPoint = true);
    CanvasImage *addImagePlaceholder(const MediaSource &source, const QPointF &canvasPos,
                                     double scale = 0.5,
                                     bool centerOnPoint = true);
    // 2026-09-17: キャッシュから同期で作れた場合はそのバブルを返す（非同期取得に
    // なった場合は nullptr を返し、作成後に onAsyncCreated を呼ぶ）。
    CanvasImage *loadRemoteImageUrl(const QString &imageUrl, const QPointF &canvasPos,
                                    double scale = 0.5,
                                    bool positionIsTopLeft = false,
                                    const std::function<void(CanvasImage*)> &onAsyncCreated = {});
    // セッション1アイテム分の書き出し／作成（CanvasSessionSerializer.cpp）。
    // Undo/Redo で削除したバブルを作り直すのにも使う。
    QJsonObject sessionEntryForItem(CanvasItem *item) const;
    CanvasItem *createItemFromSessionEntry(
        const QJsonObject &entry,
        const std::function<void(CanvasItem*)> &onAsyncCreated = {});

    // ---- 元に戻す / やり直しの内部（CanvasUndoHistory.cpp）----
    // 1ステップ分の「キャンバスの形」。アイテム/フレームは生ポインタではなく
    // 履歴用の通し番号で持つ（削除→作り直しでポインタが変わっても追えるように）。
    // 作り直しに必要な中身（動画URL等）は m_undoItemBlobs に番号ごとに1つだけ持つ。
    struct UndoItemState  {
        quint64 id = 0; QPointF pos; double scale = 1.0;
        // 2026-09-23: ラベルの長押しによるURLの切り替え（Solo → Mix 等）。
        // 比較は回数（epoch）だけで行う。連続再生で次の動画へ進んだだけでは
        // 変わらないので、ふつうの移動の取り消しが再生位置を巻き戻さない。
        quint64 holdEpoch = 0;
        QString holdUrl;
        // 再生を止めない差し替えで戻すときのラベル（プラグインが言ったこと）。
        QJsonObject holdLabel;
        // 2026-09-23: 本体のリスト（バブル融合）。比較は coreListKey（種類とURLの
        // 並び）だけで行い、戻すときは coreList を書き戻す。リストでなければ空。
        QJsonObject coreList;
        QString     coreListKey;
    };
    struct UndoSnapshot {
        QVector<UndoItemState>  items;   // m_items の並び（＝重なり順）
    };
    // 追加系の関数を囲む。すでにステップが開いている（マウス操作中・ドロップ
    // 処理中）なら何もせず、その外側のステップにまとめる。
    struct UndoAutoStep {
        explicit UndoAutoStep(CanvasWidget *w);
        ~UndoAutoStep();
        CanvasWidget *widget;
        bool began;
    };
    // この間に起きた変化は履歴に積まない（セッション復元・Undo適用中）。
    struct UndoSuppressScope {
        explicit UndoSuppressScope(CanvasWidget *w) : widget(w) { ++widget->m_undoSuppressDepth; }
        ~UndoSuppressScope() { --widget->m_undoSuppressDepth; }
        CanvasWidget *widget;
    };
    // キーイベントが AppSettings のショートカット（id）の割り当てと一致するか。
    static bool matchesCanvasShortcut(const QKeyEvent *event, const QString &id);
    UndoSnapshot captureUndoSnapshot();
    static bool undoSnapshotsEqual(const UndoSnapshot &a, const UndoSnapshot &b);
    void beginUndoStep();
    void commitUndoStep();
    void applyUndoSnapshot(const UndoSnapshot &snapshot);
    bool canApplyUndoNow();
    quint64 undoIdForItem(CanvasItem *item);
    void bindUndoItemId(CanvasItem *item, quint64 id);
    // destroyItem() から呼ぶ。アイテムは作り直し用の中身を保存してから忘れる。
    void forgetUndoItem(CanvasItem *item);
    void trimUndoHistory();
    void pruneUndoBlobs();

    QVector<UndoSnapshot> m_undoStack;
    QVector<UndoSnapshot> m_redoStack;
    UndoSnapshot m_undoPendingBefore;
    bool    m_undoStepOpen = false;
    int     m_undoSuppressDepth = 0;
    quint64 m_undoNextId = 1;
    QHash<CanvasItem*, quint64>  m_undoIdByItem;
    QHash<quint64, CanvasItem*>  m_undoItemById;
    QHash<quint64, QJsonObject>  m_undoItemBlobs;
    CanvasLauncher *addLauncher(const QString &path, const QPointF &canvasPos,
                                double scale = 1.0,
                                bool centerOnPoint = true);
    QStringList m_webIconPendingHosts;   // favicon取得中のホスト（重複通信防止）

    Renderer m_renderer;
    CanvasRenderScheduler m_renderScheduler;
    CanvasStaticLayerCache m_staticLayerCache;
    // 2026-09-16: シーンレイヤー。キャンバスの中身（グリッド・枠・バブル・
    // ライティング・選択枠など、UIレイヤーより下の全部）を描き終えた時点の
    // 絵を保持する。島やラベルの形だけが変わるフレーム（Backdrop）では
    // これを貼って、その上にUIレイヤー（ぼかし＋ガラス）だけを描き直す。
    // UIが動いている間だけ確保し、しばらく動かなければ解放する。
    CanvasStaticLayerCache m_sceneLayer;
    QElapsedTimer m_backdropActivityClock;

    // 2026-07-19: バブル縁の疑似ガラスハイライト（SDF法線ベース照明+
    // Quality時のキャプチャ疑似屈折）。Rendererとは責務を分けた別クラス
    // （詳細はGlassHighlightEffect.h先頭のコメント参照）。
    GlassHighlightEffect m_glassHighlight;
    // 2026-10-12: 1フレームの区間ごとのGPU時間・描画回数（Shift+Pで表示）
    GpuProfiler m_gpuProfiler;
    // 2026-10-12: バブルごとの下地（影・接触影・枠・リサイズの印）の焼き付け。Shift+K で切替
    BubbleLayerCache m_bubbleLayers;
    bool m_bubbleLayersSharpenPending = false;
    // 2026-10-12: Shift+L でライトの向きを固定する（カメラを動かしても光が動かない）
    bool m_lightLocked = false;
    bool m_perfOverlay = false;
    bool m_perfOverlayRefreshPending = false;   // 0.5秒後の描き直し予約は1つだけ
    void drawPerfOverlay();
    QElapsedTimer m_glassHighlightClock; // draw()に渡すdt計測用
    qint64 m_glassHighlightLastMs = 0;
    // 2026-07-19: フォーカス（カメラが寄っていくアニメーション）中は
    // ガラスハイライトが拡大された映像に混ざって見えるため、フォーカス
    // 開始と同時にフェードアウトさせる。その現在値（1=通常、0=消灯）。
    float m_glassFocusFade = 1.0f;
    // 2026-10-11: ウィンドウ固定でバブル全画面中の「余白を黒で埋める」度合い(0〜1)。
    // 入る/出るときになめらかに変化させる（paintGL()参照）。
    float m_focusBlackFade = 0.0f;
    // デバッグ用: Shift+Dでガラスのデバッグ表示をサイクルする
    // （0=通常 1=実効法線 2=屈折レイヤー単体。handleKeyPress()参照）。
    int m_glassDebugMode = 0;
    // Quality屈折はバブルのアクティブ演出と同期。開始/終了時に0↔1へ
    // 補間し、屈折率とエッジブーストを滑らかに切り替える。
    CanvasItem *m_glassRefractingItem = nullptr;
    float m_glassRefractionFade = 0.0f;
    bool m_glassBubbleActive = false;

    QList<CanvasItem*> m_items;
    // ドロップ/リサイズ確定時に他のバブルを重ならない位置へ押し出す
    // アニメーション。キー＝押し出されているアイテム。
    QHash<CanvasItem*, QVariantAnimation*> m_overlapAnims;

    // ---- 範囲選択（複数選択） ----
    QSet<CanvasItem*> m_selectedItems;
    // 複数選択したバブルをまとめてドラッグしている間、各アイテムの
    // ドラッグ開始時点の位置を保持する（キー＝アイテム）。空＝単体ドラッグ中。
    QHash<CanvasItem*, QPointF> m_multiDragStartPositions;

    // ---- フェードイン（バブルをフェードで再表示する）----
    QHash<CanvasItem*, QVariantAnimation*> m_fadeInAnims;
    QHash<CanvasItem*, double>             m_fadeInAlpha; // 0.0〜1.0。エントリがある間だけpaintGLで適用する。

    // ---- 2026-09-22: バブル融合（BubbleFusion.h / tickBubbleFusion）----
    BubbleFusion   m_fusion;
    QElapsedTimer  m_fusionClock;
    // ドラッグ中の「マウスそのまま」の位置。吸い寄せはこの位置に足して置く。
    QPointF        m_fusionRawDragPos;
    bool           m_fusionRawValid = false;
    // 吸い込み中の値（開始時に控える）
    QPointF        m_fusionAbsorbFromCenter;   // 吸われる側の中心（canvas座標）
    double         m_fusionAbsorbFromLift = 1.0;
    QPointF        m_fusionAbsorbRestorePos;   // Undoで戻す位置（掴む前の位置）
    // 融合の演出で liftScale を触ったバブル（演出が終わったら 1.0 に戻す）
    QSet<CanvasItem*> m_fusionLiftItems;

    // ---- 範囲選択ドラッグ中の一時矩形（canvas座標） ----
    QPointF m_rectSelectStartCanvas;
    QRectF  m_rectSelectCurrentCanvas;

    CanvasItem        *m_activeItem      = nullptr;
    CanvasItem        *m_focusedItem     = nullptr;
    CanvasItem        *m_pendingFocusItem = nullptr;
    // 2026-07-15 バグ修正（D&Dが🚫固定になる不具合、層1の根本対策）:
    // focusItem()/notifyWindowMaximizedChanged()の非同期完了ラムダは、従来
    // 「m_pendingFocusItem/m_focusedItemが今も同じitemポインタを指しているか」
    // で自分がまだ有効かを判定していた。しかしitemが破棄された後、同じ
    // アドレスに新しいCanvasItemが再確保された場合（ABA問題）、ポインタ比較
    // だけでは「別物だが同じアドレス」を誤って「まだ有効」と判定してしまう
    // 危険がある。フォーカスの開始/解除/対象変更のたびに必ずインクリメント
    // する世代カウンタを導入し、各非同期ラムダは「自分が発行された時点の
    // 世代と今の世代が一致するか」だけを見ればよいようにする
    // （ポインタの生死やアドレス再利用を気にする必要がなくなる）。
    quint64 m_focusGeneration = 0;

    // カメラ
    QPointF m_cameraPos  {0.0, 0.0};
    double  m_cameraZoom {1.0};
    QPointF m_canvasOffset {0.0, 0.0};
    double  m_canvasScale  {1.0};

    // 全画面前の状態
    QPointF m_preFocusCameraPos  {0.0, 0.0};
    double  m_preFocusCameraZoom {1.0};

    // カメラアニメーション
    // 2026-07-15: 従来はX/Y/ズームを3本のQVariantAnimationで独立にワールド
    // 座標補間していたが、位置の残り距離が増大中のズームに掛け算されるため、
    // 画面上では「先にズームが寄り切ってから、端にあるバブルが横から
    // スライドしてくる」ように見えていた。進行度0→1のアニメ1本に統一し、
    // 毎tickで「アンカー点(CameraAnimAnchor参照)の画面上の位置が進行度に
    // 比例して直線的に動く」ようにカメラ位置を逆算する（ズームは対数補間）。
    // これで寄り/引きと位置合わせが体感的にも同時に進行し、同時に完了する。
    //
    // 一般式: camPos(f) = P − ((1−f)·off0 + f·off1) / zoom(f)
    //   P    = アンカー点(ワールド座標)
    //   off0 = 開始時のPの画面中心からのオフセット(px)
    //   off1 = 着地時のPの画面中心からのオフセット(px)
    // Pの画面オフセットはoff0→off1へ線形に補間される。off0/off1の定義から
    // f=0で開始カメラ位置、f=1で着地カメラ位置に厳密に一致する。
    // 2026-07-15: 120Hz化。QVariantAnimationはQt内部のQUnifiedTimer
    // (既定16ms≒60fps)で駆動され、Widgetsアプリではこの周期を上げる公開API
    // がない。そこでカメラ演出はMainWindowの120Hzロジックタイマー
    // (syncLogic()→tickCameraAnimation())からQElapsedTimerベースで手動駆動
    // する。イージングがInOutCubicなのは、画面座標ベース補間では進行度の
    // 微分がそのままバブルの画面上速度になるため: OutCubicだと開始瞬間に
    // 平均の3倍速(=1フレーム数十px)で飛び出してがくつく。InOutCubicは
    // 両端速度0で、最高速は目が追従済みの中盤に来る。
    struct CamAnimDriver {
        bool          running    = false;
        QElapsedTimer clock;
        int           durationMs = 600;
        QEasingCurve  easing { QEasingCurve::InOutCubic };
    };
    CamAnimDriver m_camAnimDriver;
    QPointF m_camAnimTargetPos {0.0, 0.0};           // 着地カメラ位置(ワールド)
    double  m_camAnimTargetZoom {1.0};               // 着地ズーム
    double  m_camAnimFromZoom {1.0};                 // 開始ズーム(対数補間の基点)
    QPointF m_camAnimAnchorPos {0.0, 0.0};           // P: アンカー点(ワールド)
    QPointF m_camAnimAnchorOff0 {0.0, 0.0};          // off0(px)
    QPointF m_camAnimAnchorOff1 {0.0, 0.0};          // off1(px)
    bool    m_camAnimAnchorIsTarget = true;          // retarget時のアンカー再設定用

    // 2026-07-16: ホイール中クリックのダブルクリックによる原寸リセット
    // (resetFocusCameraToNative()) 専用。このアニメ中だけ、毎tickの
    // カメラ位置をclampCameraToFocusedImage()と同じ規則で対象アイテムの
    // 矩形内に収まるようクランプする。通常のフォーカスイン/アウト演出
    // （キャンバス全体⇔アイテムの間を行き来する）は対象外
    // （そちらは意図的にアイテム範囲の外から演出するため、ここでクランプすると
    // 軌道が壊れる）。
    // 背景: off0/off1のアンカー方式は始点・終点では厳密にアイテム内に
    // 収まる値になるが、中間フレームは「アンカー点の画面オフセットの線形
    // 補間」でしかないため、ズームアウト中に一瞬だけカメラがアイテム範囲外を
    // 指し、フォーカス対象の外側（透過背景/他のバブル）が見えてしまう
    // ことがあった。
    bool       m_camAnimClampToItemBounds = false;
    CanvasItem *m_camAnimClampItem = nullptr;

    // ドラッグ状態
    QString m_activeMode;
    QPointF m_dragStartPos;
    QPointF m_dragStartScreenPos;
    QPointF m_startItemPos;
    double  m_startItemScale {1.0};
    double  m_resizeBaseW    {1.0};
    QPointF m_startCameraPos;

    // ---- フォーカス中ドラッグパン（focus_image_pan）の慣性 ----
    // 2026-07-12: 速度サンプリング・摩擦減衰・tickループはCameraPanInertia
    // クラスへ切り出した（振る舞いは変えていない、純粋な移設）。
    CameraPanInertia *m_focusPanInertia = nullptr;
    // 2026-07-12: 通常キャンバスパン（camera_pan）の慣性。上のm_focusPanInertia
    // と同じCameraPanInertiaクラスを使うが、フォーカス中パンとは同時に
    // 起こらない（片方のモードしかアクティブにならない）ため、状態が混ざらない
    // よう分けてインスタンス化してある（コード上の見通しの良さを優先）。
    CameraPanInertia *m_canvasPanInertia = nullptr;

    // ---- ホイールズームのなめらか化 ----
    // 2026-07-17: ホイール1コリごとにm_cameraZoomを即座に書き換えていたのを
    // やめ、目標値へ毎フレームなめらかにイージングするようにした
    // （実体はCameraZoomEase、tickCallbackはhandleWheel()内で設定）。
    // m_targetCameraZoomは「今向かっている先」のズーム値。イージング中で
    // ない時にホイールが来たら現在のm_cameraZoomに同期し直してから factor を
    // 掛ける（他の手段でズームが変わった後もズレないように）。
    // m_zoomAnchorScreenPos/CanvasPosは「カーソル位置の下のキャンバス座標を
    // 画面上で固定したまま拡大縮小する」ための基準点。m_zoomAnchorClampRectは
    // フォーカス中ズーム時のみ使う壁クランプ対象（フォーカス中でなければ空）。
    CameraZoomEase *m_zoomEase = nullptr;
    double  m_targetCameraZoom = 1.0;
    QPointF m_zoomAnchorScreenPos;
    QPointF m_zoomAnchorCanvasPos;
    bool    m_zoomAnchorHasClampRect = false;
    QRectF  m_zoomAnchorClampRect;
    // 中ボタンのダブルクリック検出用（前回の中ボタン押下からの経過時間・座標を見る
    // 簡易実装。左/右クリックのダブルクリック判定〈m_singleClickTimer等〉と違い、
    // 対象アイテムのヒットテストは不要〈カメラ操作なのでどこを押しても同じ扱い〉
    // なので、専用のQElapsedTimerで十分）。
    // 2026-07-12: 時間だけでなく座標も見るようにした。中ボタンドラッグ→
    // すぐ離してまた中ボタンドラッグ、という「連続パン操作」を誤ってダブル
    // クリックと判定してしまう不具合があったため（1回目のドラッグで移動した
    // 先の座標で2回目を押しても、300ms以内なら常にダブルクリック扱いになって
    // いた）。前回の押下位置からほぼ動いていない場合だけダブルクリックとみなす。
    QElapsedTimer  m_middleClickClock;
    QPointF        m_lastMiddleClickPos;

    // シングル/ダブルクリック判定用
    QTimer             *m_singleClickTimer   = nullptr;
    CanvasItem         *m_pendingClickItem   = nullptr;
    QPointF             m_pendingClickPos;
    CanvasItem         *m_lastClickItemForDouble = nullptr;
    // 2026-09-20: ダブルクリック2回目を押した時刻（double_press中の暗転の遅延用）。
    QElapsedTimer       m_doublePressClock;
    // 逆輸入ヒント（背景暗転）。
    QTimer             *m_exportHintPollTimer = nullptr;
    float               m_exportHintAlpha     = 0.0f;
    // フェード（30Hz・フェード中だけ動く）。
    QTimer             *m_exportHintFadeTimer = nullptr;
    QElapsedTimer       m_exportHintFadeClock;
    float               m_exportHintFadeFrom  = 0.0f;
    bool                m_exportHintTarget    = false;

    // 2026-09-20: 通常時（フォーカス外）のホイールクリック判定。
    // ホイールは押した瞬間からキャンバスのパン（camera_pan）なので、
    // 「押してから離すまでの移動量が小さく・短い」ものだけをクリックとみなし、
    // 前回クリックから一定時間内・近い位置の2回目のクリックをダブルクリック
    // とする（離したときに確定。handleMouseRelease の camera_pan 参照）。
    QElapsedTimer       m_canvasMiddlePressClock;     // 今回押してからの時間
    QElapsedTimer       m_canvasMiddleClickClock;     // 前回クリックを離してからの時間
    int                 m_canvasMiddleClickCount = 0; // 連打の何回目か（2=整列 3=名前順）
    QPointF             m_canvasMiddlePressPos;
    QPointF             m_lastCanvasMiddleClickPos;
    // 2026-09-23: フレーム機能は本体から外した（old_fn/frames）。セッションに
    // あったフレーム情報（矩形とタイトル）だけ保持し、保存時に書き戻す。
    QJsonArray          m_legacyFramesJson;

    // 右クリック用（バブルをアクティブ化する専用ボタン）。
    // 左クリックの再生/一時停止トグルと混線しないよう、左クリック用の
    // シングル/ダブル判定状態とは完全に分離して持つ。
    // シングル右クリック: アクティブ化のみ（再生/一時停止トグルはしない）。
    // ダブル右クリック: 左のダブルクリックと同様、全画面切替/解除。
    QTimer             *m_rightClickTimer          = nullptr;
    CanvasItem         *m_rightPendingClickItem    = nullptr;
    CanvasItem         *m_rightLastClickItemForDouble = nullptr;

    // 2026-09-17: 右ドラッグでウィンドウ移動。
    // Pending: 右ボタンを押している間（まだクリックかドラッグか未確定）。
    // Dragging: しきい値を超えてウィンドウ移動中。
    // Anchor: 押した位置のウィンドウ内での割合（0〜1）。
    bool    m_rightWindowDragPending      = false;
    bool    m_rightWindowDragging         = false;
    bool    m_rightWindowDragPendingRefit = false;
    QPoint  m_rightWindowDragStartGlobal;
    QPointF m_rightWindowDragAnchor;
    // 右クリック（アクティブ化）は離したときに判定する。押した位置と、
    // 押している間にしきい値以上動いて無効になったかどうか。
    QPointF m_rightPressScreenPos;
    bool    m_rightClickCancelledByDrag   = false;

    // ---- 動画全画面中の「長押しで2倍速」----
    // 長押し確定と判定するまでの時間待ち。確定したら通常のシングルクリック
    // （再生/一時停止トグル）はキャンセルする。
    QTimer               *m_longPressTimer  = nullptr;
    QPointer<CanvasVideo> m_longPressVideo;
    bool                  m_longPressActive {false};

    // ---- 動画全画面中の操作フィードバックUI ----
    // 2倍速中（m_longPressActive）は画面上部中央に image/svg/ico_x2.svg を表示する。
    // 5秒スキップ操作をした瞬間は、方向に応じて左右どちらかに矢印マークを
    // 一定時間フェードアウトさせながら表示する。
    int                m_seekIndicatorDir   {0}; // -1=巻き戻し, 0=非表示, 1=早送り
    double             m_seekIndicatorAlpha {0.0};
    QVariantAnimation *m_seekIndicatorAnim  = nullptr;

    // ホイール中クリックのダブルクリックによる原寸リセット時の「強い枠グロー」
    // 演出用（triggerStrongBorderGlow()/paintStrongBorderGlow()参照）。
    // 1.0=発生直後の最大強度、0.0=フェードアウト完了。
    double             m_strongBorderGlowAlpha {0.0};
    // 2026-09-15 E-1: 強グローの再描画要求を30Hzへ間引くためのクロック。
    // 値(m_strongBorderGlowAlpha)は毎tick更新するので明るさのカーブは
    // 変わらない。間引くのは requestFullRender の回数だけ（実装側の
    // コメント参照）。発生時と終了時は invalidate して必ず1コマ描く。
    QElapsedTimer      m_strongGlowRenderClock;
    QVariantAnimation *m_strongBorderGlowAnim  = nullptr;
    // 2026-09-14 D-2: 外周線はカーソルを見ない（濃さ固定）ので、追従用の
    // 位置は持たなくなった。パスは窓サイズが変わったときだけ作り直す。
    QPainterPath       m_borderPath;
    QSize              m_borderPathSize;
    void showSeekIndicator(int dir);
    bool updateFocusFeedbackAnimation(int dtMs);

    // 2026-07-15: x2バッジ（長押しで2倍速）の出現/消失アニメーション。
    // CanvasVideo::paintPauseIcon()と同じ「壁時計dt＋OutBackポップ」の
    // 考え方を流用。ただしこちらは消える時に縮小はさせず、フェードアウト
    // のみにする（要望通り）。
    float  m_speedBadgeScale {0.0f};
    float  m_speedBadgeAlpha {0.0f};
    bool   m_speedBadgePopActive {false};
    double m_speedBadgePopElapsedMs {0.0};
    bool   m_speedBadgeWasActive {false};

    // リフト・パルスアニメーション
    QVariantAnimation *m_liftAnim  = nullptr;
    double m_pulseAlpha {40.0};
    QVariantAnimation *m_pulseAnim = nullptr;
    // 2026-09-14 Phase2: パルス（アクティブ枠の明滅）が原因の再描画を間引く
    // ためのクロック。実測で、アクティブなバブルが1つでもあると
    // m_pulseAnim の valueChanged（約60Hz）が毎回 requestItemRender() を呼び、
    // 何も動いていない状態でもアプリ全体が 60fps で回り続けていた
    // （[RenderSchedule] fps=60.0 ui=300 full=0 video=0 camera=0）。
    // 明滅の周期は900msなので、20Hz程度に落としても見た目は変わらない。
    QElapsedTimer m_pulseRepaintClock;
    // 2026-09-14 Phase4: ガラスの2つのフェード（フォーカス消灯/点灯・掴み
    // 屈折の立ち上がり）が自分で要求する再描画を30Hzへ間引くためのクロック。
    QElapsedTimer m_glassFadeFrameClock;
    // 2026-09-14 Phase4: アンビエント発光の色送り（syncLogicMedium内の
    // 全アイテム走査）を15Hzへ間引くためのクロック。restart()の戻り値を
    // そのままdtに使うので、間引いてもフェードの到達時間は変わらない。
    QElapsedTimer m_ambientTickClock;
    // 2026-09-15 E-2: 停止マークのアニメ用フレームを30Hzへ間引くクロック
    // （CanvasVideo::pauseIconNeedsFrames() のコメントに経緯）。
    QElapsedTimer m_pauseIconTickClock;
    // パルス対象（アクティブ／選択中のバブル）の有無に応じて
    // m_pulseAnim を start/stop する。対象が無い間はアニメーション自体を
    // 止めてしまい、valueChanged のコールバックも発生させない。
    // 呼び出し元は syncLogicMedium()（60Hz）。アクティブ／選択の変更経路が
    // 多数あるため、個別のsetterへ仕込まず1箇所で毎tick確認する。
    void updatePulseAnimationState();

    TrashZone *m_trashZone = nullptr;
    BubbleDragExporter *m_bubbleDragExporter = nullptr;

    bool m_showGrid {false};
    int  m_gridSize {50};

    // 透明効果OFF時の半透明フォールバック背景を描くかどうか
    bool m_dimBackgroundEnabled {false};

    // D/F: 再生バー表示制御
    bool   m_playbarVisible   {true};
    int    m_playbarFadeTimer {0};
    QElapsedTimer m_playbackOverlayUpdateClock;
    // 2026-09-14: イコライザだけが動いているフレームの再描画を30Hzへ
    // 間引くためのクロック（syncLogicMedium 参照）。
    QElapsedTimer m_equalizerUpdateClock;
    // 2026-09-14 PhaseC-1: リストのスクロールを120Hz(syncLogic)で進める
    // ための実経過クロック。
    QElapsedTimer m_labelScrollClock;
    QPoint m_lastCursorPos;

    // アクティブが切り替わった/外れた瞬間に再生バーがパッと消えてしまわない
    // よう、直前までアクティブだった動画を「フェードアウト中」として保持し、
    // アルファが0になるまでは引き続きtick/描画を続ける（QPointerなので
    // アイテム自体が削除された場合は自動的にnullへ戻る）。
    QPointer<CanvasVideo> m_playbarOutgoingVideo;

    // ---- ラベルの固定（2026-09-12追加）----
    // 固定されたラベルを持つ動画。固定は同時に1つだけ。別のバブルを
    // アクティブにしてもこのラベル（＋開いているリスト）を左上に出し続ける。
    // ただし別の動画を全画面（フォーカス）にしている間は、そちらのラベルを
    // 優先する（そのときは他の動画は止まっているので、見るべきは手前側）。
    // QPointerなので、対象のバブルが削除されれば固定は自動的に外れる。
    QPointer<CanvasVideo> m_pinnedLabelVideo;
    // ラベルを長押しして引っ張っている最中の対象（m_activeMode=="labelPull"）。
    QPointer<CanvasVideo> m_labelPullVideo;
    // リストの行を掴んでいる最中の対象（m_activeMode=="labelRowDrag"）。
    QPointer<CanvasVideo> m_labelRowDragVideo;
    // いま画面左上のラベルを担当している動画（固定が勝っていれば固定側、
    // そうでなければアクティブな動画）。クリック・ホイール・ホバーの
    // 宛先はすべてこれで決める。
    CanvasVideo *labelOwnerVideo() const;
    // 掴んだままreleaseを取りこぼしたドラッグモード（"labelRowDrag" /
    // "labelPull"）の後始末。syncLogicMedium()の頭から毎tick呼ばれ、
    // 左ボタンが押されていなければ操作を確定させずに取り消す。
    void cancelStaleLabelDragModes();
public:
    // ラベル／行のドラッグを、並べ替えの確定も固定トグルもせずに取り消す。
    // MainWindow::changeEvent（ウィンドウ非アクティブ）からも呼ばれる。
    void cancelLabelDragModes();
    // いま画面左上に出ているラベル（＋開いていればリスト）の矩形。
    // このウィジェットの座標系。何も出ていなければ空。
    // 島（DynamicIsland）の出現判定から、この範囲を除くために使う。
    QRect labelUiRect() const;
    // 2026-09-16 F-1: ラベル／リストを「まだ描いている」動画すべての和。
    // オーバーレイの無効化範囲はこちらを使う（実装側のコメントに経緯）。
    QRect labelUiDamageRect() const;
private:
    // 固定ラベルが優先される状況か。
    bool pinnedLabelWins() const;
    // 上記の切り替え検出用に、直前フレームでのアクティブ動画を保持する。
    QPointer<CanvasVideo> m_prevActiveVideoForPlaybar;

    CanvasLauncher *m_launcherTooltipItem {nullptr};
    QPoint          m_launcherTooltipCursor;
    int             m_launcherTooltipHoverMs {0};
    bool            m_launcherTooltipVisible {false};

    // ウィンドウ全画面前の保存（focusItem()/unfocusItem()専用。
    // FocusSessionController.h の FocusWindowGeometryState 参照）
    FocusWindowGeometryState m_focusWindowGeometry;
    // 2026-07-08: focusItem()/unfocusItem()は、mw->resizeAnim()（MainWindow側の
    // ウィンドウリサイズ用QPropertyAnimation、両者で共有）のfinishedへ毎回
    // 新しいラムダをconnect()している。前の呼び出しで繋いだラムダを
    // disconnectしないまま次のフォーカス操作でresizeAnim()をstop()すると
    // （stop()はfinishedを発火しないため）、そのラムダは繋がったまま
    // 宙に浮き、次の（無関係な）resizeAnim完了時に一緒に発火してしまう。
    // これにより、実際にはカメラが動いていないのにm_focusedItemが古い
    // アイテムへ書き換えられる（＝見た目は全画面化していないのに内部的には
    // フォーカス中と誤判定される）不具合が起きていた。常にこの1つの
    // メンバで「今つないでいる接続」を管理し、新しく繋ぐ前に必ず
    // disconnectする。
    QMetaObject::Connection m_resizeAnimFinishedConn;
    // beginFocusPlaybackPolicy()/endFocusPlaybackPolicy()/forgetFocusPausedVideo()
    // 専用。FocusSessionController.h の FocusSleepBookkeeping 参照。
    // generation の役割: endFocusPlaybackPolicy() が発行する遅延ウェイク
    // (QTimer::singleShot)は、発火するまでの間に別のフォーカスセッションが
    // 開始/終了して、対象動画が「新しいセッションの都合で」再びFocusSleepへ
    // 入れ直されることがある。遅延ウェイク側はvideo->bubbleState()==
    // FocusSleepしか見ていないため、このケースでは新しいセッションが再度
    // 寝かせた動画を、古いセッションのタイマーが横取りして起こしてしまう
    // （フォーカス中の動画に他の音声が混ざる不具合の原因）。世代カウンタで
    // 「このタイマーを発行した時点のセッション」を記録し、発火時に現在の
    // 世代と一致しない場合は何もしないようにして、この横取りを防ぐ。
    FocusSleepBookkeeping m_focusSleepBookkeeping;

    // ウィンドウ全画面アニメーション中のキャンバス内容フェード用。
    // QGraphicsOpacityEffectをQOpenGLWidgetにかける方式は効果が薄く、
    // 見た目もむしろ不安定になったため、paintGL()内でRenderer::setGlobalAlpha()
    // に渡す単純な値として自前アニメーションする方式にしている。
    double              m_contentOpacity     {1.0};
    QVariantAnimation  *m_contentOpacityAnim {nullptr};

    // 球体アニメ（未使用・削除予定）

    // 2026-09-14: サラウンドモード廃止（m_surroundMode /
    // m_surroundVolumeQueue を削除）。S / Q キーは未割り当てになった。

    // ---- マスターボリューム ----
    MasterVolumeController m_masterVolume;

    // ---- D3D11レイヤー分離方式: ステップ1（最小疎通確認） ----
    // VideoLayerHost::initialize() が失敗した場合は nullptr のままにせず
    // isInitialized()==false の状態で保持し、以降の処理はすべてスキップする。
    std::unique_ptr<VideoLayerHost> m_videoLayerHost;
};
