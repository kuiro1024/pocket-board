#pragma once
#include "Blur.h"
#include "CanvasItem.h"
#include "Renderer.h"
#include "BubbleController.h"
#include "engine/MpvPlayer.h"
#include "MediaSource.h"
#include "ThumbnailLoader.h"
#include "PluginMediaResolver.h"
#include "VideoSleepCoordinator.h"
#include "VideoRenderResolutionPolicy.h"
#include "CanvasVideoLabel.h"
#include <QObject>
#include <QImage>
#include <QPair>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QElapsedTimer>
#include <QColor>
// 一覧のスナップショットのセッション保存（panelSnapshotToJson）用。
#include <QJsonObject>
#include <atomic>
#include <functional>
#include "plugin/PbPluginApi.h"
#include "plugin/PbThumbnailApi.h"
// 一覧のスナップショット（m_panelSnapshot）がPbPlaylistItemを受け取るため。
#include "plugin/PbPlaylistApi.h"

class QPainter;

class CanvasVideo : public QObject, public CanvasItem
{
    Q_OBJECT

public:
    explicit CanvasVideo(QObject *parent = nullptr);
    ~CanvasVideo() override;

    QRectF itemRect() const override;
    void   paintGL(Renderer &r, const QRectF &screenRect,
                   bool isActive, double pulseAlpha,
                   double screenScale,
                   double contentCornerFactor = 1.0) override;
    bool supportsChromeLayer() const override { return true; }
    void paintChromeGL(Renderer &r, const QRectF &screenRect, bool isActive,
                       double pulseAlpha, double screenScale,
                       double contentCornerFactor = 1.0) override {
        paintOverlay(r, screenRect, isActive, pulseAlpha, screenScale, contentCornerFactor);
    }
    GlassContentSource glassContentSource() const override {
        return m_glassContentSource;
    }

    // dynamic_cast<CanvasVideo*>(item) 代替の型判定用（CanvasItem.h参照）。
    CanvasVideo*       asVideo()       override { return this; }
    const CanvasVideo* asVideo() const override { return this; }

    void loadUrl(const QString &mediaUrl);
    void loadDirectUrl(const QString &url);
    void loadMediaSource(const MediaSource &source);
    void restoreLazy(const QString &sourceUrl,
                     const QString &thumbnailUrl,
                     qint64 resumePositionMs,
                     float volume,
                     int originalWidth,
                     int originalHeight,
                     MediaKind kind = MediaKind::LocalVideo,
                     const QString &title = QString(),
                     const QString &pluginIdHint = QString());

    void paintPlaybackOverlay(QPainter &painter, int windowW, int windowH,
                              BackdropBlur::Regions *blurRegions = nullptr);
    bool handlePlaybackBarClick(const QPointF &pos, int windowW, int windowH);

    // 2026-07-17追加: フォーカス中バブル左上の「ロゴ＋テキスト」ラベル
    // （PbResolvedMedia::labelIconSvg/labelText参照）。再生バーと全く同じ
    // フェード条件（m_playbarFadeAlpha）で表示/非表示する想定のため、
    // CanvasWidget側の呼び出し条件もpaintPlaybackOverlay()と揃えてある
    // （CanvasWidget.cppの呼び出し箇所参照）。実際の描画ロジックは
    // CanvasVideoLabel.h/.cppに切り出してあり、ここは薄い委譲のみ。
    // ---- Phase 8+: プラグイン制御シークバー（配信/動画判別用）----
    // 進捗トラック（塗り＋ドラッグハンドル）を描画・シーク可能にするか。
    // プラグイン未経由（ローカルファイル等）は常にtrue。
    bool seekBarVisible() const { return m_seekBarVisible; }
    // 時刻テキストがクリックされた領域かどうかの判定＋トグル実行。
    // ロックされている場合は何もせずfalseを返す。
    bool toggleSeekTimeModeIfUnlocked();
    // 時刻テキストの台（ガラスのカプセル）の矩形＝ヒット領域（画面座標）。
    // CanvasWidgetのクリック/ホバー判定用。表示モードで大きさは変わらない。
    QRectF timeTextScreenRect(int windowW, int windowH) const;
    // 時刻の台のホバー（2026-09-13追加）。ラベルと同じく、カーソルを重ねて
    // いる間だけほんのり明るくする（＝押して切り替えられることの合図）。
    // CanvasWidget側のtickから毎フレーム呼ぶこと。
    void setTimeHover(bool hover) { m_timeHover = hover; }
    void updateTimePadAnim(float dt);

    // 直近に描いたフォーカスラベル（左上のロゴ＋テキスト）の画面座標。
    // 描いていない場合は空。CanvasWidget側のクリック/ホバー判定用で、
    // 「UIの上を押した＝動画本体のクリックではない」の判定に使う。
    // 引っ張っている間はその分ずれた位置に描かれるので、ヒット判定も同じ
    // 分だけずらす。
    QRectF labelScreenRect() const {
        const QRectF r = m_labelPainter.lastPaintedRect();
        return r.isEmpty() ? r : r.translated(0.0, m_labelPull);
    }

    // ---- ラベルのホバー（2026-09-12追加、PbResolvedMedia::labelHoverable）----
    // プラグインが「反応してよい」と言ったラベルだけ、カーソルを重ねた
    // 間ほんのり明るくする。押しても何も起きないラベル（Live・単発動画）は
    // 反応させない＝そこに機能があるかどうかを見た目で伝える。
    // どんな反応にするかは本体が決める（プラグインは可否だけを渡す）。
    bool labelHoverable() const { return m_labelHoverableEffective; }
    void setLabelHover(bool hover) { m_labelHover = hover; }

    // ---- ラベルの固定（2026-09-12追加、PbResolvedMedia::labelPinnable）----
    //
    // 固定すると、別のバブルをアクティブにしてもそのラベル（＋開いている
    // リスト）が画面左上に居座り続ける。画像を全画面にしている間なども
    // 「次に何が来るか」を見ていられる。
    // 固定してよいかはプラグインが決める（Liveやロゴだけのラベルは対象外）
    // が、固定の仕組み・操作・優先順位は本体の持ち物。
    // 操作: ラベルを長押ししたまま下へ引っ張り、しきい値を超えて離すと
    // 固定／解除がトグルする。引っ張らずに離した場合はリストの開閉。
    bool labelPinnable() const { return m_labelPinnableEffective; }
    // ラベルの縁の再生位置表示を、いま描き直す必要があるか
    // （前回描いた位置から1px以上進んだか）。CanvasWidget のtickが見る。
    bool labelProgressNeedsRepaint() const;
    bool labelPinned() const { return m_labelPinned; }
    void setLabelPinned(bool pinned);
    // 他のバブルの固定ラベルが左上を占めている間、こちらのラベルは描かない
    // （同じ場所に2枚出さないため）。CanvasWidget側が毎フレーム決める。
    void setLabelSuppressedByPin(bool suppressed) { m_labelSuppressedByPin = suppressed; }
    // 固定されたラベル（＋リスト）だけを描く。再生バーは描かない。
    // 非アクティブなバブルに対してもCanvasWidgetから呼ばれる。
    // ---- アンビエント発光（2026-09-13追加）----
    //
    // 【これは何か】いま画面に出ている映像の平均色を、バブルの背後へ
    // 淡く広げる。テレビの裏に付けるアンビライトと同じ発想で、暗い所で
    // 複数のバブルがそれぞれの色で光る。
    //
    // 【ガラスの縁のハイライト（GlassHighlightEffect）との違い】
    // あちらは「バブルの形と光の向き」から縁の光り方を計算するもので、
    // 映像の色は見ていない。こちらは形を見ずに色だけを使い、光を
    // バブルの外＝キャンバス側へ出す。役割が重ならないので両立する。
    //
    // 【更新の流れ】専用のタイマーは持たない。
    //   1. CanvasWidget::paintGL() が、既に画面へ出したテクスチャ
    //      （glassContentSource）から平均色を採り、ここへ渡す（間引きあり）
    //   2. 毎tick updateAmbientGlow() が現在の色をその目標へ寄せる
    //   3. 描くのも paintGL()。再生中のバブルはどのみち動画フレームごとに
    //      同じ矩形が描き直されるので、追加のフレーム要求はほぼ要らない
    //      （要るのは止まって消えていく間だけ。ambientGlowNeedsFrames()）
    //
    // 採り直してよい頃合いか（時間で間引く）。
    bool ambientSampleDue() const;
    // 採った色を渡す。実際に出る色はここから時定数付きで追従する。
    // 全体の平均に加えて、上下左右それぞれの寄りの色を持つ。1色だけだと
    // 「色の付いた影」にしか見えないので、辺ごとに違う色を出して
    // 「その辺から漏れている」ように見せるため。
    struct AmbientSample
    {
        bool   valid = false;
        QColor average;
        QColor left, right, top, bottom;
    };
    void setAmbientSampledColor(const AmbientSample &sample);
    // 辺ごとの色（0=全体, 1=左, 2=右, 3=上, 4=下）。アルファは共通の濃さ。
    enum AmbientSide { AmbientAverage = 0, AmbientLeft, AmbientRight,
                       AmbientTop, AmbientBottom, AmbientSideCount };
    QColor ambientGlowColor(int side) const;
    // 映像の明るさ（0〜1）。光の筋の強さに掛ける。暗い映像で筋が出ると
    // 嘘くさいので、明るいときだけ伸ばす。
    double ambientBrightness() const { return m_ambientBrightness; }
    // 色を目標へ進める。
    void updateAmbientGlow(double dtSec);
    // いま描くべき色（アルファ＝濃さ）。出さないときはアルファ0。
    QColor ambientGlowColor() const { return ambientGlowColor(AmbientAverage); }
    // 2026-10-12: 中身の平均色（再生していなくても、一度採れていれば返す）。
    // ambientGlowColor() は再生中しか色を返さないので、グミの影用に別に用意した。
    // 2026-10-13: 光の色（m_ambientCur）は止まると m_ambientHasColor が落ちて、
    // 採り直しのたびに有り無しを行き来し、影の色が切り替わってちらついた。
    // 最後に採れた素の平均色を持ち続けて返す。
    QColor contentAverageColor() const override { return m_contentAvgColor; }
    // 消えていく途中などで、こちらからフレームを要求する必要があるか。
    bool ambientGlowNeedsFrames() const;

    // 2026-09-15 E-2: 停止マーク（動画中央のGL描画）のアニメが進行中か。
    //
    // 【なぜ要るか】paintPauseIcon() はアニメの状態を paintGL の中で壁時計
    // ベースに進めるので、「フレームが描かれたときにしか進まない」。それ
    // なのに自分からフレームを要求する仕組みが無く、
    //   ・非フォーカス … パルス明滅(約15Hz)に相乗り＝300msのポップが4〜5コマ
    //   ・全画面       … パルスの対象外なので、再生バーのフェード中に
    //                    BackdropBlurの形が変わるついででしか描かれない
    //                    （バーが既に出ていると1コマも来ず、途中で固まりうる）
    // という状態だった。逆に、止まって静止しているだけのバブルには15Hzが
    // 来続けていた（要らないのに来る）。この関数で「今アニメしているか」を
    // 伝え、CanvasWidget側が要るときだけ30Hzで要求する。
    bool pauseIconNeedsFrames() const;

    // 固定ラベルを「いま出すべきか」。別の動画を全画面にした等で引っ込む
    // 間もパッと消さず、この指示を目標にフェードする。
    void setPinnedOverlayVisible(bool visible) { m_pinnedOverlayVisible = visible; }
    // 出入りのフェード量（0=消えている）。描く側の判定にも使う。
    float pinnedOverlayFade() const { return m_pinnedOverlayFade; }
    void paintPinnedLabelOverlay(QPainter &painter, int windowW, int windowH,
                                 BackdropBlur::Regions *blurRegions = nullptr);
    // ラベルの引っ張り操作。押した位置からの縦移動量(px)を渡す。
    void beginLabelPull();
    void updateLabelPull(double dyPx);
    // 離したとき。true = 固定/解除が起きた（＝リストの開閉はしない）。
    bool endLabelPull();
    // 引っ張りを「無かったこと」にする。しきい値を超えていても固定は
    // トグルしない。releaseを取りこぼしたときの保険用（CanvasWidget::
    // cancelStaleLabelDragModes）。
    void cancelLabelPull();
    bool labelPullActive() const { return m_labelPullActive; }

    // ---- ラベルの長押し（2026-09-23、PbLabelActionApi.h）----
    // 押し続けると台の縁に沿って時計回りにゲージがたまり、たまりきると
    // CanvasWidget が中身（本体: フォルダで List / プラグイン: 例 Mix）を実行する。
    // このラベルで長押しを受け付けるか（直近に描いたラベルの判定）。
    bool labelHoldAvailable() const { return m_labelHoldAvailableEffective; }
    // たまりきった瞬間を1回だけ返す（CanvasWidget が毎tick聞く）。
    bool takeLabelHoldFire();
    // 押している間のカーソルの移動量（押した位置から、px）。一定以上動いたら
    // 長押しはやめる（引っ張り＝固定の操作に譲る）。
    void updateLabelHoldPointer(double distancePx);
    // たまりきった後、実行の結果を待っているか。
    bool labelHoldWaiting() const { return m_labelHoldPhase == LabelHoldPhase::Waiting; }
    // 今回の押し込みで長押しが発動したか（離したときに固定・開閉をしない）。
    bool labelHoldSwallowRelease() const { return m_labelHoldSwallowRelease; }
    // 実行の結果。失敗ならゲージが震えて戻る。
    void finishLabelHold(bool success);
    // 長押しで別のURLへ切り替える（プラグインの結果）。
    //   keepPlaying=true  … 再生はそのまま、URLとラベルだけ差し替える
    //                       （同じものを再生したまま意味づけだけ変わる場合）
    //   keepPlaying=false … 今の再生位置を保って開き直す（一瞬止まる）
    void switchSourceByLabelHold(const QString &newUrl, bool keepPlaying,
                                 const QByteArray &labelIconSvg = QByteArray(),
                                 const QString &labelText = QString(),
                                 bool labelHoverable = false,
                                 bool labelPinnable = false,
                                 bool labelProgress = false);
    // 元に戻す用: 長押しの切り替えを何回したか（URLの変化のうち長押しに
    // よるものだけを区別するため。連続再生で次へ進んでも変わらない）。
    quint64 labelHoldEpoch() const { return m_labelHoldEpoch; }
    // 元に戻す／やり直しで、その時点のURLと回数へ戻す。
    void restoreLabelHoldState(const QString &url, quint64 epoch,
                               const QJsonObject &pluginLabel);
    // 本体の長押し（ローカル動画）: 同じフォルダの同じ種類のファイルを
    // 名前順（数字は数として）に並べた本体のリスト。2件未満なら空。
    QJsonObject buildFolderListForLabelHold() const;
    bool labelHoldIsCoreAction() const { return m_labelHoldCore; }
    // ラベル関連のアニメーションだけを進める（非アクティブな固定ラベル用）。
    void tickLabelAnims(double dtMs);
    // 2026-09-14 PhaseC-1: リストのスクロールだけを進める（120Hz、
    // CanvasWidget::syncLogic() から）。戻り値＝まだ動いているか。
    bool tickLabelScroll(double dtSec);
    bool labelScrollAnimating() const {
        return m_labelPanelAnim > 0.001f
            && qAbs(m_labelPanelScrollTarget - m_labelPanelScroll) > 0.01;
    }
    bool labelAnimsNeedFrames() const;
    // 2026-09-12: 上の内訳。イコライザだけが動いているフレームでは、
    // オーバーレイ全体ではなく equalizerDamageRect() の範囲だけを
    // 描き直せばよいので、判定を分けてある（CanvasWidget側で使う）。
    bool equalizerNeedsFrames() const;
    bool labelAnimsNeedFramesExceptEqualizer() const;
    // イコライザ（棒＋音符の飛翔範囲）の矩形。ウィジェット座標。
    // 描かれていない状況では空を返す。
    QRectF equalizerDamageRect() const;
    // 2026-09-14 C-2: スクロールだけが動いているフレームで描き直す範囲
    // （板＋余白＋板の外へ出る音符）。空なら全体更新が要る。
    QRectF labelPanelDamageRect() const;

    // ---- ラベルのコントローラ（2026-09-12追加）----
    //
    // ラベルをクリックすると、その真下に「前の動画／今の動画／次の動画」を
    // 並べたパネルが開く。中身はプラグインが供給する（PbPlaylistApi.h）。
    // 本体は「前後に並ぶ項目の列」としか解釈せず、再生リスト・Mix・関連
    // 動画といった区別は一切持たない。
    bool labelPanelOpen() const { return m_labelPanelOpen; }
    // このラベルが展開できるか（＝プラグインが並びを返すURLか）。
    // Liveやただの動画は展開する中身が無いので、クリック判定も通さない。
    bool labelPanelSupported() const;
    // 島が出ている間など、開いたまま一時的に畳んでおきたいときに立てる。
    // 開閉状態（m_labelPanelOpen）は保持したまま、伸び縮みのアニメーション
    // だけを閉じる方向へ向かわせるので、理由が消えれば同じ演出で開き直す。
    // CanvasWidget側のtickから毎フレーム設定する。
    void setLabelPanelSuppressed(bool suppressed) { m_labelPanelSuppressed = suppressed; }

    // ---- 最小化（2026-09-13追加）------------------------------------
    // ウィンドウが狭く、島（DynamicIsland）が展開するとラベルとぶつかる
    // 場合の最後の逃げ道。リストを畳む（setLabelPanelSuppressed）だけでは
    // まだ当たるとき、ラベル自身を「ロゴと丸い台だけ」まで縮める。
    // リストが開いていてもいなくても効く（開いていなくてもラベルの横幅
    // だけで島と当たるため）。判定はCanvasWidget側のtickが持つ。
    void setLabelCompact(bool compact) { m_labelCompact = compact; }
    // 縮めていない状態でのラベル右端（ウィジェット座標）。判定は常にこの
    // 「本来の幅」で行う。縮んだ後の幅で判定すると、縮む→当たらなくなる→
    // 戻る→また当たる、の往復になる。
    double labelFullRightEdge() const { return m_labelRectForPanel.right(); }
    // ラベルがクリックされた。開けるものが無い場合は何も起きない。
    void toggleLabelPanel();
    void closeLabelPanel();
    // 直近に描いたパネルの矩形（閉じていれば空）。CanvasWidget側の
    // クリック/ホバー判定用。
    QRectF labelPanelScreenRect() const { return m_labelPanelRect; }
    // パネル上のクリック。行をクリックしたらそのURLへ移動する。
    // 消費した場合 true。
    bool handleLabelPanelClick(const QPointF &pos);
    // ---- 行のクリック／並べ替え（2026-09-12追加）----
    // 行は「押した瞬間」ではなく「離したとき」に反応する（押したまま
    // 動かすと並べ替えのドラッグになるため）。
    // 座標にある行の番号（一覧全体での位置）。無ければ -1。
    int  panelRowIndexAt(const QPointF &pos) const;
    void beginRowDrag(int rowIndex, double pressY, double pressX = 0.0);
    void updateRowDrag(const QPointF &pos);
    // ---- 2026-09-23: 一覧からの取り出し（本体のリストだけ）----
    // 掴んだ行を右へ引っ張ると、ラベルの板の上にいる間は行が押しつぶされ、
    // 板の外へ出た瞬間に取り出せる状態になる（CanvasWidget がその場で
    // バブルを出し、そのまま掴んだ状態へ移る）。
    // 引っ張り量（右方向、px）。取り出せない一覧では常に0。
    double rowPullAmount() const;
    // いま離れた位置 pos で取り出してよいか。
    bool rowExtractReady(const QPointF &pos) const;
    // 掴んでいる行を並びから外して返す（行ドラッグも終わる）。今再生中の
    // 行なら、隣の動画へ移ってから外す。取り出せなければ kind=Unknown。
    MediaSource takeRowForExtract();
    // 離したとき。動かしていれば並べ替えを確定し、動かしていなければ
    // その行へ移動する（＝クリック）。
    void endRowDrag();
    void cancelRowDrag();
    bool rowDragActive() const { return m_rowDragIndex >= 0; }
    // パネル上でのホイール。リストをスクロールする（キャンバスの
    // 拡大縮小に取られないよう、CanvasWidget側が先にここへ回す）。
    // 消費した場合 true。
    bool handleLabelPanelWheel(const QPointF &pos, int angleDelta);
    void updateVolAnim(float dt);
    void setVolHover(bool hover)   { m_volHover = hover; }
    float volSliderAnim() const    { return m_volSliderAnim; }

    // シークバー本体の進捗ハンドル（白丸）。常時表示だとうるさいため、
    // シークバーにカーソルを合わせている間（or ドラッグ中）だけ
    // フェードで出す。CanvasWidget側のtickから毎フレーム呼ぶ。
    void updateSeekHandleAnim(float dt);
    void setSeekHover(bool hover)   { m_seekHover = hover; }

    // ---- 再生バーUIの出現/消失アニメーション（島のUIと統一感を出す）----
    // targetVisible: このフレームでバーを表示すべきか（CanvasWidget側の
    // m_playbarVisible && アクティブモードが"panning"でない、等の判定結果）。
    // 毎フレーム（表示/非表示に関わらず）呼ぶこと。フェードの経過を
    // 進めるだけで実際の描画は行わない。
    void updatePlaybackBarAnim(bool targetVisible, double dtMs);
    // このバブルが「アクティブになった瞬間」にだけ1回呼ぶ。以後の
    // ドラッグ・全画面・アイドル非表示からの再表示等はポップさせず、
    // 単純なフェードのみになる（毎回ぽいんぽいんするとうるさいため）。
    void triggerPlaybarActivationPop();
    // 何らかのアニメーションが残っている（描画を続けるべき）か。
    bool playbackBarAnimActive() const {
        return m_playbarFadeAlpha > 0.001f || m_playbarActivationPopActive;
    }
    bool playbackUiNeedsAnimationFrames(bool targetVisible) const;
    // イコライザを除いた判定（上の内訳）。
    bool playbackUiNeedsAnimationFramesExceptEqualizer(bool targetVisible) const;
    void adjustVolume(float delta);
    void setVolume(float volume);
    float volume() const { return m_userVolume; }
    QString thumbnailUrl() const { return m_thumbnailUrl; }
    QImage thumbnailImage() const { return m_thumbnailImage; }
    void setRestoredThumbnailImage(const QImage &image);
    void setRestoredProviderThumbnailFallback(const QImage &image,
                                              const QString &base64);
    bool hasThumbnailProvider() const;
    void updateThumbnailResidency(bool insidePrefetch, bool insideEvictionBoundary,
                                  bool pinned, qint64 nowMs,
                                  bool allowNewRequest = true);
    quint64 thumbnailCpuBytesEstimate() const;
    quint64 thumbnailGpuBytesEstimate() const;
    QString thumbnailResidencyName() const;
    bool thumbnailRequestEligible(qint64 nowMs) const;
    bool hasPendingThumbnailUpload() const { return !m_thumbnailPending.isNull(); }
    void setThumbnailUploadAllowedForFrame(bool allowed)
    {
        m_thumbnailUploadAllowedForFrame = allowed;
    }

    // 2026-07-11: レイヤー切り替え（save→restore往復）のたびにサムネイルを
    // PNG再エンコード/再デコードし直すのを避けるためのキャッシュ。
    // seedThumbnailPngBase64()は復元直後、そのとき使ったbase64文字列を
    // そのまま「種」として記録しておく。thumbnailPngBase64ForSession()は
    // thumbnailImage()がそのときから変化していなければ（QImage::cacheKey()
    // で判定）その種をそのまま返し、変化していれば通常通りPNGエンコード
    // する。CanvasSessionSerializer::sessionToJson()/restoreSessionFromJson()
    // から使う。
    void seedThumbnailPngBase64(const QString &base64);
    QString thumbnailPngBase64ForSession() const;
    QString title() const { return m_source.title; }
    qint64 savedResumePosition() const { return m_resumePositionMs; }
    bool isVolSliderVisible() const { return m_volSliderAnim > 0.01f; }
    QRectF volSliderScreenRect(int windowH) const;

    // ---- Phase 1: BubbleController 経由の再生操作 ----
    // BC（BubbleController）が状態を管理する。
    void togglePlayPause();
    void bcPlay();
    void bcPause();
    void bcSeek(qint64 ms);
    // 高速（キーフレーム基準）シーク。シークバーのドラッグ中の
    // mouseMoveEventから連続で呼ぶ用途向け。ドラッグ終了時にはbcSeek()を
    // 1回呼んで正確な位置へ合わせ直すこと。
    void bcSeekFast(qint64 ms);
    // 再生速度（1.0 = 等倍）。動画全画面中の「長押しで2倍速」用。
    void setPlaybackSpeed(double speed);
    void enterFocusSleep() {
        if (m_controller.state() == BubbleState::Idle) {
            // Playback has not opened yet, so only mark the pending focus sleep.
            m_focusSleepPending = true;
            qDebug("[FocusSleep] enterFocusSleep on Idle: set m_focusSleepPending");
            return;
        }
        m_controller.requestFocusSleep();
    }
    void wakeFromFocusSleep(bool resumePlayback);
    void wakeFromDeepFocusSleep(bool resumePlayback);
    qint64 focusSleepElapsedMs() const { return m_controller.focusSleepElapsedMs(); }
    // Idle状態でenterFocusSleep()された場合にセットされる「保留中の
    // FocusSleep」フラグを明示的に解除する。このバブル自身が新たに
    // フォーカス対象になった時に呼ぶ。呼ばずに残しておくと、次に
    // このバブルを再生しようとした瞬間（Idle→Loading）に横取りされて
    // FocusSleepへ送られてしまい、サムネ・再生時間は取得できるのに
    // 実際には一切再生されない不具合になる。
    void clearPendingFocusSleep() { m_focusSleepPending = false; }
    bool hasPendingFocusSleep() const { return m_focusSleepPending; }

    void requestSleepAfterPlaybackEnd() { m_controller.requestSleepAfterPlaybackEnd(); }
    bool canWakeFromFocusPolicy() const { return m_controller.canAutoWakeFromSleep(); }

    // スクリーン矩形通知（旧 notifyScreenSize の代替）
    void notifyScreenRect(const QRectF &sr, const QRectF &vp)
    {
        // 2026-07-14: 【実験中】ライブメディアの画面外AudioOnlyを再解禁。
        // 以前ここでライブに限り suppressHiddenOptimizationFor() を
        // 掛け続け、画面外でもPlaying維持にしていた（AudioOnly中に
        // ストリーム消費が止まりSRT受信キューが溢れる実害への対策）。
        // その後、真因が「画面外カリングでrenderが止まる→vo_libmpvが
        // 再生ごと凍結」と判明しカリング除外（requiresOffscreenPaint）
        // で解消したため、vid=no（AudioOnly）自体が安全かを検証する。
        // mpvが選択解除した映像トラックのパケットをキャッシュに溜め込む
        // 挙動だった場合は、画面外放置で再びSRT警告洪水が出る
        // （その場合はPlaying維持方式へ戻す）。
        m_controller.notifyScreenRect(sr, vp);
    }

    void loadThumbnail(const QString &url);
    // 2026-07-08: ローカル動画ファイル用。Windows Explorerが表示している
    // のと同じサムネイル（ThumbnailLoader::requestLocalThumbnail()経由）を
    // 取得する。loadThumbnail()とは取得元が違うだけで、結果の保持・表示
    // (m_thumbnailImage経由) は完全に共通。
    void loadLocalThumbnail(const QString &filePath);

    // ---- Phase 2: MpvPlayer 初期化 ----
    // OpenGLコンテキストcurrent必須。初回再生時はstartPlayback()が
    // CanvasWidget::makeCurrent()して呼び、Sleep復帰時はpaintGL()から呼ぶ。
    // セッション復元直後のIdleバブルには呼ばず、サムネイルだけで表示する。
    void initializeMpv();
    void warmupPlaybackSurface();

    QString sourceUrl() const { return m_source.originalPathOrUrl; }
    MediaKind mediaKind() const { return m_source.kind; }
    const MediaSource &mediaSource() const { return m_source; }
    QString resolverPluginId() const { return m_source.resolverPluginId; }

    // ---- 2026-09-22: 本体のリスト（バブル融合で作られる、BubbleFusion.h）----
    // 並び全体（今の動画を含む）を本体が持つ。2件以上あるときだけ「本体の
    // List」で、ラベルから一覧を開ける・終わったら次へ進む・行で切り替える・
    // 並べ替える、ができる。並びの意味（再生リストか等）は本体は持たない
    // （利用者が重ねた順に並ぶだけ）。ローカルとプラグインのメディアが
    // 混ざっていてよい。
    // 【未実装】セッション保存・Undo での巻き戻し・一覧からの取り出し。
    bool hasCoreList() const { return m_coreList.size() > 1; }
    int  stackCount() const { return hasCoreList() ? static_cast<int>(m_coreList.size()) : 1; }
    // 並び全体（リストでなければ今の動画1件）。融合で他のバブルへ渡すときに使う。
    QList<MediaSource> listItems() const;
    // 並びの末尾へ足す（融合の受け側）。
    void appendListItems(const QList<MediaSource> &items);
    // セッション保存・元に戻す用（2026-09-23）。
    //   {"items":[{"k":種類,"u":URL,"t":タイトル,"th":サムネURL,"p":プラグインID}],
    //    "index":今の位置}
    // リストでなければ空。includeIndex=false は元に戻す用（今どこを再生して
    // いるかは「操作」ではないので記録しない）。
    QJsonObject coreListToJson(bool includeIndex = true) const;
    // 並びを戻す。2件未満なら本体のリストを解除する。"index" が無い・合わない
    // ときは今の動画のURLで位置を探す。
    void restoreCoreListFromJson(const QJsonObject &json);
    // 元に戻すの比較用（種類とURLの並びだけ。タイトルやサムネイルが後から
    // 埋まっただけでは変わらない）。リストでなければ空。
    QString coreListIdentityKey() const;

    // ---- 2026-09-23: プラグインのラベルのセッション保存 ----
    // プラグインが最後に返したラベル（ロゴ・テキスト・反応/固定/再生位置の可否）。
    // 保存しておけば、復元直後の「まだ一度も再生していない（＝解決して
    // いない）」バブルでも、ラベル（Solo / List / Mix / Live …）と、List・Mix
    // なら保存済みの一覧（panelSnapshot）をすぐ出せる。本体は中身を解釈せず、
    // プラグインが言ったことをそのまま覚えて戻すだけ。
    // プラグインのメディアで答えが分かっていなければ空。
    QJsonObject pluginLabelToJson() const;
    void restorePluginLabelFromJson(const QJsonObject &json);

    // 画面外カリング中でもpaintGL()を呼ぶ必要がある場合にtrue。
    //
    // 対象:
    //   - ライブメディア: vo_libmpvを進行させるためrenderToFboが必要。
    //   - Sleep/再生成/遅延破棄: GL current下で保留処理を消費する必要。
    //
    // 以前はライブだけを例外にしていたため、画面外のFocusSleep→Sleepや
    // PausedHidden→SleepでpaintGL()が永久にカリングされ、MpvPlayerの
    // 完全解放フラグが消費されないことがあった。
    bool requiresOffscreenPaint() const
    {
        const bool liveRenderRequired =
            !m_seekBarVisible
            && m_source.kind == MediaKind::PluginMedia
            && m_mpvPlayer != nullptr
            && !m_controller.isSleeping();
        const bool pendingSleepWork =
            m_sleepPending.glReleasePending()
            || m_sleepPending.mpvPlayerTeardownPending()
            || m_sleepPending.mpvRecreateForWakePending();
        return liveRenderRequired
            || pendingSleepWork
            || m_thumbnailGpuReleasePending
            || !m_playersAwaitingGlDelete.isEmpty();
    }

    // ---- BC 経由の状態・情報取得 ----
    BubbleState bubbleState() const { return m_controller.state(); }
    bool    isPlaying()  const { return m_controller.isPlaying(); }
    bool    isSleeping() const { return m_controller.isSleeping(); }
    qint64  position()   const { return m_controller.position(); }
    qint64  duration()   const { return m_controller.duration(); }
    const void *debugPlayerPtr() const { return m_mpvPlayer; }
    // Sleep状態で、通常プレイヤー・Flow先読み・GL破棄待ち・再生成予約の
    // いずれも残っていない時だけtrue。BubbleState::Sleepへの遷移直後は
    // paintGL()での同期破棄前なのでfalseになる。
    bool isDecodeFullyReleased() const
    {
        return m_controller.state() == BubbleState::Sleep
            && m_mpvPlayer == nullptr
            && !m_sleepPending.glReleasePending()
            && !m_sleepPending.mpvPlayerTeardownPending()
            && !m_sleepPending.mpvRecreateForWakePending()
            && m_playersAwaitingGlDelete.isEmpty();
    }

    void logMemoryDiagnostics() const;

    // 破棄前に呼び出し側（現在のGLコンテキストがmakeCurrent済みであること）から
    // 明示的に呼び、GPU側リソース（Qtフォールバックtex / サムネtex / mpv FBO）を
    // すべて解放する。~CanvasVideo() はGLコンテキストの有無を保証できないため、
    // 実際のGL解放はここに集約し、デストラクタは保険としてこれを呼ぶだけにする。
    void releaseGlResources(Renderer &r);

    int  currentQuality() const { return m_currentQuality; }

    // 2026-09-14: サラウンドモード（位置による音量減衰）を廃止。
    // setSurroundExcluded / isSurroundExcluded / setSurroundGain と
    // m_surroundGain 乗算レイヤーを削除した。
    void applyEffectiveVolume();

    // ---- マスターボリューム ----
    // 島の+ボタン上でのホイール操作によるマスターボリューム(0.0-1.0)。
    // 個別音量(m_userVolume)への乗算レイヤーとして
    // applyEffectiveVolume()が使う。CanvasWidget::applyMasterGainToAllVideos()
    // からバブル追加時・マスターボリューム変更時に呼ばれる。
    bool setMasterGain(float gain, bool applyImmediately = true);

signals:
    void frameReady();
    // mpvの再生位置はAudioOnly（画面外で映像停止）中も進む。映像フレーム
    // とは独立して下部シークバー／時刻表示を更新するための通知。
    void playbackPositionChanged(qint64 positionMs);
    void playbackUiChanged();
    // 実際のメディア解像度が判明し、itemRect()（バブルの見た目のサイズ）が
    // プレースホルダー値から変わったタイミングで発火する。ドロップ直後は
    // 仮サイズで重なり判定を行っているため、これを受けて再度重なりを
    // チェックし直す必要がある（CanvasWidget::addMediaSource 参照）。
    void videoDimensionsReady();

private slots:
    // audioUrl: DASH分離ストリーム対応。空でなければ streamUrl とは別の
    // 音声専用URLとして扱う（IPbPlayer::open()参照）。
    void onUrlFetched(const QString &streamUrl, int w, int h,
                      const QString &thumbnailUrl,
                      const QStringList &httpHeaders,
                      const QString &audioUrl = QString());
    void startPlayback();

private:
    void paintLocalAudioCard(Renderer &r, const QRectF &screenRect,
                             float contentRadius);
    // 2026-07-08: paintOverlay()内にあった一時停止アイコン描画を切り出した
    // もの。paintLocalAudioCard()がサムネイル（ジャケット画像）を全面に
    // 描画するケースでも、そのサムネイルの上から同じアイコンを重ねて
    // 描き直すために共有する（paintOverlay()はpaintLocalAudioCard()より
    // 先に呼ばれるため、そのままだとサムネイルに埋もれて見えなくなる）。
    // 2026-07-15: 一時停止アイコンの出現/消失アニメーション用状態。
    // 出現時はOutBackイージングで「ぽよん」と弾みながら拡大、
    // 消失時（再生再開時）は縮小しながらフェードアウトする。
    // paintPauseIcon()の呼び出しごとの経過時間(壁時計)から自前で
    // 進めるため、専用のtick駆動フックは不要（呼ばれない間＝完全に
    // 非表示の間は進める必要がないため、これで問題ない）。
    // 2026-09-14: 再生バーの部品ごとの描画キャッシュ。
    //
    // 【なぜ要るか】実測で paintPlaybackOverlay() だけで5.0〜5.6ms/フレーム
    // かかっていた（[CanvasOverlay][stat] の playback=）。高いのは面積では
    // なくガラスの作り方で、paintGlassPlate() は縁の減衰を作るために同じ
    // パスをグラデーションのペンで最大12本×5組＝のべ56本重ねて描いている。
    // 丸ボタンも放射グラデーション＋クリップパスを何枚も重ねる。
    // ところが中身は「押した」「ホバーした」「音量が段階を跨いだ」「窓の
    // 幅が変わった」以外まったく変わらない。部品ごとに画像へ焼いておき、
    // キーが同じフレームは貼るだけにする（リストの板＝m_panelCache と
    // 同じ方式）。
    //
    // 【部品ごとに分ける理由】1枚にまとめると、ホバーのアニメーション中は
    // 全部が作り直しになる。分けておけば、カーソルが乗っている音量ボタン
    // （32x32）だけが作り直され、溝も時刻の台も再生ボタンも貼るだけで済む。
    //
    // 【毎フレーム描くもの】進捗の塗り・時刻の数字・音量スライダー・
    // ハンドル・ホバーの白。いずれも矩形か文字1つぶんで安い。
    struct UiPartCache {
        QImage  image;
        quint64 key = 0;
        qreal   dpr = 0.0;
    };
    UiPartCache m_playBtnCache;
    UiPartCache m_volBtnCache;
    UiPartCache m_seekTrackCache;
    UiPartCache m_timePlateCache;
    // logicalRect の内容を draw() で画像へ焼き、その画像を貼る。key が前回と
    // 同じなら焼き直さない。painter 側の不透明度（m_contentOpacity 等）は
    // 貼るときに効くので、draw() の中で気にする必要はない。
    void drawCachedUiPart(QPainter &painter, UiPartCache &cache,
                          const QRectF &logicalRect, quint64 key,
                          const std::function<void(QPainter &)> &draw);

    float  m_pauseIconScale {0.0f};     // 黒い丸背景のスケール
    float  m_pauseIconMarkScale {0.0f}; // ▶/||マーク自体のスケール（丸とは独立）
    float  m_pauseIconAlpha {0.0f};
    bool   m_pauseIconPopActive {false};
    double m_pauseIconPopElapsedMs {0.0};
    qint64 m_pauseIconLastTickMs {0};
    bool   m_pauseIconWasPlaying {true};

    void paintPauseIcon(Renderer &r, const QRectF &screenRect);
    void paintOverlay(Renderer &r, const QRectF &screenRect,
                      bool isActive, double pulseAlpha, double screenScale,
                      double chromeAlpha = 1.0);
    // paintGL() から毎フレーム呼ぶ。durationが初めて判明した時点で1回だけ
    // (duration - m_liveEdgeBufferMs) へ絶対シークする（ライブ配信の
    // 初期バッファ確保用。詳細はPbResolvedMedia::liveEdgeBufferMs参照）。
    void tickLiveEdgeBufferSeek();
    bool beginDeferredLoadForPlayback();
    void scheduleLocalAudioResumeWorkaroundIfNeeded(bool wasEnded);
    // Error状態のバブルをクリックした時に呼ぶ再試行処理。ローカルファイルは
    // 同じストリームURLで再オープン、プラグイン解決動画（動画サイト等）は
    // 署名付きURLの失効に備えてプラグインへ再解決(resolveViaPlugin())させる。
    // どちらも直前まで再生できていた位置から再開する。
    void retryAfterError();

    // 2026-07-13: ライブメディア（プラグインがseekBarVisible=falseを指定。
    // SRT画面共有・ライブ配信等）の一時停止要求を「切断」へ
    // 差し替える。切断した場合trueを返し、呼び出し側は通常の一時停止を
    // 行わない。対象外（通常動画・ローカルファイル等）はfalse。
    // 詳細な背景はcpp側の実装コメント参照。
    bool liveDisconnectInsteadOfPause();

    // ---- Phase 8: Plugin API ----
    // MediaKind::PluginMedia の場合に loadMediaSource() から呼ばれる。
    // PluginManager 経由で url を解決し、成功したら onUrlFetched() を
    // 呼び出して既存の再生経路（startPlayback）へ合流させる。
    void resolveViaPlugin(const QString &url);

    // 2026-07-17追加: playbackEnded受信時点でm_nextUrlが空だった場合の
    // 救済策。同じURLをもう一度だけresolve()し直し、nextUrlだけを
    // 確認する（streamUrl等は使わない。既にこの動画は再生し終わって
    // いるため、mpvへの再ロードは行わない）。nextUrlが埋まっていれば
    // loadMediaSource()で続けて再生し、それでも空ならスリープへ移行する。
    // 詳細はCanvasVideo.cppの実装コメント参照。
    void probeContinuationAndAdvance();

    // 失敗表示（赤塗り＋×＋3桁エラーコード）を描画する。サイズ計算は
    // paintLocalAudioCard() のタイトル描画と同じく、渡された screenScale
    // ではなく実際の screenRect サイズから自前で算出する（カメラズームや
    // 掴み上げアニメーションとのズレを防ぐため）。
    void paintResolveFailedOverlay(Renderer &r, const QRectF &screenRect, double screenScale);

    // Phase 8: Plugin API — 解決失敗状態（エラー表示・クリックでの
    // 再試行判定に使う）。PluginMediaResolver.h の PluginResolveFailure 参照。
    PluginResolveFailure m_pluginResolveFailure;

    // ---- Phase 8+: プラグイン制御シークバー（配信/動画判別用、2026-07-03）----
    // resolveViaPlugin() のコールバックで PbResolvedMedia の値から設定される。
    // プラグイン未経由（ローカルファイル・resolve失敗のまま等）は
    // 全てホスト既定値（青・表示・ElapsedTotal・ロックなし）のまま。
    bool    m_seekBarVisible {true};
    QColor  m_seekBarColor;       // 無効(isValid()==false) = 既定色(青)を使う
    PbResolvedMedia::TimeDisplayMode m_seekTimeMode {PbResolvedMedia::TimeDisplayMode::ElapsedTotal};
    bool    m_seekTimeModeLocked {false};

    // ライブ配信の初期バッファ（PbResolvedMedia::liveEdgeBufferMs参照）。
    // 0 = 調整なし。resolveViaPlugin() のたびにリセットされ、
    // tickLiveEdgeBufferSeek() が duration 判明時に1回だけ処理して
    // trueにする（再生中に繰り返しシークし直さないためのワンショット制御）。
    int     m_liveEdgeBufferMs {0};
    bool    m_liveBufferSeekApplied {false};

    // 2026-07-17追加: フォーカスラベル（PbResolvedMedia::labelIconSvg/
    // labelText参照）。resolveViaPlugin()のたびにクリア→成功時に再設定
    // される。実際の描画・GLテクスチャキャッシュはCanvasVideoLabelPainter
    // （CanvasVideoLabel.h/.cpp）に委譲する。
    QByteArray m_labelIconSvg;
    QString    m_labelText;
    CanvasVideoLabelPainter m_labelPainter;
    // ラベルのホバー可否（プラグインが指定）と、その現在の状態。
    // m_labelHoverableEffective は描画側が実際に使う値で、ラベル本体と同じく
    // 「移動中の一時的な空白」の間は直前の値を保つ（m_labelTextSticky と対）。
    bool  m_labelHoverable {false};
    bool  m_labelHoverableSticky {false};
    bool  m_labelHoverableEffective {false};
    bool  m_labelHover {false};
    float m_labelHoverAnim {0.0f};
    // ---- 固定（ピン留め）----
    // 2026-09-13: ラベルの台の縁へ再生位置を出してよいか
    // （PbResolvedMedia::labelProgress）。ホバー可否・固定可否と同じく、
    // プラグインが渡すのは可否だけで、太さや色は本体の裁量。
    bool  m_labelProgress {false};
    bool  m_labelProgressSticky {false};
    bool  m_labelProgressEffective {false};
    // 直近に描いた進み具合。1px ぶん動いたときだけ描き直すための記録
    // （毎フレーム描き直すとオーバーレイ全体の再描画が復活してしまう）。
    mutable double m_labelProgressPainted {-1.0};
    // 出入りのフェード（0=消えている）。曲が変わる瞬間にパッと消えたり
    // 位置が飛んだりしないよう、一度消してから出し直すために使う。
    double m_labelProgressFade {0.0};
    // 描くときに使う進み具合。尺が分からない間（解決中など）は最後の
    // 値のまま保持し、フェードアウトしてから新しい曲の頭へ戻す。
    double m_labelProgressShown {0.0};
    // 台の縁に沿って光を一周させる（進み具合 0〜1）。
    void paintLabelProgressRing(QPainter &painter, const QRectF &rect,
                                double leftRadius, double rightRadius,
                                double progress, double alpha) const;

    bool   m_labelPinnable {false};          // プラグインの指定
    bool   m_labelPinnableSticky {false};
    bool   m_labelPinnableEffective {false}; // 描画・操作が実際に見る値
    // 固定ラベルの出入り（setPinnedOverlayVisible / pinnedOverlayFade 参照）。
    bool   m_pinnedOverlayVisible {true};
    float  m_pinnedOverlayFade {1.0f};
    bool   m_labelPinned {false};
    bool   m_labelSuppressedByPin {false};
    // 引っ張り。raw=指の移動量、target=ゴムで減衰させた目標、
    // m_labelPull=実際に描画へ使う量（離すとバネで0へ戻る）。
    // ---- 長押し（2026-09-23）----
    enum class LabelHoldPhase { Idle, Charging, Waiting, Done, Failed };
    LabelHoldPhase m_labelHoldPhase {LabelHoldPhase::Idle};
    bool    m_labelHoldAvailableSticky {false};
    bool    m_labelHoldAvailableEffective {false};
    bool    m_labelHoldCore {false};          // 本体の長押し（ローカル）か
    QString m_labelHoldHint;                  // 今のラベルの言葉（描くたびに決め直す）
    QString m_labelHoldHintShown;             // 押した時点の言葉（ためている間に出す）
    QString m_labelHoldQueryUrl;              // プラグインへ聞いたURL（毎フレーム聞かない）
    int     m_labelHoldQueryKind {0};         // PbLabelHoldActionKind（Noneが0）
    QString m_labelHoldQueryHint;
    QString m_labelHoldQueryTarget;           // 押しきった後にラベルが変わる語
    QString m_labelHoldTargetText;            // 今のラベルの入れ替え先（描画用）
    double  m_labelHoldShimmer {0.0};         // ゲージのきらめきの位相
    // にじみ（ぼかし）を作る小さな作業用画像。ラベルの台の周りだけなので
    // 小さく、毎フレーム作り直さずここで使い回す。
    mutable QImage m_labelHoldGlowBuf;
    mutable double m_labelHoldGlowDpr {0.0};
    double  m_labelHoldMs {0.0};              // 押してからの時間（ため始めの遅れ込み）
    double  m_labelHold {0.0};                // 描くゲージ 0〜1
    double  m_labelHoldVel {0.0};
    double  m_labelHoldWaitMs {0.0};          // 結果待ちの時間
    double  m_labelHoldShakeMs {-1.0};        // 失敗の震え（負なら無し）
    double  m_labelHoldFlash {0.0};           // 成功の光（1→0）
    double  m_labelHoldHintAlpha {0.0};
    // 2026-09-23: 押す前の合図。カーソルを少し置いていると、ゲージが通る道筋を
    // 薄く1周ぶん出し、言葉も出す（押してみるまで分からない、を無くすため）。
    double  m_labelHoldHoverMs {0.0};
    double  m_labelHoldTrack {0.0};
    bool    m_labelHoldFirePending {false};
    bool    m_labelHoldSwallowRelease {false};
    quint64 m_labelHoldEpoch {0};
    // 直近の長押しの切り替えが「再生を止めない差し替え」だったか。
    // 元に戻す／やり直しも同じやり方で行う（無駄に開き直さない）。
    bool    m_labelHoldLastInPlace {false};
    qint64  m_pendingNavigateResumeMs {0};    // 次の loadMediaSource で使う再開位置
    static QString coreLabelHoldHint();
    static QString coreLabelHoldTarget();
    void    tickLabelHold(double dtSec);
    void    applyLabelHoldSwitchInPlace(const QString &newUrl,
                                        const QByteArray &labelIconSvg,
                                        const QString &labelText,
                                        bool labelHoverable, bool labelPinnable,
                                        bool labelProgress);
    bool    labelHoldNeedsFrames() const;
    void    paintLabelHoldGauge(QPainter &painter, const QRectF &rect,
                                double leftRadius, double rightRadius,
                                double progress, double alpha,
                                double trackAmount) const;
    bool   m_labelPullActive {false};
    double m_labelPullRaw {0.0};
    double m_labelPullTarget {0.0};
    double m_labelPull {0.0};
    // ラベル／リストを描くときの不透明度。通常は再生バーと同じ
    // （m_playbarFadeAlpha）だが、固定ラベルとして描くときは1.0。
    float  m_labelFadeAlpha {0.0f};
    void paintLabelAndPanel(QPainter &painter, int W, int H,
                            BackdropBlur::Regions *blurRegions, float fade);
    void updateLabelAnims(double dtSec);
    // 固定中（または固定しきい値まで引っ張っている間）に出す鍵マーク。
    void paintLabelLockBadge(QPainter &painter, const QRectF &labelRect,
                             double amount);
    // ---- ヘッダの簡易コントロール（2026-09-12追加）----
    // 「List」の右に並ぶ ◀◀ / ▶(||) / ▶▶。台（丸い枠）は持たず、
    // アイコンだけを置く。リストを開いている間だけ出す。
    void paintLabelHeaderButtons(QPainter &painter, const QRectF &headerRect,
                                 double alpha);
    // 一覧の中で現在位置から delta 個ずれた項目へ移動する（-1=前, +1=次）。
    bool navigateRelativeInPanel(int delta);
    // 行の並べ替え（ユーザーが手で決めた順）。プラグインから来る一覧に
    // 対して、このURLの並びを優先して適用する。空なら素の順。
    // 2026-09-12: mutable なのは、窓の切り詰め（trimPanelSnapshotWindow）が
    // const の buildPanelRows() から走るため。並べ替えの意味を変える処理では
    // なく、窓から出た項目の記録を捨てるだけ。
    mutable QStringList m_panelOrderOverride;
    // 並べ替えるたびに増やす。描画キャッシュのキーに混ぜて、順が変われば
    // 必ず描き直させるため。
    mutable quint32 m_panelOrderRevision {0};

    // ---- 一覧のスナップショット（2026-09-12追加、md/2026-09-12-label-
    //      controller-handoff.md の 2-2）----
    //
    // 【これは何か】プラグインが「前後にはこれが並んでいる」と言ってきた
    // 内容を、本体がそのまま覚えているだけの写し。本体はこの列の意味
    // （再生リストなのかMixなのか、なぜその順なのか）を一切解釈しない。
    // 覚えているのは「前後に並ぶ項目の列」だけ、という約束は維持する。
    //
    // 【何のためにあるか】
    //  1. セッションに保存し、次回起動時に**通信ゼロで即座に**リストを
    //     出すため（プラグインの答えが返ったらマージする）
    //  2. プラグインがまだ答えられない間（pending）でも一覧を出すため
    struct PanelSnapshotItem
    {
        QString url;
        QString title;
        QString thumbUrl;
        int     durationSec = 0;
    };
    // 2026-09-12: プラグインの答えが届くのが const の buildPanelRows() の
    // 中なので、スナップショットの更新系は mutable / const で通している
    // （既存の m_panelRowsCache 等の描画キャッシュと同じ扱い）。
    mutable QList<PanelSnapshotItem> m_panelSnapshot;
    // プラグインの答えをスナップショットへ溶かし込む。既知の項目は位置を
    // 保ったまま中身だけ更新し、知らない項目はプラグインの順で後ろへ足す
    // （＝ユーザーが並べ替えた順を壊さない）。
    void mergePluginItemsIntoSnapshot(const QList<PbPlaylistItem> &items) const;
    // 現在位置の前後だけを残して約50件へ切り詰める。端では足りない側を
    // 反対側で補う（先頭付近なら後ろを多めに残す）。
    void trimPanelSnapshotWindow(const QString &currentUrl) const;
    // 列そのものが入れ替わっていたら、覚えていた列を捨てる
    // （別の動画・別のリストを開いた等）。実装側のコメント参照。
    void dropPanelSnapshotIfListChanged(const PbPlaylistContext &ctx) const;
    // 「いま再生している項目」が列のどれかを決める（実装側のコメント参照）。
    // 分からなければ空。
    QString resolvePanelCurrentUrl(const PbPlaylistContext &ctx) const;
    // 直近に特定できた現在位置。プラグインが一時的に currentIndex=-1 を
    // 返しても見失わないように覚えておく。
    mutable QString m_panelCurrentUrl;
    // 「現在位置が特定できない」警告を同じURLで繰り返さないための記録。
    mutable QString m_panelCurrentMissUrl;

public:
    // セッション保存・復元。保存するのは項目の列とユーザーの並べ替え順だけで、
    // サムネイル画像は保存しない（URLだけ）。中身が無ければ空を返す。
    QJsonObject panelSnapshotToJson() const;
    void restorePanelSnapshotFromJson(const QJsonObject &json);
private:
    // 直近に描いた行の「一覧全体での位置」。m_labelPanelRowRectsと同じ並び。
    QList<int> m_labelPanelRowIndices;
    QList<int> m_panelCacheRowIndices;
    // 再生中の行の矩形（キャッシュ画像の外に重ねるイコライザ用）。
    QRectF m_labelPanelNowRowRect;
    QRectF m_panelCacheNowRowRect;
    // ドラッグ中の行と、いま離せば入る位置。
    int    m_rowDragIndex {-1};
    int    m_rowDropIndex {-1};
    bool   m_rowDragMoved {false};
    double m_rowDragY {0.0};
    // 2026-09-23: 取り出し用の横方向（押した位置と今の位置）
    double m_rowDragPressX {0.0};
    double m_rowDragX {0.0};
    // つかんだ行の上端からカーソルまでの距離（追従描画のずらし量）。
    double m_rowDragGrabDy {0.0};
    // 隙間の位置（連続値）。落とし先が変わってもパッと入れ替わらず、
    // まわりの行がなめらかに退く／戻るようにするための補間値。
    double m_rowGapAnim {0.0};
    // いま隙間があるべき位置（つかんだ行を抜いた並びでの整数位置）。
    int rowGapSlotTarget() const;
    // イコライザ／音符の位相（秒）。音とは無関係の見た目だけのもの。
    double m_eqPhase {0.0};
    // 「鳴っている度合い」0〜1。止めたときにカクッと消えないよう、
    // これを掛けて棒と音符をなだらかに畳む。
    double m_eqLevel {0.0};
    // 簡易イコライザ。棒はリストの中に収めたいのでクリップして描き、
    // 音符は板の外へ飛んでいってよいのでクリップ無しで描く（2回に分ける）。
    void paintNowPlayingEqualizer(QPainter &painter, const QRectF &rowRect,
                                  double alpha, bool drawBars, bool drawNotes);
    // 「音が鳴っている」か。画面外（AudioOnly）や読み込み中（Buffering）でも
    // 音は出ているので、イコライザは動かし続ける。
    bool isSoundingForEqualizer() const;
    QRectF m_labelBtnPrev;
    QRectF m_labelBtnPlay;
    QRectF m_labelBtnNext;

    // ---- 連続再生（2026-07-17追加、PbResolvedMedia::nextUrl参照）----
    // 直近の resolveViaPlugin() 成功時にプラグインが返した「次に再生
    // すべきURL」。空文字列 = 次はない。playbackEnded 受信時にこれが
    // 空でなければ、スリープへ移行する代わりに loadMediaSource() で
    // このURLへ差し替えて再生を続ける（本体側は「プレイリスト」等の
    // 意味は一切解釈しない。詳細はPbPluginApi.hのコメント参照）。
    QString m_nextUrl;
    // 2026-07-18追加: m_nextUrlが空だった場合に「もう一度resolve()し直せば
    // 答えが変わる可能性がある」とプラグインが申告したか
    // （PbResolvedMedia::nextUrlPending参照）。playbackEnded受信時、
    // これがtrueの場合に限りprobeContinuationAndAdvance()で聞き直す。
    // 従来は無条件に聞き直していたため、連続再生と無関係な単発動画でも
    // 再生終了のたびに解決処理一式（解決ツール起動）が無駄に走っていた。
    bool m_nextUrlPending {false};

    // ---- ラベルのコントローラ（2026-09-12追加）----
    //
    // 「前の動画」は2段構えで求める。
    //   1. このバブルで実際に再生してきた履歴（m_playHistory）の最後。
    //      連続再生でもクリック移動でも積むので、常に「さっき見ていた
    //      動画」へ確実に戻れる。
    //   2. 履歴が空なら、プラグインが返す並びの1つ前
    //      （＝リストの途中から開いた直後のケース）。
    // Mix/Radioのような動的な並びでは2.が当てにならないため、1.を優先する。
    struct PlayHistoryEntry
    {
        QString url;
        QString title;
    };
    QList<PlayHistoryEntry> m_playHistory;
    // 履歴の上限。これ以上は古い方から捨てる（1バブルを何時間も
    // 流しっぱなしにしたときに無制限に伸びるのを防ぐ）。
    static constexpr int kMaxPlayHistory = 50;

    bool   m_labelPanelOpen {false};
    bool   m_labelPanelSuppressed {false};
    // 島に場所を譲るための最小化（setLabelCompact参照）。
    bool   m_labelCompact {false};
    // 0=通常のラベル、1=ロゴと丸い台だけ。台の幅がそのまま縮むので、
    // 縁の再生位置表示（paintLabelProgressRing）も一緒に追従する。
    float  m_labelCompactAnim {0.0f};
    // 伸び縮みアニメーションの目標値。開いていて、かつ一時的に畳まれても
    // いないときだけ1（開く方向）。
    bool   labelPanelExpandTarget() const {
        return m_labelPanelOpen && !m_labelPanelSuppressed;
    }
    // 0=ラベルのまま、1=カードに開き切った状態。ラベルの台がそのまま
    // 伸びて変形する演出（島UIと同じ考え方）の進捗。
    float  m_labelPanelAnim {0.0f};
    // ラベルのコントローラから移動した直後かどうか。BubbleControllerは
    // ロード完了時に「直前の状態」次第で一時停止のまま止まることがあり
    // （onPlayerFirstFrameReady内の分岐）、ユーザーが明示的に選んだ移動で
    // それが起きると「クリックしたのに再生が始まらない」になる。Readyに
    // 達した時点で確実に再生を開始させるためのワンショットフラグ。
    bool   m_navigatePlayPending {false};
    // 2026-09-13: 移動1回ごとの世代番号。m_navigatePlayPending から予約する
    // 遅延再生（QTimer::singleShot）は予約時点の世代を覚えておき、発火時に
    // 世代が変わっていたら何もしない。
    //
    // 【なぜ必要か】予約は「この移動先が読み込み終わったら再生する」という
    // 意味しか持たないのに、タイマー側には何の目印も無かった。移動先Bが
    // 再生を始めた直後（700msの保険が残っている間）に続けてCを選ぶと、Cの
    // 解決（プラグインで1〜2秒）を待っている最中に古い保険が発火する。その時点
    // でmpvが抱えているのはまだBなので、BubbleController::play()は「Bの
    // 再生を再開」してしまう。＝リストのUIはCを指しているのに流れている
    // のはB、という「たまに移動できない」不具合の正体。
    quint32 m_navigateGen {0};
    // 移動を始めてから、移動先が実際に再生を始める（または失敗する）まで
    // の間だけ true。
    //  (1) 世代番号と合わせて、古いmpvの中身を再生してしまう操作を入口で
    //      止める（bcPlay）。
    //  (2) 切り替えに伴う一時停止で画面中央に停止マーク（▶）が出るのを
    //      抑える。実際に止まってはいるが、他の動画サービスと同じく
    //      「切り替え中」は再生中の見た目のままにするため。
    bool   m_navigateInFlight {false};
    // 移動の開始／終了。世代番号と上のフラグの操作を1箇所に集約する。
    void beginNavigation();
    void endNavigation();
    // BubbleController（＝mpv）がいま抱えているのが m_source の動画かどうか。
    // false の間は「UI上の現在の動画」と「実際に読み込まれている動画」が
    // 食い違っている＝解決待ちの最中、という意味になる。
    bool controllerHoldsCurrentSource() const;
    // 逆に「mpvが別の動画を抱えている」状態（解決待ちの最中）。この間の
    // play() は前の動画を再開させてしまうので、入口で止める。
    bool controllerHoldsStaleSource() const;
    // 切り替え中（移動先の解決・ロード待ち）かどうか。描画側が「見た目は
    // 再生中のまま」に保つために見る。
    bool isSwitchingVideo() const { return m_navigateInFlight; }
    // パネルの行に出すサムネイル（キー: サムネイルURL）。表示する数件
    // だけを必要になった時点で取りに行く（50件先読みはしない）。
    QHash<QString, QImage> m_panelThumbs;
    QSet<QString>          m_panelThumbPending;
    // 取得に失敗したサムネイルURL。毎フレーム再要求してリトライを
    // 繰り返さないための記録。
    QSet<QString>          m_panelThumbFailed;

    // リストのスクロール量（px）。ホイールで動かす。
    // m_labelPanelScrollTarget がホイールで直接動く「行き先」で、
    // 実際に描画へ使う m_labelPanelScroll がそこへ毎フレーム寄っていく
    // （1ノッチでカクッと飛ばさず、なめらかに追従させるため）。
    double m_labelPanelScroll {0.0};
    double m_labelPanelScrollTarget {0.0};
    // 直近にスクロール位置を自動調整した時点の「現在の動画」URL。
    // 現在位置が変わったときだけ、その行が見えるよう寄せ直す。
    QString m_labelPanelScrollAnchorUrl;
    // 直近に描いた「行が見えている領域」と中身全体の高さ（スクロール量の
    // 上限計算に使う）。
    QRectF m_labelPanelViewRect;
    double m_labelPanelContentHeight {0.0};
    void requestPanelThumbnail(const QString &sourceUrl, const QString &thumbUrl);
    // 描画中は要求を出さず、ここへ控えて描き終わってから出す
    // （プラグインのrequestThumbnail()が初回だけ数百ms固まることがある）。
    void queuePanelThumbnail(const QString &sourceUrl, const QString &thumbUrl);
    void processPanelThumbnailQueue();
    QList<QPair<QString, QString>> m_panelThumbQueue; // {sourceUrl, thumbUrl}
    bool m_panelThumbQueueScheduled = false;
    // 直近のpaintLabelPanel()が確定させたヒット領域。描画とクリック判定で
    // レイアウト計算を二重に持たないよう、描いた側が記録する。
    QRectF      m_labelPanelRect;
    // ヘッダ（ラベルと同じ高さの帯）の高さ。ここをクリックすると閉じる。
    double      m_labelPanelHeaderHeight {0.0};
    // 直近の（空でない）ラベル矩形。移動直後はresolveViaPlugin()が
    // ラベルを一旦クリアするため、そのままだと開いているパネルが
    // ロード中だけ消えてしまう。その間の形として使い回す。
    QRectF      m_labelRectForPanel;
    // 直近の（空でない）ラベルの中身。コントローラを開いたまま次の動画へ
    // 移ると、解決中はラベルが空になってリストごと消えてしまうため、
    // その間だけこちらを描画に使う（paintPlaybackOverlay参照）。
    QByteArray  m_labelIconSvgSticky;
    QString     m_labelTextSticky;
    // 2026-09-22: プラグインのメディアで、解決の答え（ラベル）が返ったか。
    // false の間は本体の基本ラベルも出さない（paintLabelAndPanel）。
    bool        m_pluginLabelKnown {false};
    // 本体の基本ラベルのロゴ（灰色の再生ボタン）
    static const QByteArray &coreLabelIconSvg();
    QList<QRectF> m_labelPanelRowRects;
    QStringList   m_labelPanelRowUrls;

    // 1行分の内容。paintLabelPanel()が組み立て、ヒット判定にも使う。
    struct PanelRow
    {
        QString kind;   // "PREV" / "NOW" / "NEXT"
        QString title;
        QString url;
        QString thumbUrl;
        int     durationSec = 0;
    };
    // 今のURLから前後の行を組み立てる（プラグイン＋履歴）。
    // 2026-09-12: 結果は下のキャッシュに置き、毎フレーム組み直さない。
    // 50件ぶんのリストを毎フレーム作り直すのは、内容がほぼ変わらない
    // 割に高くつく（リスト表示中だけCPUが上がる原因のひとつ）。
    QList<PanelRow> buildPanelRows() const;
    // 一覧（プラグインの順＋ユーザーの並べ替え m_panelOrderOverride）を返す。
    // 描画・クリック・前後移動はすべてこちらを使う。
    QList<PanelRow> orderedPanelRows() const;
    // 並べ替え結果のキャッシュ（毎フレームの総当たりを避ける）。
    mutable QList<PanelRow> m_orderedRowsCache;
    mutable quint32 m_orderedRowsGen {0};
    mutable quint32 m_orderedRowsRev {0};
    // 元の一覧（buildPanelRows）の世代。作り直すたびに増える。
    mutable quint32 m_panelRowsGen {0};
    // 行1つぶんの中身（ピン・サムネイル・種別/長さ・タイトル）を描く。
    // リスト本体と、並べ替えでつかんで浮いている行の両方から呼ぶ。
    void paintPanelRowContent(QPainter &painter, const QRectF &rowRect,
                              const PanelRow &row, bool isCurrent,
                              double alpha, bool requestThumbs);
    mutable QList<PanelRow> m_panelRowsCache;
    mutable QString         m_panelRowsCacheUrl;
    mutable QElapsedTimer   m_panelRowsCacheAge;
    // 省略済みタイトルの記憶（キー: 幅|原文）。elidedText()は日本語だと
    // 整形が走るため、毎フレーム・行数ぶん呼ぶと無視できない。
    mutable QHash<QString, QString> m_panelElideCache;
    // labelPanelSupported() の短時間キャッシュ。
    mutable bool          m_panelSupportCached = false;
    mutable QString       m_panelSupportUrl;
    mutable QElapsedTimer m_panelSupportAge;

    // ---- 計測（2026-09-12追加） --------------------------------------
    // コントローラの板＋行をQPainterで描くのに1フレームあたり何msかかって
    // いるかを2秒ごとに出す（[LabelPanel][stat]）。BackdropBlur側の計測と
    // 同じ考え方で、推測ではなく数字で削る場所を決めるため。
    QElapsedTimer m_panelStatWindow;
    double m_panelStatMsSum = 0.0;
    double m_panelStatMsMax = 0.0;
    int    m_panelStatFrames = 0;
    int    m_panelStatRows = 0;
    QSizeF m_panelStatPlateSize;
    // 板の形（アニメーション中の途中の形も含む）。描画結果をキャッシュする
    // ため、「描く前に形だけ知る」用に切り出してある。
    struct PlateGeometry
    {
        bool   valid = false;
        QRectF plate;
        double leftRadius = 0.0;
        double rightRadius = 0.0;
    };
    PlateGeometry labelPanelPlateGeometry(const QRectF &labelRect,
                                          int windowW, int windowH,
                                          int rowCount) const;
    // ラベルの台がカードへ伸びる板の描画（ガラスの縁だけ。中身は別）。
    void paintLabelPanelPlate(QPainter &painter, const PlateGeometry &geo, float fade);
    // 描いた板の画像を使い回せるかの判定値。見た目に影響する要素だけを混ぜる。
    quint64 labelPanelCacheKey(const QList<PanelRow> &rows,
                               const PlateGeometry &geo, bool withRows) const;
    // 2026-09-14: 上のキーを構成する値を、混ぜる前の形のまま持つ。
    // キャッシュが外れたときに「どの値が変わったせいか」を突き止めるため
    // だけのもの（[LabelPanel][stat] の miss= に出す）。実測で、リストを
    // 開いている間だけ板の描画が6〜9ms/フレームに跳ね上がり fps が30まで
    // 落ちるのを確認したが、キャッシュが毎フレーム外れているのか、貼り付け
    // そのものが重いのかが数字から分からなかったため。
    struct PanelCacheKeyParts {
        qint64  plateL = 0, plateT = 0, plateW = 0, plateH = 0;
        qint64  radL = 0, radR = 0;
        qint64  fadeStep = 0;
        quint32 accent = 0;
        int     withRows = 0;
        qint64  scroll = 0;
        quint32 orderRev = 0;
        int     dragIndex = 0;
        qint64  gapAnim = 0;
        int     dragMoved = 0;
        int     rowCount = 0;
        int     thumbs = 0;
        int     thumbFailed = 0;
        quint64 rowsHash = 0;
    };
    PanelCacheKeyParts labelPanelCacheParts(const QList<PanelRow> &rows,
                                            const PlateGeometry &geo,
                                            bool withRows) const;
    static quint64 hashPanelCacheParts(const PanelCacheKeyParts &p);
    // 板だけのキー（形・角丸・アクセント色）。スクロールや行の内容は含めない。
    static quint64 hashPlateCacheParts(const PanelCacheKeyParts &p);
    // 2026-09-14 B-4/B-5: 行の「帯」の中身のキー。スクロール量も板の位置も
    // 窓の位置も含めない（並び・サムネイル・幅・dprだけ）。窓がずれただけの
    // ときに前の帯から貼り直せるよう、窓は別に持つ。
    static quint64 hashStripContentKey(const PanelCacheKeyParts &p,
                                       int widthQ, int dprQ);
    bool ensureLabelPanelStrip(const QRectF &plateRect, const QRectF &labelRect,
                               const QList<PanelRow> &rows,
                               const PanelCacheKeyParts &parts, qreal dpr);
    void blitLabelPanelStrip(QPainter &painter, const QRectF &plateRect,
                             const QRectF &labelRect,
                             const QList<PanelRow> &rows);
    QImage  m_panelStripCache;      // 縦に長い行の帯（中身だけで決まる）
    QImage  m_panelStripScratch;    // 端をぼかすとき用。使い回す。
    quint64 m_panelStripContentKey = 0;
    int     m_panelStripFirst  = 0; // 帯の先頭の行番号
    int     m_panelStripCount  = 0; // 帯に入っている行数
    int     m_panelStripWidthQ = 0;
    int     m_panelStripDprQ   = 0;
    bool    m_panelUseStrip    = false;
    int     m_panelStatStripHits   = 0;
    int     m_panelStatStripMisses = 0;
    double  m_panelStatStripMs     = 0.0;
    // B-5: 窓をずらして貼り直した回数と、実際に描き直した行数。
    int     m_panelStatStripSlides     = 0;
    int     m_panelStatStripRedrawRows = 0;
    // 帯を貼る側（毎フレーム）の実測。ここが効いているかを見るため。
    double  m_panelStatBlitMs   = 0.0;
    int     m_panelStatBlitCalls = 0;
    // 変わった項目のビットマスク。describePanelCacheMiss() が名前へ直す。
    static quint32 diffPanelCacheParts(const PanelCacheKeyParts &a,
                                       const PanelCacheKeyParts &b);
    static QString describePanelCacheMiss(quint32 mask);
    mutable PanelCacheKeyParts m_panelKeyParts;
    // 計測用。2秒ごとの [LabelPanel][stat] に出して0へ戻す。
    int     m_panelStatHits = 0;
    int     m_panelStatMisses = 0;
    double  m_panelStatBuildMs = 0.0;
    // 2026-09-14: 1回の再構築の内訳（板 / 行）。両者を別キャッシュに分ける
    // 価値があるかを数字で決めるため。
    double  m_panelStatPlateMs = 0.0;
    double  m_panelStatRowsMs  = 0.0;
    int     m_panelStatPlateHits   = 0;
    int     m_panelStatPlateMisses = 0;
    quint32 m_panelStatMissMask = 0;
    // 2026-09-14 C-1: スクロールを進めた回数（120Hzで回っているかの確認用）。
    int     m_panelStatScrollTicks = 0;
    // 板＋行を描いた画像とその状態。毎フレーム描き直すと4〜5msかかるため、
    // 内容が変わらない間はこれを貼るだけにする（CanvasVideo.cpp参照）。
    // 2026-09-14 B-2: 板（ガラスの台）と行（スクロールする一覧）を別々の
    // 画像に分けた。板はスクロールでは変わらないので焼き直さない。
    QImage        m_panelPlateCache;
    quint64       m_panelPlateKey = 0;
    // 行の画像。中身を描いたかどうか（withRows）を覚えておき、貼るときに見る。
    bool          m_panelCacheRowsDrawn = false;
    QImage        m_panelCache;
    quint64       m_panelCacheKey = 0;
    qreal         m_panelCacheDpr = 0.0;
    QPointF       m_panelCacheOrigin;
    QList<QRectF> m_panelCacheRowRects;
    QStringList   m_panelCacheRowUrls;
    // 板の中身（サムネイル＋タイトル）。板が開き切る手前から遅れて出す。
    // paintLabelPanelRows() は行を一旦透明な画像へ描き、上下の端を
    // グラデーションで薄くしてから貼る（スパッと切れて見えないように）。
    // 実際に行を描くのは paintLabelPanelRowsBody()。
    void paintLabelPanelRows(QPainter &painter, const QRectF &plateRect,
                             const QRectF &labelRect, const QList<PanelRow> &rows);
    void paintLabelPanelRowsBody(QPainter &painter, const QRectF &plateRect,
                                 const QRectF &labelRect, const QList<PanelRow> &rows);
    // 指定URLへこのバブル上で移動する。pushHistory=trueなら移動前のURLを
    // 履歴へ積む（「戻る」で戻れるようにする）。
    void navigateToUrl(const QString &url, bool pushHistory);
    // 2026-09-22: 任意のメディア（ローカルでもプラグインでも）へ移動する。
    // navigateToUrl() の中身をこちらへ移し、プラグインのURLはここを通る。
    void navigateToSource(const MediaSource &source, bool pushHistory);
    // 本体のリストの index 番目へ移動する（範囲外なら false）。
    bool navigateToCoreListItem(int index);
    // 本体のリストの中で今再生している位置（分からなければ -1）。
    int  coreListCurrentIndex() const;
    // 本体のリストの index 番目が再生できそうか（ローカルのファイルが消えて
    // いないか）。自動送りで飛ばすかどうかに使う。
    bool coreListItemAvailable(int index) const;
    // 一覧の行のキャッシュを捨てる（本体のリストが変わったとき）。
    void invalidatePanelRows();

    MediaSource m_source;
    QString m_streamUrl;
    // DASH分離ストリーム対応。空文字列 = streamUrl単体に音声・映像が
    // 結合済み（従来通り）。
    QString m_streamAudioUrl;
    QStringList m_streamHttpHeaders;
    int m_origW {480};
    int m_origH {270};
    // ---- 動画出力FBO解像度 ----
    // m_origW/m_origH は itemRect()（バブルの見た目のサイズ）にも使われる
    // ため、常に480px幅へ正規化されている（ソース動画の実解像度とは無関係）。
    // これをそのままFBOサイズにも流用すると、720p等の高画質ストリームを
    // 選んでも内部的には常に480x270へダウンスケールされてレンダーされ、
    // 見た目には反映されない（実機で確認済みの不具合）。
    // m_renderW/m_renderHはデコード解像度そのものではなく、表示物理サイズ
    // に合わせてVideoRenderResolutionPolicyが選んだ現在のFBOサイズ。
    // バブルの見た目のサイズには一切影響させない。
    int m_renderW {480};
    int m_renderH {270};
    QSize m_nativeVideoSize;
    VideoRenderResolutionPolicy m_renderResolutionPolicy;
    // ローカル動画で実デコード解像度が判明した時点で、m_origW/m_origH
    // （itemRect()＝バブルの見た目の形状）をそのネイティブ縦横比に一度だけ
    // 追従させたか。true になった後は、ユーザーの手動リサイズ操作を
    // 上書きしないよう、paintGL()側の解像度ポーリングでは再更新しない。
    // 2026-07-13: LocalVideoだけでなく「プラグインが解像度を返さなかった
    // PluginMedia（SRT等のライブストリーム）」の縦横比一度きり適用にも
    // 同じフラグを使う（適用条件はpaintGL()の解像度ポーリング参照）。
    bool m_localVideoAspectApplied {false};

    // 直近のプラグイン解決が有効な寸法を提供したか。trueならその寸法で
    // バブル縦横比を既に確定しているため、mpvの値で形状を上書きしない。
    // FBO解像度はどちらの場合もVideoRenderResolutionPolicyが決める。
    bool m_pluginProvidedDims {false};

    bool  m_volHover {false};
    float m_volSliderAnim {0.0f};

    // 2026-07-15: 音量調整ボタンのホバー時アニメーション。
    // ボタン本体とスライダーを独立したフェーズで進める（2026-07-15b：
    // 抜ける時にボタンとスライダーを同時並行で動かすよう変更）。
    // 入り: ボタン縮小 → (縮み切ってから)スライダーが「びよーん」と伸びる。
    // 抜け: ボタンの拡大とスライダーの収縮を「同時に」即座スタートする
    //       （ボタンはすぐ弾んで元のサイズへ、スライダーは並行して
    //       びよーんと縮んで1点に戻る）。
    enum class VolButtonPhase {
        Normal,      // 定常（1.0倍）
        ShrinkingIn, // 入り: 縮小中
        Shrunk,      // ホバー中の定常（縮小済み）
        GrowingOut,  // 抜け: 拡大中（弾みながら1.0倍へ）
    };
    VolButtonPhase m_volButtonPhase {VolButtonPhase::Normal};
    double         m_volButtonPhaseElapsedMs {0.0};
    float          m_volButtonScale {1.0f};    // ボタン本体（丸+アイコン）のスケール
    float          m_volButtonGrowFrom {1.0f}; // GrowingOut開始時点のスケール（そこから1.0へ弾む）

    enum class VolSliderPhase {
        Hidden,         // 定常（非表示）
        GrowingIn,      // 入り: 伸長中（ボタンが縮み切ってから開始）
        Shown,          // ホバー中の定常（全伸長）
        CollapsingOut,  // 抜け: 収縮中
    };
    VolSliderPhase m_volSliderPhase {VolSliderPhase::Hidden};
    double         m_volSliderPhaseElapsedMs {0.0};

    // 音量スライダー上端の白いドラッグハンドル。抜ける時はスライダー本体の
    // 収縮を待たず、単独ですばやくフェードアウトする。
    float m_volHandleAlpha {0.0f};

    // シークバーの進捗ハンドル（白丸）表示用。ホバー/ドラッグ中のみ
    // フェードで表示する。
    bool  m_seekHover {false};
    float m_seekHandleAnim {0.0f};

    // 時刻表示の台のホバー（2026-09-13追加）。切り替えがロックされている
    // ときは反応させないので、行き先の計算はここに一本化する。
    bool  m_timeHover {false};
    float m_timeHoverAnim {0.0f};
    float timePadHoverTarget() const {
        return (m_timeHover && !m_seekTimeModeLocked) ? 1.0f : 0.0f;
    }

    // ---- 再生バーUIの出現/消失アニメーション ----
    // シークバー本体・時刻・再生停止ボタン・音量アイコンいずれの表示/非表示
    // にも使う共通フェード係数。updatePlaybackBarAnim()が毎フレーム
    // targetVisible(0 or 1)へ指数的に近づける（バウンドしない、単純フェード）。
    float  m_playbarFadeAlpha {1.0f};
    // 再生停止ボタン・音量アイコンのポップスケール。通常は1.0固定
    // （＝フェードのみで表示/非表示）。triggerPlaybarActivationPop()が
    // 呼ばれた直後だけ、OutBackイージングで0→1(一瞬1超え)へ弾ませる。
    // 音量アイコンの方を数フレーム先に、再生停止ボタンはその後に遅れて
    // 弾ませる（島のボタン出現ディレイと同じ演出）。
    float  m_playbarPopScaleVol {1.0f};
    float  m_playbarPopScalePlay {1.0f};
    bool   m_playbarActivationPopActive {false};
    double m_playbarActivationPopElapsedMs {0.0};

    int            m_currentQuality {0};

    GLuint m_oldTexId  {0};
    float  m_fadeAlpha {1.0f};
    // paintGL()で最終的に表示した動画/サムネイルのテクスチャ。
    // Qualityガラスの色反射は画面キャプチャせず、この参照を直接読む。
    GlassContentSource m_glassContentSource;
    // 2026-09-22: 本体のリスト（hasCoreList()）。並び全体と、今の位置の手がかり。
    // 位置は同じURLが2回並んでいても区別できるよう番号で持ち、念のため
    // URLが一致するかも確かめる（coreListCurrentIndex）。
    QList<MediaSource> m_coreList;
    int                m_coreListIndex {-1};

    // ---- アンビエント発光の状態（説明は上のメソッド群のコメント参照）----
    // 目標色（採取した平均色）と、実際に描いている色。0〜255の実数で持つ
    // （整数だと時定数の効いたゆっくりな変化が段々に見えるため）。
    struct AmbientChannel { double r {0.0}, g {0.0}, b {0.0}; };
    AmbientChannel m_ambientTarget[AmbientSideCount];
    AmbientChannel m_ambientCur[AmbientSideCount];
    double m_ambientBrightness {0.0};
    double m_ambientBrightnessTarget {0.0};
    QColor m_contentAvgColor;   // 2026-10-13: 最後に採れた中身の平均色（グミの影用）
    // 濃さ。鳴っていない間は0へ落ちる。
    double m_ambientAlpha {0.0};
    bool   m_ambientHasColor {false};
    mutable QElapsedTimer m_ambientSampleClock;
    QMetaObject::Connection m_seekConn;

    QString  m_thumbnailUrl;
    QImage   m_thumbnailPending;
    QImage   m_thumbnailImage;
    QSize    m_thumbnailPixelSize;
    // thumbnailPngBase64ForSession()用のキャッシュ（詳細はヘッダ上部の
    // 宣言コメント参照）。mutable: constなgetterから遅延更新するため。
    mutable QString m_cachedThumbnailPngBase64;
    mutable qint64  m_cachedThumbnailPngBase64Key {-1};
    GLuint   m_thumbTexId   {0};
    int      m_thumbW       {0};
    int      m_thumbH       {0};
    bool     m_thumbReady   {false};
    enum class ThumbnailResidency
    {
        Cold,
        Loading,
        CpuReady,
        GpuResident,
        Failed,
    };
    ThumbnailResidency m_thumbnailResidency {ThumbnailResidency::Cold};
    quint64 m_thumbnailRequestId {0};
    qint64 m_thumbnailFarSinceMs {-1};
    qint64 m_thumbnailRetryAfterMs {0};
    bool m_thumbnailGpuReleasePending {false};
    bool m_thumbnailUploadAllowedForFrame {true};
    // Old sessions may carry an embedded image. Keep it only as a migration
    // fallback until the provider has successfully populated its own cache.
    bool m_thumbnailProviderFallbackActive {false};
    mutable qint64 m_thumbnailProviderCheckMs {0};
    mutable bool m_thumbnailProviderAvailable {false};

    // ---- 音声Bubbleタイトルのごまかしフェード（ぽよん/ドラッグ/リサイズ中は非表示）----
    bool     m_titleWasLifted   {false};
    qint64   m_titleFadeStartMs {0};

    // サムネイルのダウンロード・デコードは ThumbnailLoader（Phase 7 Step 1で
    // 分離）が担当する。生成は loadThumbnail() 初回呼び出し時の遅延生成
    // （旧 m_nam と同じタイミング）。
    ThumbnailLoader *m_thumbnailLoader {nullptr};
    bool     m_deferredLoad {false};
    bool     m_deferredFetchInProgress {false};
    bool     m_playAfterDeferredLoad {false};
    qint64   m_resumePositionMs {0};
    // startPlayback()がmpvへ実際にロードを試みるたびに退避しておく
    // 「再開しようとしていた位置」。m_resumePositionMsは楽観的に消費される
    // （ロードの成否を待たずにこの時点で0に戻される）ため、ロードが失敗
    // した場合に備えて別途保持しておき、retryAfterError()が失敗直前の
    // 再開位置を割り出すのに使う。
    qint64   m_lastAttemptedResumeMs {0};

    // 2026-07-18追加: Error状態への自動復帰リトライ。
    // これまでretryAfterError()（署名付きURL失効等に備えたプラグイン
    // 再解決＋直前位置からの再開）はバブルをクリックした時にしか
    // 呼ばれておらず、配信側の一過性の403（CDNチャンク取得失敗）等で
    // Errorに落ちると、ユーザーが気付いてクリックし直すまでバブルが
    // 固まったままになっていた（実際に報告された不具合）。
    // Error状態に入った際、少し待ってから自動でretryAfterError()相当を
    // 試みる。ただし本当に再生不可能な動画（削除済み・地域制限等）まで
    // 無限リトライしないよう、kMaxAutoRetryOnError回で打ち切る。
    // 再生が実際に始まれば（Playing到達）0へリセットする
    // （stateChangedのラムダ・onAutoRetryErrorTimeout()参照）。
    static constexpr int kMaxAutoRetryOnError = 2;
    int m_autoRetryOnErrorCount = 0;

    // ---- 音量 ----
    float m_userVolume {1.0f};
    // ---- マスターボリューム ----
    float m_masterGain {1.0f};

    // ---- Phase 1: BubbleController ----
    BubbleController m_controller;

    // ---- Phase 2: MpvPlayer ----
    MpvPlayer *m_mpvPlayer  = nullptr;
    std::atomic<bool> m_mpvFrameDirty {false};
    bool m_audioOnlyDrawLogOnce {false};
    bool m_mpvInitializationAttempted {false};
    bool m_startPlaybackWhenMpvReady {false};
    // 2026-07-15: mpvのGL初期化失敗でError状態に落ちた場合のみtrue。
    // retryAfterError()で「initializeMpv()からやり直す」専用分岐に使う
    // （このケースはIPbPlayerが一度もBubbleControllerへ渡っていないため、
    // 通常のBubbleController::retry()は使えない）。
    bool m_mpvGlInitFailedPendingRetry {false};
    QElapsedTimer m_hiddenPerfLogTimer;
    BubbleState m_hiddenPerfLogState {BubbleState::Idle};
    int m_hiddenRenderToFboCount {0};
    int m_hiddenFrameUpdatedCount {0};
    int m_focusSleepPaintDebugFrames {0};

    // Sleep（完全休止）関連の保留状態・消費処理（Phase 8 Step 1でデータを、
    // Step 4で消費処理本体も VideoSleepCoordinator.h の
    // VideoSleepCoordinator クラスへ統合）。
    //
    // glRelease: 次回paintGL()でmpv側FBO/テクスチャを解放する
    // ためのフラグ。GLコンテキストがcurrentである保証があるpaintGL()内でのみ
    // 実際の解放処理を行う（他のGL解放処理と同じパターン）。
    // 注意: FocusSleepには適用しない。md/bug.mdに記録されている
    // 「FocusSleep解除後にサムネイル固定になる」既知の不具合調査で、
    // まさにFocusSleep突入時のFBO解放が疑わしい実装として試され、
    // 解消しなかった経緯があるため、FocusSleepのFBOは温存する設計を維持する。
    //
    // 2026-07-03: メモリ節約 — Sleep突入でMpvPlayerオブジェクト自体を
    // 丸ごと解放し、復帰時に作り直すためのフラグ。両方ともGLコンテキスト
    // current保証があるpaintGL()内でのみ消費する（glReleaseと同じパターン）。
    //   mpvPlayerTeardown : Sleep突入時にセット→delete m_mpvPlayer
    //   mpvRecreateForWake: BubbleController::playerNeeded()受信時に
    //                        セット→initializeMpv()して作り直し、
    //                        完了したらwakeUp()を再試行する
    VideoSleepCoordinator m_sleepPending;

    // If focus sleep starts before playback opens, suppress auto-play later.
    bool m_focusSleepPending {false};

private:
    // サムネイルをアスペクト比を保った中央クロップ（cover）でscreenRectへ
    // 描く（2026-07-11: 16:9でないメディアのサムネ歪み修正。CanvasVideo.cpp参照）。
    void drawThumbnailCover(Renderer &r, const QRectF &screenRect,
                            float radius, float alpha = 1.0f);

    // 2026-07-08: loadThumbnail()/loadLocalThumbnail()共通の遅延生成・
    // signal接続処理。取得元（ネットワークDL or Windows Shell）が違うだけで
    // 結果の受け取り方（thumbnailReady() → m_thumbnailImage反映）は
    // 完全に共通なため、ここへまとめた。
    void ensureThumbnailLoader();
    void requestThumbnailFromProvider();
    void cancelThumbnailProviderRequest();
    void restoreProviderFallbackAsync(qint64 retryAfterMs);
    void applyPendingThumbnailGpuRelease(Renderer &r);

    // 2026-07-11 🚫（D&D禁止固定）バグ修正: GLコンテキストがcurrentでない
    // 文脈（FlowMode::removeItem等のマウスイベント/タイマー経由）で
    // MpvPlayerを破棄したい場合、直接deleteせずここへ積む。~MpvPlayer()の
    // mpv_render_context_free()はGL currentを要求するため（ゴミ箱2回🚫
    // バグと同根）、実際の破棄は次のpaintGL()冒頭（GL current保証あり）で
    // 行う（drainPlayersAwaitingGlDelete()）。
    QList<MpvPlayer*> m_playersAwaitingGlDelete;
    void queuePlayerForGlDelete(MpvPlayer *player);
    void drainPlayersAwaitingGlDelete();
};
