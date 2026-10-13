// ============================================================================
// 2026-07-12: 【新規ロジック追加時の注意】
// CanvasWidget.cppは既に肥大化しすぎている（chordbrushup.md参照。
// CanvasWidget/CanvasVideoの2大巨大ファイルを単機能クラス/ファイルへ
// 段階的に切り出す進行中のリファクタ計画がある）。
// 今後、新規機能やまとまったロジック（例: Web画像URL判定を
// WebImageUrlResolver.h/cppへ、キャッシュ管理をWebImageCache.h/cppへ
// 切り出したのと同様のもの）を追加する際は、極力このファイルへ
// 直接書き足さず、責務が明確な単位で別のcpp/hに新規作成すること。
// CanvasWidget.cpp側は「入力（ドロップ/クリック/ホイール等）を受けて
// 別ファイルの機能を呼び出す薄い接続コード」に留めるのが望ましい。
// ============================================================================

#include "CanvasWidget.h"
#include "CanvasDropHandler.h"
#include "BubbleDragExporter.h"
#include "CanvasSelectionHelper.h"
#include "FocusSessionController.h"
#include "CanvasImage.h"
#include "CanvasLauncher.h"
#include "WebLinkIcon.h"
#include "TrashZone.h"
#include "MainWindow.h"
#include "DynamicIsland.h"
#include <QGraphicsOpacityEffect>
#include "AppSettings.h"
#include "WebImageCache.h"
#include "WebImageUrlResolver.h"
#include "WebVideoCache.h"
#include "CameraPanInertia.h"
#include "CameraZoomEase.h"
#include "Blur.h"
#include <QOpenGLContext>
#include "plugin/PluginManager.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainterPath>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QDebug>
#include <QEasingCurve>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QMetaMethod>
#include <QContextMenuEvent>
#include <QTimer>
#include <QElapsedTimer>
#include <QDateTime>
#include <QKeyEvent>
#include <QPointer>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonObject>
#include <QFileInfo>
#include <QFile>
#include <QBuffer>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVariant>
#include <QVector>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QPainter>
#include <QGuiApplication>
#include <QClipboard>
#include <QCollator>
#include <QCursor>
#include <QRegion>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
#include "engine/VideoRenderThread.h" // 2026-09-15 非同期化 P1
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#include <ole2.h>
#endif

namespace {
// ---- アンビエント発光の見た目（2026-09-13）----
// 濃さそのものは CanvasVideo.cpp の kAmbientPeakAlpha が決める。ここに
// あるのは「その濃さを3種類へどう配分するか」の比率。
// 裾（広く淡い光）。1枚だけだと色の付いた影に見えるので控えめに。
constexpr double kAmbientHaloAlphaScale  = 0.55;
// 辺からの漏れ。ここがいちばん「光っている」感に効く。
constexpr double kAmbientEdgeAlphaScale  = 1.25;
// 十字の筋。強すぎると安っぽいので薄く、長く。
constexpr double kAmbientStreakAlphaScale = 0.45;
// 筋が出始める明るさ（0〜1）。暗い映像では出さない。
constexpr double kAmbientStreakThreshold = 0.45;
// 筋の長さ（バブルの短辺に対する倍率）。
constexpr double kAmbientStreakReach     = 1.15;

// ホイール中クリックのダブルクリックによる原寸リセット時の「強い枠グロー」演出
// パラメータ（triggerStrongBorderGlow()/paintStrongBorderGlow()参照）。
// 発生直後にkStrongBorderGlowMaxAlphaまで一気に発光→保持→フェードアウトの
// 合計時間がkStrongBorderGlowDurationMs、そのうち保持に使う割合が
// kStrongBorderGlowHoldFrac（残りがフェードアウトに使われる）。
constexpr int    kStrongBorderGlowDurationMs = 900;   // 保持500ms + フェード約400ms
constexpr double kStrongBorderGlowHoldFrac   = 500.0 / kStrongBorderGlowDurationMs;
constexpr double kStrongBorderGlowMaxAlpha   = 220.0; // 通常グロー上限(120)より明確に強く

// ウィンドウ枠の角丸半径。通常のカーソル追従グロー線(paintBorderOverlay)と
// 強グロー(paintStrongBorderGlow)の両方で共通して使う。片方だけ変えると
// 常時表示の枠と一瞬だけ光る枠とで角の丸みが食い違って見えるため、
// 角丸半径を変更する場合は必ずここを直す。
//
// MainWindow::applyRoundedCorners()で設定しているWindows DWMの
// DWMWA_WINDOW_CORNER_PREFERENCE(=DWMWCP_ROUND)が実際に適用する角丸半径を
// アプリ側から取得する公開APIは存在しない（プリセットを指定するだけで、
// 適用結果のpx値を読み返す手段がない）。ここではWindows 11の
// DWMWCP_ROUNDで一般的に観測される値（100%スケール時で約8px）に合わせて
// 8.0としている。実機のズレが気になる場合はこの値だけを調整すればよい。
constexpr double kWindowCornerRadius = 8.0;

// Focus操作フィードバックはCanvasOverlayWidgetのQPainter経路で使うため、
// SVGをQImageへ一度だけラスタライズする。黒半透明ピル＋白文字まで完結した
// デザインなのでティントはせず、そのままの色で描く。
const QImage &speedBadgeImage()
{
    static const QImage s_image = [] {
        static QSvgRenderer renderer(QStringLiteral(":/icons/ico_x2.svg"));
        const QSize size(140, 95); // viewBox比(27.961:18.878)を保った拡大サイズ
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        renderer.render(&p);
        p.end();
        return img;
    }();
    return s_image;
}

const QImage &seekIndicatorImage(bool flipped)
{
    static const QImage s_back = [] {
        static QSvgRenderer renderer(QStringLiteral(":/icons/back.svg"));
        const QSize size(96, 96); // back.svgのviewBoxは正方形(32x32)
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        renderer.render(&p);
        p.end();
        return img;
    }();
    static const QImage s_forward = s_back.flipped(Qt::Horizontal);
    return flipped ? s_forward : s_back;
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

QString normalizedHttpUrl(const QString &raw)
{
    QString text = raw.trimmed();
    while (!text.isEmpty() && QStringLiteral(".,)]}>\"'").contains(text.back()))
        text.chop(1);

    QUrl url(text);
    if (!url.isValid() || url.scheme().isEmpty())
        return {};

    const QString scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
        return {};

    return url.toString();
}

QString firstHttpUrlInText(const QString &text)
{
    const QRegularExpression re(QStringLiteral(R"((https?://[^\s<>"']+))"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(text);
    if (!match.hasMatch())
        return normalizedHttpUrl(text);
    return normalizedHttpUrl(match.captured(1));
}

QStringList httpUrlsInText(const QString &text)
{
    QStringList urls;
    const QRegularExpression re(QStringLiteral(R"(https?://)"),
                                QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    QList<int> starts;
    while (it.hasNext())
        starts << it.next().capturedStart();

    for (int i = 0; i < starts.size(); ++i) {
        const int start = starts.at(i);
        int end = (i + 1 < starts.size()) ? starts.at(i + 1) : text.size();
        for (int j = start; j < end; ++j) {
            if (text.at(j).isSpace() || QStringLiteral("<>\"'").contains(text.at(j))) {
                end = j;
                break;
            }
        }
        const QString url = normalizedHttpUrl(text.mid(start, end - start));
        if (!url.isEmpty())
            urls << url;
    }
    if (urls.isEmpty()) {
        const QString url = normalizedHttpUrl(text);
        if (!url.isEmpty())
            urls << url;
    }
    return urls;
}

// 2026-07-12: 直接画像URL判定（isLikelyImageUrl/resolveDirectImageUrl）は
// WebImageUrlResolver.h/cppへ切り出し済み。サイトごとの特殊ケースが増える
// たびにここが肥大化していたため（Discord/Google画像検索/Twitter等）、
// CanvasWidget.cpp本体からは分離してある。

bool isKnownVideoPageUrl(const QString &rawUrl)
{
    Q_UNUSED(rawUrl);
    return false;
}

bool isLikelyLocalVideoFile(const QString &path)
{
    static const QSet<QString> videoExts = {
        QStringLiteral("mp4"),
        QStringLiteral("m4v"),
        QStringLiteral("mov"),
        QStringLiteral("mkv"),
        QStringLiteral("webm"),
        QStringLiteral("avi"),
        QStringLiteral("wmv"),
        QStringLiteral("flv"),
        QStringLiteral("mpeg"),
        QStringLiteral("mpg"),
        QStringLiteral("ts"),
        QStringLiteral("m2ts"),
        QStringLiteral("3gp")
    };
    const QFileInfo info(path);
    return info.exists() && info.isFile() && videoExts.contains(info.suffix().toLower());
}

bool isLikelyLocalAudioFile(const QString &path)
{
    static const QSet<QString> audioExts = {
        QStringLiteral("mp3"),
        QStringLiteral("wav"),
        QStringLiteral("flac"),
        QStringLiteral("m4a"),
        QStringLiteral("ogg")
    };
    const QFileInfo info(path);
    return info.exists() && info.isFile() && audioExts.contains(info.suffix().toLower());
}

bool isLikelyLocalImageFile(const QString &path)
{
    static const QSet<QString> imageExts = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("gif"),
        QStringLiteral("webp")
    };
    const QFileInfo info(path);
    return info.exists() && info.isFile() && imageExts.contains(info.suffix().toLower());
}

MediaSource mediaSourceFromLocalFile(const QString &path, MediaKind kind)
{
    MediaSource source;
    source.kind = kind;
    source.originalPathOrUrl = QUrl::fromLocalFile(path).toString();
    source.title = QFileInfo(path).fileName();
    return source;
}

MediaSource mediaSourceFromWebImageUrl(const QString &url)
{
    MediaSource source;
    source.kind = MediaKind::WebImage;
    source.originalPathOrUrl = url;
    source.title = QFileInfo(QUrl(url).path()).fileName();
    return source;
}

// 2026-10-12: Web動画（動画ファイルの直リンク）。タイトルはURLのファイル名。
MediaSource mediaSourceFromWebVideoUrl(const QString &url)
{
    MediaSource source;
    source.kind = MediaKind::WebVideo;
    source.originalPathOrUrl = url;
    source.title = WebVideoCache::titleForUrl(url);
    return source;
}

bool isLocalFileUrlString(const QString &url)
{
    return QUrl(url).isLocalFile();
}

QString firstHttpUrlFromMime(const QMimeData *mime)
{
    if (!mime)
        return {};

    QStringList urls;
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls())
            urls << normalizedHttpUrl(url.toString());
    }

    if (mime->hasText())
        urls << httpUrlsInText(mime->text());

    urls.removeAll(QString());
    urls.removeDuplicates();

    for (const QString &url : urls) {
        const QString resolved = WebImageUrlResolver::resolveDirectImageUrl(url);
        if (!resolved.isEmpty())
            return resolved;
    }

    return {};
}

QPixmap pixmapFromImageBytes(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};

    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull())
        return {};

    return QPixmap::fromImage(image);
}

}

// ---- 2026-09-23 一時計測: 「バブルを動かした後のズームが30fpsっぽい」の切り分け ----
// ホイールズームのイージング中だけ、ズームtick・paintGL・frameSwappedの回数と
// 間隔を数え、イージング終了時に1行で出す。原因が分かったら削除する。
namespace {
struct ZoomDiag {
    bool on = false;
    QElapsedTimer clock;
    int ticks = 0, paints = 0, cameraPaints = 0, otherPaints = 0, swaps = 0;
    qint64 lastTick = -1, lastPaint = -1, lastSwap = -1;
    qint64 maxTickGap = 0, maxPaintGap = 0, maxSwapGap = 0;
    int swapGapsOver20 = 0;
    QString state;
    void start(const QString &st) {
        *this = ZoomDiag();
        on = true;
        state = st;
        clock.start();
    }
    static void gap(qint64 now, qint64 &last, qint64 &maxGap) {
        if (last >= 0) maxGap = qMax(maxGap, now - last);
        last = now;
    }
    void finish(const char *why) {
        if (!on) return;
        const double sec = qMax(0.001, clock.elapsed() / 1000.0);
        qDebug().noquote() << QStringLiteral(
            "[ZoomDiag] end=%1 dur=%2ms ticks=%3(%4/s maxGap=%5) paints=%6(%7/s cam=%8 other=%9 maxGap=%10) "
            "swaps=%11(%12/s maxGap=%13 over20ms=%14) | %15")
            .arg(QString::fromLatin1(why)).arg(clock.elapsed())
            .arg(ticks).arg(ticks / sec, 0, 'f', 1).arg(maxTickGap)
            .arg(paints).arg(paints / sec, 0, 'f', 1).arg(cameraPaints).arg(otherPaints).arg(maxPaintGap)
            .arg(swaps).arg(swaps / sec, 0, 'f', 1).arg(maxSwapGap).arg(swapGapsOver20)
            .arg(state);
        on = false;
    }
};
ZoomDiag g_zoomDiag;
} // namespace

CanvasWidget::CanvasWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    // UI(QWidget)の背後をぼかす機構(Blur.h)。ぼかす材料はこのGLパスの
    // 描画結果なので、ブラー対象の形が変わったらキャンバスを描き直す。
    BackdropBlur::instance().setRepaintRequester(
        [canvas = QPointer<CanvasWidget>(this)](QWidget *owner, const QRectF &ownerRect) {
            // BackdropBlurはシングルトンでウィジェットより長生きするため、
            // 生ポインタで捕まえない。
            if (!canvas)
                return;
            canvas->requestBlurRectRender(owner, ownerRect);
        });
    auto &cfg = AppSettings::instance();
    m_showGrid = cfg.showGrid();
    m_gridSize = cfg.gridSize();
    // 2026-09-17: 元に戻せる回数を減らしたら、はみ出た古い履歴をすぐ捨てる。
    connect(&cfg, &AppSettings::changed, this, [this]() {
        trimUndoHistory();
        pruneUndoBlobs();
    });

    setAcceptDrops(true);
    setMouseTracking(true);
    // 2026-07-17: CanvasWidgetは元々NoFocus(デフォルト)のままで、クリック
    // してもQtのキーボードフォーカスを一切要求していなかった。そのため
    // Island設定パネルのテキスト入力(QLineEdit、ClickFocus)を一度でも
    // クリックすると、パネルを閉じてもキーボードフォーカスがそこに残り
    // 続け、以後バブルをクリックしても矢印キー/SpaceがCanvasWidgetまで
    // 届かずQLineEdit側にカーソル移動として食われてしまっていた
    // （5秒シークが「たまに効かない」の実体）。StrongFocusにして、
    // クリック時にフォーカスを奪い返せるようにする。
    setFocusPolicy(Qt::StrongFocus);
    updateCanvasFromCamera();

    // ウィンドウ全画面アニメーション中のキャンバス内容フェード用
    m_contentOpacityAnim = new QVariantAnimation(this);
    connect(m_contentOpacityAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        m_contentOpacity = v.toDouble();
        requestFullRender(CanvasRenderReason::UiAnimation);
        // CanvasOverlayWidget is a separate QWidget, so the OpenGL canvas
        // repaint above does not update it. Keep the window-space playback UI
        // on the same fullscreen transition opacity as the canvas contents.
        emit playbackOverlayUpdateRequested();
        emit focusOverlayUpdateRequested();
        emit borderOverlayUpdateRequested();
        emit launcherTooltipOverlayUpdateRequested();
    });

    m_singleClickTimer = new QTimer(this);
    m_singleClickTimer->setSingleShot(true);
    m_singleClickTimer->setInterval(280);
    connect(m_singleClickTimer, &QTimer::timeout, this, [this](){
        executeSingleClick(m_pendingClickItem, m_pendingClickPos);
        m_pendingClickItem = nullptr;
    });

    // 2026-09-20: 逆輸入（バブルをPB外へ書き出すドラッグ）の視覚ヒント。
    // Shiftを押している間／ダブルクリックの2回目を押したままの間、
    // FlowModeと同じ要領で背景を暗くして「今掴むと外へ出せる」ことを示す。
    // Shiftはウィンドウが非アクティブでも効かせたいので、キーイベントではなく
    // 短い間隔で修飾キーの状態を見に行く。
    // 【描画コスト】暗転はバブルの下に敷く。バブルは半透明のガラスで下地を
    // 透かす／屈折させるため、下地の濃さが変わればバブルの見た目も変わり、
    // シーン全体の描き直しが要る（上に別レイヤーを重ねる方式ではバブルまで
    // 暗くなる）。そこで描き直すのはフェード中だけに絞る：
    //   ・フェード中 … 30Hz・200ms（約6フレーム）
    //   ・押している間／離している間 … 0Hz（ポーリングは状態を見るだけ）
    //   ・パン中 … もともと毎フレーム描いているので矩形1枚ぶんが乗るだけ
    m_exportHintFadeTimer = new QTimer(this);
    m_exportHintFadeTimer->setInterval(33);   // 30Hz
    connect(m_exportHintFadeTimer, &QTimer::timeout, this, [this](){
        constexpr double kFadeMs = 200.0;
        const double t = qMin(1.0, m_exportHintFadeClock.elapsed() / kFadeMs);
        const double eased = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);   // OutCubic
        const float target = m_exportHintTarget ? 1.0f : 0.0f;
        const float next = t >= 1.0
            ? target
            : m_exportHintFadeFrom + float((target - m_exportHintFadeFrom) * eased);
        if (next != m_exportHintAlpha) {
            m_exportHintAlpha = next;
            requestFullRender(CanvasRenderReason::Scene);
        }
        if (t >= 1.0)
            m_exportHintFadeTimer->stop();   // フェード完了＝以後0Hz
    });
    m_exportHintPollTimer = new QTimer(this);
    m_exportHintPollTimer->setInterval(40);
    connect(m_exportHintPollTimer, &QTimer::timeout, this, [this](){
        // 離したイベントを取りこぼした「ダブルクリック2回目押下中」の後始末。
        if (m_activeMode == QStringLiteral("double_press")
            && !(QGuiApplication::mouseButtons() & Qt::LeftButton))
            m_activeMode.clear();
        updateExportHint();
    });
    m_exportHintPollTimer->start();

    // 右クリック用（バブルをアクティブ化する専用ボタン）のダブルクリック
    // 判定タイマー。左と違い「一定時間後に何かを実行する」役目は無く、
    // isActive()で「直前に同じアイテムを右クリックしたばかりか」を
    // 判定するためだけに使う（アクティブ化自体は押した瞬間に即実行するため、
    // タイムアウト時に実行すべき処理が無い）。
    m_rightClickTimer = new QTimer(this);
    m_rightClickTimer->setSingleShot(true);
    m_rightClickTimer->setInterval(350); // 2026-09-17: 離したとき判定にしたので少し長め

    // 動画全画面中の「長押しで2倍速」用。この時間だけ押しっぱなしにしたら
    // 長押し確定として扱う。
    // 重要: m_singleClickTimer（280ms、押した瞬間からの経過時間で発火し
    // 再生/一時停止トグルを実行する）より必ず短い時間にすること。
    // 以前は350msにしていたため、280ms時点で先にシングルクリック側の
    // トグルが実行されてしまい、その後で長押しが確定して2倍速も重ねて
    // かかる（再生停止トグルと2倍速が同時に起きる）不具合が起きていた。
    m_longPressTimer = new QTimer(this);
    m_longPressTimer->setSingleShot(true);
    m_longPressTimer->setInterval(220);
    connect(m_longPressTimer, &QTimer::timeout, this, [this](){
        if (!m_longPressVideo) return;
        // 長押しが確定したので、通常のシングルクリック（再生/一時停止トグル）
        // は起きないようキャンセルする。
        m_singleClickTimer->stop();
        m_pendingClickItem = nullptr;
        setLongPressActive(true);
        m_longPressVideo->setPlaybackSpeed(2.0);
        emit focusOverlayUpdateRequested();
    });

    // 2026-07-12: フォーカス中ドラッグパン（focus_image_pan）の慣性。
    // 速度サンプリング・摩擦減衰・tickループの実体はCameraPanInertiaへ
    // 切り出してある。ここでは「対象が消えた／他アニメが割り込んできたら
    // 中断する」「壁クランプする」というfocus_image_pan固有の判断だけを
    // tickCallbackとして渡している。
    m_focusPanInertia = new CameraPanInertia(this);
    m_focusPanInertia->setTickCallback([this](const QPointF &delta) -> bool {
        CanvasItem *focusTarget = m_focusedItem ? m_focusedItem : m_pendingFocusItem;
        if (!focusTarget || dynamic_cast<CanvasLauncher*>(focusTarget) || isCameraFocusAnimationRunning())
            return false;
        m_cameraPos += delta;
        clampCameraToFocusedImage(focusTarget->itemRect());
        updateCanvasFromCamera();
        requestFullRender(CanvasRenderReason::Camera);
        return true;
    });

    // 2026-07-12: 通常キャンバスパン（camera_pan）の慣性。フォーカス版と違い
    // 壁クランプが無いので、tickCallbackはカメラを進めて再描画するだけで
    // よい。ただし「フォーカスに入った／入ろうとしている最中」は
    // focusItem()側でstop()を呼ぶのが基本だが、念のためtick側でも
    // フォーカス状態になっていたら継続しないようにしてある（保険）。
    m_canvasPanInertia = new CameraPanInertia(this);
    m_canvasPanInertia->setTickCallback([this](const QPointF &delta) -> bool {
        if (m_focusedItem || m_pendingFocusItem || isCameraFocusAnimationRunning())
            return false;
        m_cameraPos += delta;
        updateCanvasFromCamera();
        requestFullRender(CanvasRenderReason::Camera);
        return true;
    });

    // 2026-07-17: ホイールズームのなめらか化。handleWheel()側は目標ズーム値
    // (m_targetCameraZoom)を更新するだけにし、実際にm_cameraZoomを毎フレーム
    // 目標へ近づける処理はここのtickCallbackにまとめる。カーソル位置の下の
    // キャンバス座標を画面上で固定したまま拡大縮小するため、handleWheel()が
    // 記録したm_zoomAnchorScreenPos/CanvasPosを毎フレーム使い回す
    // （通常のドラッグパン慣性と違い「対象が消えたら中断」という判断は不要。
    // ズーム自体はフォーカス対象の有無に関わらず常に安全に適用できるため）。
    m_zoomEase = new CameraZoomEase(this);
    // 2026-09-23: ズームのイージングは表示（vsync）ごとに進める（CameraZoomEase::stepFromVsync）
    connect(this, &QOpenGLWidget::frameSwapped, this, [this]() {
        if (m_zoomEase)
            m_zoomEase->stepFromVsync();
    });
    connect(this, &QOpenGLWidget::frameSwapped, this, []() {
        if (!g_zoomDiag.on) return;
        const qint64 now = g_zoomDiag.clock.elapsed();
        if (g_zoomDiag.lastSwap >= 0 && now - g_zoomDiag.lastSwap > 20)
            ++g_zoomDiag.swapGapsOver20;
        ZoomDiag::gap(now, g_zoomDiag.lastSwap, g_zoomDiag.maxSwapGap);
        ++g_zoomDiag.swaps;
    });
    m_zoomEase->setTickCallback([this](double zoom) -> bool {
        if (!g_zoomDiag.on) {
            auto *av = canvasItemAsVideo(m_activeItem);
            int playing = 0;
            for (CanvasItem *it : std::as_const(m_items))
                if (auto *v = canvasItemAsVideo(it); v && v->isPlaying()) ++playing;
            g_zoomDiag.start(QStringLiteral(
                "active=%1(%2) selected=%3 mode='%4' glassBubbleActive=%5 refracting=%6 "
                "pulse=%7 lift=%8 fusionFrames=%9 playing=%10 items=%11")
                .arg(m_activeItem ? QStringLiteral("yes") : QStringLiteral("no"))
                .arg(av ? (av->isPlaying() ? QStringLiteral("playing") : QStringLiteral("stopped"))
                        : QStringLiteral("-"))
                .arg(m_selectedItems.size())
                .arg(m_activeMode)
                .arg(m_glassBubbleActive ? 1 : 0)
                .arg(m_glassRefractingItem ? 1 : 0)
                .arg(m_pulseAnim && m_pulseAnim->state() == QAbstractAnimation::Running ? 1 : 0)
                .arg(m_liftAnim && m_liftAnim->state() == QAbstractAnimation::Running ? 1 : 0)
                .arg(m_fusion.needsFrames() ? 1 : 0)
                .arg(playing)
                .arg(m_items.size()));
        }
        ZoomDiag::gap(g_zoomDiag.clock.elapsed(), g_zoomDiag.lastTick, g_zoomDiag.maxTickGap);
        ++g_zoomDiag.ticks;
        if (std::abs(zoom - m_targetCameraZoom) < 1e-9)
            QTimer::singleShot(100, this, []() { g_zoomDiag.finish("target"); });
        if (isCameraFocusAnimationRunning()) {
            g_zoomDiag.finish("focusAnim");
            return false;
        }
        m_cameraZoom = zoom;
        updateCanvasFromCamera();
        m_cameraPos -= (m_zoomAnchorScreenPos - canvasToScreen(m_zoomAnchorCanvasPos)) / m_cameraZoom;
        if (m_zoomAnchorHasClampRect)
            clampCameraToFocusedImage(m_zoomAnchorClampRect);
        updateCanvasFromCamera();
        // 2026-07-17: handleWheel()側でドラッグ中（camera_pan/focus_image_pan）
        // はイージングを使わず即時反映にしたため、通常はこのifへは来ない
        // はず（ドラッグ開始時にもm_zoomEase->stop()している）。それでも
        // 何らかの経路でイージング中にドラッグ状態へ入った場合の保険として、
        // 基準点だけは毎フレーム取り直しておく。
        if (m_activeMode==QStringLiteral("focus_image_pan")) {
            m_dragStartPos = m_zoomAnchorScreenPos;
            m_startCameraPos = m_cameraPos;
        }
        requestFullRender(CanvasRenderReason::Camera);
        return true;
    });

    // 動画全画面中の5秒スキップ操作フィードバック（>> / <<）用フェードアウト。
    m_seekIndicatorAnim = new QVariantAnimation(this);
    m_seekIndicatorAnim->setStartValue(1.0);
    m_seekIndicatorAnim->setEndValue(0.0);
    m_seekIndicatorAnim->setDuration(550);
    connect(m_seekIndicatorAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        m_seekIndicatorAlpha = v.toDouble();
        emit focusOverlayUpdateRequested();
    });
    connect(m_seekIndicatorAnim, &QVariantAnimation::finished, this, [this](){
        m_seekIndicatorDir = 0;
        emit focusOverlayUpdateRequested();
    });

    // ホイール中クリックのダブルクリックで原寸リセットした際の演出用。
    // 0→kStrongBorderGlowHoldFrac の区間は最大値のまま保持し、その後
    // 1.0まで滑らかにフェードアウトする（paintStrongBorderGlow()参照）。
    m_strongBorderGlowAnim = new QVariantAnimation(this);
    m_strongBorderGlowAnim->setDuration(kStrongBorderGlowDurationMs);
    m_strongBorderGlowAnim->setStartValue(1.0);
    m_strongBorderGlowAnim->setKeyValueAt(0.0, 1.0);
    m_strongBorderGlowAnim->setKeyValueAt(kStrongBorderGlowHoldFrac, 1.0);
    m_strongBorderGlowAnim->setKeyValueAt(1.0, 0.0);
    m_strongBorderGlowAnim->setEndValue(0.0);
    m_strongBorderGlowAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_strongBorderGlowAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        // 値は毎tick(約60Hz)更新する。明るさの時間カーブはこれで正しいまま。
        m_strongBorderGlowAlpha = v.toDouble();
        // 2026-09-15 E-1: 描き直しの要求だけを30Hzへ間引く。
        //
        // 【なぜ】この演出は requestFullRender ＝キャンバス全面＋オーバーレイ
        // 全面を毎tick投げており、900msのあいだ約54回、静的レイヤーキャッシュ
        // を無効にしたまま33アイテム全部を描き直していた。外周の演出なので
        // 範囲は絞れない（4辺の帯にしても外接矩形がウィンドウ全体に戻る）が、
        // 回数は半分にできる。
        constexpr qint64 kStrongGlowRenderIntervalMs = 33;   // 約30Hz
        if (m_strongGlowRenderClock.isValid()
            && m_strongGlowRenderClock.elapsed() < kStrongGlowRenderIntervalMs) {
            return;
        }
        if (m_strongGlowRenderClock.isValid())
            m_strongGlowRenderClock.restart();
        else
            m_strongGlowRenderClock.start();
        requestFullRender(CanvasRenderReason::UiAnimation);
        // 強グロー中はoverlay側の通常白線を隠し、終了後に戻す。
        emit borderOverlayUpdateRequested();
    });
    // 最後の1コマは間引かずに必ず描く。ここを落とすと、消え切ったはずの
    // 縁が残ったまま（かつ paintBorderOverlay の通常白線が抑止されたまま）
    // 次の再描画まで固まる。
    connect(m_strongBorderGlowAnim, &QVariantAnimation::finished, this, [this]{
        m_strongBorderGlowAlpha = 0.0;
        m_strongGlowRenderClock.invalidate();
        requestFullRender(CanvasRenderReason::UiAnimation);
        emit borderOverlayUpdateRequested();
    });

    m_liftAnim = new QVariantAnimation(this);
    m_liftAnim->setDuration(250);
    connect(m_liftAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        if (m_activeItem) {
            m_activeItem->setLiftScale(v.toDouble());
            requestItemRender(m_activeItem, CanvasRenderReason::UiAnimation);
        }
    });

    // 2026-07-08: アクティブ表記の視認性をさらに強化。下限アルファを 150→190 へ再度
    // 引き上げ、かつ周期も 1200ms→900ms に短縮して明滅にメリハリを持たせた。
    m_pulseAnim = new QVariantAnimation(this);
    m_pulseAnim->setDuration(900);
    m_pulseAnim->setStartValue(190.0);
    m_pulseAnim->setEndValue(255.0);
    m_pulseAnim->setEasingCurve(QEasingCurve::InOutSine);
    m_pulseAnim->setLoopCount(-1);
    connect(m_pulseAnim, &QVariantAnimation::currentLoopChanged, this, [this](int loop){
        if (loop%2==0) { m_pulseAnim->setStartValue(190.0); m_pulseAnim->setEndValue(255.0); }
        else           { m_pulseAnim->setStartValue(255.0); m_pulseAnim->setEndValue(190.0); }
    });
    connect(m_pulseAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        // 値そのものは毎回更新しておく（代入1回なので安い）。他の理由で
        // 再描画が走ったフレームでは、常に最新のアルファが使われる。
        m_pulseAlpha = v.toDouble();

        // 2026-09-14 Phase2: ここから先（＝パルスだけを理由にした再描画要求）
        // を間引く。
        //
        // 【背景】実測（[RenderSchedule]）では、動画を止めてバブルが
        // Sleepに落ちた完全アイドル状態でも
        //     fps=60.0 frames=300 full=0 video=0 camera=0 ui=300
        //     avgDamage=46.4%
        // となっていた。ui=300 は「UiAnimationを理由とする再描画」が5秒間に
        // 300回＝60Hz。その発生源がこのコールバックで、アクティブなバブルが
        // 1つでも存在する限り永久に続いていた（m_pulseAnimはコンストラクタで
        // setLoopCount(-1)＋start()）。つまり「バブルを1回クリックしたら、
        // 以降アプリ全体が常時60fpsで回り続ける」状態。
        // 巻き添えでBackdropBlur（0.6〜0.7ms/frame）とCanvasOverlayWidgetの
        // QPainter描画（1.0〜1.2ms/frame）も60Hzで走っていた。
        //
        // 【なぜ間引いてよいか】明滅は190→255のアルファを900ms周期で
        // 往復させるだけ。20Hzでも片道あたり9ステップあり、アルファの
        // フェードとしては十分に滑らかに見える。
        constexpr qint64 kPulseRepaintIntervalMs = 50;   // 約20Hz
        if (!m_pulseRepaintClock.isValid()) {
            m_pulseRepaintClock.start();
        } else if (m_pulseRepaintClock.elapsed() < kPulseRepaintIntervalMs) {
            return;
        } else {
            m_pulseRepaintClock.restart();
        }

        // m_activeItem（単体アクティブ）だけでなく m_selectedItems（複数選択）が
        // ある場合も明滅を反映して再描画する。
        // Playing動画は次の動画フレーム（通常は約16ms以内）で同じpulse値を
        // 描画できる。ここから別のupdateを送ると、60fps動画と約60Hzのpulseが
        // 互い違いに発火してCanvas全体が約120fpsでPresentされてしまう。
        // 2026-09-14 Phase3: 全画面フォーカス中のアイテムは、パルスだけを
        // 理由にした再描画の対象から外す。
        // フォーカス中はバブルが画面いっぱいなのでダメージが画面100%になり、
        // かつ一時停止中は他に再描画の理由が無いため、見えるか見えないか
        // 分からない外周の枠のために15Hzで全画面を塗り直し続けていた。
        // 再生中は動画フレームで再描画が来るので、そちらに便乗して明滅は
        // これまでどおり維持される（m_pulseAlphaの更新自体は止めていない）。
        const CanvasItem *focusTarget =
            m_pendingFocusItem ? m_pendingFocusItem : m_focusedItem;
        auto needsStandalonePulseFrame = [focusTarget](CanvasItem *item) {
            if (item == focusTarget)
                return false;
            auto *video = canvasItemAsVideo(item);
            return !video || video->bubbleState() != BubbleState::Playing;
        };
        // 2026-09-23: ズームのイージング中はカメラの再描画が毎フレーム来るので、
        // 明滅だけを理由にした再描画は出さない（別のタイミングで割り込むと
        // 表示の間隔が乱れる）。
        if (m_zoomEase && m_zoomEase->isActive())
            return;
        if (m_activeItem && needsStandalonePulseFrame(m_activeItem))
            requestItemRender(m_activeItem, CanvasRenderReason::UiAnimation);
        for (CanvasItem *item : m_selectedItems)
            if (item != m_activeItem && needsStandalonePulseFrame(item))
                requestItemRender(item, CanvasRenderReason::UiAnimation);
    });
    // 2026-09-14 Phase2: ここでの無条件start()をやめた。パルス対象が
    // 存在するときだけ syncLogicMedium() 経由で開始する
    // （updatePulseAnimationState()）。対象が無い間はアニメーション自体が
    // 止まり、valueChangedのコールバックも発生しない。
    updatePulseAnimationState();

    // 2026-07-15: カメラアニメは進行度0→1の1本(m_camAnimDriver)。
    // QVariantAnimation(60Hz駆動)ではなく、120HzのsyncLogic()から
    // tickCameraAnimation()で手動駆動する(CanvasWidget.hのメンバコメント
    // 参照)。ここでの初期化は不要(値メンバ+クラス内初期化子)。

    m_trashZone = new TrashZone(this);
    m_trashZone->hide();
    m_bubbleDragExporter = new BubbleDragExporter(this);

}

CanvasWidget::~CanvasWidget()
{
    cancelAllOverlapAnims();
    cancelAllFadeInAnims();
    makeCurrent();
    for (auto *item : m_items) {
        if (auto *vid = canvasItemAsVideo(item)) {
            vid->releaseGlResources(m_renderer);
        } else if (auto *image = dynamic_cast<CanvasImage*>(item)) {
            image->releaseGlResources(m_renderer);
        }
    }
    qDeleteAll(m_items);
    m_staticLayerCache.release();
    m_sceneLayer.release();
    releaseAmbientReadback();
    doneCurrent();

    // D3D11レイヤー分離方式: ステップ1（最小疎通確認）
    // Qt/OpenGL側のリソース解放後に破棄する（依存関係はないが順序を明示しておく）
    m_videoLayerHost.reset();
}

QPointF CanvasWidget::screenToCanvas(const QPointF &s) const
{
    QPointF sc(width()/2.0, height()/2.0);
    return (s - sc) / m_cameraZoom + m_cameraPos;
}
QPointF CanvasWidget::canvasToScreen(const QPointF &c) const
{
    QPointF sc(width()/2.0, height()/2.0);
    return (c - m_cameraPos) * m_cameraZoom + sc;
}

void CanvasWidget::update()
{
    m_renderScheduler.setViewportSize(size());
    m_renderScheduler.invalidateFull(CanvasRenderReason::External);
    QOpenGLWidget::update();
}

void CanvasWidget::update(const QRect &rect)
{
    m_renderScheduler.setViewportSize(size());
    m_renderScheduler.invalidateRect(rect, CanvasRenderReason::External);
    QOpenGLWidget::update(rect);
}

void CanvasWidget::requestFullRender(CanvasRenderReason reason)
{
    m_renderScheduler.setViewportSize(size());
    m_renderScheduler.invalidateFull(reason);
    QOpenGLWidget::update();
}

// 2026-09-15 D-5: BackdropBlur の形状変化を、全面ではなくその矩形だけの
// 再描画にする（Blur.h の setRepaintRequester のコメントに経緯）。
//
// 【安全性】paintGL() は damage で切り取らず常に全画面を描き直し、その末尾で
// renderAll() が全エントリをぼかす。つまりFBOの内容はどのフレームでも完成品で、
// damage が担っているのは「静的レイヤーを再利用してよいか」の判定と
// 「Qtにどこを合成させるか」だけ。よってここを絞ってもぼかしが二重に掛かる
// ことはない。逆に、全面を要求していたせいで dynamicOnlyRequest が常に false に
// なり、静的レイヤーキャッシュが使われていなかった（retained=12/280）。
void CanvasWidget::requestBlurRectRender(QWidget *owner, const QRectF &ownerRect)
{
    // 範囲不明、または別ウィンドウのウィジェット（＝renderOne()がそもそも
    // スキップする対象）は従来どおり全面で安全側に倒す。
    if (!owner || ownerRect.isEmpty() || owner->window() != window()) {
        requestFullRender(CanvasRenderReason::UiAnimation);
        return;
    }
    // ownerローカル → キャンバスローカル。BackdropBlurGL::renderOne() と
    // 同じ経路で変換すること（食い違うと消し残る）。
    const QPoint originInWindow = owner->mapTo(owner->window(), QPoint(0, 0));
    const QPoint originInCanvas = mapFrom(window(), originInWindow);
    const QRectF canvasRect = ownerRect.translated(originInCanvas);

    m_renderScheduler.setViewportSize(size());
    // 2026-09-16: UIレイヤーだけの更新として積む（paintGL がシーンレイヤーを
    // 再利用できる）。以前は UiAnimation で、キャンバス全体の描き直しになっていた。
    m_renderScheduler.invalidateRect(canvasRect, CanvasRenderReason::Backdrop);
    m_backdropActivityClock.start();
    const QRect updateRect = canvasRect.toAlignedRect().intersected(rect());
    if (!updateRect.isEmpty())
        QOpenGLWidget::update(updateRect);
}

void CanvasWidget::requestItemRender(CanvasItem *item,
                                     CanvasRenderReason reason)
{
    if (!item) {
        requestFullRender(reason);
        return;
    }

    m_renderScheduler.setViewportSize(size());
    // 2026-09-14 Phase3: パディング計算を itemDamageRect() へ切り出した。
    // paintGL() の dynamicDamageRegion が別式（bezel*2.5・アンビエント項なし）
    // を使っていたため、発光中のバブルでは damage の方が常に大きくなり、
    // damageBelongsToDynamicSuffix が永久に false ＝ 静的レイヤーキャッシュが
    // 一度も使われなかった（[RenderSchedule] retained=0）。必ず同じ式を使う。
    const QRectF damageRect = itemDamageRect(item);
    m_renderScheduler.invalidateRect(damageRect, reason);
    QRect updateRect = damageRect.toAlignedRect().intersected(rect());
    if (updateRect.isEmpty()) {
        // Live streams and deferred GL teardown still require paintGL even
        // while culled. A 1px Qt update schedules the pass without claiming
        // that the off-screen item damaged visible pixels.
        auto *video = canvasItemAsVideo(item);
        if (!video || !video->requiresOffscreenPaint())
            return;
        updateRect = QRect(0, 0, 1, 1);
        m_renderScheduler.invalidateRect(updateRect, reason);
    }
    QOpenGLWidget::update(updateRect);
}

// 2026-09-14 Phase3: 1アイテムが「汚す」画面矩形。
//
// バブル本体（contentRect）＋比例ベゼル＝boardRect に、外周の影・ハイライトの
// ブルーム・リサイズハンドル・アンビエント発光の十字フレアが収まるだけの
// パディングを足したもの。
//
// 【なぜ関数に切り出したか】以前は requestItemRender() と paintGL() の
// dynamicDamageRegion が別々の式を持っていた:
//     requestItemRender  : max(64, bezel*1.5)、発光中は minSide*1.30 まで拡大
//     dynamicDamageRegion: max(64, bezel*2.5)（アンビエント項なし）
// 発光しているバブルでは前者が常に大きくなるため
// (damage - dynamicDamageRegion) が空にならず、damageBelongsToDynamicSuffix が
// 恒久的に false ＝ 静的レイヤーキャッシュが一度も再利用されなかった。
// 2つの式が食い違わないよう、唯一の定義をここに置く。
QRectF CanvasWidget::itemDamageRect(CanvasItem *item) const
{
    if (!item)
        return QRectF();

    const QRectF contentRect = itemScreenRect(item);
    const bool launcher = dynamic_cast<CanvasLauncher *>(item) != nullptr;
    const double bezel = launcher
        ? CanvasItem::launcherBezelWidth(contentRect)
        : CanvasItem::mediaBezelWidth(contentRect);
    const QRectF boardRect = contentRect.adjusted(-bezel, -bezel, bezel, bezel);

    // 外周の影・ハイライトのブルーム・リサイズハンドルを覆う。
    double padding = qMax(64.0, bezel * 1.5);
    // 2026-09-13: アンビエント発光は十字の筋がバブルの外へ長く伸びる。
    // そのぶん無効化する範囲を広げないと、筋の先が消し残って尾を引く。
    // 光っていないバブルでは従来どおりの範囲のまま。
    // 2026-09-15 D-4: ここが「全画面だと何もかも重い」の主因だった。
    // paintGL()の十字の筋は reach = minSide * kAmbientStreakReach * streak で、
    // streak は明るさ由来（0〜1、暗い動画では0＝筋は一切描かれない）。
    // それなのに damage は streak を見ずに常に最大長を足していたため、
    // 筋が出ていないフレームでも damage が 1.36Mpx（1821x1317の57%）に
    // なっていた。半透明の CanvasOverlayWidget はGLキャンバスの兄弟なので、
    // この damage がそのまま overlay の paintEvent 領域として降ってくる。
    if (auto *glowVideo = canvasItemAsVideo(item)) {
        if (glowVideo->ambientGlowColor().alpha() > 0) {
            const double minSide = qMin(boardRect.width(), boardRect.height());
            const double bright = glowVideo->ambientBrightness();
            const double streak = qBound(0.0,
                (bright - kAmbientStreakThreshold) / (1.0 - kAmbientStreakThreshold),
                1.0);
            // 縮む方向のフレームで筋の先端を消し残さないよう、前フレームの
            // streak との max を使う（実際に描かれているのは前フレームの長さ）。
            double &last = m_ambientStreakDamage[item];
            const double streakForDamage = qMax(streak, last);
            last = streak;
            if (m_ambientStreakDamage.size() > 256)
                m_ambientStreakDamage.clear(); // 削除済みアイテムぶんの掃除
            if (streakForDamage > 0.01) {
                padding = qMax(padding,
                               minSide * (kAmbientStreakReach * streakForDamage + 0.15));
            }
        }
    }
    return boardRect.adjusted(-padding, -padding, padding, padding);
}

void CanvasWidget::updateCanvasFromCamera()
{
    QPointF sc(width()/2.0, height()/2.0);
    m_canvasOffset = sc - m_cameraPos * m_cameraZoom;
    m_canvasScale  = m_cameraZoom;

    // 2026-09-14 Phase5: 慣性パンの可変レート判定は「1フレームに画面上で
    // 何px動くか」で行うため、キャンバス単位→画面pxの倍率を渡しておく。
    // カメラ値が変わるたびに必ずここを通るので、ズーム中でも追従する。
    if (m_canvasPanInertia)
        m_canvasPanInertia->setViewScale(m_cameraZoom);
    if (m_focusPanInertia)
        m_focusPanInertia->setViewScale(m_cameraZoom);
}

QRectF CanvasWidget::itemScreenRect(CanvasItem *item) const
{
    QRectF ir = item->itemRect();
    QPointF anchor = item->liftAnchor();
    double ls = item->liftScale();
    auto toScreen = [&](QPointF p) -> QPointF {
        return canvasToScreen(anchor + (p - anchor) * ls);
    };
    return QRectF(toScreen(ir.topLeft()), toScreen(ir.bottomRight()));
}

double CanvasWidget::itemScreenScale(CanvasItem *item) const
{
    return m_canvasScale * item->liftScale();
}

void CanvasWidget::initializeGL()
{
    initializeOpenGLFunctions();
    // UI背後ぼかし(Blur.h)のGLリソース解放。aboutToBeDestroyedはQOpenGLWidget
    // ではなくQOpenGLContextのシグナルで、ここでは呼び出し時点のcontext()へ
    // 繋ぐ必要がある(コンストラクタ時点ではまだcontext()が無い)。
    // 破棄直前に呼ばれるので、makeCurrent()で明示的にカレントにしてから解放する。
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        // 2026-09-15 非同期化 P1: 共有元のコンテキストが消える前に、動画描画
        // ワーカー（共有コンテキスト）を必ず先に畳む。残ジョブは全部実行して
        // から止まる（同期）。多重呼び出し安全。
        VideoRenderThread::shutdownShared();
        makeCurrent();
        BackdropBlur::instance().releaseGL();
        m_gpuProfiler.release();
        m_bubbleLayers.release();
        // テクスチャはコンテキストと一緒に消えるので、再利用されないよう捨てる。
        m_sceneLayer.release();
        m_staticLayerCache.release();
        doneCurrent();
    }, Qt::DirectConnection);
    m_renderer.initialize();
    m_renderer.setLogicalSize(width(), height());
    m_glassHighlight.initialize();
    m_gpuProfiler.initialize();
    m_bubbleLayers.initialize();
    m_glassHighlightClock.start();
    // 2026-09-15 非同期化: 動画描画ワーカーを起動する（全動画の mpv 描画・解放を担当）。
    // 設計: md/async_video_render_design_2026-09-15.md
    VideoRenderThread::startShared(context());
    // セッション復元された動画はサムネイルだけでIdle表示できるため、ここで
    // 全バブル分のMpvPlayerを先行生成しない。以前のループは、未再生の動画
    // バブル1個ごとにmpv_handleとOpenGL render contextを常駐させ、8本の
    // 復元セッションで起動直後から数百MBを消費していた。
    //
    // 初回再生時はCanvasVideo::startPlayback()がこのCanvasWidget経由で
    // makeCurrent()→initializeMpv()を実行する。Sleepからの復帰時も
    // VideoSleepCoordinatorがpaintGL()内で再生成するため、ここでの一括
    // 初期化は不要。

    // ---- D3D11レイヤー分離方式: ステップ1（最小疎通確認） ----
    // 【一旦無効化】実機検証の結果、このプロセス内で D3D11CreateDevice
    // （D3D_DRIVER_TYPE_HARDWARE）を呼び出すだけで、Qt::WA_TranslucentBackground による
    // ウィンドウ全体の透過が失われることが判明した（子HWND化・BGRA_SUPPORTフラグ除去
    // でも再現、単一GPU環境でアダプター不一致説も除外済み）。原因未特定のため、
    // 次のチャットで本格調査するまでD3D11レイヤー関連は無効化しておく。
    // 詳細は d3d11_layer_separation_design_2026-07-01.md の追記を参照。
#if 0
    m_videoLayerHost = std::make_unique<VideoLayerHost>(this);
    HWND canvasHwnd = reinterpret_cast<HWND>(winId());
    const int physW = static_cast<int>(width()  * devicePixelRatioF());
    const int physH = static_cast<int>(height() * devicePixelRatioF());
    if (!m_videoLayerHost->initialize(static_cast<void*>(canvasHwnd), physW, physH)) {
        qWarning() << "CanvasWidget: VideoLayerHost initialize failed; D3D11 layer disabled";
    } else {
        const QRectF testRect(100, 100, 400, 300);
        m_videoLayerHost->createTestSurface(testRect);
    }
#endif
}

void CanvasWidget::resizeGL(int w, int h)
{
    m_renderScheduler.setViewportSize(QSize(w, h));
    // D3D11レイヤー分離方式: ステップ1（最小疎通確認）
    // 専用子HWNDのサイズをCanvasWidgetの物理ピクセルサイズに追従させる。
    if (m_videoLayerHost && m_videoLayerHost->isInitialized()) {
        const int physW = static_cast<int>(width()  * devicePixelRatioF());
        const int physH = static_cast<int>(height() * devicePixelRatioF());
        m_videoLayerHost->resizeHostWindow(physW, physH);
    }

    updateCanvasFromCamera();
    if (m_trashZone)
        m_trashZone->move(16, height() - m_trashZone->height() - 16);

    bool animRunning = m_camAnimDriver.running;
    // 2026-09-14 Phase1: ワープ調査用の[RESIZEGL-TRACE]無条件ログを削除した。
    // resizeGL()はウィンドウリサイズアニメーション中に毎フレーム呼ばれるため、
    // qDebug()のファイルI/Oがそのままアニメーションのコストになっていた。
    // 2026-07-04: フォーカス遷移中（focusItem()呼び出し直後〜ウィンドウリサイズ
    // アニメ完了まで）は、m_focusedItemは古いフォーカス対象を指したままで、
    // 実際に今向かおうとしている先はm_pendingFocusItemに入っている
    // （focusItem()参照）。以前はここでm_focusedItemだけを見ていたため、
    // Flowチェーンで「全画面中の動画A→次の動画Bへホットスワップ」する際、
    // animateToAspect()のウィンドウリサイズアニメ中に何度も呼ばれる
    // resizeGL()が、まだ更新されていない古いA側のitemRect()を着地先として
    // 誤って再設定し続け、Bへ向かっていたカメラパン/ズームアニメが
    // 実質的にAの位置へ引き戻されて止まってしまう（＝カメラが次の動画へ
    // 移行しないように見える）不具合になっていた。
    CanvasItem *focusTargetItem = m_pendingFocusItem ? m_pendingFocusItem : m_focusedItem;
    if (focusTargetItem) {
        QRectF ir = focusTargetItem->itemRect();
        if (ir.width() > 0 && ir.height() > 0) {
            const QPointF targetPos  = ir.center();
            const double  targetZoom = qBound(0.1, qMin((double)w/ir.width(), (double)h/ir.height()), 10.0);
            if (animRunning) {
                // フォーカスイン/アウト演出のカメラアニメ中にウィンドウサイズが
                // 変わったケース（例: 動画フォーカス直後にウィンドウの全画面を
                // 解除した等）。進行中のアニメを打ち切らず、着地先（終端値）だけ
                // 新しいウィンドウサイズに合わせて補正する。これをしないと、
                // アニメ完了後のズームが「サイズ変更前のウィンドウ」基準のまま
                // 固定されてしまい、動画が誤ったアスペクトで全画面表示され続けた。
                // 2026-07-15: 画面座標ベース補間への書き換えに伴い、終端値の
                // 差し替えは連続性を保った張り直し(retargetCameraAnimation)で行う。
                retargetCameraAnimation(targetPos, targetZoom);
            } else {
                // アニメ非実行時はカメラを即座に着地先へ上書きする。
                // 2026-09-14 Phase1: 調査用の[WARP-CHECK]無条件ログを削除
                // （この分岐もリサイズ中に毎フレーム踏まれ得る）。
                m_cameraPos  = targetPos;
                m_cameraZoom = targetZoom;
                updateCanvasFromCamera();
            }
        }
    }
}

void CanvasWidget::paintGL()
{
    // ---- 2026-07-08 一時計測(調査用): バブル全画面ズーム中のカクつき原因切り分け ----
    // 1フレームの各区間の所要時間を計測し、合計が30ms(≒33fps)を超えた
    // フレームだけ内訳をログに出す。原因箇所を特定したら削除する。
    QElapsedTimer __perfTimer;
    __perfTimer.start();

    m_renderScheduler.setViewportSize(size());
    const CanvasRenderRequest __renderRequest =
        m_renderScheduler.takeRequest();
    if (g_zoomDiag.on) {
        ZoomDiag::gap(g_zoomDiag.clock.elapsed(), g_zoomDiag.lastPaint, g_zoomDiag.maxPaintGap);
        ++g_zoomDiag.paints;
        if (__renderRequest.reasons.testFlag(CanvasRenderReason::Camera))
            ++g_zoomDiag.cameraPaints;
        else
            ++g_zoomDiag.otherPaints;
    }

    // 前のフレームで投げたアンビエント発光の色読み戻しを、届いていれば拾う。
    // 届いていなければ何もせず次フレームへ（待たない）。
    collectAmbientSample();

    // 2026-10-12: 区間ごとのGPU計測（GpuProfiler.h）。ここから「背景」。
    m_gpuProfiler.beginFrame();
    m_bubbleLayers.beginFrame();
    m_glassHighlight.resetBatchStats();

    // ---- 2026-09-16: レイヤー分離（シーンレイヤー / UIレイヤー）----------
    //
    // 【何を分けたか】
    //   シーンレイヤー … グリッド・枠・バブル・ライティング・選択枠など
    //   UIレイヤー     … BackdropBlur のぼかしとガラス（島・ラベル・ボタン等）
    // 以前はUIレイヤーをシーンと同じ1枚に焼き込んでいたため、島が1フレーム
    // 動くだけで（ぼかしの材料を作り直すために）キャンバス全体＝全バブルを
    // 描き直していた。バブルが多いほど・解像度が高いほど島の展開が重かった
    // 直接の原因。
    //
    // 【どう分けたか】シーンを描き終えた時点（UIレイヤーを描く直前）の絵を
    // m_sceneLayer に取っておく。次のフレームの理由が Backdrop だけなら、
    // シーンの描画を丸ごと飛ばし、その絵を貼ってUIレイヤーだけを描く。
    // 判定は「理由が Backdrop だけか」の1つだけ。シーン側を変える経路は必ず
    // 別の理由（update() も External）を積むので、取り違えは起きない。
    {
        constexpr qint64 kSceneLayerKeepMs = 2000;
        const qreal layerDpr = devicePixelRatioF();
        const int layerPhysW = static_cast<int>(width()  * layerDpr);
        const int layerPhysH = static_cast<int>(height() * layerDpr);
        const bool uiLayerOnly =
            __renderRequest.reasons == CanvasRenderReasons(CanvasRenderReason::Backdrop);
        if (uiLayerOnly && m_sceneLayer.canReuse(layerPhysW, layerPhysH, 0)) {
            m_renderer.begin(layerPhysW, layerPhysH);
            m_renderer.setLogicalSize(width(), height());
            // 保持した絵は content opacity 等を掛け終えた完成品なので素通しで貼る。
            m_renderer.setGlobalAlpha(1.0f);
            m_sceneLayer.draw(m_renderer, width(), height());
            m_gpuProfiler.enter(GpuProfiler::Section::Backdrop);
            BackdropBlur::instance().renderAll(this, layerDpr, layerPhysW, layerPhysH,
                                               m_sceneLayer.validTexture());
            m_gpuProfiler.endFrame();
            drawPerfOverlay();
            m_renderer.end();
#ifndef NDEBUG
            const QString __uiReport = m_renderScheduler.recordPresented(
                __renderRequest, __perfTimer.elapsed(), false, true);
            if (!__uiReport.isEmpty())
                qDebug().noquote() << __uiReport;
#endif
            return;
        }
        // UIが一定時間動いていなければ、シーンレイヤーの絵を捨てる。
        // 2026-09-22: 以前はここで release()（テクスチャとFBOごと破棄）していた。
        // すると「UIが2秒止まっている間にキャンバスが1回でも描き直された」後に
        // 島を出すと、展開の最初のフレームでウィンドウ大のテクスチャ＋FBOを
        // 作り直すことになり、その1フレームだけ重くなって展開アニメが
        // 「ガクッ」と飛んでいた（毎回ではなく、たまに起きる理由）。
        // 絵だけ無効にしてメモリ（ウィンドウ物理サイズ×4バイト）は持ち続ける。
        // ウィンドウのサイズが変わったとき・GLコンテキスト破棄時は従来どおり
        // 作り直し／破棄される（ensureStorage / aboutToBeDestroyed）。
        if (!m_backdropActivityClock.isValid()
            || m_backdropActivityClock.elapsed() > kSceneLayerKeepMs) {
            m_sceneLayer.invalidate();
        }
    }

    // A retained static prefix is exact only when every continuously changing
    // video is a suffix of the z-ordered item list. Clicking a playing video
    // already raises it to the end, so this covers the common case. Any static
    // item above a playing video automatically selects the original full path.
    auto isDynamicVideo = [](CanvasItem *item) {
        auto *video = canvasItemAsVideo(item);
        if (!video)
            return false;
        const BubbleState state = video->bubbleState();
        return state == BubbleState::Loading
            || state == BubbleState::Playing
            || state == BubbleState::Buffering;
    };
    // 2026-09-16: 「再生中の動画がリストの末尾にまとまっている」ことを条件に
    // するのをやめ、「動画より後ろ（＝上に描かれる）止まったバブルが、動画と
    // 見た目で重なっていない」ことを条件にした。
    //
    // 【なぜ】旧条件は、動画の再生後に別のバブルをクリックして選ぶ（＝そのバブルが
    // 末尾へ移る）、新しいバブルを置く、動画を2本再生する、といった普段の操作
    // だけで崩れ、静的レイヤーが使われず毎フレーム全部を描き直していた。
    // このアプリはバブル同士が重ならない（重ねようとすると弾かれる）ので、
    // 重なっていない限り描く順番は見た目に影響しない。
    //
    // 【どう描くか】静的レイヤーを使える間は、止まっているものを先に
    // （リストの順で）全部描いて静的レイヤーへ取り、そのあと再生中の動画を
    // （リストの順で）描く。再利用するフレームは静的レイヤーを貼って動画だけ描く。
    //
    // 重なりの判定範囲は、本体＋ベゼル＋影・ガラスのハイライトが広がる分。
    // 動画のアンビエント発光（最大260px）は含めない。発光は従来から、リストで
    // 動画より前にあるバブルには上から掛かり、後ろにあるバブルには下になる、
    // と並び順しだいで変わっていたもので、ここで入れると隣にバブルがあるだけで
    // 静的レイヤーが使えなくなるため。
    QVector<int> staticOrder;
    QVector<int> dynamicOrder;
    staticOrder.reserve(m_items.size());
    for (int i = 0; i < m_items.size(); ++i) {
        if (isDynamicVideo(m_items.at(i)))
            dynamicOrder.push_back(i);
        else
            staticOrder.push_back(i);
    }
    const int dynamicItemStart = dynamicOrder.isEmpty() ? -1 : dynamicOrder.first();
    bool staticAboveOverlapsDynamic = false;
    if (!dynamicOrder.isEmpty()) {
        const auto paintExtent = [this](CanvasItem *it) {
            const QRectF sr = itemScreenRect(it);
            const bool launcher = dynamic_cast<CanvasLauncher *>(it) != nullptr;
            const double bezel = launcher ? CanvasItem::launcherBezelWidth(sr)
                                          : CanvasItem::mediaBezelWidth(sr);
            const double pad = bezel + qMax(16.0, bezel * 1.2);
            return sr.adjusted(-pad, -pad, pad, pad);
        };
        QVector<QRectF> dynamicExtents;
        dynamicExtents.reserve(dynamicOrder.size());
        for (int i : dynamicOrder)
            dynamicExtents.push_back(paintExtent(m_items.at(i)));
        for (int i : staticOrder) {
            if (i < dynamicItemStart)
                continue;   // 動画より下に描かれるものは順番が変わらない
            const QRectF ext = paintExtent(m_items.at(i));
            for (const QRectF &d : dynamicExtents) {
                if (ext.intersects(d)) {
                    staticAboveOverlapsDynamic = true;
                    break;
                }
            }
            if (staticAboveOverlapsDynamic)
                break;
        }
    }
    // 静的レイヤーの中身を識別する値。再生中の動画の顔ぶれが変われば
    // 「止まっているもの」の集合も変わるので、取り直しになる。
    int staticLayerKey = -1;
    if (!dynamicOrder.isEmpty()) {
        size_t h = qHash(dynamicOrder.size());
        for (int i : dynamicOrder)
            h = qHashMulti(h, quintptr(m_items.at(i)));
        // 2026-09-20: 逆輸入ヒントの暗転は静的レイヤーに焼き込まれるので、
        // 濃さが変わったら取り直す。
        h = qHashMulti(h, int(m_exportHintAlpha * 255.0f));
        staticLayerKey = static_cast<int>(h & 0x3fffffff);
    }
    // Focus already culls virtually the entire static scene. Retaining a
    // full-window copy there costs tens of MB while saving very little work.
    const bool focusModeActive = m_focusedItem || m_pendingFocusItem;
    // 2026-09-22: バブル融合中は2つのバブルが重なって描かれるので、
    // 描く順番を入れ替える静的レイヤーは使わない（リストの順で描く）。
    const bool fusionPairActive = m_fusion.pairActive() || m_fusion.absorbing();
    const bool canRetainStaticPrefix =
        !focusModeActive
        && !dynamicOrder.isEmpty()
        && !staticAboveOverlapsDynamic
        && !fusionPairActive;

    int physW = static_cast<int>(width()  * devicePixelRatioF());
    int physH = static_cast<int>(height() * devicePixelRatioF());
    m_renderer.begin(physW, physH);
    m_renderer.setLogicalSize(width(), height());
    m_glassHighlight.begin(width(), height());
    const qint64 __glassNowMs = m_glassHighlightClock.elapsed();
    const double __glassDt = m_glassHighlightLastMs > 0
        ? qMax(0.0, (__glassNowMs - m_glassHighlightLastMs) / 1000.0) : 0.0;
    m_glassHighlightLastMs = __glassNowMs;

    // 2026-09-14 Phase4: ガラスの2つのフェード（フォーカスの消灯/点灯と、
    // 掴んだときの屈折の立ち上がり/立ち下がり）は、どちらも他に再描画の
    // 理由が無いとき自分で update() を呼んで自走している。
    // 継続時間はそれぞれ約0.3秒と0.28秒で、その間だけ60Hzで回っていた。
    // dtベースの計算なので、フレームを間引いても到達時間は変わらない。
    // 30Hzへ落とす（0.3秒なら約9ステップ。フェードとしては十分）。
    const auto requestGlassFadeFrame = [this]() {
        constexpr qint64 kGlassFadeFrameIntervalMs = 33;   // 約30Hz
        if (!m_glassFadeFrameClock.isValid()) {
            m_glassFadeFrameClock.start();
        } else if (m_glassFadeFrameClock.elapsed()
                   < kGlassFadeFrameIntervalMs) {
            return;
        } else {
            m_glassFadeFrameClock.restart();
        }
        update();
    };

    // 2026-07-19: 動画/画像フォーカス中はガラスハイライトをフェードアウト。
    // フォーカス開始時はまず m_pendingFocusItem が立ち（カメラの寄りアニメと
    // 同時）、確定後は m_focusedItem に移るため、どちらかが存在すれば消灯目標。
    // 解除時は両ポインタが即クリアされる（カメラ戻りアニメ開始と同時）ので、
    // 戻りと同時にフェードインが始まる。指数平滑（時定数~100ms）なので
    // カメラアニメと厳密同期ではないが体感は同時。
    {
        const float fadeTarget = (m_focusedItem || m_pendingFocusItem) ? 0.0f : 1.0f;
        const float blend = 1.0f - std::exp(-static_cast<float>(__glassDt) * 10.0f);
        m_glassFocusFade += (fadeTarget - m_glassFocusFade) * blend;
        if (std::abs(fadeTarget - m_glassFocusFade) < 0.01f)
            m_glassFocusFade = fadeTarget;
        else
            requestGlassFadeFrame(); // フェード完了までは再描画を継続（30Hz）
    }
    // 2026-10-11: ウィンドウ固定のバブル全画面中は、動画の外側（余白）を黒で埋める。
    // ウィンドウは透明（背景はOS側のぼかし/薄い塗り）なので、何も描かないと
    // 余白は黒ではなく透けて見える。そこで不透明の黒を背面に敷き、
    // フォーカス対象以外のバブルはフェードで消す。入る/出るときはなめらかに変化。
    {
        const bool blackTarget = m_focusWindowGeometry.windowFixedFocusActive
            && (m_focusedItem || m_pendingFocusItem);
        const float target = blackTarget ? 1.0f : 0.0f;
        const float blend = 1.0f - std::exp(-static_cast<float>(__glassDt) * 12.0f);
        m_focusBlackFade += (target - m_focusBlackFade) * blend;
        if (std::abs(target - m_focusBlackFade) < 0.01f)
            m_focusBlackFade = target;
        else
            requestGlassFadeFrame();
    }
    const float currentGlassFadeTarget =
        (m_focusedItem || m_pendingFocusItem) ? 0.0f : 1.0f;
    const bool glassFocusIsStable =
        std::abs(currentGlassFadeTarget - m_glassFocusFade) < 0.001f;
    const bool staticItemIsRefracting =
        m_glassRefractingItem
        && !isDynamicVideo(m_glassRefractingItem);
    bool uiTargetsAreDynamic = true;
    if (m_activeItem && !isDynamicVideo(m_activeItem)) {
        uiTargetsAreDynamic = false;
    }
    if (uiTargetsAreDynamic) {
        for (CanvasItem *selected : m_selectedItems) {
            if (!isDynamicVideo(selected)) {
                uiTargetsAreDynamic = false;
                break;
            }
        }
    }
    const CanvasRenderReasons fullPassReasons =
        CanvasRenderReason::External
        | CanvasRenderReason::Expose
        | CanvasRenderReason::Camera
        | CanvasRenderReason::Scene
        | CanvasRenderReason::Lighting;
    QRegion dynamicDamageRegion;
    if (canRetainStaticPrefix) {
        // 2026-09-14 Phase3: requestItemRender() と同じ itemDamageRect() を使う
        // （以前は別式だったため、発光中のバブルで damage がこの領域から
        //   はみ出し、静的レイヤーキャッシュが永久に無効化されていた）。
        //
        // kDynamicDamageSlack: damage を積んだ時点（frameReady受信時）と
        // この paintGL の間にカメラやアイテムが少し動くことがあるため、
        // 丸め誤差ぶんの余裕を持たせる。ここは「静的側が変化していないか」を
        // 見るためのヒューリスティックな判定で、再利用時は静的テクスチャを
        // 画面全面へ貼り直してから動的アイテムを上描きするので、
        // 多少広く見積もっても画素が消え残ることはない。
        constexpr double kDynamicDamageSlack = 64.0;
        for (int i : dynamicOrder) {
            dynamicDamageRegion += QRegion(
                itemDamageRect(m_items.at(i))
                    .adjusted(-kDynamicDamageSlack, -kDynamicDamageSlack,
                               kDynamicDamageSlack,  kDynamicDamageSlack)
                    .toAlignedRect()
                    .intersected(rect()));
        }
    }
    const bool damageBelongsToDynamicSuffix =
        (__renderRequest.damage - dynamicDamageRegion).isEmpty();
    const bool dynamicOnlyRequest =
        !__renderRequest.full
        && !__renderRequest.reasons.testAnyFlags(fullPassReasons)
        && (__renderRequest.reasons.testFlag(CanvasRenderReason::VideoFrame)
            || __renderRequest.reasons.testFlag(CanvasRenderReason::UiAnimation))
        && uiTargetsAreDynamic
        && damageBelongsToDynamicSuffix;
    const bool reuseStaticPrefix =
        dynamicOnlyRequest
        && canRetainStaticPrefix
        && glassFocusIsStable
        && m_focusBlackFade <= 0.001f
        && !staticItemIsRefracting
        && std::abs(m_contentOpacity - 1.0) < 0.001
        && m_staticLayerCache.canReuse(
            physW, physH, staticLayerKey);
    // Match the content corners to the glass inner boundary in normal mode.
    // Focus uses the existing focus fade, eased once more so the corners
    // smoothly squeeze back to their original square shape in both directions.
    const float contentCornerFactor =
        m_glassFocusFade * m_glassFocusFade * (3.0f - 2.0f * m_glassFocusFade);
    // ウィンドウ全画面アニメーション中のキャンバス内容フェード用。
    // 1.0未満の間は以降の全描画呼び出しのアルファに乗算される。
    m_renderer.setGlobalAlpha(m_contentOpacity);

    // 「透明効果」OFF環境向けフォールバック：DWMのアクリル/ブラーに頼らず、
    // ウィンドウ全面に薄暗い半透明の塗りを自前で描画する（単純なアルファブレンド）。
    // 他の描画より先に行い、背景として敷く。
    if (reuseStaticPrefix) {
        m_staticLayerCache.draw(m_renderer, width(), height());
    } else if (m_dimBackgroundEnabled) {
        m_renderer.drawRect(QRectF(0, 0, width(), height()),
                            QColor(60, 60, 65, 240), Qt::transparent, 0.0f,
                            static_cast<float>(kWindowCornerRadius));
    }

    // ---- D3D11レイヤー分離方式: ステップ1（最小疎通確認） ----
    // 赤い矩形の生成・描画は initializeGL() で1回だけ行う（ここでは何もしない）。
    // DirectComposition側は静止画のまま合成され続けるので、paintGL()で毎フレーム
    // 作り直す/Presentし直す必要はない（それをやると毎フレームD3D11のクリア＋Presentが
    // 走ってしまい、動作が重くなる）。

    const qint64 __tBegin = __perfTimer.elapsed();

    if (!reuseStaticPrefix)
        paintGrid();
    const qint64 __tGrid = __perfTimer.elapsed();

    // 2026-10-11: ウィンドウ固定フォーカス中の黒い余白（上のm_focusBlackFade参照）。
    // グリッドの上、バブルの下に敷く。
    if (m_focusBlackFade > 0.001f) {
        m_renderer.drawRect(QRectF(0, 0, width(), height()),
                            QColor(0, 0, 0, qRound(255.0f * m_focusBlackFade)),
                            Qt::transparent, 0.0f,
                            static_cast<float>(kWindowCornerRadius));
    }

    // 2026-09-20: 逆輸入ヒント（背景だけ暗くし、バブルはその上に描く）。
    if (!reuseStaticPrefix && m_exportHintAlpha > 0.0f)
        paintExportHintBackground();
    const qint64 __tFlowBg = __perfTimer.elapsed();

    const qint64 __tFrames = __perfTimer.elapsed();

    // 2026-07-08: 画面外カリング。バブル全画面ズームのように大量のバブルが
    // キャンバス上にある状態でカメラが1つのバブルへ大きくズームすると、
    // 残りのバブルはほぼ全て画面外に押し出されるにも関わらず、これまでは
    // 全アイテムを無条件に item->paintGL() していたため、バブル数に比例して
    // 毎フレームのコストが線形に増え続けていた（実測: バブル33個で
    // items区間だけで30〜50ms、フォーカスズーム中のカクつきの直接原因）。
    // 画面矩形と全く交差しないバブルは描画自体をスキップする
    // （少しだけ余白を持たせ、画面端ギリギリのバブルがちらつかないようにする）。
    const QRectF viewportRect(0.0, 0.0, static_cast<double>(width()), static_cast<double>(height()));
    constexpr double kCullMarginPx = 64.0;
    const QRectF cullRect = viewportRect.adjusted(
        -kCullMarginPx, -kCullMarginPx, kCullMarginPx, kCullMarginPx);

    // 疑似ガラスハイライト用の点光源。パンによるバブルとの位置関係の
    // 変化は残し、ズーム倍率だけを後段で取り除いて方向を求める。
    const QPointF glassLightAnchor(width() * 0.3, -height() * 0.25);
    const QPointF viewportCenter(width() * 0.5, height() * 0.5);
    // 2026-07-19: 島の「ガラス表現」品質設定。Performanceならガラス描画を
    // 丸ごとスキップ（＝一番素の状態）。Qualityはライティングに加えて
    // 自FBOキャプチャによる疑似屈折を有効化する
    // （設計: md/glass_refraction_design_2026-07-19.md）。
    const bool glassEnabled = AppSettings::instance().glassEffectQuality()
                              != AppSettings::GlassEffectQuality::Performance;
    // デバッグ: Shift+Dでガラスのデバッグ表示をサイクル（handleKeyPress()参照）。
    m_glassHighlight.setDebugMode(m_glassDebugMode);
    // 2026-09-16: ガラススタイル（標準 / 削り出し）。2026-10-11: グミを追加。
    switch (AppSettings::instance().glassStyle()) {
    case AppSettings::GlassStyle::Carved:
        m_glassHighlight.setStyle(GlassHighlightEffect::Style::Carved);
        break;
    case AppSettings::GlassStyle::Gummy:
        m_glassHighlight.setStyle(GlassHighlightEffect::Style::Gummy);
        break;
    case AppSettings::GlassStyle::Standard:
        m_glassHighlight.setStyle(GlassHighlightEffect::Style::Standard);
        break;
    }
    const bool gummyGlass = AppSettings::instance().gummyGlassActive();
    const bool qualityRefraction = glassEnabled
        && AppSettings::instance().glassEffectQuality()
           == AppSettings::GlassEffectQuality::Quality;
    // 屈折の開始条件はpanning限定ではなく、バブルの既存アクティブ演出
    // (startLiftUp/startLiftDown)と同期する。これにより右クリックによる
    // アクティブ化やリサイズでも同じ見た目になり、離した瞬間から
    // フェードアウトへ移る。
    CanvasItem *activeRefractionTarget =
        qualityRefraction && m_glassBubbleActive ? m_activeItem : nullptr;
    float refractionActivation = 0.0f;
    if (!qualityRefraction) {
        m_glassRefractingItem = nullptr;
        m_glassRefractionFade = 0.0f;
        m_glassHighlight.setRefractionActivation(0.0f);
        m_glassHighlight.releaseCaptureTexture();
    } else {
        constexpr float kRefractionFadeSeconds = 0.28f;
        const float fadeDt =
            static_cast<float>(qMin(__glassDt, 0.05));
        if (activeRefractionTarget) {
            if (m_glassRefractingItem != activeRefractionTarget) {
                // アクティブ開始の最初のフレームは必ず屈折率0から始める。
                m_glassRefractingItem = activeRefractionTarget;
                m_glassRefractionFade = 0.0f;
            } else {
                m_glassRefractionFade =
                    qMin(1.0f, m_glassRefractionFade
                               + fadeDt / kRefractionFadeSeconds);
            }
        } else if (m_glassRefractingItem) {
            // アクティブ解除後も同じバブルを対象としてキャプチャを続け、
            // 0へ到達してからテクスチャ/FBOを解放する。
            m_glassRefractionFade =
                qMax(0.0f, m_glassRefractionFade
                           - fadeDt / kRefractionFadeSeconds);
            if (m_glassRefractionFade <= 0.0f)
                m_glassRefractingItem = nullptr;
        }

        if (m_glassRefractingItem) {
            // Smoothstepで開始・終了の速度を0にする。屈折率と
            // エッジブーストの両方へ同じ係数を掛け、固定法線は変えない。
            const float t = m_glassRefractionFade;
            refractionActivation = t * t * (3.0f - 2.0f * t);
            m_glassHighlight.setRefractionActivation(refractionActivation);
            const bool fadeInProgress =
                activeRefractionTarget && m_glassRefractionFade < 1.0f;
            const bool fadeOutProgress =
                !activeRefractionTarget && m_glassRefractionFade > 0.0f;
            if (fadeInProgress || fadeOutProgress)
                requestGlassFadeFrame();   // 2026-09-14 Phase4: 30Hzへ間引き
        } else {
            m_glassRefractionFade = 0.0f;
            m_glassHighlight.setRefractionActivation(0.0f);
            // 非アクティブかつフェード完了後は、blit/drawだけでなく
            // 小領域のresolve/captureテクスチャも保持しない。
            m_glassHighlight.releaseCaptureTexture();
        }
    }
    CanvasItem *refractionTarget = m_glassRefractingItem;
    // デバッグモード2は「屈折レイヤー単体」。通常のitem描画を先に行うと、
    // 屈折マスクが0の中央にランチャー本体・アイコン・ラベル等が残り、
    // 屈折像の一部であるかのように見えてしまう。アクティブ中と終了フェード中
    // だけitem描画を除外し、キャプチャ背景の屈折出力だけを観察できるようにする。
    const bool refractionOnlyDebug =
        refractionTarget && m_glassDebugMode == 2;

    if (!canRetainStaticPrefix) {
        // The retained layer only helps while at least one video is changing.
        // Drop its GL storage as soon as playback/loading has stopped instead
        // of permanently raising the application's idle memory floor.
        if (focusModeActive || dynamicOrder.isEmpty())
            m_staticLayerCache.release();
        else
            m_staticLayerCache.invalidate();
    }

    int thumbnailUploadBudget = 2;
    bool thumbnailUploadsDeferred = false;

#ifndef NDEBUG
    // 2026-09-14: itemsループの内訳計測（Debugのみ・計測だけで挙動は不変）。
    // 再生開始やSleep遷移の瞬間に items= が20〜47msへ跳ねる単発ヒッチが
    // 残っており、「33個のうちどれが何msか」が分からないと原因を推測で
    // 埋めることになるため、上位3件だけを記録する。
    // ループ内に continue が複数あるので、各反復の先頭で「1つ前のアイテムの
    // 終了時刻」を確定させる方式にしてある（末尾に置くとスキップされる）。
    qint64 __itemWorstMs[3]  = {0, 0, 0};
    int    __itemWorstIdx[3] = {-1, -1, -1};
    qint64 __itemMark        = __tFrames;
    int    __itemPrevIndex   = -1;
    const auto __closeItemCost = [&](int nextIndex) {
        const qint64 now = __perfTimer.elapsed();
        const qint64 cost = now - __itemMark;
        if (__itemPrevIndex >= 0 && cost >= 2) {
            for (int s = 0; s < 3; ++s) {
                if (cost > __itemWorstMs[s]) {
                    for (int t = 2; t > s; --t) {
                        __itemWorstMs[t]  = __itemWorstMs[t - 1];
                        __itemWorstIdx[t] = __itemWorstIdx[t - 1];
                    }
                    __itemWorstMs[s]  = cost;
                    __itemWorstIdx[s] = __itemPrevIndex;
                    break;
                }
            }
        }
        __itemMark = now;
        __itemPrevIndex = nextIndex;
    };
#endif

    // 描く順番。静的レイヤーを使う（取る／貼る）間は「止まっているもの → 動画」、
    // それ以外はリストの順そのまま（上の staticAboveOverlapsDynamic 参照）。
    QVector<int> drawOrder;
    int staticCount = -1;   // drawOrder のうち、静的レイヤーに入る先頭の個数
    if (canRetainStaticPrefix) {
        drawOrder = staticOrder;
        staticCount = drawOrder.size();
        drawOrder += dynamicOrder;
    } else {
        drawOrder.reserve(m_items.size());
        for (int i = 0; i < m_items.size(); ++i)
            drawOrder.push_back(i);
    }

    // ---- 2026-09-22: バブル融合（BubbleFusion.h）----
    // 融合して描く2つのバブルの板の形を先に求めておく。式はこの下のループと
    // 同じ（動画バブルだけが融合するのでメディア用の比例ベゼル）。
    struct FusionGeom {
        QRectF board;
        double bezel = 0.0;
        double radius = 0.0;
        double contentRadius = 0.0;
    };
    CanvasItem *fusionTarget = m_fusion.pairActive() ? m_fusion.pairTarget() : nullptr;
    CanvasItem *fusionDragged = fusionTarget ? m_fusion.pairDragged() : nullptr;
    if (fusionTarget && (!m_items.contains(fusionTarget) || !m_items.contains(fusionDragged)))
        fusionTarget = fusionDragged = nullptr;
    FusionGeom fusionGeomTarget, fusionGeomDragged;
    float fusionK = 0.0f;
    if (fusionTarget) {
        const auto geomOf = [&](CanvasItem *it) {
            FusionGeom g;
            const QRectF sr = itemScreenRect(it);
            g.bezel = CanvasItem::mediaBezelWidth(sr);
            g.board = sr.adjusted(-g.bezel, -g.bezel, g.bezel, g.bezel);
            g.radius = CanvasItem::mediaBoardCornerRadius(sr);
            g.contentRadius = CanvasItem::contentCornerRadius(sr, contentCornerFactor);
            return g;
        };
        fusionGeomTarget = geomOf(fusionTarget);
        fusionGeomDragged = geomOf(fusionDragged);
        fusionK = static_cast<float>(
            m_fusion.bridgeStrength() * BubbleFusion::kBridgeBezelRatio
            * qMin(fusionGeomTarget.bezel, fusionGeomDragged.bezel));
        if (fusionK <= 0.01f)
            fusionTarget = fusionDragged = nullptr;
    }
    // 2026-09-22: 融合中は、掴んでいる側（屈折しているバブル）が受け側の
    // 中心へ近づくほど屈折を弱める。重なりが浅いうちは「別のガラス」として
    // 屈折し、深く入るほど1枚に溶け込んで見えるようにする。
    // 距離は受け側の板の半対角で正規化（0=中心が重なった、1=板の角あたり）。
    if (fusionTarget && refractionTarget == fusionDragged && refractionActivation > 0.0f) {
        const QPointF delta = fusionGeomDragged.board.center() - fusionGeomTarget.board.center();
        const double halfDiag = 0.5 * std::hypot(fusionGeomTarget.board.width(),
                                                 fusionGeomTarget.board.height());
        const double t = halfDiag > 1.0
            ? qBound(0.0, std::hypot(delta.x(), delta.y()) / halfDiag, 1.0) : 1.0;
        // 0.15 より内側で0、0.85 より外側で元の強さ（なめらかにつなぐ）
        const double u = qBound(0.0, (t - 0.15) / 0.70, 1.0);
        const double depthScale = u * u * (3.0 - 2.0 * u);
        // 吸い込み中は進み具合でも確実に0へ向かわせる
        const double absorbScale = m_fusion.absorbing()
            ? (1.0 - m_fusion.absorbProgress()) : 1.0;
        m_glassHighlight.setRefractionActivation(
            refractionActivation * static_cast<float>(depthScale * absorbScale));
    }

    // ---- 2026-10-12: ガラスのまとめ描き ----
    // GlassHighlightEffect::draw() はまとめられるとき値をためるだけなので、ためたガラスと
    // 重なる描画の前・静的レイヤーの取り込み前・ループの最後に吐き出す。グミのランチャーは
    // ゲルの上にアイコンを描くので、アイコンの描画も一緒に後回しにする（afterGlassDraws）。
    QVector<std::function<void()>> afterGlassDraws;
    const auto flushGlassBatch = [&]() {
        if (!m_glassHighlight.hasPendingBatch() && afterGlassDraws.isEmpty())
            return;
        m_glassHighlight.flushBatch();
        for (const auto &fn : afterGlassDraws)
            fn();
        afterGlassDraws.clear();
    };

    for (int orderPos = 0; orderPos < drawOrder.size(); ++orderPos) {
        const int itemIndex = drawOrder.at(orderPos);
        // 融合の設定は、そのバブルを描いている間だけ有効にする。
        m_renderer.clearShapeFusion();
        m_renderer.clearContentMelt();
        m_glassHighlight.clearFusion();
#ifndef NDEBUG
        __closeItemCost(itemIndex);
#endif
        CanvasItem *item = m_items.at(itemIndex);
        if (reuseStaticPrefix && orderPos < staticCount)
            continue;
        m_gpuProfiler.enter(GpuProfiler::Section::Item);
        if (!reuseStaticPrefix
            && canRetainStaticPrefix
            && orderPos == staticCount) {
            flushGlassBatch();
            m_staticLayerCache.captureCurrentFramebuffer(
                physW, physH, staticLayerKey);
        }
        // 2026-10-11: 黒い余白が出ている間、フォーカス対象以外はフェードで消す
        // （余白に隣のバブルが見えないように）。黒が完全なら描画自体を省く。
        if (m_focusBlackFade > 0.001f) {
            CanvasItem *blackFocusTarget = m_focusedItem ? m_focusedItem : m_pendingFocusItem;
            const bool otherThanFocus = item != blackFocusTarget;
            if (otherThanFocus && m_focusBlackFade >= 0.999f)
                continue;
            m_renderer.setGlobalAlpha(static_cast<float>(m_contentOpacity)
                                      * (otherThanFocus ? 1.0f - m_focusBlackFade : 1.0f));
        }
        const QRectF sr = itemScreenRect(item);
        const double ss = itemScreenScale(item);
        const bool isLauncherBubble =
            dynamic_cast<CanvasLauncher*>(item) != nullptr;
        const double bezelPx = isLauncherBubble
            ? CanvasItem::launcherBezelWidth(sr)
            : CanvasItem::mediaBezelWidth(sr);
        // itemScreenRect()は中身だけの矩形。比例ベゼルを含む実際の
        // バブル矩形で判定し、画面端で枠だけ先に消えないようにする。
        const QRectF boardRect = sr.adjusted(
            -bezelPx, -bezelPx, bezelPx, bezelPx);
        // boardRect.center()と画面固定ライトの両方を同じズーム中立座標へ
        // 戻してから方向を求める。片方だけを戻すと、画面上の実効ライト
        // 位置がズーム倍率に応じて中央へ寄り、指定位置とは異なる場所で
        // ハイライトが反転していた。
        const double zoomForLighting = qMax(m_cameraZoom, 0.000001);
        const QPointF zoomNeutralCenter =
            viewportCenter
            + (boardRect.center() - viewportCenter) / zoomForLighting;
        const QPointF zoomNeutralLightAnchor =
            viewportCenter
            + (glassLightAnchor - viewportCenter) / zoomForLighting;
        // 2026-10-12: Shift+L でライト固定。全バブルが同じ向き（画面中央から見た
        // ライトの方向）から照らされ、カメラを動かしても光が動かない。
        const QVector2D glassLightDirection = m_lightLocked
            ? QVector2D(static_cast<float>(glassLightAnchor.x() - viewportCenter.x()),
                        static_cast<float>(glassLightAnchor.y() - viewportCenter.y()))
            : QVector2D(
                  static_cast<float>(
                      zoomNeutralLightAnchor.x() - zoomNeutralCenter.x()),
                  static_cast<float>(
                      zoomNeutralLightAnchor.y() - zoomNeutralCenter.y()));
        if (!boardRect.intersects(cullRect)) {
            // 2026-07-14: ライブメディア（SRT画面共有等）のバブルは画面外でも
            // 描画処理を回し続ける。vo_libmpvはクライアントがrender
            // （CanvasVideo::paintGL内のrenderToFbo）を呼び続けないと再生
            // 全体（音声含む）が停止し、プッシュ型SRTでは受信キュー溢れ→
            // libsrt警告洪水（アプリ応答なし）になるため
            // （CanvasVideo::requiresOffscreenPaint()のコメント参照）。
            // 画面外座標への実描画はGPUがクリップするため、コストは実質
            // FBOレンダーのみ。通常動画・画像は従来どおりスキップする。
            auto *vid = canvasItemAsVideo(item);
            if (!(vid && vid->requiresOffscreenPaint()))
                continue;
        }
        // 2026-09-14: サラウンドモード廃止。ここにあった surroundRing の
        // 判定（アイテムごとに canvasItemAsVideo + 除外フラグ参照）も削除。

        // フレームをシェイクで消した直後など、掴む前の位置へ瞬時に戻ってから
        // フェードインしているアイテムは、一時的にアルファを絞って描画する。
        // 2026-07-08: 複数選択時のバブルも、単体クリック時の「アクティブ」表記
        // （accentカラーの枠＋pulseAlphaによる明滅）と統一した見た目にするため、
        // m_selectedItems に含まれる場合も isActive 扱いにする。
        // これにより paintSelectionOverlay() 側の別描画（固定サイズの矩形枠）は不要になった。
        // 2026-09-22: バブル融合。融合相手の板と smin でつながった形として描く。
        // 受け側もアクティブの見た目にそろえる（掴んでいる側と同じ縁・グローに）。
        const bool fusionSelf =
            fusionTarget && (item == fusionTarget || item == fusionDragged);
        // 2026-09-23: 上に描かれて溶けている側の溶け具合（0=溶けていない）。
        // 中身の縁が薄くなると、中身より先に描く影・発光・屈折が相手の中身の
        // 上に透けて「黒っぽい境目」になる。これらをこの値で引っ込める。
        double meltTopAmount = 0.0;
        if (fusionSelf) {
            const bool selfIsTarget = (item == fusionTarget);
            const FusionGeom &other = selfIsTarget ? fusionGeomDragged : fusionGeomTarget;
            Renderer::ShapeFusion rf;
            rf.selfRect = boardRect;
            rf.otherRect = other.board;
            rf.otherRadius = static_cast<float>(other.radius);
            rf.k = fusionK;
            // 板は先に描かれる受け側がまとめて描く（Renderer::ShapeFusion 参照）
            rf.drawsShape = selfIsTarget;
            m_renderer.setShapeFusion(rf);
            GlassHighlightEffect::Fusion gf;
            gf.otherKey = selfIsTarget ? fusionDragged : fusionTarget;
            gf.otherRect = other.board;
            gf.otherRadius = static_cast<float>(other.radius);
            gf.otherBezelPx = static_cast<float>(other.bezel);
            gf.otherContentRadius = static_cast<float>(other.contentRadius);
            gf.k = fusionK;
            // ガラスの光は両方の動画より後に描かれる側がまとめて描く。
            // ドラッグ中は掴んでいる側（上に来る）。吸い込み中は吸われる側が
            // フェードアウトするので、受け側に持たせる（吸われる側の動画は
            // 受け側の中へ沈んでいくので、光の下に入っても不自然にならない）。
            gf.drawsLighting = m_fusion.absorbing() ? selfIsTarget : !selfIsTarget;

            // 2026-09-23: 中身（動画）も溶かす。溶けるのは2枚の中身が重なって
            // いるところだけ。重なりの中はどちらの中身に深く入っているかで上下の
            // 中身をクロスフェードし、溶け目をぼかす（Renderer::ContentMelt 参照）。
            // 上に描かれるのは掴んでいる側（受け側が先に描かれる）。
            Renderer::ContentMelt melt;
            const FusionGeom &selfGeom = selfIsTarget ? fusionGeomTarget : fusionGeomDragged;
            melt.selfRect = selfGeom.board.adjusted(
                selfGeom.bezel, selfGeom.bezel, -selfGeom.bezel, -selfGeom.bezel);
            melt.selfRadius = static_cast<float>(selfGeom.contentRadius);
            melt.otherRect = other.board.adjusted(
                other.bezel, other.bezel, -other.bezel, -other.bezel);
            melt.otherRadius = static_cast<float>(other.contentRadius);
            // 上下は実際の描画順で決める（通常は掴んでいる側が上）。
            {
                CanvasItem *otherItem = selfIsTarget ? fusionDragged : fusionTarget;
                const int otherPos = drawOrder.indexOf(m_items.indexOf(otherItem));
                melt.selfIsTop = otherPos >= 0 ? (orderPos > otherPos) : !selfIsTarget;
            }
            const double bridge = qBound(0.0, m_fusion.bridgeStrength(), 1.0);
            const double absorb = m_fusion.absorbing() ? m_fusion.absorbProgress() : 0.0;
            melt.amount = static_cast<float>(qBound(0.0, qMax(bridge, absorb), 1.0));
            melt.blurPx = static_cast<float>(
                qBound(6.0, qMin(boardRect.width(), boardRect.height()) * 0.06, 22.0));
            m_renderer.setContentMelt(melt);
            if (melt.selfIsTop)
                meltTopAmount = melt.amount;
            // 屈折プリパスは相手の中身の上では引っ込める（GlassHighlightEffect::Fusion）
            gf.prepassHole = melt.otherRect;
            gf.prepassHoleAmount = static_cast<float>(meltTopAmount);
            m_glassHighlight.setFusion(gf);
        }
        const double meltKeep = 1.0 - meltTopAmount;
        const bool isActiveLook = (m_activeItem == item) || m_selectedItems.contains(item)
                                  || fusionSelf;
        const auto glassSurfaceProfile = isLauncherBubble
            ? GlassHighlightEffect::SurfaceProfile::Launcher
            : GlassHighlightEffect::SurfaceProfile::Media;
        const double boardRadius = isLauncherBubble
            ? CanvasItem::launcherBoardCornerRadius(sr)
            : CanvasItem::mediaBoardCornerRadius(sr);
        const double contentRadius =
            CanvasItem::contentCornerRadius(sr, contentCornerFactor);
        const auto fadeIt = m_fadeInAlpha.constFind(item);
        float glassFade = static_cast<float>(m_contentOpacity) * m_glassFocusFade;
        if (fadeIt != m_fadeInAlpha.constEnd())
            glassFade *= static_cast<float>(fadeIt.value());

        // Qualityは「背景キャプチャ→全面屈折プリパス→item本体→ライティング」
        // の順で描く。屈折をitemの後から外周だけ置換すると、中央をマスクで
        // 抜く必要が生じ、コンテンツ矩形に沿った境界が必ず残ってしまう。
        // 背景レイヤーとして先に全面屈折させれば、中央にも屈折を残しながら、
        // 後から描くアイコン・動画・フレームは一切消さずに済む。
        const bool itemRefract = item == refractionTarget;

        // 2026-10-12: このバブルが描くもの（影・動画の色の光・選択の光・ガラスのにじみ）の
        // 範囲が、まだ描いていないガラスと重なるなら、先に吐き出して重なり順を守る。
        // 屈折（背景を取り込む）・融合（相手の板まで描く）は無条件に吐き出す。
        if (m_glassHighlight.hasPendingBatch() || !afterGlassDraws.isEmpty()) {
            bool mustFlush = itemRefract || fusionSelf;
            if (!mustFlush) {
                double reach = qMax(qMax(8.0, bezelPx * 1.25), bezelPx * 0.95) + 4.0;
                if (isActiveLook)
                    reach = qMax(reach, 60.0 * ss);
                if (auto *glowVideo = glassEnabled ? canvasItemAsVideo(item) : nullptr) {
                    if (glowVideo->ambientGlowColor().alpha() > 0) {
                        const double minSide = qMin(boardRect.width(), boardRect.height());
                        reach = qMax(reach, qMax(qBound(30.0, minSide * 0.55, 260.0),
                                                 minSide * kAmbientStreakReach) + 4.0);
                    }
                }
                mustFlush = m_glassHighlight.batchIntersects(
                    boardRect.adjusted(-reach, -reach, reach, reach));
            }
            if (mustFlush) {
                m_gpuProfiler.enter(GpuProfiler::Section::Glass);
                flushGlassBatch();
                m_gpuProfiler.enter(GpuProfiler::Section::Item);
            }
        }

        if (itemRefract) {
            // 屈折サンプルは外周でboardRectの外側へ到達する。以前は固定で
            // 1ベゼル分しかキャプチャしていなかったため、edgeBoostを上げて
            // 変位がその範囲を越えると未キャプチャの透明画素を読み、
            // 「歪みが強くなる」のではなく縁が削れたように見えていた。
            // R/B色収差の最遠サンプルまで含む固定の最大到達距離を使う。
            // フェード中も範囲サイズを一定にし、テクスチャ再確保を防ぐ。
            m_gpuProfiler.enter(GpuProfiler::Section::Refraction);
            const double normalScalePx =
                GlassHighlightEffect::normalScaleForBezel(bezelPx);
            // 線形補間の隣接画素もキャプチャ内に収める。
            const double capPadPre =
                GlassHighlightEffect::requiredCapturePadding(
                    normalScalePx, glassSurfaceProfile);
            // 2026-09-22: 融合中も屈折は自分の板の形だけ（GlassHighlightEffect::draw 参照）。
            m_glassHighlight.captureRegion(
                boardRect.adjusted(-capPadPre, -capPadPre, capPadPre, capPadPre),
                devicePixelRatioF(), physW, physH);
        }

        m_gpuProfiler.enter(GpuProfiler::Section::AmbientGlow);
        // 2026-09-13: アンビエント発光。
        //
        // 【1枚のグローでは光に見えない】等方的に減衰する塗りを1枚置くと、
        // どれだけ色を付けても「色の付いた影」にしかならない。光に見える
        // ために要るのは (a) 近くの強い芯と遠くの淡い裾の差、(b) 方向、
        // (c) 直線的な筋。ここでは3種類を重ねて作る。
        //   1. 裾  … バブル全体を包む広くて淡い光
        //   2. 辺  … 上下左右それぞれの縁に細い棒を置き、その辺の色で光ら
        //             せる。棒の内側はバブル本体が上に描かれて隠れるので、
        //             結果として「その辺から外へ漏れる光」だけが見える
        //   3. 筋  … 中心を通る横棒・縦棒を長く光らせた十字のフレア。
        //             明るい映像のときだけ伸ばす（暗い映像で出ると嘘くさい）
        // いずれも既存の角丸SDFグロー（drawGlowRect）1回ずつなので、
        // 新しいシェーダーもブラー用FBOも要らない。
        // 【品質設定】ガラス表現と同じ扱いにする＝パフォーマンス優先では
        // 出さない（glassEnabled は「中間以上」を意味する）。
        if (auto *ambientVideo = glassEnabled ? canvasItemAsVideo(item) : nullptr) {
            const QColor base = ambientVideo->ambientGlowColor();
            if (base.alpha() > 0) {
                float itemFade = 1.0f;
                if (fadeIt != m_fadeInAlpha.constEnd())
                    itemFade = static_cast<float>(fadeIt.value());
                const double fade = itemFade * m_contentOpacity;
                const double minSide = qMin(boardRect.width(), boardRect.height());
                const auto withAlpha = [&](const QColor &c, double scale) {
                    QColor out = c;
                    out.setAlpha(qBound(0, qRound(c.alpha() * scale * fade), 255));
                    return out;
                };

                // 1. 裾（広く淡く）
                m_renderer.drawGlowRect(
                    boardRect, withAlpha(base, kAmbientHaloAlphaScale),
                    static_cast<float>(boardRadius),
                    static_cast<float>(qBound(30.0, minSide * 0.55, 260.0)));

                // 2. 辺ごとの漏れ。棒は1pxで、外へ出る光だけが見える。
                const double sideSpread = qBound(18.0, minSide * 0.30, 150.0);
                const double inset = minSide * 0.10;
                const struct { int side; QRectF bar; } edges[] = {
                    {CanvasVideo::AmbientLeft,
                     QRectF(boardRect.left(), boardRect.top() + inset,
                            1.0, qMax(1.0, boardRect.height() - inset * 2.0))},
                    {CanvasVideo::AmbientRight,
                     QRectF(boardRect.right() - 1.0, boardRect.top() + inset,
                            1.0, qMax(1.0, boardRect.height() - inset * 2.0))},
                    {CanvasVideo::AmbientTop,
                     QRectF(boardRect.left() + inset, boardRect.top(),
                            qMax(1.0, boardRect.width() - inset * 2.0), 1.0)},
                    {CanvasVideo::AmbientBottom,
                     QRectF(boardRect.left() + inset, boardRect.bottom() - 1.0,
                            qMax(1.0, boardRect.width() - inset * 2.0), 1.0)},
                };
                for (const auto &e : edges) {
                    const QColor c = ambientVideo->ambientGlowColor(e.side);
                    if (c.alpha() <= 0)
                        continue;
                    m_renderer.drawGlowRect(e.bar, withAlpha(c, kAmbientEdgeAlphaScale * meltKeep),
                                            0.5f, static_cast<float>(sideSpread));
                }

                // 3. 十字の筋。明るさに比例して伸びる。
                const double bright = ambientVideo->ambientBrightness();
                const double streak = qBound(0.0, (bright - kAmbientStreakThreshold)
                                                  / (1.0 - kAmbientStreakThreshold), 1.0);
                if (streak > 0.01) {
                    const QColor c = withAlpha(base, kAmbientStreakAlphaScale * streak * meltKeep);
                    if (c.alpha() > 0) {
                        const double reach = minSide * kAmbientStreakReach * streak;
                        m_renderer.drawGlowRect(
                            QRectF(boardRect.left(), boardRect.center().y() - 1.0,
                                   boardRect.width(), 2.0),
                            c, 1.0f, static_cast<float>(reach));
                        m_renderer.drawGlowRect(
                            QRectF(boardRect.center().x() - 1.0, boardRect.top(),
                                   2.0, boardRect.height()),
                            c, 1.0f, static_cast<float>(reach * 0.75));
                    }
                }
            }
        }

        m_gpuProfiler.enter(GpuProfiler::Section::Shadow);
        // 中間以上では、バブル全体の背面に控えめなアンビエントシャドウを置く。
        // 既存の角丸SDFグローを使うため、追加テクスチャやブラー用FBOは
        // 不要。サイズに比例させ、拡大したバブルでも影の
        // 見た目の割合を維持する。Qualityの背景キャプチャ後に描くことで、
        // バブル自身の影が屈折元テクスチャへ混入するのも避ける。
        // 2026-10-12: グミの影の色。高品質では中身の平均色を暗くした色にする
        // （オレンジのアイコンなら焦げ茶の影）。分からないとき・中間品質は従来の青緑。
        QColor ambientShadowColor = gummyGlass ? QColor(5, 31, 38) : QColor(0, 0, 0);
        if (gummyGlass && qualityRefraction) {
            const QColor avg = item->contentAverageColor();
            if (avg.isValid()) {
                int h = 0, sat = 0, val = 0;
                avg.getHsv(&h, &sat, &val);
                ambientShadowColor = h < 0
                    ? QColor::fromHsv(0, 0, 45)
                    : QColor::fromHsv(h, qMin(255, qRound(sat * 1.1)), 60);
            }
        }
        const auto drawAmbientShadow = [&]() {
            if (glassEnabled && !refractionOnlyDebug && glassFade > 0.001f) {
                float itemFade = 1.0f;
                if (fadeIt != m_fadeInAlpha.constEnd())
                    itemFade = static_cast<float>(fadeIt.value());
                // 2026-10-11: グミは厚みがあるので、濃く広い・青緑がかった影にする
                // （ラボで決めた値。標準 45 / ×0.64 / 黒）。
                const int shadowAlpha = qRound(
                    (gummyGlass ? 100.0f : 45.0f)   // 2026-10-12: グミ 150→100（薄く）
                    * m_glassFocusFade * itemFade * static_cast<float>(meltKeep));
                const double shadowBlur = qMax(8.0, bezelPx * (gummyGlass ? 1.21 : 0.64));
                const QColor shadowColor = ambientShadowColor;
                m_renderer.drawGlowRect(
                    boardRect,
                    QColor(shadowColor.red(), shadowColor.green(), shadowColor.blue(),
                           qBound(0, shadowAlpha, 255)),
                    static_cast<float>(boardRadius),
                    static_cast<float>(shadowBlur));
            }
        };
        // コンテンツとガラス反射の境界を締める接触影（描く位置は下の屈折プリパスの後）。
        const bool hasContentSurface =
            canvasItemAsVideo(item) || dynamic_cast<CanvasImage*>(item);
        const auto drawContactShadow = [&]() {
            if (qualityRefraction && !refractionOnlyDebug && hasContentSurface
             && glassFade > 0.001f) {
                float itemFade = 1.0f;
                if (fadeIt != m_fadeInAlpha.constEnd())
                    itemFade = static_cast<float>(fadeIt.value());
                // 2026-10-11: グミは接触影を少し弱める（標準 82）。
                const int shadowAlpha = qRound(
                    (gummyGlass ? 67.0f : 82.0f)
                    * m_glassFocusFade * itemFade * static_cast<float>(meltKeep));
                m_renderer.drawGlowRect(
                    sr, QColor(2, 4, 8, qBound(0, shadowAlpha, 255)),
                    static_cast<float>(contentRadius),
                    static_cast<float>(bezelPx * 0.22));
            }
        };

        // ---- 2026-10-12: 下地の焼き付け（BubbleLayerCache） ----
        // 影・接触影・枠・リサイズの印を1枚に焼いて貼る。位置が変わるだけなら
        // 描き直さない。見た目が時間で変わる状態（選択中の点滅・屈折・融合・
        // フェード・全画面）や、グミのランチャー（ゲルの上に枠を重ねる順番の
        // ため）は今まで通りその場で描く。
        bool chromeFromLayer = false;
        {
            const bool eligible = m_bubbleLayers.isEnabled()
                && item->supportsChromeLayer()
                && !isActiveLook && !itemRefract && !refractionOnlyDebug
                && !fusionSelf && meltKeep >= 0.999
                && fadeIt == m_fadeInAlpha.constEnd()
                && m_glassFocusFade >= 0.999f
                && std::abs(m_contentOpacity - 1.0) < 0.001
                && contentCornerFactor >= 0.999f
                && m_glassDebugMode == 0
                && m_focusBlackFade <= 0.001f
                && !(gummyGlass && isLauncherBubble);
            if (eligible) {
                const double shadowReach =
                    qMax(8.0, bezelPx * (gummyGlass ? 1.21 : 0.64));
                const double pad = std::ceil(shadowReach) + 2.0;
                const QRectF layerRect = boardRect.adjusted(-pad, -pad, pad, pad);
                const quint64 contentKey =
                    (glassEnabled ? 1ull : 0ull)
                    | (qualityRefraction ? 2ull : 0ull)
                    | (hasContentSurface ? 4ull : 0ull)
                    | (gummyGlass ? 8ull : 0ull)
                    | (isLauncherBubble ? 16ull : 0ull)
                    | (quint64(AppSettings::instance().glassStyle()) << 8)
                    | (quint64(qRound(devicePixelRatioF() * 100.0)) << 16)
                    // 影の色（グミは中身の平均色）。4bitずつに丸めて、動画の色の
                    // 細かな揺れで毎フレーム焼き直さないようにする
                    | (quint64(ambientShadowColor.red() >> 4) << 32)
                    | (quint64(ambientShadowColor.green() >> 4) << 36)
                    | (quint64(ambientShadowColor.blue() >> 4) << 40);
                const auto res = m_bubbleLayers.draw(
                    m_renderer, item, layerRect, contentKey, devicePixelRatioF(),
                    [&]() {
                        drawAmbientShadow();
                        drawContactShadow();
                        item->paintChromeGL(m_renderer, sr, false, m_pulseAlpha, ss,
                                            contentCornerFactor);
                    });
                chromeFromLayer = res != BubbleLayerCache::Result::NotCached;
            }
        }
        if (!chromeFromLayer)
            drawAmbientShadow();
        m_gpuProfiler.enter(GpuProfiler::Section::Refraction);
        if (itemRefract) {
            m_glassHighlight.draw(
                item, boardRect, static_cast<float>(boardRadius),
                static_cast<float>(bezelPx),
                static_cast<float>(contentRadius),
                glassLightDirection, __glassDt, glassFade, 0.0f, 1.0f,
                true, {}, glassSurfaceProfile);
        }

        m_gpuProfiler.enter(GpuProfiler::Section::Shadow);
        // コンテンツとガラス反射の境界を締める接触影。必ずitem本体より先に
        // 描き、動画・画像の上へ影が重ならない正しいレイヤー順にする。
        // drawGlowRectはsrの外側だけへ減衰するため、影はガラス面側に出る。
        if (!chromeFromLayer)
            drawContactShadow();

        // 疑似ガラスのライティング（item描画の後に重ねるのが基本）。
        const auto drawGlassLighting = [&](const GlassContentSource &glassContent) {
            // 2026-07-19: 非アクティブ0.55はハイライトが灰色っぽく見えた
            // ため0.8へ引き上げ（アルファが下がると白いリムが背景と混ざって
            // グレーになる。「白さ」はほぼこの値で決まる）。
            // m_contentOpacity: ウィンドウ全画面アニメーション中のコンテンツ
            // フェード。Renderer側はsetGlobalAlpha()で全描画に乗算されるが、
            // GlassHighlightEffectは独立クラスのため明示的に乗算する必要が
            // ある（乗算しないと全画面切替時にガラスだけ残って見える）。
            // fadeIt: バブル個別のフェードイン中も同様に追従させる。
            const float glassIntensity = (isActiveLook ? 1.0f : 0.8f) * glassFade;
            // ランチャー（exe/lnk/フォルダ等）は内側のハイライト線が
            // 「四角の中の四角」に見えて不自然なため、外周リムだけ残して
            // 内側レイヤーを無効化する（動画・画像はフル表示）。
            // （全オフ = 0.0f固定 も一度試したが、比較の結果こちらに戻した）
            const float glassInnerMix = isLauncherBubble ? 0.0f : 1.0f;
            // 屈折はitem描画前のプリパスで完了済み。ここではライティングだけを
            // 重ねる。Quality時はプリパスでバネを1回進めたためdt=0を渡す。
            m_glassHighlight.draw(item, boardRect, static_cast<float>(boardRadius),
                                  static_cast<float>(bezelPx),
                                  static_cast<float>(contentRadius),
                                  glassLightDirection, itemRefract ? 0.0 : __glassDt,
                                  glassIntensity, glassInnerMix, 0.0f, false,
                                  glassContent, glassSurfaceProfile);
        };
        // 2026-10-11: グミのランチャーは、ゲルの板をアイコンより先に描いて
        // アイコンをゲルの上に乗せる。後から重ねると、アイコンを見せるために
        // 真ん中のゲルを薄くするしかなく、そこがくぼみに見えたため。
        const bool gummyLauncherUnderlay = glassEnabled && gummyGlass && isLauncherBubble;
        m_gpuProfiler.enter(GpuProfiler::Section::Glass);
        if (gummyLauncherUnderlay) {
            // アイコンの色をにじませるため、グミのときだけアイコンのテクスチャを渡す
            // （Qualityのみ。テクスチャはアイコン初回描画で作られるので最初の1フレームは無し）
            drawGlassLighting(qualityRefraction && !refractionOnlyDebug
                                  ? item->glassContentSource() : GlassContentSource{});
        }
        // ゲルをまとめ描きに回したなら、その上に描くアイコンも後回しにする
        const bool deferItemPaint =
            gummyLauncherUnderlay && m_glassHighlight.lastDrawWasBatched();

        m_gpuProfiler.enter(GpuProfiler::Section::Item);
        m_gpuProfiler.countBubble();
        bool thumbnailUploadScheduled = false;
        if (auto *video = canvasItemAsVideo(item)) {
            const bool allowUpload = thumbnailUploadBudget > 0;
            thumbnailUploadScheduled = allowUpload && video->hasPendingThumbnailUpload();
            thumbnailUploadsDeferred = thumbnailUploadsDeferred
                || (!allowUpload && video->hasPendingThumbnailUpload());
            video->setThumbnailUploadAllowedForFrame(allowUpload);
        }
        // 停止中のアクティブ動画は中身を暗くする（旧: 最後に黒70/255を上塗り）
        {
            auto *dimVideo = (item == m_activeItem) ? canvasItemAsVideo(item) : nullptr;
            m_renderer.setContentDim(dimVideo && !dimVideo->isPlaying() ? 70.0f / 255.0f : 0.0f);
        }
        if (chromeFromLayer)
            m_renderer.setChromeMode(Renderer::ChromeMode::NoChrome);
        if (deferItemPaint && !refractionOnlyDebug) {
            const float itemAlpha = fadeIt != m_fadeInAlpha.constEnd()
                ? static_cast<float>(m_contentOpacity * fadeIt.value())
                : static_cast<float>(m_contentOpacity);
            const bool activeLook = isActiveLook;
            const double itemScale = ss;
            const QRectF itemRectScreen = sr;
            afterGlassDraws.append([this, item, itemRectScreen, activeLook, itemScale,
                                    contentCornerFactor, itemAlpha]() {
                m_renderer.setGlobalAlpha(itemAlpha);
                item->paintGL(m_renderer, itemRectScreen, activeLook, m_pulseAlpha,
                              itemScale, contentCornerFactor);
                m_renderer.setGlobalAlpha(static_cast<float>(m_contentOpacity));
            });
        } else if (!refractionOnlyDebug) {
            if (fadeIt != m_fadeInAlpha.constEnd()) {
                m_renderer.setGlobalAlpha(static_cast<float>(m_contentOpacity * fadeIt.value()));
                item->paintGL(m_renderer, sr, isActiveLook, m_pulseAlpha, ss,
                              contentCornerFactor);
                m_renderer.setGlobalAlpha(static_cast<float>(m_contentOpacity));
            } else {
                item->paintGL(m_renderer, sr, isActiveLook, m_pulseAlpha, ss,
                              contentCornerFactor);
            }
        }
        m_renderer.setContentDim(0.0f);
        m_renderer.setChromeMode(Renderer::ChromeMode::All);
        if (thumbnailUploadScheduled)
            --thumbnailUploadBudget;

        // アンビエント発光用の平均色。いま実際に画面へ出したテクスチャ
        // （item自身が控えた glassContentSource）から採る。新しい
        // キャプチャもFBOも増やさない。間引きは CanvasVideo 側の判断。
        // パフォーマンス優先では発光を出さないので、読み戻しもしない。
        //
        // 2026-09-14: 読み戻しはPBO＋フェンスの非同期に変えた。ここでは
        // 命令を積むだけで、結果を受け取るのは後続フレームの
        // collectAmbientSample()（CanvasWidget.hのAmbientReadback参照）。
        if (auto *ambientVideo = glassEnabled ? canvasItemAsVideo(item) : nullptr) {
            if (ambientVideo->ambientSampleDue()) {
                const GlassContentSource src = ambientVideo->glassContentSource();
                if (src.isValid())
                    issueAmbientSample(ambientVideo, src.textureId);
            }
        }

        // Qualityでは、直前にitem自身が実際に表示した動画/画像テクスチャを
        // ガラスへ直接渡す。画面キャプチャを増やさず、コンテンツだけの色を
        // 内周反射・外周透過光へ利用する。ソースを持たないアイテムは無効値。
        // 2026-10-11: ランチャーのアイコンはグミのときだけ渡す（標準・削り出しの見た目は従来どおり）。
        const GlassContentSource glassContent =
            qualityRefraction && !refractionOnlyDebug
            && (!isLauncherBubble || gummyGlass)
                ? item->glassContentSource() : GlassContentSource{};

        // 疑似ガラスハイライト。board/radius/bezelは各アイテム描画と
        // 同じ比例計算を使い、枠線・グローと同じ角丸矩形に重ねる。
        // （本体は drawGlassLighting。グミのランチャーはアイコンより先に描き済み）
        m_gpuProfiler.enter(GpuProfiler::Section::Glass);
        if (glassEnabled && !gummyLauncherUnderlay)
            drawGlassLighting(glassContent);
    }
    m_renderer.clearShapeFusion();
    m_glassHighlight.clearFusion();
#ifndef NDEBUG
    __closeItemCost(-1);   // 最後のアイテムぶんを確定させる
#endif
    m_gpuProfiler.enter(GpuProfiler::Section::Glass);
    flushGlassBatch();
    m_gpuProfiler.enter(GpuProfiler::Section::Overlay);
    // 2026-10-12: 下地の焼き付け。大きさが変わっている途中で引き伸ばして貼ったものが
    // あれば、止まった後に焼き直せるよう少し後に1回描き直す（予約は1つだけ）。
    m_bubbleLayers.endFrame();
    if (m_bubbleLayers.needsSharpenFrame() && !m_bubbleLayersSharpenPending) {
        m_bubbleLayersSharpenPending = true;
        QTimer::singleShot(170, this, [this]() {
            m_bubbleLayersSharpenPending = false;
            update();
        });
    }
    m_renderer.setGlobalAlpha(static_cast<float>(m_contentOpacity));
    if (thumbnailUploadsDeferred)
        requestFullRender(CanvasRenderReason::Scene);
    const qint64 __tItems = __perfTimer.elapsed();

    // 2026-09-23: 停止中のアクティブ動画の暗さは、上から黒い四角を重ねるのを
    // やめ、item->paintGL の中で中身の色そのものを暗くする（Renderer::setContentDim）。
    // 黒い四角は融合で中身を溶かしても形が変わらず、境目として残っていた。

    // Window-space UI is painted by CanvasOverlayWidget. Keeping playback
    // it out of this OpenGL pass lets progress/fade updates avoid presenting
    // the whole canvas framebuffer.
    const qint64 __tWindowOverlaySplit = __perfTimer.elapsed();

    // 範囲選択のマーキー矩形・選択中バブルのハイライト枠はバブルより手前に描く。
    paintSelectionOverlay();

    // Focus操作フィードバックはCanvasOverlayWidgetで描く。
    const qint64 __tSelection = __perfTimer.elapsed();

    // 通常の境界線はCanvasOverlayWidget。背景反転が必要な強グローだけGL。
    paintStrongBorderGlow();
    const qint64 __tStrongGlow = __perfTimer.elapsed();

    // Launcher tooltipはCanvasOverlayWidgetで描く。

    // UIの背後ぼかし。島などのQWidgetはQPainter描画でシェーダーを使えないため、
    // 下地であるこのGLパスの側で該当矩形をぼかしておき、UIはその上へ半透明で
    // 重ねる(設計と使い方はBlur.h参照)。すべての描画が終わった後に実行する。
    // 2026-09-16: ここまでがシーンレイヤー。UIが動いている間だけ絵を取っておき、
    // 次に Backdrop だけのフレームが来たら貼って済ませる（paintGL 冒頭参照）。
    // MSAAのFBOからの blit はそのまま resolve を兼ねる。
    if (m_backdropActivityClock.isValid()
        && m_backdropActivityClock.elapsed() <= 2000) {
        m_sceneLayer.captureCurrentFramebuffer(physW, physH, 0);
    } else {
        m_sceneLayer.invalidate();
    }

    m_gpuProfiler.enter(GpuProfiler::Section::Backdrop);
    BackdropBlur::instance().renderAll(this, devicePixelRatioF(), physW, physH,
                                       m_sceneLayer.validTexture());

    m_gpuProfiler.endFrame();
    drawPerfOverlay();
    m_renderer.end();
    const qint64 __tEnd = __perfTimer.elapsed();

#ifndef NDEBUG
    const QString __renderReport =
        m_renderScheduler.recordPresented(
            __renderRequest, __tEnd, reuseStaticPrefix);
    if (!__renderReport.isEmpty())
        qDebug().noquote() << __renderReport;
#endif

    // 2026-09-14: しきい値を30msから16msへ下げた。60fpsの1フレームは16.7ms
    // なので、30msだと「2フレーム以上落ちたとき」しか出ず、体感の
    // 「もっさり」（16〜30ms＝40fps前後）が一度もログに出てこなかった。
    if (__tEnd >= 16) {
        qDebug("[PerfDiag][paintGL] total=%lldms begin=%lld grid=%lld flowBg=%lld frames=%lld "
               "items(x%d)=%lld overlaySplit=%lld selection=%lld strongGlow=%lld rendererEnd=%lld "
               "winSize=%dx%d",
               (long long)__tEnd,
               (long long)__tBegin,
               (long long)(__tGrid - __tBegin),
               (long long)(__tFlowBg - __tGrid),
               (long long)(__tFrames - __tFlowBg),
               m_items.size(),
               (long long)(__tItems - __tFrames),
               (long long)(__tWindowOverlaySplit - __tItems),
               (long long)(__tSelection - __tWindowOverlaySplit),
               (long long)(__tStrongGlow - __tSelection),
               (long long)(__tEnd - __tStrongGlow),
               width(), height());
#ifndef NDEBUG
        // 2026-09-14: itemsが重かったフレームは「どのアイテムか」まで出す。
        // 上位3件のみ。2ms未満のアイテムは最初から記録していない。
        if (__itemWorstIdx[0] >= 0) {
            QString __worst;
            for (int s = 0; s < 3; ++s) {
                if (__itemWorstIdx[s] < 0)
                    break;
                CanvasItem *wi = m_items.at(__itemWorstIdx[s]);
                QString tag = QStringLiteral("other");
                if (auto *wv = canvasItemAsVideo(wi))
                    tag = wv->isPlaying() ? QStringLiteral("video/playing")
                                          : QStringLiteral("video/stopped");
                else if (dynamic_cast<CanvasLauncher *>(wi))
                    tag = QStringLiteral("launcher");
                else if (dynamic_cast<CanvasImage *>(wi))
                    tag = QStringLiteral("image");
                if (!__worst.isEmpty())
                    __worst += QStringLiteral(" | ");
                __worst += QStringLiteral("#%1 %2 %3ms")
                               .arg(__itemWorstIdx[s])
                               .arg(tag)
                               .arg((long long)__itemWorstMs[s]);
            }
            qDebug().noquote()
                << QStringLiteral("[PerfDiag][paintGL][items] ") + __worst;
        }
#endif
    }
}

// 2026-10-12: GpuProfiler の集計を左上に出す（Shift+P）。計測区間の外で描くので
// この表示自体の描画は数字に含まれない。表示は約0.5秒ごとに変わる。
void CanvasWidget::drawPerfOverlay()
{
    if (!m_perfOverlay)
        return;
    QStringList lines = m_gpuProfiler.summaryLines();
    if (lines.isEmpty())
        return;
    lines << QStringLiteral("下地の焼き付け(Shift+K) %1  貼る %2 / 焼き直し %3 / その場で描画 %4  (%5枚 %6MB)")
                 .arg(m_bubbleLayers.isEnabled() ? QStringLiteral("ON") : QStringLiteral("OFF"))
                 .arg(m_bubbleLayers.hitsThisFrame())
                 .arg(m_bubbleLayers.bakesThisFrame())
                 .arg(m_bubbleLayers.stretchedThisFrame())
                 .arg(m_bubbleLayers.entryCount())
                 .arg(double(m_bubbleLayers.bytes()) / (1024.0 * 1024.0), 0, 'f', 1);
    lines << QStringLiteral("ガラスのまとめ描き(Shift+I) %1  まとめ %2個を%3回で / 1個ずつ %4個")
                 .arg(m_glassHighlight.batchingEnabled() ? QStringLiteral("ON") : QStringLiteral("OFF"))
                 .arg(m_glassHighlight.statBatched())
                 .arg(m_glassHighlight.statFlushes())
                 .arg(m_glassHighlight.statSingles());
    lines << QStringLiteral("ライト固定(Shift+L) %1")
                 .arg(m_lightLocked ? QStringLiteral("ON") : QStringLiteral("OFF"));
    QFont font(QStringLiteral("Yu Gothic UI"));
    font.setPixelSize(13);
    font.setBold(true);
    const QFontMetrics fm(font);
    const double lineH = fm.height() + 2.0;
    double maxW = 0.0;
    for (const QString &l : lines)
        maxW = qMax(maxW, double(fm.horizontalAdvance(l)));
    const QRectF bg(10.0, 10.0, maxW + 20.0, lineH * lines.size() + 14.0);
    const float prevAlpha = 1.0f;
    m_renderer.setGlobalAlpha(prevAlpha);
    m_renderer.drawRect(bg, QColor(0, 0, 0, 185), Qt::transparent, 0.0f, 8.0f);
    double y = bg.top() + 7.0;
    for (int i = 0; i < lines.size(); ++i) {
        const QColor c = i == 0 ? QColor(140, 195, 255) : QColor(235, 238, 245);
        m_renderer.drawText(lines.at(i), QPointF(bg.left() + 10.0, y), font, c, 1.0f);
        y += lineH;
    }
    // 表示中は数字を更新し続けたいので、止まっていても0.5秒ごとに描き直す。
    // 2026-10-12: 以前は毎フレーム予約を積んでいたため、表示しているだけで
    // 描き直しが止まらなくなっていた（止まっているときの数字が測れない）。予約は1つだけ。
    if (!m_perfOverlayRefreshPending) {
        m_perfOverlayRefreshPending = true;
        QTimer::singleShot(500, this, [this]() {
            m_perfOverlayRefreshPending = false;
            if (m_perfOverlay) update();
        });
    }
}

void CanvasWidget::setDimBackgroundEnabled(bool enabled)
{
    if (m_dimBackgroundEnabled == enabled) return;
    m_dimBackgroundEnabled = enabled;
    update();
}

void CanvasWidget::paintGrid()
{
    if (!m_showGrid) return;

    // 2026-07-17: 以前はグリッド交点1つ1つをdrawCircle()（呼び出しごとに
    // VBO再アップロード+ドローコール）で打っていたため、カメラを引いて
    // 交点数が増えるほど重くなっていた。Apple Freeform等と同様、画面全体を
    // 覆う四角形1枚だけをフラグメントシェーダーで描き、各ピクセルの色を
    // 「対応するキャンバス座標から最寄りの格子点までの距離」で決める方式
    // （drawDotGrid）に変更。ドローコールは常に1回なので、ズームアウトして
    // 交点がいくら増えても負荷が変わらない。間隔が画面上で潰れる領域は
    // シェーダー内でなめらかにフェードアウトするため、以前の「間隔を段階的
    // に間引く」方式のような見た目のガタつきも出ない。
    const double gs = qMax(10, m_gridSize);
    QColor gc(160,160,160,80);
    const float dotRadius = 1.5f;
    m_renderer.drawDotGrid(QRectF(0,0,width(),height()), m_cameraPos, (float)m_cameraZoom, (float)gs, dotRadius, gc);
}

// 2026-09-14 D-2: 外周の白線。
//
// 【変更】カーソルとの距離で濃さを変えるのをやめ、常に同じ濃さにした。
// 理由は見た目の好みではなく実測で、
//   (1) syncLogicMedium がカーソルの移動を見て 60Hz で
//       borderOverlayUpdateRequested() を出していた。この再描画は
//       paintEvent 全体を通るので、ラベルのパネルも再生バーも巻き込んで
//       描き直していた（スクロール中はカーソルが動いているので常時）。
//   (2) この線はウィンドウの外周をぐるっと一周するので、コストが周長に
//       比例する。実測 border=0.40ms(707x476) → 1.21ms(1821x1317)。
//       UI本体は窓の大きさでほぼ変わらないのに、ここだけ3倍だった。
// 濃さを固定すれば (1) の更新要求そのものを消せる（＝0Hz）。
// (2) は「ダメージ領域が外周の帯に掛かっていないフレームでは描かない」
// ことと、パスを窓サイズごとに使い回すことで実質ゼロにする。
//
// 濃さはこの定数ひとつ。変えたければここだけ触ればよい
// （従来はカーソルが縁にあるとき120、離れているとき20だった）。
constexpr int kBorderLineAlpha = 40;
// 線の太さ + アンチエイリアスのはみ出しぶん。ダメージ判定の帯の幅。
constexpr int kBorderDamageBand = 8;

void CanvasWidget::paintBorderOverlay(QPainter &painter, const QRegion &damage)
{
    // 強グローはGL側で背景を反転して描く。QWidget側の白線を重ねると外縁が
    // 白く濁るため、強グローが見えている間だけ通常線を隠す。
    if (m_strongBorderGlowAlpha > 0.001)
        return;
    if (width() <= 0 || height() <= 0)
        return;

    // ---- 外周の帯に掛かっていないなら描かない（D-2）----
    // ラベルのスクロールやイコライザだけが理由のフレームでは、ダメージは
    // 板の周りだけなので外周の線は1画素も見えない。それでも毎回パスを
    // 作ってストロークしていたのが、全画面で効いていた。
    if (!damage.isEmpty()) {
        const QRect r = rect();
        QRegion band(QRect(0, 0, r.width(), kBorderDamageBand));
        band += QRect(0, r.height() - kBorderDamageBand,
                      r.width(), kBorderDamageBand);
        band += QRect(0, 0, kBorderDamageBand, r.height());
        band += QRect(r.width() - kBorderDamageBand, 0,
                      kBorderDamageBand, r.height());
        if (!damage.intersects(band))
            return;
    }

    // ---- パスは窓サイズごとに作り直すだけ（毎フレーム作らない）----
    const double m = 1.5;
    if (m_borderPathSize != size()) {
        // ウィンドウの角丸(kWindowCornerRadius)に合わせて丸角の矩形パスを描く。
        // 直線4本で四角くつないでいた旧実装は、角丸を他所でいくら大きくしても
        // ここだけ直角のまま=尖って見える原因になっていたため、パスにしてある。
        const double insetRadius = qMax(0.0, kWindowCornerRadius - m);
        m_borderPath = QPainterPath();
        m_borderPath.addRoundedRect(QRectF(m, m, (double)width() - 2.0 * m,
                                           (double)height() - 2.0 * m),
                                    insetRadius, insetRadius);
        m_borderPathSize = size();
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setOpacity(painter.opacity() * m_contentOpacity);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, kBorderLineAlpha), 2.0));
    painter.drawPath(m_borderPath);
    painter.restore();
}

void CanvasWidget::paintStrongBorderGlow()
{
    if (m_strongBorderGlowAlpha <= 0.001)
        return;

    const int strongAlpha = static_cast<int>(qBound(
        0.0, kStrongBorderGlowMaxAlpha * m_strongBorderGlowAlpha, 255.0));
    const double strongLw = 2.0 + 10.0 * m_strongBorderGlowAlpha;
    const QRectF outer(0.0, 0.0, static_cast<double>(width()),
                      static_cast<double>(height()));
    const double feather = qMax(3.0, strongLw * 0.7);

    // GLのdestination-colorを参照する反転合成。透明な兄弟QWidgetからは
    // 下のQOpenGLWidget画素を参照できないため、この演出だけはここに残す。
    m_renderer.beginInvertBlend();
    m_renderer.drawRoundedRectStroke(
        outer, QColor(255, 255, 255, strongAlpha),
        kWindowCornerRadius, strongLw, feather);
    m_renderer.endInvertBlend();
}

// ホイール中クリックのダブルクリックによる原寸リセット（resetFocusCameraToNative()）
// が発生した瞬間に呼ぶ。ウィンドウ外周をGLで反転する強グローを開始する。
// 「窓の縁に白い枠を描く」仕組みを流用しつつ、より強いアルファ値で即座に
// 発光させ、約0.5秒保持したのち滑らかにフェードアウトさせる。
void CanvasWidget::triggerStrongBorderGlow()
{
    m_strongBorderGlowAnim->stop();
    m_strongBorderGlowAlpha = 1.0;
    // 間引きクロックを畳んでおき、開始直後の1コマ目は必ず描く。
    m_strongGlowRenderClock.invalidate();
    m_strongBorderGlowAnim->start();
    requestFullRender(CanvasRenderReason::UiAnimation);
    emit borderOverlayUpdateRequested();
}

void CanvasWidget::showSeekIndicator(int dir)
{
    m_seekIndicatorDir   = dir;
    m_seekIndicatorAlpha = 1.0;
    m_seekIndicatorAnim->stop();
    m_seekIndicatorAnim->setStartValue(1.0);
    m_seekIndicatorAnim->setEndValue(0.0);
    m_seekIndicatorAnim->start();
    emit focusOverlayUpdateRequested();
}

// 動画全画面中の操作フィードバック（2倍速表示・5秒スキップの矢印）。
// アニメーション状態は描画から分離し、既存の60Hzロジックで進める。
// 2026-07-15: MainWindow::isOnUI()のうち「再生バー」「音量スライダー」
// 判定部分だけをCanvasWidget側でも参照できるように切り出したもの。
// 島・レイヤーバーはMainWindow側の管轄なのでここでは見ない
// （再生バー自動非表示の判定にはそもそも無関係のため）。
// 2026-09-13: テクスチャの色を、上下左右に分けて読む。
//
// ミップマップを作らせて「両辺が2以上ある中でいちばん小さい段」を読む。
// 427x240 のテクスチャなら 6x3 = 18テクセル（72バイト）程度。全画素を
// 読んでCPUで平均するより桁違いに安く、しかも1x1の平均と違って
// 「左は青い / 右は赤い」という向きの情報が残る。
//
// 【注意】GPUからCPUへの読み戻しは同期点になる。CanvasVideo側で
// 150msに1回まで間引いているので、この関数を毎フレーム呼ばないこと。
// 読み戻した粗いミップ段（lw×lh のRGBA8）から、全体・左右・上下の平均色を作る。
// ここにはGL呼び出しが1つも無い＝待ちが発生しない。
static CanvasVideo::AmbientSample averageAmbientTexels(const unsigned char *buf,
                                                       int lw, int lh)
{
    CanvasVideo::AmbientSample out;
    if (!buf || lw < 2 || lh < 2)
        return out;

    // 全体・左半分・右半分・上半分・下半分で足し合わせる。
    struct Acc { double r = 0, g = 0, b = 0; int n = 0; };
    Acc all, left, right, top, bottom;
    const auto add = [](Acc &a, const unsigned char *p) {
        a.r += p[0]; a.g += p[1]; a.b += p[2]; ++a.n;
    };
    int opaque = 0;
    for (int y = 0; y < lh; ++y) {
        for (int x = 0; x < lw; ++x) {
            const unsigned char *p = buf + (y * lw + x) * 4;
            if (p[3] < 8)
                continue;      // まだ中身の無いテクスチャ
            ++opaque;
            add(all, p);
            if (x <  lw / 2)      add(left,   p);
            if (x >= (lw + 1) / 2) add(right, p);
            if (y <  lh / 2)      add(top,    p);
            if (y >= (lh + 1) / 2) add(bottom, p);
        }
    }
    if (opaque == 0 || all.n == 0)
        return out;

    const auto toColor = [](const Acc &a) {
        if (a.n <= 0)
            return QColor();
        return QColor(qBound(0, qRound(a.r / a.n), 255),
                      qBound(0, qRound(a.g / a.n), 255),
                      qBound(0, qRound(a.b / a.n), 255));
    };
    out.average = toColor(all);
    out.left    = toColor(left);
    out.right   = toColor(right);
    out.top     = toColor(top);
    out.bottom  = toColor(bottom);
    out.valid   = out.average.isValid();
    return out;
}

// 読み戻しの発行。glGetTexImage()の宛先をPBOにすると、この呼び出しは
// 「あとでここへ書いておいて」と命令を積むだけで即座に返る
// （CanvasWidget.h の AmbientReadback のコメント参照）。
void CanvasWidget::issueAmbientSample(CanvasVideo *video, unsigned int texId)
{
    if (!video || texId == 0)
        return;
    // 同時に飛ばすのは1件だけ。150ms間隔なので、動画が複数あっても
    // 順番に回れば十分に間に合う。
    if (m_ambientReadback.pending)
        return;

    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, texId);

    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 1 || h <= 1) {
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
        return;
    }

    // 両辺が2以上ある最小の段を探す（＝いちばん粗いが向きは残る段）。
    // 念のため8x8を上限にして、読み戻す量が増えすぎないようにする。
    int level = 0, lw = w, lh = h;
    while (lw > 8 || lh > 8) {
        if ((lw >> 1) < 2 || (lh >> 1) < 2)
            break;
        lw >>= 1; lh >>= 1; ++level;
    }
    while (lw > 2 && lh > 2 && (lw > 4 || lh > 4)) {
        if ((lw >> 1) < 2 || (lh >> 1) < 2)
            break;
        lw >>= 1; lh >>= 1; ++level;
    }
    if (lw < 2 || lh < 2) {
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
        return;
    }

    if (m_ambientReadback.pbo == 0) {
        glGenBuffers(1, &m_ambientReadback.pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_ambientReadback.pbo);
        // 段の上限は8x8なので、この大きさで足りる（作り直しは不要）。
        glBufferData(GL_PIXEL_PACK_BUFFER, 8 * 8 * 4, nullptr, GL_STREAM_READ);
    } else {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_ambientReadback.pbo);
    }

    glGenerateMipmap(GL_TEXTURE_2D);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // PBOがバインドされている間、最後の引数はポインタではなくバッファ内の
    // オフセットとして解釈される（＝クライアントメモリへは書かない）。
    glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));

    if (m_ambientReadback.fence) {
        glDeleteSync(m_ambientReadback.fence);
        m_ambientReadback.fence = nullptr;
    }
    m_ambientReadback.fence   = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    m_ambientReadback.owner   = video;
    m_ambientReadback.width   = lw;
    m_ambientReadback.height  = lh;
    m_ambientReadback.pending = true;
}

// 発行済みの読み戻しが完了していれば取り込む。フェンスをタイムアウト0で
// 見るだけなので、まだ終わっていなければ即座に戻る（＝待たない）。
void CanvasWidget::collectAmbientSample()
{
    if (!m_ambientReadback.pending)
        return;
    if (m_ambientReadback.fence) {
        const GLenum status =
            glClientWaitSync(m_ambientReadback.fence, 0, 0);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
            return; // 次のフレームで見に来る
        glDeleteSync(m_ambientReadback.fence);
        m_ambientReadback.fence = nullptr;
    }

    const int lw = m_ambientReadback.width;
    const int lh = m_ambientReadback.height;
    CanvasVideo *owner = m_ambientReadback.owner.data();
    m_ambientReadback.pending = false;
    m_ambientReadback.owner = nullptr;
    if (!owner || m_ambientReadback.pbo == 0)
        return;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_ambientReadback.pbo);
    const auto *mapped = static_cast<const unsigned char *>(
        glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
    CanvasVideo::AmbientSample sample;
    if (mapped)
        sample = averageAmbientTexels(mapped, lw, lh);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // 無効でも渡す（CanvasVideo側が次回の採取時刻を更新するため）。
    owner->setAmbientSampledColor(sample);
}

void CanvasWidget::releaseAmbientReadback()
{
    if (m_ambientReadback.fence) {
        glDeleteSync(m_ambientReadback.fence);
        m_ambientReadback.fence = nullptr;
    }
    if (m_ambientReadback.pbo) {
        glDeleteBuffers(1, &m_ambientReadback.pbo);
        m_ambientReadback.pbo = 0;
    }
    m_ambientReadback.pending = false;
    m_ambientReadback.owner = nullptr;
}

int CanvasWidget::topOverlayObstacleLeft() const
{
    // 島はこのウィジェットの兄弟（同じウィンドウの子）なので、ウィンドウ
    // 経由で探してこちらの座標系へ移す。見つからない/隠れている場合は
    // 「邪魔なものは無い」としてwidth()を返す。
    const QWidget *top = window();
    if (!top)
        return width();
    const DynamicIsland *island = top->findChild<const DynamicIsland *>();
    if (!island || !island->isVisible())
        return width();

    // 2026-09-12: 島が畳まれる方向に入った瞬間から「もう邪魔ではない」と
    // 扱う。不透明度が0になるのを待つと、島が消えていく間ずっとリストが
    // 畳まれたままで、譲る必要が無くなってから開き直すまでが間延びして
    // 見える。畳み始めと同時に戻り始めれば、入れ替わりが1つの動きになる。
    if (island->isRetracting())
        return width();

    // 【重要】島は setVisible(false) では隠れない。DynamicIslandController は
    // QGraphicsOpacityEffect の不透明度を 0 へフェードして見えなくする方式
    // （DynamicIsland.cpp の paintEvent / tick のコメント参照）なので、
    // isVisible() は出ていない間も常に true になる。実際に見えているかは
    // 不透明度で判定しないと「島が無いのに有る」扱いになってしまう。
    if (const auto *effect = qobject_cast<const QGraphicsOpacityEffect *>(
            island->graphicsEffect())) {
        if (effect->opacity() < 0.05)
            return width();
    }

    const QPoint islandTopLeft =
        mapFromGlobal(island->mapToGlobal(QPoint(0, 0)));
    return islandTopLeft.x();
}

bool CanvasWidget::isCursorOverPlaybackBarArea(const QPoint &local) const
{
    auto *av = activeVideo();
    if (!av)
        return false;
    if (m_playbarVisible && local.y() >= height() - 56 - 20)
        return true;
    if (av->isVolSliderVisible() && av->volSliderScreenRect(height()).contains(local))
        return true;
    // 2026-09-12: 左上のフォーカスラベルもクリック可能なUIになったため、
    // その上にカーソルを置いている間は自動非表示のフェードを進めない
    // （押そうとして手を止めた瞬間に消えるのを防ぐ。シークバーと同じ扱い）。
    // 2026-09-12追記: 押せるラベル（＝展開できるList）のときだけ。押しても
    // 何も起きないラベルの上でUIを留めておく理由はない。
    if (av->labelPanelSupported() && av->labelScreenRect().contains(local))
        return true;
    // コントローラのパネルも同様（開いている間は勝手に消えないように）。
    if (av->labelPanelScreenRect().contains(local))
        return true;
    // 固定ラベル（別のバブルのものが左上に出ていることがある）。
    if (auto *owner = labelOwnerVideo()) {
        if (owner != av && (owner->labelScreenRect().contains(local)
                            || owner->labelPanelScreenRect().contains(local)))
            return true;
    }
    return false;
}

bool CanvasWidget::updateFocusFeedbackAnimation(int dtMs)
{
    bool changed = false;
    const double frameMs = qBound(0.0, static_cast<double>(dtMs), 100.0);

    if (m_longPressActive != m_speedBadgeWasActive) {
        if (m_longPressActive) {
            m_speedBadgePopActive = true;
            m_speedBadgePopElapsedMs = 0.0;
            m_speedBadgeScale = 0.0f;
        } else {
            m_speedBadgePopActive = false;
        }
        m_speedBadgeWasActive = m_longPressActive;
        changed = true;
    }

    if (m_longPressActive) {
        if (m_speedBadgePopActive) {
            m_speedBadgePopElapsedMs += frameMs;
            static QEasingCurve s_speedBadgePopEase(QEasingCurve::OutBack);
            const double durationMs = 300.0;
            const double t = qBound(0.0, m_speedBadgePopElapsedMs / durationMs, 1.0);
            m_speedBadgeScale = static_cast<float>(s_speedBadgePopEase.valueForProgress(t));
            if (t >= 1.0) {
                m_speedBadgePopActive = false;
                m_speedBadgeScale = 1.0f;
            }
            changed = true;
        }
        const float previousAlpha = m_speedBadgeAlpha;
        m_speedBadgeAlpha += (1.0f - m_speedBadgeAlpha)
                              * static_cast<float>(qMin(1.0, 14.0 * frameMs / 1000.0));
        if (m_speedBadgeAlpha > 0.995f)
            m_speedBadgeAlpha = 1.0f;
        changed = changed || m_speedBadgeAlpha != previousAlpha;
    } else if (m_speedBadgeAlpha > 0.0f) {
        m_speedBadgeScale = 1.0f;
        const float previousAlpha = m_speedBadgeAlpha;
        const float k = static_cast<float>(qMin(1.0, 12.0 * frameMs / 1000.0));
        m_speedBadgeAlpha -= m_speedBadgeAlpha * k;
        if (m_speedBadgeAlpha < 0.01f)
            m_speedBadgeAlpha = 0.0f;
        changed = changed || m_speedBadgeAlpha != previousAlpha;
    }

    return changed;
}

void CanvasWidget::paintFocusOverlay(QPainter &painter,
                                     BackdropBlur::Regions *blurRegions)
{
    if (!canvasItemAsVideo(m_focusedItem))
        return;

    const double W = width(), H = height();
    painter.save();
    painter.setOpacity(painter.opacity() * m_contentOpacity);

    if (m_speedBadgeAlpha > 0.001f) {
        const double baseIconW = 84.0, baseIconH = baseIconW * 18.878 / 27.961;
        const QPointF center(W / 2.0, 18.0 + baseIconH / 2.0);
        const double iconW = baseIconW * m_speedBadgeScale;
        const double iconH = baseIconH * m_speedBadgeScale;
        // 背後ぼかし。半径をフェードに比例させることで、出現/消滅に合わせて
        // ぼかしも滑らかに立ち上がる(0になった時点で自動的に無効化される)。
        if (blurRegions) {
            // ico_x2.svgの中で実際に見えているピルは、アイコン枠いっぱいでは
            // なく viewBox(27.961x18.878) の中の
            // rect(x=0, y=8.709, w=25.169, h=10.169, rx=5.084) の部分だけ。
            // 枠全体をピル扱いすると、上に余白ぶんはみ出し・右にずれた形の
            // ブラーになってしまうため、SVG内の比率でピルの矩形を出す。
            constexpr double kVbW = 27.961, kVbH = 18.878;
            constexpr double kPillX = 0.0, kPillY = 8.709;
            constexpr double kPillW = 25.169, kPillH = 10.169;
            const QRectF iconRect(center.x() - iconW / 2.0,
                                  center.y() - iconH / 2.0, iconW, iconH);
            const QRectF pillRect(iconRect.left() + iconW * (kPillX / kVbW),
                                  iconRect.top()  + iconH * (kPillY / kVbH),
                                  iconW * (kPillW / kVbW),
                                  iconH * (kPillH / kVbH));
            blurRegions->add(pillRect, pillRect.height() * 0.5,
                             BackdropBlur::kDefaultUiBlurRadius,
                             painter.opacity() * m_speedBadgeAlpha);
        }
        painter.save();
        painter.setOpacity(painter.opacity() * m_speedBadgeAlpha);
        painter.drawImage(
            QRectF(center.x() - iconW / 2.0, center.y() - iconH / 2.0, iconW, iconH),
            speedBadgeImage());
        painter.restore();
    }

    if (m_seekIndicatorDir != 0 && m_seekIndicatorAlpha > 0.01) {
        const bool flipped = m_seekIndicatorDir > 0;
        const double iconSize = 64.0;
        const double cx = flipped ? (W * 0.72) : (W * 0.28);
        const QPointF topLeft(cx - iconSize / 2.0, H / 2.0 - iconSize / 2.0);
        // 2026-09-11: 5秒シークのインジケータはブラーを掛けない。
        // back.svgの円はviewBox中心から少しずれた位置(cx=16.811, r=12.745)に
        // あり、アイコン枠と一致しないため領域がずれて見えるうえ、そもそも
        // 一瞬しか出ない演出なので費用対効果が薄い。
        painter.save();
        painter.setOpacity(painter.opacity()
                           * qBound(0.0, m_seekIndicatorAlpha, 1.0));
        painter.drawImage(QRectF(topLeft.x(), topLeft.y(), iconSize, iconSize),
                          seekIndicatorImage(flipped));
        painter.restore();
    }
    painter.restore();
}

void CanvasWidget::clearLauncherTooltip()
{
    if (!m_launcherTooltipItem && !m_launcherTooltipVisible && m_launcherTooltipHoverMs == 0)
        return;
    m_launcherTooltipItem = nullptr;
    m_launcherTooltipHoverMs = 0;
    m_launcherTooltipVisible = false;
    emit launcherTooltipOverlayUpdateRequested();
}

void CanvasWidget::updateLauncherTooltipHover(const QPoint &cursorPos, int dtMs)
{
    if (!underMouse() || !m_activeMode.isEmpty() || m_focusedItem || m_pendingFocusItem) {
        clearLauncherTooltip();
        return;
    }

    const QPointF canvasPos = screenToCanvas(cursorPos);
    CanvasLauncher *hitLauncher = nullptr;
    for (auto it = m_items.rbegin(); it != m_items.rend(); ++it) {
        auto *launcher = dynamic_cast<CanvasLauncher*>(*it);
        if (launcher && launcher->boardRect().contains(canvasPos)) {
            hitLauncher = launcher;
            break;
        }
    }

    if (!hitLauncher) {
        clearLauncherTooltip();
        return;
    }

    const int moveDistance = (cursorPos - m_launcherTooltipCursor).manhattanLength();
    if (hitLauncher != m_launcherTooltipItem || moveDistance > 2) {
        m_launcherTooltipItem = hitLauncher;
        m_launcherTooltipCursor = cursorPos;
        m_launcherTooltipHoverMs = 0;
        if (m_launcherTooltipVisible) {
            m_launcherTooltipVisible = false;
            emit launcherTooltipOverlayUpdateRequested();
        }
        return;
    }

    const bool wasVisible = m_launcherTooltipVisible;
    m_launcherTooltipHoverMs += qMax(0, dtMs);
    m_launcherTooltipVisible = m_launcherTooltipHoverMs >= 500;
    if (m_launcherTooltipVisible != wasVisible)
        emit launcherTooltipOverlayUpdateRequested();
}

QRect CanvasWidget::launcherTooltipOverlayRect() const
{
    if (!m_launcherTooltipVisible || !m_launcherTooltipItem)
        return {};

    QString text = m_launcherTooltipItem->displayName();
    if (text.isEmpty())
        return {};

    QFont font(QStringLiteral("Segoe UI"), 10);
    QFontMetrics fm(font);
    const int maxTextWidth = qMax(120, width() / 3);
    text = fm.elidedText(text, Qt::ElideRight, maxTextWidth);

    const double padX = 8.0;
    const double padY = 5.0;
    const double textW = fm.horizontalAdvance(text);
    const double textH = fm.height();
    QPointF topLeft = QPointF(m_launcherTooltipCursor) + QPointF(14.0, 18.0);
    QRectF box(topLeft.x(), topLeft.y(),
               textW + padX * 2.0,
               textH + padY * 2.0);

    if (box.right() > width() - 8.0)
        box.moveRight(width() - 8.0);
    if (box.bottom() > height() - 8.0)
        box.moveBottom(height() - 8.0);
    if (box.left() < 8.0)
        box.moveLeft(8.0);
    if (box.top() < 8.0)
        box.moveTop(8.0);

    return box.toAlignedRect();
}

void CanvasWidget::paintLauncherOverlay(QPainter &painter)
{
    if (!m_launcherTooltipVisible || !m_launcherTooltipItem)
        return;

    QString text = m_launcherTooltipItem->displayName();
    if (text.isEmpty())
        return;

    const QFont font(QStringLiteral("Segoe UI"), 10);
    const QFontMetrics fm(font);
    text = fm.elidedText(text, Qt::ElideRight, qMax(120, width() / 3));
    const QRectF box(launcherTooltipOverlayRect());
    if (box.isEmpty())
        return;

    constexpr double padX = 8.0;
    constexpr double padY = 5.0;
    painter.save();
    painter.setOpacity(painter.opacity() * m_contentOpacity);
    painter.setPen(QPen(QColor(255, 255, 255, 54), 1.0));
    painter.setBrush(QColor(24, 26, 30, 215));
    painter.drawRoundedRect(box, 4.0, 4.0);
    painter.setFont(font);
    painter.setPen(QColor(245, 247, 252, 235));
    painter.drawText(
        box.adjusted(padX, padY, -padX, -padY),
        Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine,
        text);
    painter.restore();
}


// 2026-09-20: 逆輸入ヒント。FlowModeの暗転と同じく、グリッドの上・バブルの
// 下に全面の黒を敷く（バブルだけが浮いて見える＝掴めば外へ出せる）。
void CanvasWidget::paintExportHintBackground()
{
    const float a = qBound(0.0f, m_exportHintAlpha, 1.0f);
    m_renderer.drawRect(QRectF(0, 0, width(), height()),
                        QColor(0, 0, 0, int(120 * a)), Qt::transparent, 0.0f,
                        static_cast<float>(kWindowCornerRadius));
}

bool CanvasWidget::cursorOverCanvas(QPoint *localPos) const
{
    const QPoint local = mapFromGlobal(QCursor::pos());
    if (localPos) *localPos = local;
    if (!isVisible() || !rect().contains(local))
        return false;
#ifdef Q_OS_WIN
    // 他のウィンドウがPBの上に重なっている場所ではPBの上とみなさない。
    POINT pt;
    if (GetCursorPos(&pt)) {
        const HWND under = WindowFromPoint(pt);
        const QWidget *top = window();
        if (under && top && GetAncestor(under, GA_ROOT) != reinterpret_cast<HWND>(top->winId()))
            return false;
    }
#endif
    return true;
}

bool CanvasWidget::exportHintWanted() const
{
    if (!isVisible())
        return false;
    // 外部ドラッグ中はカーソルがPB外に出ても暗いまま。
    if (m_activeMode == QStringLiteral("external_drag_prepare"))
        return true;
    // ダブルクリックの2回目を押したまま：少し待つか動かしたら暗くする
    // （普通のダブルクリック＝全画面で一瞬だけ暗くなるのを避ける）。
    if (m_activeMode == QStringLiteral("double_press")) {
        if (m_doublePressClock.isValid() && m_doublePressClock.elapsed() >= 140)
            return true;
        const QPointF local = mapFromGlobal(QCursor::pos());
        return (local - m_dragStartScreenPos).manhattanLength() > 3;
    }
    // Shift（単独）押下中。Ctrl/Alt併用は別の操作なので出さない。
    if (m_focusedItem || m_pendingFocusItem || m_items.isEmpty())
        return false;
    const Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (mods != Qt::ShiftModifier)
        return false;
    // 2026-09-20: 何かのドラッグ中（キャンバスのパン・範囲選択など）は
    // Shiftを押している限りそのまま。以前はここで消していたため、Shiftを
    // 押しながらキャンバスを動かすと暗転が一瞬外れていた。ドラッグ中は
    // カーソルがPBの外に出ることもあるので、位置の判定もしない。
    if (!m_activeMode.isEmpty())
        return true;
    return cursorOverCanvas();
}

void CanvasWidget::updateExportHint()
{
    const bool want = exportHintWanted();
    if (want == m_exportHintTarget)
        return;   // 変化なし＝描画しない（ポーリングの大半はここで抜ける）
    m_exportHintTarget = want;
    // フェード途中で反転しても、今の濃さから続けて戻す（跳ばない）。
    m_exportHintFadeFrom = m_exportHintAlpha;
    m_exportHintFadeClock.start();
    if (m_exportHintFadeTimer && !m_exportHintFadeTimer->isActive())
        m_exportHintFadeTimer->start();
}




// ---- 範囲選択のマーキー矩形描画 ----
// 2026-07-08: 選択中バブル個々のハイライト枠は、単体アクティブ時と同じ見た目に
// 統一するため item->paintGL() 側の isActive 分岐（paintGL()呼び出し箇所を参照）
// に一本化した。ここでは範囲選択中のマーキー矩形のみ描画する。
void CanvasWidget::paintSelectionOverlay()
{
    if (m_activeMode == "rect_select") {
        const QRectF screenRect(canvasToScreen(m_rectSelectCurrentCanvas.topLeft()),
                                 canvasToScreen(m_rectSelectCurrentCanvas.bottomRight()));
        // ドラッグ開始直後や開始地点付近まで戻したときの極小矩形は描画しない
        // （見た目上意味がなく、点状のゴミ描画になるだけのため）。
        if (screenRect.width() < 2.0 && screenRect.height() < 2.0)
            return;
        const QColor col(130, 175, 255, 230);
        QColor fill = col; fill.setAlpha(30);
        // 角丸半径は矩形の短辺に収まる範囲へ縮める（Renderer側でもクランプ
        // されるが、小さい矩形で角丸が辺全体を食うのを避け見た目を安定させる）。
        const float baseRadius = 1.0f;
        const float radius = qMin(baseRadius,
            (float)(qMin(screenRect.width(), screenRect.height()) * 0.5));
        m_renderer.drawRect(screenRect, fill, col, 1.5f, radius);
    }
}

// 2026-09-14: サラウンドモード（バブルの画面上の位置に応じた音量減衰）を
// 廃止した。updateSurroundPans() は syncLogicMedium() から60Hzで呼ばれ、
// サラウンドがOFFのときも全バブルの itemScreenRect() を再計算していた。

// ============================================================
//  Phase 6: セッション保存整理
//
//  保存形式は MediaSource 中心（kind / sourceUrl / title）に統一し、
//  そこへアイテム共通の position / size / zOrder と、動画固有の
//  playbackPosition / volume / paused を付加する形とする。
//
//  旧形式（youtubeUrl / filePath / sourceType）は読み込み互換としてのみ
//  restoreSessionFromJson() 側で受理し、新規保存では書き出さない。
// ============================================================

void CanvasWidget::stopAllVideosForBackground()
{
    for (CanvasItem *item : m_items) {
        if (auto *video = canvasItemAsVideo(item))
            video->bcPause();
    }
}

void CanvasWidget::setShowGrid(bool v)
{
    if (m_showGrid == v)
        return;
    m_showGrid = v;
    AppSettings::instance().setShowGrid(v);
    update();
}

void CanvasWidget::setGridSize(int v)
{
    v = qBound(10, v, 400);
    if (m_gridSize == v)
        return;
    m_gridSize = v;
    AppSettings::instance().setGridSize(v);
    update();
}

int CanvasWidget::applyMasterVolumeWheelSteps(int steps)
{
    if (m_masterVolume.applyWheelSteps(steps))
        applyMasterGainToAllVideos();
    return m_masterVolume.value();
}

void CanvasWidget::applyMasterGainToAllVideos()
{
    const float gain = m_masterVolume.gain();
    for (CanvasItem *item : m_items) {
        if (auto *vid = canvasItemAsVideo(item))
            vid->setMasterGain(gain);
    }
}

// 2026-09-14: queueSurroundVolumeApply() / flushSurroundVolumeQueue() も
// サラウンドモード専用だったため削除した。

// 2026-09-14 Phase2: パルス（アクティブ枠の明滅）アニメーションの起動/停止。
//
// 以前はm_pulseAnimをコンストラクタでstart()したきり止めていなかったため、
// 明滅させる対象が1つも無い状態でも約60Hzでコールバックが走り続けていた。
// 対象の有無だけを見て start/stop する。判定はポインタとリストの空判定
// だけなので、60Hzから毎tick呼んでも実質ゼロコスト。
//
// アクティブ／選択の変更経路は多数（クリック・範囲選択・削除・フォーカス・
// Flow・セッション復元など）あるため、個別のsetterへ仕込まずここ1箇所で
// 定期的に確認する方式にしている（仕込み漏れで「止まったまま復帰しない」
// という不具合が出るより安全）。
void CanvasWidget::updatePulseAnimationState()
{
    if (!m_pulseAnim)
        return;
    const bool hasPulseTarget = (m_activeItem != nullptr)
                             || !m_selectedItems.isEmpty();
    const bool running = (m_pulseAnim->state() == QAbstractAnimation::Running);
    if (hasPulseTarget == running)
        return;

    if (hasPulseTarget) {
        // 再開時は間引きクロックも仕切り直す（最初の1フレームを
        // 待たずに描けるようにする）。
        m_pulseRepaintClock.invalidate();
        m_pulseAnim->start();
    } else {
        m_pulseAnim->stop();
        // 停止中の値は使われない（描くのはアクティブ／選択中のバブルだけ）が、
        // 次にアクティブ化された瞬間に前回の途中値から描き始めないよう、
        // 明滅の一番明るい側へ戻しておく。
        m_pulseAlpha = 255.0;
    }
}

// ---- ラベルのリストのスクロールだけを進める（2026-09-14 C-3、120Hz）----
//
// 【なぜ専用タイマーか】syncLogic は Phase4 で 8ms(120Hz) → 16ms(60Hz) へ
// 落としてある（全動画の画面矩形計算・サムネイル常駐のソートがぶら下がって
// いるため）。そこへ相乗りするとスクロールも60Hzのままだし、8msへ戻すと
// Phase4 で削ったぶんが丸ごと復活する。スクロールだけを別の120Hzタイマーで
// 回す。ここでやるのは「位置を進める」ことと「板の矩形だけ再描画を頼む」
// ことだけなので、増える仕事は行の貼り直し(blit≒0.7ms)に限られる。
//
// dt は「前回ここを通ってからの実経過」を渡すので、寄せる速さ・到達時間は
// 60Hzのときと変わらない。変わるのは刻みの細かさだけ。
void CanvasWidget::syncLabelScroll()
{
    double scrollDtSec = 1.0 / 120.0;
    if (m_labelScrollClock.isValid())
        scrollDtSec = qMin(0.05, m_labelScrollClock.restart() / 1000.0);
    else
        m_labelScrollClock.start();

    // 進める対象は「リストを持ちうるバブル」＝アクティブなものと
    // 固定ラベルの持ち主。同じバブルを二度進めないよう重複を弾く
    // （二度進めると寄せる速さが倍になる）。
    CanvasVideo *scrollTargets[2] = {
        canvasItemAsVideo(m_activeItem),
        m_pinnedLabelVideo.data(),
    };
    if (scrollTargets[1] == scrollTargets[0])
        scrollTargets[1] = nullptr;

    bool scrollMoved = false;
    QRegion scrollDamage;
    bool scrollDamageKnown = true;
    for (CanvasVideo *v : scrollTargets) {
        if (!v || !v->tickLabelScroll(scrollDtSec))
            continue;
        scrollMoved = true;
        // 2026-09-14 C-2: スクロールで変わるのは板の中だけ。オーバーレイ
        // 全体を描き直すと全画面で3ms前後かかるので、板の範囲だけを
        // 描き直す（イコライザの部分更新と同じ仕組みに相乗り）。
        // 範囲が分からない（まだ一度も描いていない等）ときだけ全体更新。
        const QRectF damage = v->labelPanelDamageRect();
        if (damage.isEmpty())
            scrollDamageKnown = false;
        else
            scrollDamage += damage.toAlignedRect();
    }
    if (!scrollMoved)
        return;
    if (scrollDamageKnown && !scrollDamage.isEmpty())
        emit playbackOverlayEqualizerUpdateRequested(scrollDamage.boundingRect());
    else
        emit playbackOverlayUpdateRequested();
}

// ---- syncLogic ----
void CanvasWidget::syncLogic()
{
    // 120Hz: カメラのイーズアニメーション（ズーム/パン）の追従精度が
    // 必要な処理のみ。マウスドラッグによる手動パンは別経路
    // （mouseMoveEvent内で直接update()）で既にイベント駆動・タイマー非依存。
    // 2026-07-15: カメラ演出の進行はここ(120Hz)で駆動する。
    tickCameraAnimation();
    updateCanvasFromCamera();
    // 2026-09-22: バブル融合（重ね待ち・吸い込み・膨らみ）
    tickBubbleFusion();

    const QRectF viewport(0, 0, width(), height());
    const QRectF thumbnailPrefetch = viewport.adjusted(
        -width() * 0.75, -height() * 0.75,
         width() * 0.75,  height() * 0.75);
    const QRectF thumbnailEvictionBoundary = viewport.adjusted(
        -width() * 1.75, -height() * 1.75,
         width() * 1.75,  height() * 1.75);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    struct ThumbnailCandidate
    {
        CanvasVideo *video = nullptr;
        bool pinned = false;
        bool visible = false;
        bool prefetch = false;
        bool insideEviction = false;
        double distanceSquared = 0.0;
        quint64 estimatedBytes = 0;
    };
    QVector<ThumbnailCandidate> thumbnailCandidates;
    thumbnailCandidates.reserve(m_items.size());
    const QPointF viewportCenter = viewport.center();

    for (CanvasItem *item : m_items) {
        auto *vid = canvasItemAsVideo(item);
        if (!vid)
            continue;
        const QRectF sr = itemScreenRect(vid);
        vid->notifyScreenRect(sr, viewport);
        if (!vid->hasThumbnailProvider())
            continue;

        const BubbleState state = vid->bubbleState();
        const bool playbackPinned = state != BubbleState::Idle
            && state != BubbleState::Sleep
            && state != BubbleState::FocusSleep
            && state != BubbleState::Ended
            && state != BubbleState::Error;
        ThumbnailCandidate candidate;
        candidate.video = vid;
        candidate.pinned = playbackPinned
            || item == m_activeItem
            || item == m_focusedItem
            || m_selectedItems.contains(item);
        candidate.visible = sr.intersects(viewport);
        candidate.prefetch = sr.intersects(thumbnailPrefetch);
        candidate.insideEviction = sr.intersects(thumbnailEvictionBoundary);
        const QPointF delta = sr.center() - viewportCenter;
        candidate.distanceSquared = delta.x() * delta.x() + delta.y() * delta.y();
        candidate.estimatedBytes = qMax<quint64>(
            qMax(vid->thumbnailCpuBytesEstimate(), vid->thumbnailGpuBytesEstimate()),
            640ULL * 480ULL * 4ULL);
        thumbnailCandidates.append(candidate);
    }

    std::sort(thumbnailCandidates.begin(), thumbnailCandidates.end(),
              [](const ThumbnailCandidate &a, const ThumbnailCandidate &b) {
        if (a.pinned != b.pinned)
            return a.pinned;
        if (a.visible != b.visible)
            return a.visible;
        if (a.prefetch != b.prefetch)
            return a.prefetch;
        return a.distanceSquared < b.distanceSquared;
    });

    constexpr quint64 kThumbnailGpuBudgetBytes = 64ULL * 1024ULL * 1024ULL;
    constexpr int kMaxConcurrentThumbnailRequests = 3;
    quint64 reservedBytes = 0;
    int activeThumbnailRequests = 0;
    for (const ThumbnailCandidate &candidate : std::as_const(thumbnailCandidates)) {
        if (candidate.video->thumbnailResidencyName() == QStringLiteral("Loading"))
            ++activeThumbnailRequests;
    }
    int availableRequestSlots = qMax(
        0, kMaxConcurrentThumbnailRequests - activeThumbnailRequests);
    for (const ThumbnailCandidate &candidate : std::as_const(thumbnailCandidates)) {
        const bool required = candidate.pinned || candidate.visible;
        const bool wantsResidency = required || candidate.prefetch;
        // This is a hard process-side thumbnail residency budget. Candidates
        // are already sorted pinned -> visible -> prefetch -> distance, so when
        // the cap is reached the least important visible items temporarily use
        // placeholders instead of silently exceeding 64 MiB.
        const bool fitsBudget =
            reservedBytes + candidate.estimatedBytes <= kThumbnailGpuBudgetBytes;
        const bool admitted = wantsResidency && fitsBudget;
        if (admitted)
            reservedBytes += candidate.estimatedBytes;

        // 予算から外れたprefetch対象はeviction境界内にいても解放候補へ
        // 回す。10秒の猶予はCanvasVideo側で共通適用されるため、予算境界を
        // カメラが往復しても即時のロード/解放スラッシングにはならない。
        const bool retainedByDistance = candidate.insideEviction
            && !(candidate.prefetch && !admitted);
        const bool mayNeedRequest = candidate.video->thumbnailRequestEligible(nowMs);
        const bool allowNewRequest = admitted && availableRequestSlots > 0;
        if (allowNewRequest && mayNeedRequest)
            --availableRequestSlots;
        candidate.video->updateThumbnailResidency(
            admitted, retainedByDistance, candidate.pinned, nowMs,
            allowNewRequest);
    }

    // update() は各シグナルから呼ばれる。
    // 120Hzループから更新するのは、操作感に直結するカメラ系などだけ。
    // m_pulseAnimは自身のvalueChanged（Qtのアニメーション更新、約60Hz）で
    // 必要なときだけupdate()する。ここにも含めると、パルス対象が無い間も
    // 常時RunningのアニメーションによってCanvas全体が120Hz描画され続ける。
    if (m_camAnimDriver.running)
        requestFullRender(CanvasRenderReason::Camera);
}

// 2026-09-12: 掴んだままreleaseを取りこぼしたドラッグモードの後始末。
// syncLogicMedium()の頭から毎tick呼ばれる。正常時は最初のifで即抜ける。
void CanvasWidget::cancelStaleLabelDragModes()
{
    if (QGuiApplication::mouseButtons() & Qt::LeftButton)
        return; // まだ押されている＝正常なドラッグ中
    cancelLabelDragModes();
}

// ラベル／行のドラッグを、操作を確定させずに取り消す。
// 呼び元は2つ:
//   - cancelStaleLabelDragModes()（releaseの取りこぼし、毎tickの保険）
//   - MainWindow::changeEvent（ウィンドウが非アクティブになった瞬間）
// 掴んだまま他のアプリへ行くとカーソルの移動が届かなくなり、行が最後の
// 位置で固まったまま「継続中」になる。戻ってくると再開するが中途半端なので、
// 非アクティブになった時点で畳んでしまう。
void CanvasWidget::cancelLabelDragModes()
{
    const bool labelDragMode = (m_activeMode == QStringLiteral("labelRowDrag")
                                || m_activeMode == QStringLiteral("labelPull"));
    if (!labelDragMode)
        return;

    const bool wasRowDrag = (m_activeMode == QStringLiteral("labelRowDrag"));
    m_activeMode.clear();

    if (wasRowDrag) {
        if (auto *owner = m_labelRowDragVideo.data())
            owner->cancelRowDrag();      // 並べ替えは確定させない
        m_labelRowDragVideo = nullptr;
        if (m_singleClickTimer)
            m_singleClickTimer->stop();
    } else {
        if (auto *owner = m_labelPullVideo.data())
            owner->cancelLabelPull();    // 固定トグルは起こさない
        m_labelPullVideo = nullptr;
    }

    emit playbackOverlayUpdateRequested();
    update();
}

void CanvasWidget::syncLogicMedium(int dtMs)
{
    // 2026-09-23: ラベルの長押しがたまりきったら、その中身を実行する
    // （ゲージ・待ち・失敗の演出は CanvasVideo 側が持つ）。
    if (auto *holdVideo = m_labelPullVideo.data()) {
        if (holdVideo->takeLabelHoldFire())
            runLabelHoldAction(holdVideo);
    }

    // 60Hz相当: 再生バーのフェード・ボリュームスライダー・サラウンド・
    // FlowModeの状態監視など、120Hzほどの精度が不要な処理。

    // 2026-09-13: アンビエント発光の色を進める。
    // 再生中のバブルは動画フレームで毎フレーム描き直されるので、ここで
    // フレームを要求するのは「止まって消えていく間」だけ
    // （CanvasVideo::ambientGlowNeedsFrames()がその判断を持っている）。
    // パフォーマンス優先のときは発光そのものを出さないので、色も進めない
    // （ガラス表現と同じ線引き。paintGL側のglassEnabledと同じ条件）。
    //
    // 2026-09-14 Phase4: この色送り自体を15Hzへ間引く。
    // 色は指数平滑（時定数: 色0.55秒 / 濃さ0.38秒）でゆっくり寄せるので、
    // 60Hzで刻む必要がない。dtは「前回このブロックを実行してからの実経過」
    // を渡すので、間引いても到達時間・カーブは変わらない。
    // 全アイテムを走査するループごと1/4になる（再生中は発光の描画自体は
    // 動画フレームに便乗しているので、見た目にはここは効かない。効くのは
    // 止まって消えていく間の再描画要求の回数）。
    if (AppSettings::instance().glassEffectQuality()
        != AppSettings::GlassEffectQuality::Performance) {
        constexpr qint64 kAmbientTickIntervalMs = 66;   // 約15Hz
        qint64 ambientElapsedMs = dtMs;
        bool ambientTickDue = true;
        if (!m_ambientTickClock.isValid()) {
            m_ambientTickClock.start();
        } else if (m_ambientTickClock.elapsed() < kAmbientTickIntervalMs) {
            ambientTickDue = false;
        } else {
            ambientElapsedMs = m_ambientTickClock.restart();
        }

        if (ambientTickDue) {
            const double dtSec = qMax<qint64>(0, ambientElapsedMs) / 1000.0;
            for (CanvasItem *item : m_items) {
                auto *video = canvasItemAsVideo(item);
                if (!video)
                    continue;
                const bool wasFading = video->ambientGlowNeedsFrames();
                video->updateAmbientGlow(dtSec);
                if (wasFading || video->ambientGlowNeedsFrames())
                    requestItemRender(item, CanvasRenderReason::UiAnimation);
            }
        }
    }

    // 2026-09-15 E-2: 停止マーク（動画中央のGL描画）のアニメ用フレーム。
    //
    // paintPauseIcon() はアニメの状態を paintGL の中で壁時計ベースに進める
    // ので、フレームが描かれないと止まったままになる。これまでは自分から
    // 要求する仕組みが無く、パルス明滅(約15Hz)への相乗りで動いていた。
    // その結果、
    //   ・非フォーカス … 300msのポップが4〜5コマでカクついていた
    //   ・全画面       … パルスの対象外なので、再生バーのフェード中に
    //                    BackdropBlurの形が変わるついででしか描かれず、
    //                    バーが既に出ていると途中で固まりうる
    //   ・静止中       … アニメが終わっているのに15Hzが来続けていた
    // という3つのズレが同時に起きていた。
    //
    // 要るときだけ60Hzで出す。damageはマークのあるバブルの矩形なので全面には
    // ならない。アニメが終われば needsFrames が false になって自動的に0Hzへ戻る。
    //
    // 2026-09-16 F-0: 30Hzから60Hzへ引き上げ。ポップは300msのOutBackで、
    // 30Hzだと9コマしかない。OutBackは出だしで一気に伸びて行き過ぎてから
    // 戻る曲線なので、山と谷が3〜4コマずつになり「5Hz以下」に見えていた
    // （緩やかに減衰するだけの強グローが30Hzで足りるのとは事情が違う）。
    {
        constexpr qint64 kPauseIconTickIntervalMs = 16;   // 約60Hz
        if (!m_pauseIconTickClock.isValid()
            || m_pauseIconTickClock.elapsed() >= kPauseIconTickIntervalMs) {
            if (m_pauseIconTickClock.isValid())
                m_pauseIconTickClock.restart();
            else
                m_pauseIconTickClock.start();
            for (CanvasItem *item : m_items) {
                auto *video = canvasItemAsVideo(item);
                if (video && video->pauseIconNeedsFrames())
                    requestItemRender(item, CanvasRenderReason::UiAnimation);
            }
        }
    }

    // 2026-09-12: ドラッグモードの取り消し（保険）。
    // ラベル／行を掴んでいる間にウィンドウが非アクティブになる、別アプリが
    // 前に出る等でreleaseイベントが届かないと、m_activeModeが
    // "labelRowDrag"/"labelPull" のまま残り、行がカーソルに追従し続ける。
    // Qtは通常グラブ先へ確実にreleaseを配るので発生確率は低いが、
    // 「左ボタンが押されていないのにドラッグモードが残っている」状態は
    // どう転んでも不正なので、ここで畳んでおく。
    // 操作の確定（並べ替え／固定トグル）はしない。取り消すだけ。
    cancelStaleLabelDragModes();

    // 2026-09-14: ここにあった updateSurroundPans() を削除（サラウンド廃止）。

    // 2026-09-14 Phase2: パルス明滅の対象（アクティブ／選択中のバブル）が
    // あるときだけアニメーションを回す。判定はポインタ比較とリストの
    // 空判定だけ。
    updatePulseAnimationState();

    // 元は8ms固定タイマーの呼び出し回数を数えて概ね30秒おきに出力していた。
    // タイマー周期に依存しないよう、経過時間ベースに変更（挙動は同じ約30秒間隔）。
    static QElapsedTimer memLogClock;
    if (!memLogClock.isValid()) memLogClock.start();
    if (memLogClock.elapsed() >= 30000) {
        memLogClock.restart();
#ifdef Q_OS_WIN
        PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
            qDebug("[Memory][Process] WorkingSet=%lluMB PrivateUsage=%lluMB CommitSize~PrivateUsage=%lluMB PeakWorkingSet=%lluMB PagefileUsage=%lluMB PeakPagefileUsage=%lluMB",
                   (unsigned long long)pmc.WorkingSetSize/(1024*1024),
                   (unsigned long long)pmc.PrivateUsage/(1024*1024),
                   (unsigned long long)pmc.PrivateUsage/(1024*1024),
                   (unsigned long long)pmc.PeakWorkingSetSize/(1024*1024),
                   (unsigned long long)pmc.PagefileUsage/(1024*1024),
                   (unsigned long long)pmc.PeakPagefileUsage/(1024*1024));
        int videoIndex = 0;
        int thumbnailColdCount = 0;
        int thumbnailFailedCount = 0;
        int thumbnailLoadingCount = 0;
        int thumbnailCpuCount = 0;
        int thumbnailGpuCount = 0;
        quint64 thumbnailCpuBytes = 0;
        quint64 thumbnailGpuBytes = 0;
        for (CanvasItem *item : m_items)
            if (auto *vid = canvasItemAsVideo(item)) {
                // 2026-07-15: QMediaPlayerフォールバック撤去に伴い、以前ここで
                // 読んでいたCanvasVideo::textureId()（m_texId、QMediaPlayer
                // 専用の手動アップロードテクスチャ）も削除した。mpv側のFBO
                // テクスチャサイズは直後のvid->logMemoryDiagnostics()の
                // [Memory][MpvPlayer] ログで別途確認できるため、ここでは0のまま
                // 出力する。
                const int tw=0, th=0;
                qDebug("[Memory][Bubble %d] q=%dp state=%s tex=%ux%u (~%lluMB) pos=%llds",
                       videoIndex++,
                       vid->currentQuality(),
                       qPrintable(bubbleStateToString(vid->bubbleState())),
                       tw, th,
                       (unsigned long long)(tw*th*4)/(1024*1024),
                       (long long)(vid->position()/1000));
                vid->logMemoryDiagnostics();
                const QString residency = vid->thumbnailResidencyName();
                if (residency == QStringLiteral("Cold")) ++thumbnailColdCount;
                else if (residency == QStringLiteral("Loading")) ++thumbnailLoadingCount;
                else if (residency == QStringLiteral("CpuReady")) ++thumbnailCpuCount;
                else if (residency == QStringLiteral("GpuResident")) ++thumbnailGpuCount;
                // 2026-09-13: Failed を Cold と一緒くたにしていたため、
                // 「取得に失敗して止まっている」のか「まだ要求していない
                // （画面から遠い）」のかがログから区別できなかった。
                else if (residency == QStringLiteral("Failed")) ++thumbnailFailedCount;
                thumbnailCpuBytes += vid->thumbnailCpuBytesEstimate();
                thumbnailGpuBytes += vid->thumbnailGpuBytesEstimate();
            }
        qDebug("[Memory][Thumbnails] cold=%d failed=%d loading=%d cpuReady=%d gpuResident=%d cpu=%.2fMiB gpu=%.2fMiB",
               thumbnailColdCount, thumbnailFailedCount, thumbnailLoadingCount,
               thumbnailCpuCount, thumbnailGpuCount,
               static_cast<double>(thumbnailCpuBytes) / (1024.0 * 1024.0),
               static_cast<double>(thumbnailGpuBytes) / (1024.0 * 1024.0));
#endif
    }

    // 2026-09-14 D-2: ここには「カーソルが動いたら外周線を描き直す」要求が
    // あった。外周線の濃さを固定した（paintBorderOverlay 参照）ので不要。
    // この1行がスクロール中ずっと 60Hz でオーバーレイ全体の再描画を
    // 呼んでいた（外周線のためだけに、ラベルのパネルも再生バーも巻き込んで）。
    const QPoint local = mapFromGlobal(QCursor::pos());
    updateLauncherTooltipHover(local, dtMs);
    if (updateFocusFeedbackAnimation(dtMs))
        emit focusOverlayUpdateRequested();

    if (m_focusedItem) {
        // 2026-07-15: カーソルがシークバー/音量スライダー領域（＝キャンバス
        // 操作をブロックする判定＝MainWindow::isOnUI()と同じ領域）の上に
        // ある間は、静止していても自動非表示のフェードタイマーを進めない。
        // シークバーをつまもうとして少し止まった瞬間に消えてしまう
        // 不具合を防ぐため。
        const bool cursorOverPlaybackBar = underMouse() && isCursorOverPlaybackBarArea(local);
        if (underMouse() && local != m_lastCursorPos) {
            m_lastCursorPos=local; m_playbarVisible=true; m_playbarFadeTimer=0;
        } else if (cursorOverPlaybackBar) {
            m_playbarVisible=true; m_playbarFadeTimer=0;
        } else {
            m_playbarFadeTimer += dtMs;
            if (m_playbarFadeTimer > 2000) m_playbarVisible = false;
        }
    } else {
        m_playbarVisible=true; m_playbarFadeTimer=0;
    }

    {
        CanvasVideo *av = canvasItemAsVideo(m_activeItem);
        bool playbackOverlayChanged = (av != m_prevActiveVideoForPlaybar);
        // 2026-09-12: 「イコライザだけが動いている」フレーム用。
        // 全体更新が要らないときは、この矩形だけを描き直す。
        bool equalizerOnlyChanged = false;
        QRegion equalizerDamage;

        // アクティブ動画が切り替わった（別バブルへ移った／非アクティブ化された）
        // 瞬間に再生バーがパッと消えてしまわないよう、直前までアクティブだった
        // 動画をフェードアウト対象として引き継ぐ。
        if (av != m_prevActiveVideoForPlaybar) {
            if (m_prevActiveVideoForPlaybar && m_prevActiveVideoForPlaybar != av)
                m_playbarOutgoingVideo = m_prevActiveVideoForPlaybar;
            // ポップ(弾む)演出は「バブルがアクティブになった瞬間」だけ。
            // 以後のドラッグ・全画面・アイドル非表示からの再表示等は
            // 単純なフェードのみにする（毎回弾むとうるさいため）。
            if (av)
                av->triggerPlaybarActivationPop();
            m_prevActiveVideoForPlaybar = av;
        }
        if (av && m_playbarOutgoingVideo == av)
            m_playbarOutgoingVideo = nullptr; // 掴み直し等で再びアクティブに戻った

        // 2026-09-12: 島とリストの当たり判定。固定されたラベルは
        // 「リストを見せ続ける」のが目的なので、島が出ているだけでは畳まず、
        // 島が実際にリストの幅まで迫ってきたときだけ畳む（アクティブなだけの
        // ラベルは従来通り、島が出たら譲る）。固定中のバブルがアクティブな
        // ままフォーカスだけ外れた場合もこちらの規則で判定する。
        // リストは左上マージン12px＋幅244pxなので右端はおよそ256px。
        constexpr int kPinnedPanelRightEdge = 272;
        const int topObstacleLeft = topOverlayObstacleLeft();
        const auto labelPanelSuppressedFor = [&](CanvasVideo *v) {
            return topObstacleLeft < (v->labelPinned() ? kPinnedPanelRightEdge
                                                       : width());
        };

        // 2026-09-13: リストを畳んでも、なおラベル本体（ロゴ＋テキストの台）が
        // 島と当たる場合がある。ウィンドウを最小近くまで小さくすると、展開した
        // 島が左上まで届くため。この場合は島を優先し、ラベルを「ロゴと丸い台
        // だけ」まで縮める（CanvasVideo::setLabelCompact）。
        //
        // 判定は必ず「縮めていない本来の右端」で行う。縮んだ後の幅で判定すると、
        // 縮む→当たらなくなる→戻る→また当たる、の往復になる。
        // リストが開いているかどうかは見ない。閉じていてもラベルの横幅だけで
        // 当たるため、どちらの状態でも縮める必要がある。
        constexpr double kIslandLabelGap = 10.0;
        const auto labelCompactFor = [&](CanvasVideo *v) {
            const double fullRight = v->labelFullRightEdge();
            if (fullRight <= 0.0)
                return false;   // まだ一度も描いていない
            return topObstacleLeft < fullRight + kIslandLabelGap;
        };

        if (av) {
            const int bH=56,bY=height()-bH,by=bY+bH/2,vbx=64;
            double dx=local.x()-vbx, dy=local.y()-by;
            bool onBtn = (dx*dx+dy*dy <= 196);
            bool onSlider = av->volSliderAnim()>0.5f &&
                            local.x()>=vbx-10 && local.x()<=vbx+10 &&
                            local.y()>=(by-16-80) && local.y()<=(by-16);
            av->setVolHover(onBtn||onSlider);
            av->updateVolAnim(dtMs / 1000.0f);

            // シークバー本体のドラッグハンドル（白丸）は、シークバーに
            // カーソルを合わせている間・ドラッグ中(m_activeMode=="seeking")
            // だけ表示する。ヒット判定はhandleMousePress側と同じ矩形。
            const int sx = vbx+24, sw = width()-sx-100, sy = bY+bH/2-2;
            const bool onSeekBar = (m_activeMode == "seeking") ||
                (av->seekBarVisible() &&
                 local.x()>=sx && local.x()<=sx+sw &&
                 local.y()>=sy-10 && local.y()<=sy+14);
            av->setSeekHover(onSeekBar);
            av->updateSeekHandleAnim(dtMs / 1000.0f);

            // 2026-09-13: 時刻の台のホバー。押すと「経過/合計 ⇔ 残り」が
            // 切り替わるので、乗っている間だけ台をほんのり明るくする。
            // 判定の矩形はクリック側(handleMousePress)と同じものを使う。
            av->setTimeHover(
                av->timeTextScreenRect(width(), height())
                    .contains(QPointF(local)));
            av->updateTimePadAnim(dtMs / 1000.0f);



            // 再生バーUIのフェード/ポップアニメーションを進める。paintGL()側の
            // 表示条件（m_activeMode != "panning" && m_playbarVisible）と同じ
            // targetVisibleを渡すことで、非表示へ向かう間も裏でアニメーションを
            // 進め続け、フェードアウトを描画できるようにする。
            // 2026-09-12: 島が出ている間はラベルのコントローラを一度
            // たたんでおく（パッと消すと唐突なので、開くときと同じ
            // 伸び縮みで畳む）。島が引っ込めばそのまま開き直る。
            av->setLabelPanelSuppressed(labelPanelSuppressedFor(av));
            av->setLabelCompact(labelCompactFor(av));

            const bool wantPlaybar = (m_activeMode != "panning") && m_playbarVisible;
            // 2026-09-12: イコライザは毎フレーム動くので、これだけを理由に
            // オーバーレイ全体を描き直さないよう判定を分ける。
            playbackOverlayChanged = playbackOverlayChanged
                || av->playbackUiNeedsAnimationFramesExceptEqualizer(wantPlaybar);
            equalizerOnlyChanged = equalizerOnlyChanged || av->equalizerNeedsFrames();
            av->updatePlaybackBarAnim(wantPlaybar, static_cast<double>(dtMs));
            playbackOverlayChanged = playbackOverlayChanged
                || av->playbackUiNeedsAnimationFramesExceptEqualizer(wantPlaybar);
            equalizerOnlyChanged = equalizerOnlyChanged || av->equalizerNeedsFrames();
            if (equalizerOnlyChanged)
                equalizerDamage += av->equalizerDamageRect().toAlignedRect();
        }

        // 2026-09-12: 左上のラベルのホバー。反応してよいかはプラグインが
        // 決める（PbResolvedMedia::labelHoverable）。本体はカーソルが乗って
        // いるかだけを伝える。宛先は「いま左上に出ているラベル」の持ち主
        // （固定されていれば非アクティブなバブルのこともある）。
        if (auto *labelOwner = labelOwnerVideo()) {
            // 2026-09-13: ラベルの縁の再生位置。1px ぶん進んだときだけ
            // 描き直す（毎フレームだとオーバーレイ全体の再描画が復活する）。
            playbackOverlayChanged = playbackOverlayChanged
                || labelOwner->labelProgressNeedsRepaint();
            labelOwner->setLabelHover(
                labelOwner->labelHoverable()
                && labelOwner->labelScreenRect().contains(QPointF(local)));
        }

        // 2026-09-12: 固定ラベルを「いま出すべきか」を毎tick伝える。
        // 実際の濃さは CanvasVideo 側がフェードで追従するので、別の動画を
        // 全画面にして引っ込むときもパッと消えない。
        if (m_pinnedLabelVideo)
            m_pinnedLabelVideo->setPinnedOverlayVisible(pinnedLabelWins());

        // 固定ラベルの持ち主がアクティブでない場合、そのバブルの
        // 再生バーtickは回らない。ラベル関連のアニメーションだけは
        // ここで進めてやる（伸び縮み・スクロール・引っ張りの戻り）。
        if (m_pinnedLabelVideo && m_pinnedLabelVideo->labelPinned()
            && m_pinnedLabelVideo != canvasItemAsVideo(m_activeItem)) {
            m_pinnedLabelVideo->setLabelPanelSuppressed(
                labelPanelSuppressedFor(m_pinnedLabelVideo));
            m_pinnedLabelVideo->setLabelCompact(
                labelCompactFor(m_pinnedLabelVideo));
            playbackOverlayChanged = playbackOverlayChanged
                || m_pinnedLabelVideo->labelAnimsNeedFramesExceptEqualizer();
            m_pinnedLabelVideo->tickLabelAnims(static_cast<double>(dtMs));
            playbackOverlayChanged = playbackOverlayChanged
                || m_pinnedLabelVideo->labelAnimsNeedFramesExceptEqualizer();
            if (m_pinnedLabelVideo->equalizerNeedsFrames()) {
                equalizerOnlyChanged = true;
                equalizerDamage += m_pinnedLabelVideo->equalizerDamageRect()
                                       .toAlignedRect();
            }
        }

        // 直前アクティブだった動画は、常に非表示扱いでフェードアウトのみ進める。
        // 完全に消え切ったら追跡をやめる。
        if (m_playbarOutgoingVideo) {
            m_playbarOutgoingVideo->updatePlaybackBarAnim(false, static_cast<double>(dtMs));
            playbackOverlayChanged = true;
            if (!m_playbarOutgoingVideo->playbackBarAnimActive())
                m_playbarOutgoingVideo = nullptr;
        }
        if (playbackOverlayChanged) {
            emit playbackOverlayUpdateRequested();
        } else if (equalizerOnlyChanged && !equalizerDamage.isEmpty()) {
            // 動いているのはイコライザだけ。オーバーレイ全体ではなく
            // その矩形だけを描き直す（板はキャッシュ済みなので、この
            // 範囲に重なる部分だけが転送される）。
            //
            // 2026-09-14: 30Hzへ間引く。棒の上下と音符の明滅だけの演出で、
            // 60Hzで描く必要がない（位相 m_eqPhase は従来どおり60Hzの
            // syncLogicMedium で進めているので、動き自体の滑らかさは
            // 時間に対して正しいまま。描く回数だけを半分にしている）。
            constexpr qint64 kEqualizerIntervalMs = 33;
            if (!m_equalizerUpdateClock.isValid()
                || m_equalizerUpdateClock.elapsed() >= kEqualizerIntervalMs) {
                if (m_equalizerUpdateClock.isValid())
                    m_equalizerUpdateClock.restart();
                else
                    m_equalizerUpdateClock.start();
                emit playbackOverlayEqualizerUpdateRequested(
                    equalizerDamage.boundingRect());
            }
        }
    }

    if (m_trashZone) {
        // シングルクリック判定用タイマーが動いている間はまだ実ドラッグに
        // 入っていない（掴んだだけ／クリックの可能性がある）ので、その間は
        // ゴミ箱を出さない。移動量がしきい値を超えるとタイマーは止まる。
        bool isDragging = (m_activeMode == "panning" && m_activeItem
                            && !m_singleClickTimer->isActive());
        if (isDragging) {
            // ドラッグ開始と同時に「うっすら」表示し、以降はゴミ箱までの
            // 距離に応じてなめらかに濃く／薄くする。
            m_trashZone->move(16, height()-m_trashZone->height()-16);
            m_trashZone->updateProximity(QPointF(local));
        } else if (m_trashZone->isVisible()) {
            // 通常は handleMouseRelease() 側で明示的に消すが、外部ドラッグ開始
            // （Shift+ドラッグ）など別経路でパンニングが終わった場合の保険。
            m_trashZone->hideTrash();
            QTimer::singleShot(260, this, [this]{ if (m_activeMode!="panning") m_trashZone->hide(); });
        }
    }

    // update()のトリガー判定（カメラ/lift/pulseアニメーション・FlowMode）は
    // syncLogic()側（120Hz）に集約済みなので、ここでは呼ばない。
    // このtick内で変更した値（volSliderAnim・trashZone近接度など）は、
    // syncLogic()側のupdate()や、動画フレーム到着(frameReady)による
    // update()など、他経路の再描画時にまとめて反映される
    // （元の単一syncLogic()でも同じ条件でしかupdate()していなかったため、
    // 挙動は変わらない）。
}

void CanvasWidget::startLiftUp()
{
    if (!m_activeItem) return;
    m_glassBubbleActive = true;
    m_liftAnim->stop();
    m_liftAnim->setEasingCurve(QEasingCurve::OutQuart);
    m_liftAnim->setStartValue(m_activeItem->liftScale());
    m_liftAnim->setEndValue(1.025);
    m_liftAnim->start();
}
void CanvasWidget::startLiftDown()
{
    // マウスを離した時点で屈折は終了方向へ。リフト縮小アニメーションと
    // 屈折フェードアウトはそれぞれの時間軸で同時に進める。
    m_glassBubbleActive = false;
    if (!m_activeItem) return;
    m_liftAnim->stop();
    m_liftAnim->setEasingCurve(QEasingCurve::OutQuint);
    m_liftAnim->setStartValue(m_activeItem->liftScale());
    m_liftAnim->setEndValue(1.0);
    m_liftAnim->start();
}

// ============================================================
//  2026-09-22: バブル融合（BubbleFusion.h）
//
//  バブルを掴んで別の動画バブルに重ね、少し待ってから離すと、掴んでいた
//  方が吸い込まれて相手のリストに入る。時間の進行と演出の値は
//  BubbleFusion が持ち、ここでは
//    - 重なりの判定（findFusionCandidate / bubblesCanFuse）
//    - 吸い寄せ・縮み・膨らみを実際のバブルへ反映
//    - 吸い込みアニメの位置・大きさ・透明度
//    - 中身の移動と、吸われた側の削除
//  を行う。描画（板が液体のようにつながる部分）は paintGL の
//  「バブル融合」ブロックと Renderer::ShapeFusion / GlassHighlightEffect::Fusion。
// ============================================================
namespace {
double fusionEaseInOutCubic(double t)
{
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}
double fusionSmoothstep(double a, double b, double x)
{
    const double t = qBound(0.0, (x - a) / (b - a), 1.0);
    return t * t * (3.0 - 2.0 * t);
}
// liftScale の縮む中心をバブルの中心へ移す。見た目の位置は変えない。
void fusionCenterLiftOrigin(CanvasItem *item)
{
    const QRectF ir = item->itemRect();
    const QPointF center = ir.center();
    const QPointF anchor = item->liftAnchor();
    const double ls = item->liftScale();
    const QPointF shownCenter = anchor + (center - anchor) * ls;
    item->setOriginPoint((center - item->pos()) / qMax(item->scale(), 0.000001));
    item->setPos(item->pos() + (shownCenter - center));
}
} // namespace

bool CanvasWidget::bubblesCanFuse(CanvasItem *dragged, CanvasItem *target) const
{
    if (!dragged || !target || dragged == target)
        return false;
    // 動画バブル（ローカル動画・音声・プラグインのメディア）同士だけ。
    // 本体はメディアの中身を区別しない。
    auto *dv = canvasItemAsVideo(dragged);
    auto *tv = canvasItemAsVideo(target);
    if (!dv || !tv)
        return false;
    if (target == m_focusedItem || target == m_pendingFocusItem)
        return false;
    // プラグインのメディアは、そのプラグインに可否を聞く（PbFusionApi.h）。
    // 本体は理由（Mix・配信など）を知らない。実装していないプラグインは
    // すべて受け付ける。ローカルのメディアは本体の持ち物なので常に可。
    const auto policyOf = [](CanvasVideo *v) {
        return v->mediaKind() == MediaKind::PluginMedia
            ? PluginManager::instance().fusionPolicy(v->sourceUrl())
            : PbFusionPolicy();
    };
    if (!policyOf(tv).acceptsIncoming)
        return false;
    if (!policyOf(dv).canJoinOthers)
        return false;
    return true;
}

CanvasItem *CanvasWidget::findFusionCandidate(CanvasItem *dragged,
                                              const QRectF &draggedRect,
                                              double *overlapRatio,
                                              double *boardOverlapRatio) const
{
    if (overlapRatio)
        *overlapRatio = 0.0;
    if (boardOverlapRatio)
        *boardOverlapRatio = 0.0;
    // 板 = 中身 + 比例ベゼル（CanvasItem::mediaBezelWidth は大きさに比例するので
    // ワールド座標の矩形にそのまま使える）。
    const auto boardOf = [](const QRectF &content) {
        const double b = CanvasItem::mediaBezelWidth(content);
        return content.adjusted(-b, -b, b, b);
    };
    const QRectF draggedBoard = boardOf(draggedRect);
    const double draggedArea = draggedRect.width() * draggedRect.height();
    const double draggedBoardArea = draggedBoard.width() * draggedBoard.height();
    CanvasItem *best = nullptr;
    double bestBoardRatio = 0.0;
    double bestContentRatio = 0.0;
    for (CanvasItem *other : m_items) {
        if (other == dragged || !bubblesCanFuse(dragged, other))
            continue;
        const QRectF otherRect = other->itemRect();
        const QRectF otherBoard = boardOf(otherRect);
        const QRectF boardOverlap = draggedBoard.intersected(otherBoard);
        if (boardOverlap.isEmpty())
            continue;
        const double minBoardArea =
            qMin(draggedBoardArea, otherBoard.width() * otherBoard.height());
        if (minBoardArea <= 0.0)
            continue;
        const double boardRatio =
            boardOverlap.width() * boardOverlap.height() / minBoardArea;
        if (boardRatio <= bestBoardRatio)
            continue;
        bestBoardRatio = boardRatio;
        best = other;
        const QRectF overlap = draggedRect.intersected(otherRect);
        const double minArea = qMin(draggedArea, otherRect.width() * otherRect.height());
        bestContentRatio = (!overlap.isEmpty() && minArea > 0.0)
            ? overlap.width() * overlap.height() / minArea : 0.0;
    }
    // 板が触れた瞬間からつながり、板が離れるまで切らない。
    // 結合できる状態（離したら吸い込む）はこれまで通り、中身の重なりと
    // kOverlapThreshold で決める（BubbleFusion::tick）。
    if (!best || bestBoardRatio < 0.002)
        return nullptr;
    if (overlapRatio)
        *overlapRatio = bestContentRatio;
    if (boardOverlapRatio)
        *boardOverlapRatio = bestBoardRatio;
    return best;
}

void CanvasWidget::tickBubbleFusion()
{
    // マウス移動（数ms間隔）と syncLogic の両方から呼ばれるので、ミリ秒の
    // 切り捨てで時間が目減りしないようナノ秒で測る。
    double dtMs = 0.0;
    if (m_fusionClock.isValid()) {
        dtMs = static_cast<double>(m_fusionClock.nsecsElapsed()) / 1.0e6;
        m_fusionClock.restart();
    } else {
        m_fusionClock.start();
    }

    // ---- 今ドラッグ中のバブルと、重なっている相手 ----
    const bool singleDrag =
        m_activeMode == QLatin1String("panning")
        && m_activeItem
        && m_multiDragStartPositions.isEmpty()
        && m_fusionRawValid
        && !(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier)
        && !m_focusedItem && !m_pendingFocusItem
        && !m_fusion.absorbing()
        && m_items.contains(m_activeItem);
    CanvasItem *dragged = nullptr;
    CanvasItem *candidate = nullptr;
    double overlapRatio = 0.0;
    double boardOverlapRatio = 0.0;
    if (singleDrag) {
        dragged = m_activeItem;
        const QRectF rawRect = dragged->itemRect()
            .translated(m_fusionRawDragPos - dragged->pos());
        candidate = findFusionCandidate(dragged, rawRect, &overlapRatio,
                                        &boardOverlapRatio);
    }

    // 吸い込み中の組は tick の前に控える（完了した tick では参照が消えるため）
    CanvasItem *absorbDragged = m_fusion.absorbing() ? m_fusion.pairDragged() : nullptr;
    CanvasItem *absorbTarget  = m_fusion.absorbing() ? m_fusion.pairTarget()  : nullptr;
    const bool hadFrames = m_fusion.needsFrames();

    const BubbleFusion::Event event =
        m_fusion.tick(dragged, candidate, dtMs, overlapRatio, boardOverlapRatio);

    // ---- ドラッグ中: 吸い寄せと縮み ----
    if (dragged) {
        QPointF pos = m_fusionRawDragPos;
        CanvasItem *hover = m_fusion.hoverTarget();
        // 吸い寄せは armTarget（外れた直後も0へ戻り切るまで残る）に向ける
        CanvasItem *magnetTo = m_fusion.armTarget();
        const double arm = (magnetTo && m_items.contains(magnetTo)) ? m_fusion.armedness() : 0.0;
        if (arm > 0.0) {
            const QPointF rawCenter = dragged->itemRect()
                .translated(pos - dragged->pos()).center();
            pos += (magnetTo->itemRect().center() - rawCenter)
                   * (BubbleFusion::kMagnet * arm);
        }
        if (dragged->pos() != pos)
            dragged->setPos(pos);
        // 掴んだときのリフト（1.025）から、結合できる状態へ近づくほど少し縮む。
        // リフトのアニメ中はそちらに任せる。
        if (m_glassBubbleActive
            && m_liftAnim->state() != QAbstractAnimation::Running) {
            dragged->setLiftScale(1.025 - BubbleFusion::kDraggedShrink * arm);
        }
        // 重ねている相手より手前に描く
        if (hover) {
            const int di = m_items.indexOf(dragged);
            const int ti = m_items.indexOf(hover);
            if (di >= 0 && ti >= 0 && di < ti) {
                m_items.removeAt(di);
                m_items.append(dragged);
            }
        }
    }

    // ---- 吸い込み ----
    // 2026-09-23: 中身の移動（リストへ足す）は、吸い込みの途中（Commit）ではなく
    // 完了時に、吸われた側の削除と同じ元に戻すの1ステップの中で行う
    // （finishFusionAbsorb）。途中で足すと、そのステップの「前」の記録に
    // 足した後のリストが入ってしまい、Ctrl+Z でリストが戻らない。
    // Commit は受け側のぷるんの合図としてだけ使う（BubbleFusion 側で処理済み）。
    if (event == BubbleFusion::Event::Finished && absorbDragged && absorbTarget) {
        finishFusionAbsorb(absorbDragged, absorbTarget);
        absorbDragged = absorbTarget = nullptr;
    } else if (m_fusion.absorbing() && absorbDragged && absorbTarget
               && m_items.contains(absorbDragged) && m_items.contains(absorbTarget)) {
        const double p = m_fusion.absorbProgress();
        const double e = fusionEaseInOutCubic(p);
        const QPointF targetCenter = absorbTarget->itemRect().center();
        absorbDragged->setCenter(m_fusionAbsorbFromCenter
                                 + (targetCenter - m_fusionAbsorbFromCenter) * e);
        absorbDragged->setLiftScale(
            m_fusionAbsorbFromLift + (0.05 - m_fusionAbsorbFromLift) * p * p * p);
        m_fadeInAlpha[absorbDragged] = 1.0 - fusionSmoothstep(0.72, 1.0, p);
    }

    // ---- 受け側の膨らみ・ぷるん（liftScale）----
    QSet<CanvasItem*> liftTargets = m_fusionLiftItems;
    if (CanvasItem *hover = m_fusion.hoverTarget())
        liftTargets.insert(hover);
    if (CanvasItem *pt = m_fusion.pairTarget())
        liftTargets.insert(pt);
    for (CanvasItem *item : std::as_const(liftTargets)) {
        if (!m_items.contains(item)) {
            m_fusionLiftItems.remove(item);
            continue;
        }
        // 掴んで操作中のバブル・吸い込まれているバブルは別の経路が大きさを持つ
        if ((item == m_activeItem && !m_activeMode.isEmpty()) || item == absorbDragged)
            continue;
        const double lift = m_fusion.liftFor(item);
        const bool atRest = std::abs(lift - 1.0) < 0.0005;
        if (!m_fusionLiftItems.contains(item)) {
            if (atRest)
                continue;
            // 膨らむ中心をバブルの中心にする（この時点では lift=1 なので見た目は動かない）
            fusionCenterLiftOrigin(item);
            m_fusionLiftItems.insert(item);
        }
        item->setLiftScale(atRest ? 1.0 : lift);
        if (atRest)
            m_fusionLiftItems.remove(item);
    }

    if (hadFrames || m_fusion.needsFrames() || !m_fusionLiftItems.isEmpty())
        requestFullRender(CanvasRenderReason::UiAnimation);
}

// ============================================================
//  2026-09-23: ラベルの長押しの中身（PbLabelActionApi.h）
//
//  本体は「何が起きるか」を知らない。ローカル動画だけは本体自身が持ち主
//  なので、同じフォルダの動画を並べて本体のリストにする。それ以外は担当の
//  プラグインに聞き、返ってきたURLへ今の再生位置のまま切り替える。
//  どちらも1ステップとして元に戻せる。
// ============================================================
void CanvasWidget::runLabelHoldAction(CanvasVideo *video)
{
    if (!video || !m_items.contains(video))
        return;

    if (video->labelHoldIsCoreAction()) {
        const QJsonObject list = video->buildFolderListForLabelHold();
        if (list.isEmpty()) {
            qDebug().noquote() << QStringLiteral(
                "[CanvasWidget] label hold: no sibling media in folder");
            video->finishLabelHold(false);
            return;
        }
        {
            UndoAutoStep undoStep(this);
            video->restoreCoreListFromJson(list);
        }
        video->finishLabelHold(true);
        emit playbackOverlayUpdateRequested();
        update();
        return;
    }

    QPointer<CanvasVideo> guard(video);
    const QString url = video->sourceUrl();
    const bool started = PluginManager::instance().runLabelHoldAction(
        url, [this, guard, url](const PbLabelHoldResult &result) {
            // 【重要】この関数はプラグインの中から呼ばれる（プラグイン自身の
            // 処理の途中）。ここで本体の状態を書き換えると、本体がその場で
            // プラグインへ問い合わせ直す（一覧・サムネイル等）ことがあり、
            // プラグインが自分の処理中に自分を呼ばれる形になる。安全のため、
            // 本体側の処理はイベントループへ戻してから行う（2026-09-23）。
            QTimer::singleShot(0, this, [this, guard, url, result]() {
                // 答えが返るまでにバブルが消えた／別の動画へ移った／待つのを
                // やめた場合は何もしない。
                if (!guard || !m_items.contains(guard.data())
                    || !guard->labelHoldWaiting() || guard->sourceUrl() != url)
                    return;
                if (!result.success || result.newUrl.isEmpty()) {
                    qDebug().noquote() << QStringLiteral(
                        "[CanvasWidget] label hold failed: %1").arg(result.errorMessage);
                    guard->finishLabelHold(false);
                    return;
                }
                qDebug().noquote() << QStringLiteral(
                    "[CanvasWidget] label hold: switching (keepPlaying=%1) to %2")
                    .arg(result.keepPlaying ? QStringLiteral("yes") : QStringLiteral("no"),
                         result.newUrl.left(120));
                {
                    UndoAutoStep undoStep(this);
                    guard->switchSourceByLabelHold(
                        result.newUrl, result.keepPlaying, result.labelIconSvg,
                        result.labelText, result.labelHoverable, result.labelPinnable,
                        result.labelProgress);
                }
                if (!guard)
                    return;
                guard->finishLabelHold(true);
                qDebug().noquote() << QStringLiteral("[CanvasWidget] label hold: switch done");
                emit playbackOverlayUpdateRequested();
                update();
            });
        });
    if (!started)
        video->finishLabelHold(false);
}

void CanvasWidget::extractListRowToBubble(CanvasVideo *owner, const QPointF &screenPos)
{
    if (!owner || !m_items.contains(owner))
        return;
    const MediaSource source = owner->takeRowForExtract();
    m_labelRowDragVideo = nullptr;
    m_activeMode.clear();
    if (source.kind == MediaKind::Unknown || source.originalPathOrUrl.isEmpty())
        return;

    // addMediaSource() と同じ作り方。ただしドラッグ中なので押し出しはせず
    // （離したときに通常どおり行う）、元に戻すのステップも今のマウス操作に
    // まとめる（押した時点で開いている）。
    const QPointF canvasPos = screenToCanvas(screenPos);
    auto *video = new CanvasVideo(this);
    video->setMasterGain(m_masterVolume.gain(), false);
    video->setPos(canvasPos);
    // 取り出し元と同じくらいの大きさで出す
    video->setScale(source.kind == MediaKind::LocalAudio ? 1.0 : owner->scale());
    video->setCenter(canvasPos);
    connectVideoRenderSignals(video);
    connectVideoDimensionSignals(video);
    video->loadMediaSource(source);
    m_items.append(video);
    QPointer<CanvasVideo> guard(video);
    QTimer::singleShot(0, this, [this, guard]() {
        if (!guard || !m_items.contains(guard))
            return;
        makeCurrent();
        guard->initializeMpv();
        guard->warmupPlaybackSurface();
        doneCurrent();
        update();
    });

    // そのまま掴んだ状態へ（handleMousePress の panning 開始と同じ準備）
    if (m_singleClickTimer)
        m_singleClickTimer->stop();
    m_pendingClickItem = nullptr;
    m_multiDragStartPositions.clear();
    m_activeItem = video;
    m_activeMode = QStringLiteral("panning");
    m_dragStartPos = canvasPos;
    m_dragStartScreenPos = screenPos;
    m_startItemPos = video->pos();
    {
        // 持ち上げ（ぽよん）の中心をバブルの中心に
        const QRectF ir = video->itemRect();
        const double s = qMax(video->scale(), 0.000001);
        video->setOriginPoint(QPointF(ir.width() * 0.5 / s, ir.height() * 0.5 / s));
    }
    m_fusionRawValid = false;
    startLiftUp();
    qDebug().noquote() << QStringLiteral("[ListExtract] %1 taken out of %2")
                              .arg(source.originalPathOrUrl)
                              .arg(reinterpret_cast<quintptr>(owner), 0, 16);
    requestFullRender(CanvasRenderReason::Scene);
}

bool CanvasWidget::beginFusionAbsorbOnRelease()
{
    CanvasItem *absorbed = m_activeItem;
    if (!absorbed || !m_fusion.armedFor(absorbed))
        return false;
    CanvasItem *target = m_fusion.hoverTarget();
    if (!target || !bubblesCanFuse(absorbed, target))
        return false;

    m_liftAnim->stop();
    fusionCenterLiftOrigin(absorbed);
    m_fusionAbsorbFromCenter = absorbed->itemRect().center();
    m_fusionAbsorbFromLift = absorbed->liftScale();
    // Undo で戻したときは、掴む前の位置で出てくるようにする（finishFusionAbsorb）。
    m_fusionAbsorbRestorePos = m_startItemPos;
    m_fusion.startAbsorb();

    // 吸い込まれている間はアクティブ扱いをやめる（再生バーや暗転を出さない。
    // ガラスの屈折もここからフェードアウトする）。
    m_glassBubbleActive = false;
    m_activeItem = nullptr;
    requestFullRender(CanvasRenderReason::UiAnimation);
    return true;
}

void CanvasWidget::finishFusionAbsorb(CanvasItem *absorbed, CanvasItem *target)
{
    if (!absorbed || !m_items.contains(absorbed))
        return;
    m_fadeInAlpha.remove(absorbed);
    // 元に戻す（Ctrl+Z）で、吸われた側が掴む前の位置・大きさで作り直され、
    // 受け側のリストも元の並びへ戻るよう、次の3つを1ステップにまとめる:
    //   吸われた側を掴む前の位置へ置く → 受け側のリストへ足す → 吸われた側を消す
    // （ステップの「前」の記録は、位置を戻した後・リストへ足す前の状態）
    absorbed->setLiftScale(1.0);
    absorbed->setPos(m_fusionAbsorbRestorePos);
    {
        UndoAutoStep undoStep(this);
        auto *tv = canvasItemAsVideo(target);
        auto *dv = canvasItemAsVideo(absorbed);
        if (tv && dv && m_items.contains(target)) {
            // 吸われる側がリストなら、その並びのまま後ろへ続ける
            tv->appendListItems(dv->listItems());
            qDebug("[Fusion] %p absorbed into %p (list size=%d)",
                   static_cast<void*>(absorbed), static_cast<void*>(target),
                   tv->stackCount());
        }
        makeCurrent();
        destroyItem(absorbed);
        doneCurrent();
    }
#ifdef Q_OS_WIN
    // ゴミ箱削除と同じ後始末（MpvPlayer破棄後のOLE/ドロップ登録の確認）。
    OleInitialize(nullptr);
    if (auto *mw = qobject_cast<MainWindow*>(window()))
        mw->refreshDropTargets();
#endif
    requestFullRender(CanvasRenderReason::Scene);
}

void CanvasWidget::animateCamera(QPointF tp, double tz, int dur, CameraAnimAnchor anchor)
{
    // 2026-07-08 調査用: animateCamera()の全呼び出し元をトレースする
    // （ワープ調査。unfocusItem()の復元アニメ等、focusItem()以外からの
    // 呼び出しも含めて全部見えるようにする）。
    qDebug("[Focus][camera][ANIMCAM-TRACE] animateCamera from=(%.1f,%.1f)/%.3f to=(%.1f,%.1f)/%.3f dur=%d",
           m_cameraPos.x(), m_cameraPos.y(), m_cameraZoom, tp.x(), tp.y(), tz, dur);
    // 2026-07-15: 画面座標ベースの補間へ書き換え。
    // 旧実装はX/Y/ズームを3本のアニメで独立にワールド座標補間していたが、
    // バブルの画面上の位置は (bubbleCenter - camPos) × zoom で決まるため、
    // 位置の残り距離(1-f)に増大中のzoomが掛け算され、序盤はズーム拡大が
    // 残り距離を引き伸ばして「先にズームが寄り切ってから、端のバブルが
    // 横からスライドしてくる」ように見えていた。
    // 新実装は進行度f(0→1, イージング適用済み)1本で
    //   zoom(f)   = z0 × (z1/z0)^f                     … 対数補間
    //   camPos(f) = P − ((1−f)×off0 + f×off1) / zoom(f)
    // を毎tick計算する（P/off0/off1の意味はCanvasWidget.hのメンバコメント
    // 参照）。アンカー点Pの画面上のオフセットがoff0→off1へ進行度に比例して
    // 直線的に動くので、位置合わせがズームと同じ進行度で進み、同時に完了する。
    //
    // 2026-07-15(2): 引く演出(ズームアウト)も同様にするためアンカーを一般化。
    //   寄る演出: P=着地先(バブル中心), off1=0
    //     → バブルが画面中心へ直線的に引き込まれる。
    //   引く演出: P=出発点(いま画面中心にあるバブル), off0=0
    //     → バブルが着地後の画面位置へ直線的に送り出される。
    //   （Autoはズーム方向で自動選択。フォーカスイン/アウトの呼び出し元は
    //     巨大バブルへのフォーカスがズームアウトになるケースがあるため
    //     明示的にTarget/Startを渡している。）
    //
    // 2026-07-15: 120Hz化に伴い手動駆動へ(m_camAnimDriver)。開始パラメータは
    // 現在のカメラ値から取るので、必ず先にローカルへ退避してから設定する。
    // クロックをここでrestart()するので、旧QVariantAnimation時代のワープ
    // 不具合(過去の経過時間の持ち越し)は構造的に起きない。
    // 2026-07-16: 通常のanimateCamera()呼び出し（フォーカスイン/アウト等）は
    // このクランプ対象外。resetFocusCameraToNative()だけが呼び出し直後に
    // m_camAnimClampToItemBounds/m_camAnimClampItemを立てる
    // （tickCameraAnimation()参照）。
    m_camAnimClampToItemBounds = false;
    m_camAnimClampItem = nullptr;
    // 2026-07-17: ホイールズームのイージング(m_zoomEase)が裏で動いたまま
    // フォーカスイン/アウト等の本格的なカメラアニメが始まると、同じ
    // m_cameraZoom/m_cameraPosを奪い合ってガクつく（パン慣性を止めているのと
    // 同じ理由）。ここで確実に止めておく。
    if (m_zoomEase) m_zoomEase->stop();

    const QPointF fromPos = m_cameraPos;
    const double  fromZoom = m_cameraZoom;
    m_camAnimTargetPos  = tp;
    m_camAnimTargetZoom = tz;
    m_camAnimFromZoom   = fromZoom;
    if (anchor == CameraAnimAnchor::Auto)
        anchor = (tz >= fromZoom) ? CameraAnimAnchor::Target : CameraAnimAnchor::Start;
    m_camAnimAnchorIsTarget = (anchor == CameraAnimAnchor::Target);
    m_camAnimAnchorPos = m_camAnimAnchorIsTarget ? tp : fromPos;
    // off0/off1はアンカーPの「画面中心からのオフセット(px)」の開始値/着地値。
    //   off0 = (P − fromPos)×z0 … Target時は(tp−from)z0、Start時は0
    //   off1 = (P − tp)×z1     … Target時は0、Start時は(from−tp)z1
    m_camAnimAnchorOff0 = (m_camAnimAnchorPos - fromPos) * fromZoom;
    m_camAnimAnchorOff1 = (m_camAnimAnchorPos - tp) * tz;
    m_camAnimDriver.durationMs = qMax(1, dur);
    m_camAnimDriver.clock.restart();
    m_camAnimDriver.running = true;
    // f=0を即時適用(次のtickを待たずに開始状態を確定させる。数学的に
    // ちょうどfromPos/fromZoomになる)。
    applyCameraAnimationProgress(0.0);
}

void CanvasWidget::tickCameraAnimation()
{
    if (!m_camAnimDriver.running) return;
    const double t = double(m_camAnimDriver.clock.elapsed())
                     / double(m_camAnimDriver.durationMs);
    if (t >= 1.0) {
        // 最終tickは厳密にf=1で着地させてから停止する。
        m_camAnimDriver.running = false;
        applyCameraAnimationProgress(1.0);
        return;
    }
    applyCameraAnimationProgress(m_camAnimDriver.easing.valueForProgress(t));
}

void CanvasWidget::applyCameraAnimationProgress(double f)
{
    // animateCamera()のコメント参照。fはイージング適用済みの進行度(0→1)。
    const double z0 = qMax(1e-6, m_camAnimFromZoom);
    const double z1 = qMax(1e-6, m_camAnimTargetZoom);
    const double zoom = z0 * std::pow(z1 / z0, f);
    m_cameraZoom = zoom;
    m_cameraPos  = m_camAnimAnchorPos
        - ((1.0 - f) * m_camAnimAnchorOff0 + f * m_camAnimAnchorOff1) / zoom;

    // 2026-07-16: resetFocusCameraToNative()専用のクランプ（クラス宣言側の
    // m_camAnimClampToItemBoundsコメント参照）。アンカー方式の中間フレームで
    // カメラがアイテム範囲外を指し、フォーカス対象の外側が一瞬見えてしまう
    // のを防ぐ。
    if (m_camAnimClampToItemBounds && m_camAnimClampItem)
        clampCameraToFocusedImage(m_camAnimClampItem->itemRect());

    updateCanvasFromCamera();
    requestFullRender(CanvasRenderReason::Camera);
}

void CanvasWidget::retargetCameraAnimation(QPointF tp, double tz)
{
    if (!m_camAnimDriver.running) {
        // 非実行時は即座に着地（呼び出し側の従来else分岐相当の保険）。
        m_camAnimTargetPos  = tp;
        m_camAnimTargetZoom = tz;
        m_cameraPos  = tp;
        m_cameraZoom = tz;
        updateCanvasFromCamera();
        return;
    }
    // 変化が微小（整数ピクセルのウィンドウジオメトリに由来する量子化ノイズ、
    // 浮動小数の揺らぎ等）なら何もしない。微小な再照準を毎フレーム繰り返す
    // こと自体がなめらかさを削るため。
    const double tzc   = qMax(1e-6, tz);
    const double curTz = qMax(1e-6, m_camAnimTargetZoom);
    const QPointF dTp = tp - m_camAnimTargetPos;
    const bool zoomSame = std::abs(std::log(tzc / curTz)) < 1e-4;      // 0.01%
    const bool posSame  = std::hypot(dTp.x(), dTp.y())
                              * qMax(tzc, curTz) < 0.25;               // 画面上0.25px
    if (zoomSame && posSame)
        return;
    // 2026-07-15(3) なめらかさ改善: 当初は「カメラ現在値の連続性を厳密に
    // 保つ張り直し」(z0/off0を進行度f0を境に再解出)にしていたが、終盤では
    // 残りの補正が(1−f0)の区間へ圧縮されるため、速度の折れが1/(1−f0)倍に
    // 増幅される。ウィンドウリサイズ追従で毎フレーム呼ばれると、整数
    // ピクセル刻みのtargetZoom変化まで増幅されて「がくがく」して見えた。
    // そこで旧実装のsetEndValue()と同じ「終端値の差し替えだけ」にする。
    // 値はごく僅かに不連続になる(1フレームあたりの終端変化×進行度ぶん)が、
    // リサイズ追従では終端自体がリサイズアニメでなめらかに動くため
    // 体感できず、速度の増幅が起きないぶんこちらの方がなめらか。
    // どちらのアンカーでも着地の厳密性は式の形から自動的に保たれる
    // (Target: camPos(1)=P=tp / Start: camPos(1)=P−(P−tp)tz/tz=tp)。
    m_camAnimTargetPos  = tp;
    m_camAnimTargetZoom = tz;
    if (m_camAnimAnchorIsTarget)
        m_camAnimAnchorPos = tp;
    m_camAnimAnchorOff1 = (m_camAnimAnchorPos - tp) * tz;
}

bool CanvasWidget::isCameraFocusAnimationRunning() const
{
    return m_camAnimDriver.running;
}

void CanvasWidget::beginFocusPlaybackPolicy(CanvasItem *focusItem)
{
    endFocusPlaybackPolicy();

    auto *focusVideo = canvasItemAsVideo(focusItem);
    if (!focusVideo) {
        qDebug("[Focus] image focus: videos keep playing/audio-only by viewport");
        return;
    }

    // 直前の endFocusPlaybackPolicy() 呼び出し（このメソッド冒頭）が、直前の
    // フォーカスセッションでFocusSleep/deep-sleepしていた動画に対して遅延
    // ウェイク（resume=...）をスケジュール済みの可能性がある。それらの動画の
    // うち、今回このメソッドの下のループで「改めてFocusSleepさせる」対象に
    // 選ばれなかったもの（例: 呼び出し時点で既にSleep＝完全休止まで進んで
    // いて decideFocusSleepForState() の対象外になったもの）は、下のループが
    // 一切関与しないまま取り残される。この状態で何もしないと、前セッションの
    // 遅延ウェイクがそのまま生き残り、今まさに始まるこの新しいフォーカス
    // セッション中に「対象外だったはずの動画」が勝手に再生を再開してしまう
    // （フォーカス中に背後の動画の音が鳴る不具合の原因）。
    // 世代番号をここでもう一段進めておくことで、直前の end 呼び出しが
    // スケジュールした遅延ウェイクは（このメソッドの下のループで改めて
    // 拾われて再スケジュールされない限り）発火時に必ず古い世代とみなされ
    // スキップされる。これにより「新しいフォーカスセッションが動画の生死を
    // 引き継ぐのは、必ずこのメソッドの下のループが明示的にFocusSleepさせた
    // ものだけ」という前提を、既存のgeneration機構だけで保証できる。
    ++m_focusSleepBookkeeping.generation;

    // このバブルが以前「別のバブルがフォーカスされた際、自分はIdleのまま
    // FocusSleep待ちにされていた」場合、フラグ（m_focusSleepPending）が
    // 残ったままになっていることがある（endFocusPlaybackPolicy()の遅延
    // wakeはstate==FocusSleepのバブルしか対象にしないため、Idleのまま
    // だったバブルはそこで救済されない）。今まさに自分がフォーカス対象に
    // なったので、ここで明示的に解除しておく。解除し忘れると、次に
    // このバブルを再生しようとした瞬間に横取りされてFocusSleepへ送られ、
    // サムネ・再生時間は取得できるのに実際には再生されない不具合になる。
    if (focusVideo->hasPendingFocusSleep()) {
        qDebug("[Focus] clearing stale pending-focus-sleep on new focus target video=%p",
               static_cast<void*>(focusVideo));
        focusVideo->clearPendingFocusSleep();
    }

    // 【2026-07-10】新しいフォーカス対象が「旧セッションによって既に
    // FocusSleep実体へ入れられた直後」であることがある。典型例はFlow
    // チェーンの全画面切り替え: fromが全画面中にtoがFlow起動されると、
    // startPlayback()がpendingフラグを消費してtoをFocusSleepへ差し戻し、
    // その直後にvideoStarted経由でtoがフォーカス対象になる（＝カメラは
    // 次の動画へ移るのに再生が始まらず、FocusSleepの間はクリックも
    // 効かない実バグ。ログで確認済み）。フォーカス対象自身が
    // FocusSleepしているのは矛盾なので、ここで起こす。
    if (focusVideo->bubbleState() == BubbleState::FocusSleep) {
        qDebug("[Focus] waking new focus target out of stale FocusSleep video=%p",
               static_cast<void*>(focusVideo));
        focusVideo->wakeFromFocusSleep(true);
    }

    m_focusSleepBookkeeping.active = true;
    for (auto *item : m_items) {
        auto *video = canvasItemAsVideo(item);
        if (!video || video == focusVideo) continue;

        const BubbleState state = video->bubbleState();
        const FocusSleepDecision decision = FocusSessionController::decideFocusSleepForState(state);
        const bool resumePlaybackAfterFocus = decision.resumePlaybackAfterFocus;

        if (!decision.shouldEnterFocusSleep) continue;

        m_focusSleepBookkeeping.videosAsleep.insert(video);
        m_focusSleepBookkeeping.resumeAfterFocus.insert(video, resumePlaybackAfterFocus);
        qDebug("[Focus][membership] add focus-sleep video=%p player=%p state=%s pos=%.2fs focusVideo=%p focusPlayer=%p setSize=%d",
               static_cast<void*>(video),
               video->debugPlayerPtr(),
               qPrintable(bubbleStateToString(state)),
               video->position() / 1000.0,
               static_cast<void*>(focusVideo),
               focusVideo->debugPlayerPtr(),
               m_focusSleepBookkeeping.videosAsleep.size());
        makeCurrent();
        video->enterFocusSleep();
        qDebug("[Focus][membership] after enter video=%p player=%p state=%s pos=%.2fs",
               static_cast<void*>(video),
               video->debugPlayerPtr(),
               qPrintable(bubbleStateToString(video->bubbleState())),
               video->position() / 1000.0);
        doneCurrent();
    }
}

void CanvasWidget::endFocusPlaybackPolicy()
{
    // このメソッドを抜けた後にスケジュールする遅延ウェイク(下記)は、必ず
    // この呼び出し時点の世代番号を運ぶ。以後どこかで別のbegin/end呼び出しが
    // あれば世代が進み、この呼び出しでスケジュールした遅延ウェイクは発火時に
    // 自分自身を無効と判断して何もしなくなる（早期returnのno-op呼び出しも
    // 含めて毎回進めることで、待機中の古いタイマーを確実に無効化する）。
    ++m_focusSleepBookkeeping.generation;

    if (!m_focusSleepBookkeeping.active && m_focusSleepBookkeeping.videosAsleep.isEmpty()) return;

    qDebug("[Focus][membership] end policy active=%d setSize=%d",
           m_focusSleepBookkeeping.active,
           m_focusSleepBookkeeping.videosAsleep.size());

    QList<QPointer<CanvasVideo>> pausedVideos;
    pausedVideos.reserve(m_focusSleepBookkeeping.videosAsleep.size());
    QList<bool> resumePlaybackFlags;
    resumePlaybackFlags.reserve(m_focusSleepBookkeeping.videosAsleep.size());
    QList<qint64> focusSleepElapsedAtEnd;
    focusSleepElapsedAtEnd.reserve(m_focusSleepBookkeeping.videosAsleep.size());
    for (CanvasItem *item : m_items) {
        auto *video = canvasItemAsVideo(item);
        if (video && m_focusSleepBookkeeping.videosAsleep.contains(video)) {
            pausedVideos.append(QPointer<CanvasVideo>(video));
            resumePlaybackFlags.append(m_focusSleepBookkeeping.resumeAfterFocus.value(video, true));
            focusSleepElapsedAtEnd.append(video->focusSleepElapsedMs());
            // Idleのまま「保留中のFocusSleep」（m_focusSleepPending）にされて
            // いたバブルは、このフォーカスセッションが終わる時点で必ず解除
            // する。下の遅延wakeはstate==FocusSleepのバブルしか救済しない
            // ため、Idleのままだったバブルはここで解除しないと、フォーカス
            // 解除後にこのバブルをフルスクリーンにせず普通に再生しようと
            // しただけでも横取りされてFocusSleepへ送られてしまい、サムネ・
            // 再生時間は取得できるのに実際には再生されない不具合になる。
            if (video->hasPendingFocusSleep()) {
                qDebug("[Focus][membership] clearing stale pending-focus-sleep on policy end video=%p",
                       static_cast<void*>(video));
                video->clearPendingFocusSleep();
            }
        }
    }
    m_focusSleepBookkeeping.videosAsleep.clear();
    m_focusSleepBookkeeping.resumeAfterFocus.clear();
    m_focusSleepBookkeeping.active = false;

    constexpr int FocusWakeInitialDelayMs = 450;
    constexpr int FocusWakeStaggerMs = 120;
    // FocusSleep開始からこの時間を超えてフォーカスが続いた場合、元が
    // 再生中でも自動再開しない。5秒時点の完全解放後も、30秒までは
    // 誤操作からの復帰用に「元が再生中だった」記録だけを有効に保つ。
    static constexpr qint64 FocusAutoResumeWindowMs = 30000;
    const quint64 scheduledGeneration = m_focusSleepBookkeeping.generation;
    for (int i = 0; i < pausedVideos.size(); ++i) {
        const QPointer<CanvasVideo> videoPtr = pausedVideos.at(i);
        const bool resumePlayback = resumePlaybackFlags.value(i, true);
        const qint64 elapsedAtFocusEndMs = focusSleepElapsedAtEnd.value(i, -1);
        const int delayMs = FocusWakeInitialDelayMs + i * FocusWakeStaggerMs;
        qDebug("[Focus][membership] schedule wake index=%d delay=%dms resume=%d elapsed=%lldms video=%p gen=%llu",
               i,
               delayMs,
               resumePlayback,
               static_cast<long long>(elapsedAtFocusEndMs),
               static_cast<void*>(videoPtr.data()),
               static_cast<unsigned long long>(scheduledGeneration));

        QTimer::singleShot(delayMs, this, [this, videoPtr, resumePlayback, elapsedAtFocusEndMs,
                                           scheduledGeneration]() {
            if (scheduledGeneration != m_focusSleepBookkeeping.generation) {
                // このタイマーをスケジュールした後に、別のフォーカスセッションの
                // begin/endが発生している。その新しいセッションが対象動画の
                // Sleep状態を管理し直しているはずなので、ここで無条件に起こすと
                // 「新しいセッションではまだ寝ているべき動画」を横取りして
                // 再生・音声を再開させてしまう。何もせず終了する。
                qDebug("[Focus][membership] skip delayed wake stale-generation video=%p schedGen=%llu curGen=%llu",
                       static_cast<void*>(videoPtr.data()),
                       static_cast<unsigned long long>(scheduledGeneration),
                       static_cast<unsigned long long>(m_focusSleepBookkeeping.generation));
                return;
            }
            CanvasVideo *video = videoPtr.data();
            if (!video) {
                qDebug("[Focus][membership] skip delayed wake null video");
                return;
            }
            if (!m_items.contains(video)) {
                qDebug("[Focus][membership] skip delayed wake missing video=%p",
                       static_cast<void*>(video));
                return;
            }
            const bool autoResumeExpired =
                resumePlayback
                && elapsedAtFocusEndMs >= FocusAutoResumeWindowMs;
            // 2026-07-04: FocusSleepの猶予を超えて、待機中にBubbleController::
            // requestDeepFocusSleep()でデコード資源まで解放（Sleepへ降格）
            // されていた場合はここに来る。この場合wakeFromFocusSleep()は
            // 使えない（既にmpvプレイヤー自体が無く、内部のnullガードで
            // 何もせず終わってしまう）。元が再生中なら再生へ、停止中なら
            // 停止状態へ戻すため、resumePlaybackをdeep sleep復帰経路へ渡す。
            if (video->bubbleState() == BubbleState::Sleep) {
                if (!video->canWakeFromFocusPolicy()) {
                    qDebug("[Focus][membership] skip delayed wake ended-sleep video=%p",
                           static_cast<void*>(video));
                    return;
                }
                // 元々一時停止中だった動画は、5秒後に完全解放まで進んだ
                // 場合はフォーカス解除だけでは再生成しない。手動再生時に
                // 通常のSleep復帰経路で作り直す。
                if (!resumePlayback) {
                    qDebug("[Focus][membership] keep deep-sleep (was paused) video=%p elapsed=%lldms",
                           static_cast<void*>(video),
                           static_cast<long long>(elapsedAtFocusEndMs));
                    return;
                }
                // FocusSleep開始から30秒以上フォーカスが続いた場合は、
                // 元が再生中でも自動再開権を失効させ、完全解放を維持する。
                if (autoResumeExpired) {
                    qDebug("[Focus][membership] keep deep-sleep (auto-resume expired) video=%p elapsed=%lldms limit=%lldms",
                           static_cast<void*>(video),
                           static_cast<long long>(elapsedAtFocusEndMs),
                           static_cast<long long>(FocusAutoResumeWindowMs));
                    return;
                }
                qDebug("[Focus][membership] delayed wake (deep-slept, re-opening) video=%p resume=%d",
                       static_cast<void*>(video),
                       resumePlayback);
                video->wakeFromDeepFocusSleep(resumePlayback);
                return;
            }

            if (video->bubbleState() != BubbleState::FocusSleep) {
                qDebug("[Focus][membership] skip delayed wake video=%p state=%s",
                       static_cast<void*>(video),
                       qPrintable(bubbleStateToString(video->bubbleState())));
                // 保険: Idleのまま「保留中のFocusSleep」フラグだけが
                // 残っているケース（実際にはFocusSleepへ遷移していない）。
                // ここで救済しないと、次にこのバブルを再生しようとした瞬間に
                // 横取りされてFocusSleepへ送られ、再生できない不具合になる。
                if (video->hasPendingFocusSleep()) {
                    qDebug("[Focus][membership] clearing stale pending-focus-sleep video=%p",
                           static_cast<void*>(video));
                    video->clearPendingFocusSleep();
                }
                return;
            }

            // 通常は5秒でSleepへ降格済みなのでここへ来ない。万一タイマー
            // 進行が止まってFocusSleepのまま30秒を超えていた場合も、
            // 自動再開だけは行わず異常をログへ残す。
            if (autoResumeExpired) {
                qWarning("[Focus][membership] auto-resume expired but state still FocusSleep video=%p elapsed=%lldms",
                         static_cast<void*>(video),
                         static_cast<long long>(elapsedAtFocusEndMs));
                return;
            }

            qDebug("[Focus][membership] delayed wake focus-sleep video=%p player=%p state=%s pos=%.2fs",
                   static_cast<void*>(video),
                   video->debugPlayerPtr(),
                   qPrintable(bubbleStateToString(video->bubbleState())),
                   video->position() / 1000.0);
            makeCurrent();
            video->wakeFromFocusSleep(resumePlayback);
            doneCurrent();
            qDebug("[Focus][membership] after delayed wake video=%p player=%p state=%s pos=%.2fs",
                   static_cast<void*>(video),
                   video->debugPlayerPtr(),
                   qPrintable(bubbleStateToString(video->bubbleState())),
                   video->position() / 1000.0);
        });
    }
}

void CanvasWidget::forgetFocusPausedVideo(CanvasVideo *video)
{
    if (!video) return;
    m_focusSleepBookkeeping.videosAsleep.remove(video);
    m_focusSleepBookkeeping.resumeAfterFocus.remove(video);
}

void CanvasWidget::focusItem(CanvasItem *item)
{
    if (!item) return;
    // 2026-07-12: 通常キャンバスパンの慣性で画面が動いている最中にフォーカス
    // に入ると、これから始まる「寄る」カメラアニメと同じm_cameraPosを奪い
    // 合ってガクつくため、フォーカス開始時点で即座に止める。
    m_canvasPanInertia->stop();
    beginFocusPlaybackPolicy(item);
    // 2026-07-13 バグ修正（保険）: フォーカス対象が掴み上げ（lift）中の
    // 場合、以下のズーム計算はitemRect()（lift無し）基準なのに、描画は
    // liftScaleを掛けた大きさになるため、liftScaleが1.0でないと全画面が
    // 数%拡大されたように収まらない。右ダブルクリックでの全画面化など、
    // リフトが戻り切る前にフォーカスへ入る経路があるため、フォーカス
    // 開始時点でリフトを確実に解除しておく。
    if (m_activeItem == item)
        m_liftAnim->stop();
    item->setLiftScale(1.0);
    m_preFocusCameraPos  = m_cameraPos;
    m_preFocusCameraZoom = m_cameraZoom;
    m_pendingFocusItem   = item;
    // 2026-07-15: このフォーカス試行の世代を記録する（下の非同期完了ラムダは
    // ポインタ一致ではなく世代一致で有効性を判定する。CanvasWidget.h の
    // m_focusGeneration のコメント参照）。
    const quint64 focusGen = ++m_focusGeneration;
    QRectF ir = item->itemRect();
    double targetW = width(), targetH = height();
    if (auto *mw = qobject_cast<MainWindow*>(window())) {
        QSize fs = mw->calcFocusSize(ir.width(), ir.height());
        targetW = fs.width(); targetH = fs.height();
    }
    double z = FocusSessionController::computeFocusZoomLevel(ir.width(), ir.height(), targetW, targetH);
    // 2026-07-04調査用: Flowチェーンの全画面切り替え直後にカメラが
    // 対象バブルへ移動していないように見える不具合の調査ログ。
    // animateCamera()の実際の引数と、その後どちらの分岐（ウィンドウ
    // 最大化中の即時パス／通常のリサイズアニメパス）を通るかを可視化する。
    qDebug("[Focus][camera] focusItem item=%p ir=(%.1f,%.1f %.1fx%.1f) target=%.1fx%.1f z=%.3f curCam=(%.1f,%.1f) curZoom=%.3f",
           static_cast<void*>(item), ir.x(), ir.y(), ir.width(), ir.height(),
           targetW, targetH, z, m_cameraPos.x(), m_cameraPos.y(), m_cameraZoom);
    // 巨大バブルへのフォーカスはズームアウトになり得るため、Autoではなく
    // 明示的にTarget(寄り先のバブル基準)を渡す。
    animateCamera(ir.center(), z, 500, CameraAnimAnchor::Target);
    if (auto *mw = qobject_cast<MainWindow*>(window())) {
        const auto focusMode = AppSettings::instance().focusWindowMode();
        const bool lockWindowSize = focusMode == AppSettings::FocusWindowMode::CanvasFixed;
        const bool fixWindow      = focusMode == AppSettings::FocusWindowMode::WindowFixed;
        m_focusWindowGeometry.preWindowMaximized = mw->isWindowFullscreen();
        m_focusWindowGeometry.preWindowGeometry = mw->geometry();
        qDebug("[Focus][camera] preWindowMaximized=%d lockWindowSize=%d",
               m_focusWindowGeometry.preWindowMaximized, lockWindowSize);

        // 2026-10-11: ウィンドウ固定モード。ウィンドウ全画面の場合と同じく、
        // ウィンドウには一切触れずカメラだけで動画を収める（対比が違えば黒帯）。
        // ここで即座にフォーカス確定する（リサイズアニメ完了待ちが無いため）。
        m_focusWindowGeometry.windowFixedFocusActive = fixWindow;
        if (fixWindow) {
            m_focusWindowGeometry.windowSizeLockFocusActive = false;
            m_focusWindowGeometry.aspectFitBaseSize = QSize();
            m_focusedItem = item;
            m_pendingFocusItem = nullptr;
            m_focusWindowGeometry.focusEntryGeometry = mw->geometry();
            return;
        }

        if (m_focusWindowGeometry.preWindowMaximized) {
            // ウィンドウそのものが全画面（最大化）状態の場合は、ウィンドウサイズを
            // 動画のアスペクト比に合わせて変更しない。カメラズームは既に
            // 「現在のウィンドウサイズに収まる」計算になっているため、
            // 動画とウィンドウのアスペクト比が異なる場合は黒帯として表示される。
            // （最大化状態からリサイズしようとすると、ちょうど画面いっぱいの
            //   サイズになるケースでジオメトリのクランプ計算が破綻し
            //   クラッシュしていたため、全画面時はそもそもリサイズしない）
            //
            // 2026-07-09 バグ修正(ウィンドウサイズロック): 従来ここで
            // windowSizeLockFocusActive を無条件にfalseへ落としていたため、
            // 「ウィンドウ全画面→バブル全画面→バブル全画面解除」の順で操作
            // すると、解除時にウィンドウ全画面が解除されず（あるいは途中で
            // notifyWindowMaximizedChanged()が上書きした動画アスペクト比の
            // ジオメトリへ戻り）、ロックしておいた本来のサイズへ戻らなかった。
            // ロック有効時は、フォーカス開始時点で既にウィンドウ全画面でも
            // 「ウィンドウ全画面化する前の通常ジオメトリ」
            // (MainWindow::preFullscreenGeometry())を復元先として記録し、
            // ロックセッションを有効のまま維持する。これによりバブル全画面
            // 解除時は必ずウィンドウ全画面も解除され、全画面化前のサイズへ戻る。
            {
                const QRect preFs = mw->preFullscreenGeometry();
                m_focusWindowGeometry.windowSizeLockFocusActive =
                    lockWindowSize && preFs.isValid();
                m_focusWindowGeometry.preWindowGeometry = preFs;
                if (m_focusWindowGeometry.windowSizeLockFocusActive)
                    mw->setWindowSettingsSaveSuspended(true);
            }
            // 全画面中はウィンドウの対比フィット自体を行わないため基準サイズも
            // 無効化しておく（前のフォーカスセッションの値の持ち越し防止。
            // 全画面解除時はnotifyWindowMaximizedChanged()が改めて記録する）。
            m_focusWindowGeometry.aspectFitBaseSize = QSize();
            m_focusedItem = item;
            m_pendingFocusItem = nullptr;
            m_focusWindowGeometry.focusEntryGeometry = mw->geometry();
            return;
        }

        m_focusWindowGeometry.windowSizeLockFocusActive = lockWindowSize;
        if (lockWindowSize)
            mw->setWindowSettingsSaveSuspended(true);
        // 2026-07-18追加: 対比フィット前のウィンドウサイズを「基準サイズ」
        // として記録する。連続再生で対比が変わった際の
        // refitFocusedWindowForAspectChange()が毎回ここを基準にフィットし
        // 直す（FocusWindowGeometryState::aspectFitBaseSize参照）。
        m_focusWindowGeometry.aspectFitBaseSize = mw->geometry().size();
        qDebug("[Focus][camera] non-maximized path: animateToAspect開始 (m_focusedItemの更新はresizeAnim完了待ち)");
        // 2026-07-15: 450ms→500msへ変更。カメラの寄り演出(上のanimateCamera、
        // 500ms/InOutCubic)と長さ・カーブを完全に揃え、ウィンドウサイズの
        // 変化とカメラの寄りが同時に完了するようにする。
        mw->animateToAspect(ir.width(), ir.height(), 500);
        // 前の呼び出しで繋いだままになっている可能性のある接続を必ず切ってから
        // 新しく繋ぐ（宙に浮いたラムダが後で無関係なタイミングで発火するのを防ぐ）。
        disconnect(m_resizeAnimFinishedConn);
        m_resizeAnimFinishedConn = connect(mw->resizeAnim(), &QPropertyAnimation::finished, this, [this,item,focusGen](){
            disconnect(m_resizeAnimFinishedConn);
            // 2026-07-09 バグ修正（D&Dが🚫固定になる不具合の残存原因）:
            // フォーカスイン中(450msのリサイズアニメ完了前)に全画面解除や
            // バブル削除が行われた場合、このラムダが後から（unfocusItem()の
            // 復元アニメ完了時などに）発火して m_focusedItem を書き戻して
            // しまい、見た目は全画面でないのに isFocusDropBlocked() が
            // 立ちっぱなしになる。フォーカスが今もこのitemへ向かっている
            // 場合のみ確定させる。
            // 2026-07-15: 従来はここを m_pendingFocusItem != item （ポインタ
            // 比較）で判定していたが、item破棄後に同じアドレスへ別の
            // CanvasItemが再確保されるとポインタ比較だけでは区別できない
            // （ABA問題）。発行時点の世代(focusGen)と現在の世代を比較する
            // 方式にして、ポインタの生死に依存しないようにする。
            if (focusGen != m_focusGeneration) {
                qDebug("[Focus][camera] resizeAnim finished: フォーカスは既に解除/変更済みのため無視 (item=%p gen=%llu current=%llu)",
                       static_cast<void*>(item),
                       static_cast<unsigned long long>(focusGen),
                       static_cast<unsigned long long>(m_focusGeneration));
                return;
            }
            qDebug("[Focus][camera] resizeAnim finished: m_focusedItem=%p に更新", static_cast<void*>(item));
            m_focusedItem = item; m_pendingFocusItem = nullptr;
            if (auto *mw2 = qobject_cast<MainWindow*>(window()))
                m_focusWindowGeometry.focusEntryGeometry = mw2->geometry();
        });
    } else {
        m_focusedItem = item; m_pendingFocusItem = nullptr;
    }
}

void CanvasWidget::unfocusItem()
{
    cancelLongPress();
    endFocusPlaybackPolicy();
    // 2026-07-12: フォーカス解除の「引く」カメラアニメ（下のanimateCamera）と
    // 慣性パンが同じm_cameraPosを奪い合わないよう、解除時は慣性を即座に止める。
    stopFocusPanInertia();
    // 2026-07-09 バグ修正（D&Dが🚫固定になる不具合の残存原因）:
    // フォーカスイン中（resizeAnim完了前＝m_focusedItem確定待ち）に解除された
    // 場合、focusItem()で繋いだ「m_focusedItem = item」のラムダがまだ生きて
    // いる。従来、後述のデフォルトパス（ウィンドウサイズロック無効・非全画面）
    // ではこの接続を切らずに復元アニメ（同じ共有resizeAnim）を開始していた
    // ため、復元アニメ完了時に宙に浮いたラムダが発火し、解除したはずの
    // m_focusedItemが復活 → isFocusDropBlocked()が真のまま残り、以後の
    // ドラッグ&ドロップが全て禁止カーソルになっていた。どのパスを通るかに
    // よらず、解除時点で必ず接続を切る。
    disconnect(m_resizeAnimFinishedConn);
    // 2026-07-15: 世代を進めて、この時点までに発行された非同期完了ラムダ
    // (focusItem()/notifyWindowMaximizedChanged()由来)を確実に無効化する
    // （disconnect()だけでなく、万一別経路で同じ接続変数を取り合っていた
    // 場合の保険。CanvasWidget.h の m_focusGeneration 参照）。
    ++m_focusGeneration;
    m_focusedItem = nullptr; m_pendingFocusItem = nullptr;
    // 対比フィットの基準サイズはフォーカスセッション限りの値なので、
    // 次のセッションへ持ち越さないようここで無効化する（2026-07-18）。
    m_focusWindowGeometry.aspectFitBaseSize = QSize();
    // 2026-07-08: フォーカス解除（引く）演出は、フォーカスイン（寄る、500ms）
    // より軽快に感じられるよう、ユーザー要望でおよそ半分の300msにしている。
    // ウィンドウリサイズ側（下のanimateToGeometry呼び出し）も同じ300msに
    // 揃えている(2026-07-15: 従来は200msだったが、カメラと長さ・カーブを
    // 完全同期させるため300ms/InOutCubicへ統一)。
    // Start = いま画面中心にあるバブルを基準に、着地後の画面位置へ直線的に
    // 送り出す（寄る側と対称の画面座標補間）。
    animateCamera(m_preFocusCameraPos, m_preFocusCameraZoom, 300, CameraAnimAnchor::Start);
    if (auto *mw = qobject_cast<MainWindow*>(window())) {
        // 2026-10-11: ウィンドウ固定モードのフォーカスだった場合、ウィンドウは
        // 一度も動かしていないので、カメラの復元だけで終わり。
        if (m_focusWindowGeometry.windowFixedFocusActive) {
            m_focusWindowGeometry.windowFixedFocusActive = false;
            m_focusWindowGeometry.windowSizeLockFocusActive = false;
            return;
        }

        // 2026-07-08 バグ修正(バブル全画面まわりの根本修正、その1):
        // 「バブル全画面→ウィンドウ全画面→バブル全画面解除」の順で操作すると
        // ウィンドウ全画面の解除判定が効かなくなる(内部フラグ
        // MainWindow::m_windowFullscreenActiveがtrueのまま残る)不具合が
        // あった。原因は、ここから先の処理がmw->animateToGeometry()で
        // ウィンドウ形状を直接書き換えており、それがMainWindow::
        // setWindowFullscreen()を経由しないため、ウィンドウが実際には
        // 全画面ではなくなっているのにMainWindow側の管理フラグだけが
        // 「全画面のまま」取り残されていたこと。
        // ウィンドウ全画面のON/OFFは必ずMainWindow::setWindowFullscreen()
        // (またはそれを内部で呼ぶexitFullscreenToGeometry())経由でのみ
        // 行われるようにする。
        //
        // 2026-07-08 追記(ユーザーフィードバック): ウィンドウサイズロック
        // 設定が有効な場合、「バブル全画面→ウィンドウ全画面→バブル全画面
        // 解除」の解除時には、ウィンドウ全画面も一緒に解除し、ロックして
        // おいたフォーカス開始前のウィンドウサイズへ戻すのが正しい挙動との
        // こと。一方、ロック無効時はウィンドウ全画面はそのまま維持し、
        // バブル側はカメラズームの復元だけ行う（ウィンドウには触れない）。
        if (mw->isWindowFullscreen()) {
            if (m_focusWindowGeometry.windowSizeLockFocusActive && m_focusWindowGeometry.preWindowGeometry.isValid()) {
                mw->exitFullscreenToGeometry(m_focusWindowGeometry.preWindowGeometry, 200);
                disconnect(m_resizeAnimFinishedConn);
                m_resizeAnimFinishedConn = connect(mw->resizeAnim(), &QPropertyAnimation::finished,
                                this, [this, mw]{
                    mw->setWindowSettingsSaveSuspended(false);
                    disconnect(m_resizeAnimFinishedConn);
                });
            }
            m_focusWindowGeometry.windowSizeLockFocusActive = false;
            return;
        }

        // ウィンドウサイズロック設定は、フォーカス中にウィンドウの位置や
        // サイズがどう変わっても（ウィンドウ全画面を挟んだ場合や、一度も
        // 全画面解除しなかった場合を含む）、バブル全画面を解除したら常に
        // 「ウィンドウ全画面にする前のサイズ」へ戻す、という単純な規則で
        // 統一する。以前は「直前までウィンドウが全画面(最大化)だった場合は
        // 再度最大化する」動作だったが、バックグラウンド復帰やウィンドウ
        // 全画面解除と絡めて使うと挙動が分かりづらいというフィードバックが
        // あったため、常にウィンドウ全画面化前のサイズへ戻す形に変更した。
        // Window fullscreen is owned by MainWindow. Only restore geometry when
        // bubble focus itself resized the normal window.
        if (m_focusWindowGeometry.windowSizeLockFocusActive && m_focusWindowGeometry.preWindowGeometry.isValid()) {
            // 2026-10-11: 位置は戻さない。サイズだけフォーカス前に戻し、
            // 位置は今のウィンドウの中心を保つ（フォーカス中にウィンドウを
            // 動かしていた場合はその位置のまま）。
            // 「ウィンドウ位置を記憶」がONのときは位置も全画面前へ戻す。
            QRect targetGeom = m_focusWindowGeometry.preWindowGeometry;
            if (!AppSettings::instance().rememberWindowPosition()) {
                const QPointF curCenter = QRectF(mw->geometry()).center();
                targetGeom.moveTopLeft(QPoint(
                    qRound(curCenter.x() - targetGeom.width()  / 2.0),
                    qRound(curCenter.y() - targetGeom.height() / 2.0)));
            }
            mw->clampGeometryToScreen(targetGeom);
            mw->animateToGeometry(targetGeom, 300);
            // focusItem()側と同じ理由で、前の接続を必ず切ってから繋ぐ。
            disconnect(m_resizeAnimFinishedConn);
            m_resizeAnimFinishedConn = connect(mw->resizeAnim(), &QPropertyAnimation::finished,
                            this, [this, mw]{
                mw->setWindowSettingsSaveSuspended(false);
                disconnect(m_resizeAnimFinishedConn);
            });
            m_focusWindowGeometry.windowSizeLockFocusActive = false;
            return;
        }

        QRect cur = mw->geometry();
        QRect targetGeom = FocusSessionController::computeUnfocusRestoreGeometry(cur, m_focusWindowGeometry.focusEntryGeometry);
        mw->clampGeometryToScreen(targetGeom);
        mw->animateToGeometry(targetGeom, 300);
    }
}

void CanvasWidget::notifyWindowMaximizedChanged(bool isMaximizedNow)
{
    // 2026-07-08 バグ修正(バブル全画面まわりの根本修正、その2):
    // 「ウィンドウ全画面(OS最大化)→バブル全画面→ウィンドウ全画面解除」の
    // 順で操作すると、バブルは全画面のままなのに動画とウィンドウの
    // アスペクト比が合わず収まりが悪い、という不具合が報告された。
    //
    // 原因: focusItem()はフォーカス開始時点でウィンドウが既に最大化されて
    // いる場合、クラッシュ回避のためウィンドウのリサイズ自体をスキップし、
    // カメラズームだけで動画を収める（黒帯を許容する）設計になっている。
    // この関数はその後ウィンドウが最大化から復帰した際に呼ばれる箇所として
    // 用意されていたが、以前は中身が空(no-op)になっており、黒帯のまま
    // 何も補正されずに残っていた。
    //
    // 対応: ウィンドウが最大化から復帰した瞬間(isMaximizedNow==false)、
    // バブルが今まさにフォーカス中であれば、focusItem()の非最大化パスと
    // 同じ処理(animateToAspect()でウィンドウを動画のアスペクト比に合わせる)
    // をこの場で行う。この呼び出しはMainWindow::setWindowFullscreen()側で
    // ウィンドウの復帰リサイズが完全に終わった後にのみ発火するよう
    // 調整済み(同じ共有アニメーションを同時に取り合わないようにするため)。
    if (isMaximizedNow)
        return;

    CanvasItem *focusTarget = m_pendingFocusItem ? m_pendingFocusItem : m_focusedItem;
    if (!focusTarget)
        return;

    auto *mw = qobject_cast<MainWindow*>(window());
    if (!mw)
        return;

    // 2026-10-11: ウィンドウ固定モードではウィンドウ全画面の出入りでも
    // 対比フィット（ウィンドウのリサイズ）は行わない。
    if (m_focusWindowGeometry.windowFixedFocusActive)
        return;

    QRectF ir = focusTarget->itemRect();
    if (ir.width() <= 0 || ir.height() <= 0)
        return;

    const bool lockWindowSize =
        AppSettings::instance().focusWindowMode() == AppSettings::FocusWindowMode::CanvasFixed;
    m_focusWindowGeometry.preWindowMaximized = false;
    // 2026-07-09 バグ修正(ウィンドウサイズロックの根本原因):
    // 以前はここで無条件に preWindowGeometry を「現在のジオメトリ」で
    // 上書きしていた。フォーカス中にウィンドウ全画面のON/OFFを挟むと、
    // ウィンドウ全画面の解除先(m_preFullscreenGeometry)は既に
    // 動画アスペクト比へリサイズされた後の形になっているため、この上書きで
    // 「フォーカス開始前にロックしておいた本来のウィンドウサイズ」が
    // 動画アスペクト比のジオメトリに置き換わって失われていた。その結果、
    // バブル全画面を解除しても動画の縦横比のサイズのままになっていた。
    // 既にロックセッションが復元先を記録済みの場合は一切上書きしない
    // （復元先はフォーカスセッションを通して一度だけ確定させる）。
    if (!(m_focusWindowGeometry.windowSizeLockFocusActive
          && m_focusWindowGeometry.preWindowGeometry.isValid())) {
        m_focusWindowGeometry.preWindowGeometry = mw->geometry();
        m_focusWindowGeometry.windowSizeLockFocusActive = lockWindowSize;
        if (lockWindowSize)
            mw->setWindowSettingsSaveSuspended(true);
    }

    // 2026-07-18追加: 全画面解除で初めて対比フィットが可能になったタイミング。
    // ここでのフィット前サイズ（全画面から復帰した通常ジオメトリ）を
    // 基準サイズとして記録する（focusItem()非最大化パスと同じ役割）。
    m_focusWindowGeometry.aspectFitBaseSize = mw->geometry().size();

    // 2026-07-15: focusItem()と同じ理由で450ms→500ms(カメラと完全同期)。
    mw->animateToAspect(ir.width(), ir.height(), 500);
    disconnect(m_resizeAnimFinishedConn);
    CanvasItem *item = focusTarget;
    // 2026-07-15: focusItem()側と同じ世代カウンタ方式に統一する
    // （CanvasWidget.h の m_focusGeneration 参照）。ここでは新規フォーカスの
    // 開始ではなく既存フォーカスの再確定なので世代は進めず、現在値を
    // スナップショットするだけでよい（この後にfocusItem()/unfocusItem()が
    // 呼ばれれば、それらが世代を進めるので自動的に無効化される）。
    const quint64 focusGen = m_focusGeneration;
    m_resizeAnimFinishedConn = connect(mw->resizeAnim(), &QPropertyAnimation::finished, this, [this, item, mw, focusGen](){
        disconnect(m_resizeAnimFinishedConn);
        // focusItem()側のラムダと同じ理由のガード（2026-07-09）。アニメ完了
        // までにフォーカスが解除/変更/削除されていたら書き戻さない。
        // 2026-07-15: ポインタ比較(ABA問題あり)から世代比較に変更。
        if (focusGen != m_focusGeneration)
            return;
        m_focusedItem = item; m_pendingFocusItem = nullptr;
        m_focusWindowGeometry.focusEntryGeometry = mw->geometry();
    });
}

// 2026-07-17追加: フォーカス中のバブルの映像アスペクト比が途中で変わった
// 場合（再生リスト連続再生でnextUrlの動画へ切り替わり、前の動画と縦横比が
// 異なるケース等）に、ウィンドウを改めてフィットし直す。
// CanvasVideo::videoDimensionsReady 経由で、変化したバブルがまさに
// フォーカス中の場合にのみ呼ばれる（addMediaSource()の接続箇所参照）。
void CanvasWidget::refitFocusedWindowForAspectChange(CanvasItem *item)
{
    // フォーカス確定済み（m_pendingFocusItemではなくm_focusedItem）の
    // バブルだけを対象にする。フォーカスイン直後のリサイズアニメ進行中
    // (まだm_pendingFocusItemの段階)にここで割り込むと、focusItem()側の
    // アニメと同じ共有resizeAnimを取り合ってしまうため対象外にする。
    if (!item || item != m_focusedItem)
        return;

    auto *mw = qobject_cast<MainWindow*>(window());
    if (!mw)
        return;

    QRectF ir = item->itemRect();
    if (ir.width() <= 0 || ir.height() <= 0)
        return;

    // ウィンドウ全画面（OS最大化）中は、focusItem()の全画面パスと同じく
    // ウィンドウサイズを一切変えない（動画とのアスペクト比の差は黒帯で
    // 許容する。理由はfocusItem()内の同種コメント参照）。
    //
    // 2026-07-18追記: ただし「何もしない」だと、ウィンドウサイズが変わらない
    // ＝resizeGL()のカメラ追従補正も一切走らないため、カメラズームが
    // 旧対比のitemRect()基準のまま残り、新しい対比の動画が黒帯にすら
    // ならず収まらなくなる。全画面中はウィンドウには触れず、カメラだけを
    // 新しいitemRect()へフィットし直す（ズーム式はresizeGL()の追従分岐と
    // 同一。中心も同様にitemRect()の中心）。
    // 2026-10-11: ウィンドウ固定モードも同様に、カメラだけを新しい対比へフィットし直す。
    if (mw->isWindowFullscreen() || m_focusWindowGeometry.windowFixedFocusActive) {
        const double z = qBound(0.1, qMin((double)width() / ir.width(),
                                          (double)height() / ir.height()), 10.0);
        qDebug("[Focus][camera] refit(fullscreen): camera only item=%p ir=%.1fx%.1f z=%.3f",
               static_cast<void*>(item), ir.width(), ir.height(), z);
        animateCamera(ir.center(), z, 500, CameraAnimAnchor::Target);
        return;
    }

    // 2026-07-18改訂: 従来の「現在のウィンドウサイズの内側にフィット」だと、
    // 対比が変わるたびにウィンドウが単調に縮んでいく（16:9→9:16→16:9と
    // 続くと幅→高さ→幅と削られ、最終的に最小サイズ付近まで縮む）ため、
    // フォーカス開始時に記録した「対比フィット前の基準サイズ」
    // （m_focusWindowGeometry.aspectFitBaseSize。focusItem()/
    // notifyWindowMaximizedChanged()で記録）を基準に毎回フィットし直す。
    // 基準が無効な場合のみ従来通り現在サイズ基準（animateToAspect()内で
    // width()/height()が使われる）。
    // カメラ側はここでは動かさない: animateToAspect()によるリサイズ中、
    // resizeGL()が毎tickフォーカス対象のitemRect()へ追従補正するため、
    // ウィンドウの変化に合わせて自然にフィットする。
    qDebug("[Focus][camera] refit for aspect change: item=%p ir=%.1fx%.1f curWin=%dx%d base=%dx%d",
           static_cast<void*>(item), ir.width(), ir.height(), mw->width(), mw->height(),
           m_focusWindowGeometry.aspectFitBaseSize.width(),
           m_focusWindowGeometry.aspectFitBaseSize.height());

    mw->animateToAspect(ir.width(), ir.height(), 500,
                        m_focusWindowGeometry.aspectFitBaseSize);
    disconnect(m_resizeAnimFinishedConn);
    const quint64 focusGen = m_focusGeneration;
    m_resizeAnimFinishedConn = connect(mw->resizeAnim(), &QPropertyAnimation::finished, this,
                                        [this, item, mw, focusGen](){
        disconnect(m_resizeAnimFinishedConn);
        if (focusGen != m_focusGeneration)
            return;
        m_focusWindowGeometry.focusEntryGeometry = mw->geometry();
    });
}

void CanvasWidget::setContentOpacity(double opacity)
{
    if (m_contentOpacityAnim)
        m_contentOpacityAnim->stop();
    m_contentOpacity = qBound(0.0, opacity, 1.0);
    update();
    emit playbackOverlayUpdateRequested();
    emit focusOverlayUpdateRequested();
    emit borderOverlayUpdateRequested();
    emit launcherTooltipOverlayUpdateRequested();
}

void CanvasWidget::fadeContentOpacityTo(double targetOpacity, int durationMs,
                                        std::function<void()> onFinished)
{
    if (!m_contentOpacityAnim || durationMs <= 0) {
        setContentOpacity(targetOpacity);
        if (onFinished)
            onFinished();
        return;
    }

    m_contentOpacityAnim->stop();
    m_contentOpacityAnim->setDuration(durationMs);
    m_contentOpacityAnim->setEasingCurve(QEasingCurve::OutCubic);
    m_contentOpacityAnim->setStartValue(m_contentOpacity);
    m_contentOpacityAnim->setEndValue(qBound(0.0, targetOpacity, 1.0));

    if (onFinished) {
        auto *conn = new QMetaObject::Connection;
        *conn = connect(m_contentOpacityAnim, &QVariantAnimation::finished, this,
                        [onFinished, conn]() {
            onFinished();
            QObject::disconnect(*conn);
            delete conn;
        });
    }
    m_contentOpacityAnim->start();
}

// 2026-07-15: スフィア演出(startSphereAnim/advanceSpherePhase/paintSphere、
// バブルが球体になってから全画面へ広がる構想の残骸)と、その専用経路だった
// focusItemImmediate/unfocusItemImmediate を削除した。どこからも呼ばれて
// おらず、フォーカス演出は focusItem()/unfocusItem() に一本化されている。

// ---- マウスイベント ----
void CanvasWidget::handleMousePress(QMouseEvent *event, bool onUI)
{
    // 2026-09-20: マウスのサイドボタンで元に戻す／やり直し。
    // 奥（進む＝XButton2/Forward）がやり直し、手前（戻る＝XButton1/Back）が
    // 元に戻す。ほかのボタンを押したまま（ドラッグ中など）は操作の途中で
    // 形が変わってしまうので受け付けない。下の beginUndoStep() より先に見る
    // （サイドボタン自体を1ステップとして積まないため）。
    if (event->button() == Qt::BackButton || event->button() == Qt::ForwardButton) {
        if (event->buttons() == event->button()) {
            if (event->button() == Qt::BackButton) undo();
            else                                   redo();
        }
        event->accept();
        return;
    }

    // 2026-09-17: 元に戻す/やり直しのステップ開始。最初のボタンを押した瞬間の
    // キャンバスの形を覚え、全ボタンを離した時点（handleMouseRelease）で
    // 形が変わっていれば1ステップとして積む。
    if (event->buttons() == event->button())
        beginUndoStep();

    QPointF pos = event->position();
    if (event->button() == Qt::LeftButton)
        m_dragStartScreenPos = pos;

    auto *av = canvasItemAsVideo(m_activeItem);
    const bool playbackBarInteractive = av && m_playbarVisible;

    // 2026-09-12: フォーカスラベル（バブル左上のロゴ＋テキスト）の上での
    // 左クリックはUI操作として消費する。ここで止めないと下のバブルの
    // ヒットテストへ落ち、シングルクリック判定→togglePlayPause()まで
    // 走って「ラベルを押しただけで再生が止まる」ことになる。ラベルの矩形は
    // 直近のオーバーレイ描画が実際に使ったもの（CanvasVideo::labelScreenRect）
    // なので、フェードで消えている間は空＝素通りする。中ボタン（カメラ
    // パン）・右ボタン（アクティブ化）はラベルの上でも従来通り効かせる。
    // 2026-09-12: ラベルとリストの操作は「いま左上に出ているラベル」の
    // 持ち主へ回す。固定されている場合、その持ち主はアクティブなバブルとは
    // 限らない（固定ラベルは非アクティブでも触れる）。
    CanvasVideo *labelOwner = labelOwnerVideo();
    const bool labelInteractive = labelOwner
        && (labelOwner != av || playbackBarInteractive);
    if (event->button() == Qt::LeftButton && labelInteractive) {
        // 開いているコントローラのパネル内 → 行のクリック（＝前後の
        // 動画へ移動）として処理する。
        // 行の上を押した場合は、ここでは何もせず「掴んだ」状態にする。
        // そのまま離せばその動画へ移動、押したまま動かせば並べ替え。
        const int rowIndex = labelOwner->panelRowIndexAt(pos);
        if (rowIndex >= 0) {
            labelOwner->beginRowDrag(rowIndex, pos.y(), pos.x());
            m_labelRowDragVideo = labelOwner;
            m_activeMode = "labelRowDrag";
            m_dragStartScreenPos = pos;
            event->accept();
            return;
        }
        // ヘッダのボタン・閉じるなど（押した瞬間に効いてよいもの）。
        if (labelOwner->handleLabelPanelClick(pos)) {
            emit playbackOverlayUpdateRequested();
            event->accept();
            return;
        }
        // ラベル本体を押した → ここでは何もせず「引っ張り」を開始する。
        // 2026-09-12: 押した瞬間ではなく離したときに開閉するよう変更した。
        // 押したまま下へ引っ張るとラベルがびよーんと伸び、しきい値を
        // 超えて離すと固定（鍵マーク）／解除がトグルする。引っ張らずに
        // 離した場合は従来通りリストの開閉。
        // 対象は「開ける中身があるラベル（List）」か「固定できる／固定中の
        // ラベル」。Liveやただの動画のラベルはクリックを消費せず、下の
        // バブルの操作（再生/停止など）へそのまま通す。
        const bool labelActionable = labelOwner->labelPanelSupported()
            || labelOwner->labelPinnable() || labelOwner->labelPinned()
            // 2026-09-23: 長押しできるラベル（Solo など）も押せる
            || labelOwner->labelHoldAvailable();
        if (labelActionable && labelOwner->labelScreenRect().contains(pos)) {
            m_labelPullVideo = labelOwner;
            labelOwner->beginLabelPull();
            m_activeMode = "labelPull";
            m_dragStartScreenPos = pos;
            emit playbackOverlayUpdateRequested();
            event->accept();
            return;
        }
        // それ以外の場所を押したらコントローラは閉じる。ここでは
        // returnせず、その場所本来の操作（再生バー・バブル選択等）は
        // そのまま続行させる。
        if (labelOwner->labelPanelOpen()) {
            labelOwner->closeLabelPanel();
            emit playbackOverlayUpdateRequested();
        }
    }

    // 2026-09-17 バグ修正: 再生バー（時刻表示・シーク・再生/停止などのボタン）は
    // 左クリック専用にする。以前はボタンの種類を見ていなかったため、右クリック
    // （アクティブ化専用）や中クリック（カメラパン）でもシーク・再生停止が
    // 効いてしまっていた。左以外はこのまま下の各ボタンの分岐へ進む。
    if (event->button() == Qt::LeftButton
        && playbackBarInteractive && pos.y()>=height()-56-20) {
        // 時刻テキストのクリック → 表示モード（経過/合計 ⇔ カウントダウン）
        // のトグル。ロック中（配信等）は何も起きない。
        if (av->timeTextScreenRect(width(),height()).contains(pos)) {
            if (av->toggleSeekTimeModeIfUnlocked()) { event->accept(); return; }
        }
        const int bH=56,bY=height()-bH,bx=28,vbx=bx+36;
        const int sx=vbx+24,sw=width()-sx-100,sy=bY+bH/2-2;
        if (av->seekBarVisible() && pos.x()>=sx&&pos.x()<=sx+sw&&pos.y()>=sy-10&&pos.y()<=sy+14) {
            m_activeMode="seeking"; m_dragStartPos=pos;
            qint64 dur=av->duration();
            if (dur>0) av->bcSeek(static_cast<qint64>(dur*(pos.x()-sx)/sw));
            event->accept(); return;
        }
        if (av->handlePlaybackBarClick(pos,width(),height())) { event->accept(); return; }
    }

    if (event->button()==Qt::MiddleButton) {
        // フォーカスイン/アウト（寄る/引く）のカメラアニメ中は、中ボタン
        // ドラッグでのカメラパンを受け付けない。アニメーションと手動操作が
        // 同じm_cameraPosを奪い合ってガクつく・着地位置がずれる不具合を
        // 防ぐため（引く演出中だけ手動操作を許可する案も試したが、
        // 割り込み時にunfocusItem()が同時に走らせるウィンドウリサイズ
        // アニメが宙に浮いてしまい、次の寄る演出に悪影響が出る不具合が
        // あったため、シンプルに「アニメ中は常にブロック」に統一した）。
        if (onUI||isCameraFocusAnimationRunning()) { event->accept(); return; }

        // 2026-07-12: 全画面中は同じ中ボタンドラッグで「バブル内の表示位置の
        // パン」を行う（元は画像バブル限定・左ドラッグだった機能。動画では
        // 左ボタンに「長押しで2倍速」機能が既にあり衝突するため中ボタンへ
        // 変更し、ランチャー〈exe/lnk。そもそもフォーカス状態になり得ない〉
        // 以外の全種別に拡張した）。通常時のcamera_panと入力の流儀を揃えて
        // あるので、handleMouseMove側は同じ計算式をそのまま使い回せる。
        CanvasItem *focusTarget = m_focusedItem ? m_focusedItem : m_pendingFocusItem;
        if (focusTarget) {
            if (!dynamic_cast<CanvasLauncher*>(focusTarget)) {
                // 2026-07-12: 中ボタンのダブルクリックで、ズーム/パンで
                // ずらした表示状態をネイティブ（fit）状態へアニメーションで
                // 戻す。左/右クリックのダブルクリック判定と違い、対象への
                // ヒットテストは不要（カメラ操作なので画面のどこを押しても
                // 同じ）なため、前回の中ボタン押下からの経過時間だけを見る
                // 簡易判定にしてある。
                // 2026-07-12 追記: 経過時間だけだと「中ボタンドラッグでパン
                // →すぐ離してまた別方向へパン」という連続操作を誤ってダブル
                // クリック扱いしてしまう（1回目のドラッグで動いた先の座標で
                // 2回目を押しても300ms以内なら常に成立してしまうため）。
                // 前回の押下位置からほぼ動いていない場合だけダブルクリックと
                // みなすよう、時間に加えて座標の近さも見る。
                constexpr qint64 kMiddleDoubleClickMs = 300;
                constexpr qreal  kMiddleDoubleClickMaxDist = 6.0; // px（マンハッタン距離）
                const bool isNearLastPos = m_middleClickClock.isValid()
                    && (pos - m_lastMiddleClickPos).manhattanLength() < kMiddleDoubleClickMaxDist;
                const bool isDoubleClick = m_middleClickClock.isValid()
                                         && m_middleClickClock.elapsed() < kMiddleDoubleClickMs
                                         && isNearLastPos;
                m_middleClickClock.restart();
                m_lastMiddleClickPos = pos;

                if (isDoubleClick) {
                    resetFocusCameraToNative(focusTarget);
                    event->accept(); return;
                }

                m_activeMode = QStringLiteral("focus_image_pan");
                m_dragStartPos = pos;
                m_startCameraPos = m_cameraPos;
                // 2026-07-12: 新しいドラッグを始めたら、前回のフリックで
                // 走っていたかもしれない慣性アニメは打ち切り、速度サンプル
                // 計測も0から始め直す（同じ入力を二重に奪い合わないため）。
                m_focusPanInertia->beginDrag();
                // 2026-07-17: ホイールズームのイージング（m_zoomEase）がまだ
                // 動いている最中にドラッグを始めると、tick側の再センタリング
                // （m_cameraPosを書き換える）とここのドラッグ計算
                // （m_startCameraPosを基準にm_cameraZoomで割る）が同時に
                // m_cameraPos/m_cameraZoomを奪い合い、m_cameraZoomが毎フレーム
                // 変わり続けることでドラッグの移動量が正しく解釈されず画面が
                // 吹っ飛んで見える不具合があった。ドラッグ開始をズーム操作の
                // 打ち切りとして扱う（他の慣性系と同じ方針）。
                if (m_zoomEase) m_zoomEase->stop();
            }
            event->accept(); return;
        }

        m_activeMode="camera_pan"; m_dragStartPos=pos; m_startCameraPos=m_cameraPos;
        // 2026-09-20: ホイールクリック判定用（離したときに見る）。
        m_canvasMiddlePressPos = pos;
        m_canvasMiddlePressClock.start();
        // 2026-07-12: 新しいドラッグを始めたら、前回のフリックで走っていた
        // かもしれない慣性は打ち切り、速度サンプル計測も0から始め直す。
        m_canvasPanInertia->beginDrag();
        // 2026-07-17: 上のfocus_image_panと同じ理由。ホイールズームの
        // イージングが動いたままドラッグを始めると、m_cameraZoomが毎フレーム
        // 変わり続けてドラッグの移動量計算が破綻し、画面が吹っ飛んで見える。
        if (m_zoomEase) m_zoomEase->stop();
        qDebug("[Focus][camera][PAN-TRACE] camera_pan start camPos=(%.1f,%.1f)",
               m_cameraPos.x(), m_cameraPos.y());
        event->accept(); return;
    }

    // 右クリック：バブルをアクティブにする専用ボタン。
    // 左クリックと違い動画/音声バブルの再生/一時停止トグルは一切起こさない
    // （シングルクリックの瞬間に即アクティブ化するだけで、タイマーの
    // タイムアウトを待つ必要がない）。ダブル右クリックは左と同様、
    // 全画面切替/解除を行う。ドラッグ・リサイズ・範囲選択・フレーム化等の
    // 副作用は起こさない（アクティブ化のみに専念するボタンのため）。
    if (event->button() == Qt::RightButton) {
        // 2026-09-17: 右ドラッグでウィンドウ移動。押した時点ではまだ「候補」として
        // 位置だけ覚え、以降のアクティブ化などは従来どおり行う。しきい値以上
        // 動いたら handleMouseMove でウィンドウ移動に切り替える。
        m_rightWindowDragPending = true;
        m_rightWindowDragging = false;
        m_rightWindowDragStartGlobal = event->globalPosition().toPoint();
        if (QWidget *w = window()) {
            const QPoint local = w->mapFromGlobal(m_rightWindowDragStartGlobal);
            m_rightWindowDragAnchor = QPointF(
                w->width()  > 0 ? double(local.x()) / w->width()  : 0.5,
                w->height() > 0 ? double(local.y()) / w->height() : 0.5);
        }

        // 2026-09-17: アクティブ化・右ダブルクリック（全画面）は「離したとき」に
        // 判定する（handleMouseRelease → performRightClick）。押している間に
        // しきい値以上動いたらクリックとしては無効にし、ウィンドウ移動と
        // 重ならないようにする。
        m_rightPressScreenPos = pos;
        m_rightClickCancelledByDrag = false;
        event->accept(); return;
    }

    if (event->button()!=Qt::LeftButton) return;

    QPointF cPos = screenToCanvas(pos);

    // 全画面中：ダブルクリックによるフォーカス解除のみ許可。それ以外の操作（キャンバス移動・バブル移動・リサイズ）は全て禁止する
    bool isFocused = (m_focusedItem != nullptr || m_pendingFocusItem != nullptr);
    if (isFocused) {
        // ダブルクリック判定のみ実行し、ヒットしたアイテムが現在のフォーカスなら解除
        bool isDouble = m_singleClickTimer->isActive() && m_pendingClickItem!=nullptr &&
            [&](){
                for (auto it=m_items.rbegin();it!=m_items.rend();++it)
                    if ((*it)->boardRect().contains(cPos)||(*it)->handleRect().contains(cPos))
                        return (*it==m_pendingClickItem);
                return false;
            }();
        if (isDouble) {
            // 2026-09-20: ダブルクリックは「2回目を離したとき」に確定する
            // （handleMouseRelease の focus_double_press 参照）。
            m_singleClickTimer->stop(); m_pendingClickItem=nullptr;
            m_activeMode = QStringLiteral("focus_double_press");
            event->accept(); return;
        }
        // シングルクリック登録のみ（次のダブルクリック判定のため）、何も動かさない。
        // ヒットしたのが動画/音声バブルで、かつ既に読み込み済み・再生可能な
        // 状態（Playing/Paused/Buffering/Ready）なら、そのまま長押し判定も
        // 開始する（一定時間押しっぱなしなら2倍速。離せば通常のシングル
        // クリック判定＝再生/一時停止トグルに合流する）。
        // Idle（未読み込み）/ Loading / Ended 等の状態では長押し判定を
        // 開始しない。これらの状態で誤って長押しと判定してしまうと、
        // シングルクリックで期待している「読み込み開始」「リプレイ」操作が
        // キャンセルされてしまい、少し長めに押しただけで再生/リプレイが
        // 一切反応しなくなる不具合になるため。
        for (auto it=m_items.rbegin();it!=m_items.rend();++it) {
            if ((*it)->boardRect().contains(cPos)||(*it)->handleRect().contains(cPos)) {
                m_singleClickTimer->stop();
                m_pendingClickItem=*it; m_pendingClickPos=pos;
                m_lastClickItemForDouble=*it; m_singleClickTimer->start();
                if (auto *vid = canvasItemAsVideo(*it)) {
                    const BubbleState st = vid->bubbleState();
                    const bool speedable = st == BubbleState::Playing
                                        || st == BubbleState::Paused
                                        || st == BubbleState::Buffering
                                        || st == BubbleState::Ready;
                    if (speedable) {
                        m_longPressVideo = vid;
                        m_longPressTimer->start();
                    }
                }
                break;
            }
        }
        // 2026-07-12: 全画面中のパンは左ドラッグではなく中ボタン（ホイール
        // 押し込み）ドラッグに変更した（下のMiddleButton分岐参照）。理由:
        // 動画では左クリックに「長押しで2倍速」機能が既にあり、左ドラッグで
        // パンさせようとすると同じ左ボタンの押しっぱなし操作と衝突する。
        // 中ボタンなら通常時のcamera_panと同じ操作感で衝突しない。
        event->accept(); return;
    }

    bool isDouble = m_singleClickTimer->isActive() && m_pendingClickItem!=nullptr &&
        [&](){
            for (auto it=m_items.rbegin();it!=m_items.rend();++it)
                if ((*it)->boardRect().contains(cPos)||(*it)->handleRect().contains(cPos))
                    return (*it==m_pendingClickItem);
            return false;
        }();

    if (isDouble) {
        // 2026-09-20: ダブルクリックは押した瞬間ではなく「2回目を離したとき」に
        // 確定する。2回目を押したまま動かすと逆輸入（PB外への書き出し）ドラッグ
        // になる（handleMouseMove / handleMouseRelease の double_press 参照）。
        m_singleClickTimer->stop(); m_pendingClickItem=nullptr;
        CanvasItem *target = m_lastClickItemForDouble;
        m_multiDragStartPositions.clear();
        if (target && m_items.contains(target)) {
            if (m_activeItem != target) {
                if (m_activeItem) { m_liftAnim->stop(); m_activeItem->setLiftScale(1.0); }
                m_activeItem = target;
            }
            m_dragStartPos = cPos;
            m_startItemPos = target->pos();
        }
        m_activeMode = QStringLiteral("double_press");
        m_doublePressClock.start();
        event->accept(); return;
    }

    CanvasItem *hitItem=nullptr;
    // 2026-09-22: 融合の吸い込み中のバブルは掴めない（もうすぐ消える）。
    CanvasItem *fusionAbsorbing =
        m_fusion.absorbing() ? m_fusion.pairDragged() : nullptr;
    m_fusionRawValid = false;
    if (!onUI) {
        for (auto it=m_items.rbegin();it!=m_items.rend();++it) {
            if (*it == fusionAbsorbing)
                continue;
            if ((*it)->handleRect().contains(cPos)) {
                hitItem=*it; m_activeMode="item_resizing";
                m_dragStartPos=cPos; m_startItemScale=hitItem->scale();
                m_resizeBaseW=hitItem->itemRect().width()/hitItem->scale(); break;
            }
            if ((*it)->boardRect().contains(cPos)) {
                hitItem=*it; m_activeMode="panning";
                m_dragStartPos=cPos; m_startItemPos=hitItem->pos();
                QRectF ir=hitItem->itemRect();
                double rx=(cPos.x()-ir.left())/ir.width(), ry=(cPos.y()-ir.top())/ir.height();
                auto q=[](double v){ return v<0.33?0.0:v<0.66?0.5:1.0; };
                hitItem->setOriginPoint({q(rx)*ir.width()/hitItem->scale(), q(ry)*ir.height()/hitItem->scale()});
                break;
            }
        }
    }

    if (hitItem) {
        // 掴んだバブルが押し出しアニメーション中だった場合は中断し、
        // ドラッグ操作を優先する（アニメーションと競合させない）。
        cancelOverlapAnim(hitItem);

        m_multiDragStartPositions.clear();
        if (m_activeMode=="panning" && m_selectedItems.size()>1 && m_selectedItems.contains(hitItem)) {
            m_multiDragStartPositions = CanvasSelectionHelper::itemPositions(m_selectedItems);
            // 複数選択中のバブルの1つを掴んだ → 選択中のバブル全員をまとめてドラッグする。
            for (CanvasItem *sel : m_selectedItems) {
                cancelOverlapAnim(sel);
            }
        } else if (!m_selectedItems.isEmpty()) {
            // 選択グループとは無関係な単体操作 → 既存の複数選択は解除する。
            clearSelection();
        }

        if (m_activeItem!=hitItem) {
            if (m_activeItem) { m_liftAnim->stop(); m_activeItem->setLiftScale(1.0); }
            m_activeItem=hitItem;
        }
        m_items.removeOne(hitItem); m_items.append(hitItem);
        m_singleClickTimer->stop();
        m_pendingClickItem=hitItem; m_pendingClickPos=pos;
        m_lastClickItemForDouble=hitItem; m_singleClickTimer->start();
        if (!m_focusedItem) startLiftUp();
    } else {
        m_singleClickTimer->stop(); m_pendingClickItem=nullptr;
        if (m_focusedItem) unfocusItem();
        const bool preserveActiveForPlaybackBar =
            m_playbarVisible && pos.y() >= height() - 56 - 20 && canvasItemAsVideo(m_activeItem);
        if (!preserveActiveForPlaybackBar) {
            if (m_activeItem) startLiftDown();
            m_activeItem=nullptr;
        }
        m_activeMode.clear(); m_lastClickItemForDouble=nullptr;

        if (!onUI) {
            // 空きキャンバス上でのクリック/ドラッグ：範囲選択を開始する。
            // Shiftを押していなければ、まず既存の選択をクリアする
            // （単純クリックでの選択解除もこれでカバーされる）。
            if (!(event->modifiers() & Qt::ShiftModifier))
                clearSelection();
            m_rectSelectStartCanvas   = cPos;
            m_rectSelectCurrentCanvas = QRectF(cPos, QSizeF(0.0, 0.0));
            m_activeMode = QStringLiteral("rect_select");
        }
    }

    update(); event->accept();
}

// 2026-09-20: 整列の名前順で使う「バブルの名前」。
// ローカルファイル由来はファイル名（取り込み時に title へ入る）、プラグイン
// （動画サイト等）は解決済みのタイトル、ランチャーは表示名。画像を直接貼った・
// ドロップしたもの（元のURLが無く内容ハッシュで保存したもの）は名前無し。
static QString arrangeNameFor(CanvasItem *item)
{
    if (auto *launcher = dynamic_cast<CanvasLauncher*>(item))
        return CanvasLauncher::displayNameForPath(launcher->path());
    const MediaSource *src = nullptr;
    if (auto *vid = canvasItemAsVideo(item))
        src = &vid->mediaSource();
    else if (auto *img = dynamic_cast<CanvasImage*>(item))
        src = &img->mediaSource();
    if (!src)
        return {};
    if (src->originalPathOrUrl.startsWith(QStringLiteral("pocketboard-capture:")))
        return {};
    if (!src->title.isEmpty())
        return src->title;
    return QFileInfo(src->originalPathOrUrl).fileName();
}

// 2026-09-20: 選択中のバブルを整列する（範囲選択→ホイールダブル／トリプルクリック）。
//   BySize … 高さの近いバブル同士が同じ行に並ぶよう大きさ順に並べ、
//             行の幅は「すき間が少なく、全体が画面の縦横比に近い」ものを選ぶ。
//   ByName … 名前の昇順（数字は数として比較: 2 < 10）。名前の無いものは最後。
// 起点は選択バブルの外接矩形の左上なので、選択した場所から大きく飛ばない。
// 並べた結果が他のバブルに重なったら、いつもどおり他のバブル側を押し出す。
// 描画は確定時の1回だけ（アニメーションしない）。
bool CanvasWidget::arrangeSelectedBubbles(ArrangeMode mode)
{
    QSet<CanvasItem*> targets;
    QRectF bounds;
    for (CanvasItem *item : std::as_const(m_selectedItems)) {
        if (!item || !m_items.contains(item))
            continue;
        targets.insert(item);
        bounds = bounds.isNull() ? item->boardRect() : bounds.united(item->boardRect());
    }
    if (targets.size() < 2 || bounds.isEmpty())
        return false;

    QList<CanvasItem*> ordered;
    if (mode == ArrangeMode::ByName) {
        ordered = CanvasSelectionHelper::readingOrder(targets);
        QHash<CanvasItem*, QString> names;
        for (CanvasItem *item : std::as_const(ordered))
            names.insert(item, arrangeNameFor(item));
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::stable_sort(ordered.begin(), ordered.end(), [&](CanvasItem *a, CanvasItem *b) {
            const QString &na = names[a];
            const QString &nb = names[b];
            if (na.isEmpty() != nb.isEmpty())
                return nb.isEmpty();          // 名前のあるものが先
            if (na.isEmpty())
                return false;                 // どちらも無し → 見た目の順のまま
            return collator.compare(na, nb) < 0;
        });
    } else {
        ordered = CanvasSelectionHelper::sizeOrder(targets);
    }

    // バブル同士の間隔（キャンバス座標）。0だとベゼル同士がくっつく。
    constexpr double kArrangeSpacing = 8.0;
    const double targetAspect = height() > 0 ? double(width()) / double(height()) : 16.0 / 9.0;
    const QHash<CanvasItem*, QPointF> arranged =
        CanvasSelectionHelper::packedPositions(ordered, bounds.topLeft(),
                                               kArrangeSpacing, targetAspect);
    if (arranged.isEmpty())
        return false;

    UndoAutoStep undoStep(this);
    for (CanvasItem *item : targets)
        cancelOverlapAnim(item);
    for (auto it = arranged.constBegin(); it != arranged.constEnd(); ++it) {
        if (it.key())
            it.key()->setPos(it.value());
    }
    startOverlapPushAnimation(targets);
    update();
    qDebug("[Arrange] arranged %d bubbles (%s)", int(arranged.size()),
           mode == ArrangeMode::ByName ? "name" : "size");
    return true;
}

// 2026-09-20: 左ダブルクリックの確定処理（2回目を動かさずに離したとき）。
void CanvasWidget::performLeftDoubleClick(CanvasItem *target)
{
    if (!target || !m_items.contains(target))
        return;
    if (auto *launcher = dynamic_cast<CanvasLauncher*>(target)) {
        launcher->launch();
    } else if (m_focusedItem == target) {
        unfocusItem();
    } else {
        focusItem(target);
    }
}

// 2026-09-20: 逆輸入（PB外への書き出し）ドラッグを開始する。Shift+ドラッグと
// ダブルクリック2回目のドラッグの共通経路。
void CanvasWidget::startBubbleExportDrag(CanvasItem *dragItem)
{
    if (!dragItem)
        return;
    // 外部ドラッグはクリックではない。QDrag::exec()中もWindows側の
    // イベント処理でタイマーが進み得るため、開始前に必ず再生/停止の
    // シングルクリック判定を破棄する。
    m_singleClickTimer->stop();
    m_pendingClickItem = nullptr;
    cancelLongPress();
    m_liftAnim->stop();
    // 8pxの外部ドラッグ判定に達するまで通常のpanning経路で数pxだけ
    // 動いていることがあるため、PB上のバブルは押下開始位置へ戻してから受け渡す。
    // 2026-09-20: 範囲選択中の1つを掴んだ場合は、選択しているバブル全部を
    // まとめて書き出す（並び順は見た目の順＝上の行から左→右）。
    QList<CanvasItem*> exportItems;
    if (m_selectedItems.size() > 1 && m_selectedItems.contains(dragItem)) {
        QSet<CanvasItem*> selected;
        for (CanvasItem *item : std::as_const(m_selectedItems))
            if (item && m_items.contains(item)) selected.insert(item);
        exportItems = CanvasSelectionHelper::readingOrder(selected);
    }
    if (!m_multiDragStartPositions.isEmpty())
        CanvasSelectionHelper::applyPositionOffset(m_multiDragStartPositions, QPointF());
    else
        dragItem->setPos(m_startItemPos);
    if (exportItems.isEmpty())
        exportItems = { dragItem };
    m_activeMode = QStringLiteral("external_drag_prepare");
    updateExportHint();
    m_bubbleDragExporter->startExternalDrag(
        this, dragItem, exportItems, m_dragStartPos,
        [this, dragItem] {
            return m_activeMode == QStringLiteral("external_drag_prepare")
                && m_activeItem == dragItem;
        },
        [this, dragItem](Qt::DropAction) {
            const bool finishingThisGesture =
                m_activeMode.isEmpty()
                || m_activeMode == QStringLiteral("external_drag_prepare");
            if (finishingThisGesture && m_activeItem == dragItem)
                startLiftDown();
            if (m_activeMode == QStringLiteral("external_drag_prepare"))
                m_activeMode.clear();
            updateExportHint();
            update();
        });
}

void CanvasWidget::executeSingleClick(CanvasItem *item, const QPointF &)
{
    if (!item) return;
    auto *vid = canvasItemAsVideo(item);
    if (!vid) return;
    // Phase 1: mediaPlayer() 直接呼び出しを廃止し BC 経由に変更
    vid->togglePlayPause();
    update();
}

void CanvasWidget::cancelLongPress()
{
    m_longPressTimer->stop();
    if (m_longPressActive && m_longPressVideo)
        m_longPressVideo->setPlaybackSpeed(1.0);
    setLongPressActive(false);
    m_longPressVideo = nullptr;
}

void CanvasWidget::setLongPressActive(bool active)
{
    if (m_longPressActive == active)
        return;
    m_longPressActive = active;
    emit speedHoldChanged(active);
}

// ----------------------------------------------------------------
//  バブル（CanvasItem）同士の重なり解消（押し出し量の計算のみ）
//
//  fixedItems（ドラッグ/リサイズ/グループ移動を終えたバブル群）は一切
//  動かさず、それ以外のバブルだけが重なりを解消するのに必要な移動量
//  （オフセット）を計算して返す。実際の setPos() はここでは行わない
//  （アニメーションで少しずつ適用するため）。
//
//  fixedItems同士（複数選択して一緒に動かした場合など）は元々重なって
//  いなければ剛体移動なので相対位置は保たれる。そのためfixedItems同士の
//  ペアは常にスキップしてよい。
//
//  boardRect()（PADDING込みの矩形）同士のAABB分離を、重なりが解消される
//  まで数回反復する。無限に広がるキャンバス上での操作なので、押し出し先が
//  無くなって解決不能になることはない。
// ----------------------------------------------------------------
QHash<CanvasItem*, QPointF> CanvasWidget::computeOverlapPushOffsets(const QSet<CanvasItem*> &fixedItems) const
{
    QHash<CanvasItem*, QPointF> offsets;
    if (m_items.size() < 2) return offsets;

    // 2026-09-20: 押し出しの「順位」。fixedItems が0、そこから押された
    // バブルが1、その1に押されたバブルが2…と、押し出しが外へ広がる向きを
    // 覚えておく。順位の違うペアは「順位の大きい（外側の）方だけ」を動かし、
    // 同じ順位（どちらもまだ押されていない等）のときだけ従来どおり半分ずつ
    // 動かす。
    // 以前は固定以外のペアを常に半分ずつ動かしていたため、極端に横長の
    // バブルのように上下にたくさん隣がいるものは「落としたバブルに押し出される
    // → 隣のバブル達に半分ずつ押し戻される」を繰り返して収束せず、重なった
    // まま残っていた。
    constexpr int kUnranked = std::numeric_limits<int>::max();
    QHash<CanvasItem*, int> rank;

    QHash<CanvasItem*, QRectF> baseRect;
    for (CanvasItem *item : m_items) {
        baseRect[item] = item->boardRect();
        offsets[item]  = QPointF(0.0, 0.0);
        rank[item]     = fixedItems.contains(item) ? 0 : kUnranked;
    }

    // バブル数が多い密集配置でも収束しきるよう十分な反復回数を確保する
    // （1回のドロップ/リサイズ確定時にしか呼ばれないため、O(n^2)を
    // 増やしてもコストは無視できる）。
    static constexpr int    kIterations = 64;
    static constexpr double kSeparationEpsilon = 0.5;

    for (int iter = 0; iter < kIterations; ++iter) {
        bool anyOverlap = false;

        for (int i = 0; i < m_items.size(); ++i) {
            for (int j = i + 1; j < m_items.size(); ++j) {
                CanvasItem *a = m_items[i];
                CanvasItem *b = m_items[j];

                const bool aFixed = fixedItems.contains(a);
                const bool bFixed = fixedItems.contains(b);
                if (aFixed && bFixed) continue;

                const QRectF ra = baseRect[a].translated(offsets[a]);
                const QRectF rb = baseRect[b].translated(offsets[b]);
                const QRectF ov = ra.intersected(rb);
                if (ov.isEmpty() || ov.width() <= 0.0 || ov.height() <= 0.0) continue;

                anyOverlap = true;

                const double overlapX = ov.width();
                const double overlapY = ov.height();
                const double dx = rb.center().x() - ra.center().x();
                const double dy = rb.center().y() - ra.center().y();

                // 重なりの小さい軸方向に押し出す（最小移動量で分離する）
                QPointF delta;
                if (overlapX < overlapY) {
                    const double dir = (dx >= 0.0) ? 1.0 : -1.0;
                    delta = QPointF(dir * (overlapX + kSeparationEpsilon), 0.0);
                } else {
                    const double dir = (dy >= 0.0) ? 1.0 : -1.0;
                    delta = QPointF(0.0, dir * (overlapY + kSeparationEpsilon));
                }

                const int rankA = rank.value(a, kUnranked);
                const int rankB = rank.value(b, kUnranked);
                if (rankA < rankB) {
                    offsets[b] += delta;
                    rank[b] = rankA + 1;
                } else if (rankB < rankA) {
                    offsets[a] -= delta;
                    rank[a] = rankB + 1;
                } else {
                    offsets[a] -= delta * 0.5;
                    offsets[b] += delta * 0.5;
                }
            }
        }

        if (!anyOverlap) break;
    }

    return offsets;
}

QHash<CanvasItem*, QPointF> CanvasWidget::computeOverlapPushOffsets(CanvasItem *fixedItem) const
{
    return computeOverlapPushOffsets(QSet<CanvasItem*>{fixedItem});
}

// ----------------------------------------------------------------
//  上で計算したオフセット分だけ、対象バブルを「もにゅっ」としたイージング
//  （OutBack: 少しオーバーシュートしてから収まる）でアニメーションさせる。
//  fixedItems 自身は動かさない。
// ----------------------------------------------------------------
void CanvasWidget::startOverlapPushAnimation(const QSet<CanvasItem*> &fixedItems)
{
    QHash<CanvasItem*, QPointF> offsets = computeOverlapPushOffsets(fixedItems);
    for (CanvasItem *item : fixedItems) offsets.remove(item);
    animateItemOffsets(offsets);
}

void CanvasWidget::startOverlapPushAnimation(CanvasItem *fixedItem)
{
    startOverlapPushAnimation(QSet<CanvasItem*>{fixedItem});
}

// 計算済みのオフセット（キー＝アイテム）ぶんを「もにゅっ」としたイージング
// （OutBack）で実際にアニメーションさせる共通処理。バブル同士の重なり解消
// （startOverlapPushAnimation）と、フレーム由来の押し出し（フレーム同士/
// フレームと非メンバーバブルの重なり解消）の両方から使う。
void CanvasWidget::animateItemOffsets(const QHash<CanvasItem*, QPointF> &offsets)
{
    for (auto it = offsets.constBegin(); it != offsets.constEnd(); ++it) {
        CanvasItem   *item   = it.key();
        const QPointF offset = it.value();
        if (qAbs(offset.x()) < 0.01 && qAbs(offset.y()) < 0.01) continue;

        const QPointF startPos  = item->pos();
        const QPointF targetPos = startPos + offset;
        const QRectF startRect = item->boardRect();
        const QRectF targetRect = startRect.translated(offset);

        cancelOverlapAnim(item);

        auto *anim = new QVariantAnimation(this);
        anim->setStartValue(startPos);
        anim->setEndValue(targetPos);
        anim->setDuration(420);
        anim->setEasingCurve(QEasingCurve::OutBack);
        connect(anim, &QVariantAnimation::valueChanged, this, [this, item](const QVariant &v) {
            item->setPos(v.toPointF());
            update();
        });
        connect(anim, &QVariantAnimation::finished, this, [this, item]() {
            m_overlapAnims.remove(item);
        });
        connect(anim, &QVariantAnimation::finished, anim, &QObject::deleteLater);

        m_overlapAnims.insert(item, anim);
        anim->start();
    }
}

void CanvasWidget::cancelOverlapAnim(CanvasItem *item)
{
    auto it = m_overlapAnims.find(item);
    if (it == m_overlapAnims.end()) return;
    QVariantAnimation *anim = it.value();
    m_overlapAnims.erase(it);
    anim->stop();
    anim->deleteLater();
}

void CanvasWidget::cancelAllOverlapAnims()
{
    for (auto it = m_overlapAnims.constBegin(); it != m_overlapAnims.constEnd(); ++it) {
        it.value()->stop();
        it.value()->deleteLater();
    }
    m_overlapAnims.clear();
}

// ================================================================
//  範囲選択（複数バブル選択）
// ================================================================

// canvasRect（正規化済みでなくてよい）に少しでも触れている（boardRectが交差する）アイテムを返す。
QList<CanvasItem*> CanvasWidget::itemsInCanvasRect(const QRectF &canvasRect) const
{
    return CanvasSelectionHelper::itemsInCanvasRect(m_items, canvasRect);
}

void CanvasWidget::clearSelection()
{
    if (m_selectedItems.isEmpty()) return;
    m_selectedItems.clear();
    update();
}

// アイテム破棄の共通処理（Phase 10 Step 4）。旧・単体削除（handleMouseRelease内の
// trash分岐）と旧・deleteSelectedItems()の共通部分を1つにまとめたもの。
// 実行順序: tooltip → frame所属 → focus/click系参照 → anim系 → multiDrag →
// flow/pause系 → m_itemsから除去 → （動画/音声なら）releaseGlResources() → delete。
// 呼び出し前提:
//   - GLコンテキストが既にcurrentであること（makeCurrent()は呼び出し元の責務）。
//   - 呼び出し後、OleInitialize診断/refreshDropTargets()が必要なら呼び出し元で行うこと
//     （複数件削除時に1回にまとめる必要があるため、この関数には含めない）。
void CanvasWidget::destroyItem(CanvasItem *item)
{
    if (!item) return;

    // 2026-09-17: 元に戻すで作り直せるよう、中身を保存してから履歴の番号を外す。
    forgetUndoItem(item);

    if (m_launcherTooltipItem == item)
        clearLauncherTooltip();
    if (m_focusedItem == item || m_pendingFocusItem == item) {
        // 2026-07-09 バグ修正: フォーカス確定待ち（resizeAnim完了待ち）の
        // バブルを削除した場合、focusItem()/notifyWindowMaximizedChanged()で
        // 繋いだラムダが残っていると、後で発火して破棄済みポインタを
        // m_focusedItemへ書き戻す（D&D🚫固定＋ダングリングポインタ）。
        // ラムダ側のガードに加えて、ここでも必ず接続を切っておく。
        disconnect(m_resizeAnimFinishedConn);
        // 2026-07-15: 世代も進めておく。disconnect()は「今繋がっている接続」
        // にしか効かないため、万一破棄されたitemのアドレスが後で別の
        // CanvasItemに再利用された場合でも、世代比較で確実に無効化される
        // ようにする（CanvasWidget.h の m_focusGeneration 参照）。
        ++m_focusGeneration;
    }
    if (m_focusedItem == item)      m_focusedItem = nullptr;
    if (m_pendingFocusItem == item) m_pendingFocusItem = nullptr;
    if (m_pendingClickItem == item) { m_singleClickTimer->stop(); m_pendingClickItem = nullptr; }
    if (m_lastClickItemForDouble == item) m_lastClickItemForDouble = nullptr;
    if (m_activeItem == item) {
        m_activeItem = nullptr;
        m_glassBubbleActive = false;
    }
    if (m_glassRefractingItem == item) {
        m_glassRefractingItem = nullptr;
        m_glassRefractionFade = 0.0f;
    }
    cancelOverlapAnim(item);
    cancelFadeInAnim(item);
    // 2026-09-22: バブル融合の参照からも外す
    m_fusion.forget(item);
    m_fusionLiftItems.remove(item);
    m_multiDragStartPositions.remove(item);
    // 2026-09-17: 選択に残ったまま破棄されるとダングリングになるので外しておく
    // （Undo適用・ショートカット削除など、選択と無関係な経路からも呼ばれるため）。
    m_selectedItems.remove(item);

    if (auto *vid = canvasItemAsVideo(item)) {
        forgetFocusPausedVideo(vid);
        // 削除前に再生を完全停止する。ロード中 (Loading) でも bcPause() は
        // BC 軽量処理のみで安全。GLリソース解放(releaseGlResources)より
        // 必ず先に呼ぶこと。mpvがまだファイルオープン中（loadfileの非同期
        // 処理中）にFBO等のGPUリソースを先に破棄すると、mpv側の描画処理と
        // 競合してクラッシュする可能性があるため。
        vid->bcPause();
    }

    // 2026-07-12: Web画像バブル削除時に対応するキャッシュファイルを
    // 即座に削除する処理を、ここに一時的に置いていたが削除した。
    // 理由: キャッシュパスはsourceUrl（またはstoreCapturedImage()由来の
    // 内容ハッシュ合成識別子）から決定的に導出されるだけなので、同じ
    // 画像を複数バブルへドロップした場合や、別レイヤーに同じ画像が
    // 存在する場合、片方のバブルを消すともう片方が参照しているキャッシュ
    // ファイルまで一緒に消えてしまっていた（別レイヤー側は現在
    // m_itemsに乗っていないため、ここではそもそも存在を確認しようが
    // ない）。特にDiscordの期限切れURLやTwitter等の合成識別子（＝再取得
    // 手段が無い）ケースだと、キャッシュを失うと復元不能＝プレースホルダー
    // 化してしまうため実害がある。
    // 対応: バブル削除時の即時削除はやめ、起動時のガベージコレクション
    // （MainWindow::MainWindow()内、WebImageCache::sweepOrphans()。
    // 全レイヤーのsession_N.jsonを横断してどこからも参照されなくなった
    // ファイルだけを消す）に一本化する。即時性は失う（削除したバブルの
    // キャッシュは次回起動まで残る）が、他バブル/他レイヤーからの参照を
    // 誤って壊すことがなくなる。
    //
    // 2026-07-12 追記: 「次回起動まで消えない」のがもやっとする場合に
    // 備えて、元の即時削除コードをコメントアウトで残しておく。戻す場合は
    // 上記の「他バブル/他レイヤーが同じキャッシュを参照している可能性」を
    // 踏まえた上で有効化すること（単純に戻すと共有バブル削除時の破損が
    // 再発する）。
    // if (auto *img = dynamic_cast<CanvasImage*>(item)) {
    //     if (img->mediaKind() == MediaKind::WebImage) {
    //         const QString cachedPath = WebImageCache::cachedFilePathForUrl(img->sourceUrl());
    //         WebImageCache::removeCachedFile(cachedPath);
    //     }
    // }

    m_items.removeOne(item);
    m_bubbleLayers.forget(item);
    m_glassHighlight.forget(item); // ガラスハイライトのバネ状態も破棄（放置してもリークはしないが掃除しておく）
    // 重要: releaseGlResources()呼び出し後でも、~CanvasVideo()は無条件で
    // m_mpvPlayer を delete している（CanvasVideo.cpp参照）。~MpvPlayer() は
    // mpv_render_context_free() を呼ぶが、これは有効なOpenGLコンテキストが
    // currentであることを要求する。呼び出し元がmakeCurrent()済みであることを
    // 前提に、ここで releaseGlResources() → delete の順に必ず行う
    // （doneCurrent()より前。動画/音声バブルを2回ゴミ箱に入れて削除すると
    // Windowsのドラッグ&ドロップが🚫に固定される不具合の調査より）。
    if (auto *vid = canvasItemAsVideo(item)) {
        vid->releaseGlResources(m_renderer);
    } else if (auto *image = dynamic_cast<CanvasImage*>(item)) {
        image->releaseGlResources(m_renderer);
    }
    delete item;
}

// 選択中バブルをまとめてゴミ箱へ（trashZoneにドロップされた時に呼ぶ）。
// 単体削除（handleMouseRelease内のtrash分岐）と同じ後始末をアイテムの数だけ繰り返す。
void CanvasWidget::deleteSelectedItems()
{
    if (m_selectedItems.isEmpty()) return;

    const QList<CanvasItem*> targets = CanvasSelectionHelper::takeItems(&m_selectedItems);

    // 2026-09-17: 以前は動画/音声が含まれるときだけ makeCurrent() していたが、
    // 画像バブルも destroyItem() 内で releaseGlResources()（テクスチャ削除）を
    // 呼ぶため、GLコンテキストが current でないと削除が効かずテクスチャが残る。
    // ショートカット削除で画像1枚だけを消す経路が増えたので、GLが使える間は
    // 常に current にする（単体のゴミ箱削除経路と同じ扱い）。
    const bool needMakeCurrent = (context() != nullptr);
    if (needMakeCurrent) makeCurrent();

    // Phase 10 Step 4: 後始末一式は destroyItem() に集約。GLコンテキストの
    // 切り替えはここでループ全体を1回だけ囲む（needMakeCurrent）ため、
    // destroyItem() 側では makeCurrent()/doneCurrent() を呼ばない。
    for (CanvasItem *item : targets)
        destroyItem(item);

    if (needMakeCurrent) doneCurrent();

#ifdef Q_OS_WIN
    {
        HRESULT oleHr = OleInitialize(nullptr);
        qDebug() << "[Trash][CanvasWidget] multi-select items deleted. OleInitialize hr=0x"
                 << Qt::hex << static_cast<unsigned long>(oleHr) << Qt::dec;
        if (auto *mw = qobject_cast<MainWindow*>(window()))
            mw->refreshDropTargets();
    }
#endif

    update();
}

void CanvasWidget::startFadeInAnim(CanvasItem *item)
{
    if (!item) return;
    cancelFadeInAnim(item);
    m_fadeInAlpha[item] = 0.0;

    auto *anim = new QVariantAnimation(this);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setDuration(500);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim, &QVariantAnimation::valueChanged, this, [this, item](const QVariant &v) {
        m_fadeInAlpha[item] = v.toDouble();
        update();
    });
    connect(anim, &QVariantAnimation::finished, this, [this, item]() {
        m_fadeInAlpha.remove(item);
        m_fadeInAnims.remove(item);
    });
    connect(anim, &QVariantAnimation::finished, anim, &QObject::deleteLater);

    m_fadeInAnims.insert(item, anim);
    anim->start();
}

void CanvasWidget::cancelFadeInAnim(CanvasItem *item)
{
    auto it = m_fadeInAnims.find(item);
    if (it != m_fadeInAnims.end()) {
        QVariantAnimation *anim = it.value();
        m_fadeInAnims.erase(it);
        anim->stop();
        anim->deleteLater();
    }
    m_fadeInAlpha.remove(item);
}

void CanvasWidget::cancelAllFadeInAnims()
{
    for (auto it = m_fadeInAnims.constBegin(); it != m_fadeInAnims.constEnd(); ++it) {
        it.value()->stop();
        it.value()->deleteLater();
    }
    m_fadeInAnims.clear();
    m_fadeInAlpha.clear();
}

void CanvasWidget::fadeInAllItems()
{
    for (CanvasItem *item : m_items)
        startFadeInAnim(item);
    update();
}

// 2026-09-17: 右クリック（アクティブ化専用ボタン）の本体。以前は押した瞬間に
// handleMousePress 内で行っていたが、右ドラッグでのウィンドウ移動と重ならない
// よう、動かさずに離したときだけ呼ぶ（handleMouseRelease）。中身は従来のまま。
void CanvasWidget::performRightClick(const QPointF &pos)
{
    QPointF cPosR = screenToCanvas(pos);
    const bool isFocused = (m_focusedItem != nullptr || m_pendingFocusItem != nullptr);

    auto hitTest = [&](CanvasItem *target){
        for (auto it=m_items.rbegin(); it!=m_items.rend(); ++it)
            if ((*it)->boardRect().contains(cPosR) || (*it)->handleRect().contains(cPosR))
                return (*it==target);
        return false;
    };

    if (isFocused) {
        // 全画面中：左クリック同様、ダブルクリックによる解除のみ許可。
        const bool isDoubleR = m_rightClickTimer->isActive()
            && m_rightPendingClickItem != nullptr
            && hitTest(m_rightPendingClickItem);
        if (isDoubleR) {
            m_rightClickTimer->stop(); m_rightPendingClickItem=nullptr;
            CanvasItem *focusTarget = m_focusedItem ? m_focusedItem : m_pendingFocusItem;
            if (m_rightLastClickItemForDouble == focusTarget)
                unfocusItem();
            return;
        }
        for (auto it=m_items.rbegin(); it!=m_items.rend(); ++it) {
            if ((*it)->boardRect().contains(cPosR) || (*it)->handleRect().contains(cPosR)) {
                m_rightClickTimer->stop();
                m_rightPendingClickItem = *it;
                m_rightLastClickItemForDouble = *it;
                m_rightClickTimer->start();
                break;
            }
        }
        return;
    }

    const bool isDoubleR = m_rightClickTimer->isActive()
        && m_rightPendingClickItem != nullptr
        && hitTest(m_rightPendingClickItem);

    if (isDoubleR) {
        m_rightClickTimer->stop(); m_rightPendingClickItem=nullptr;
        if (auto *launcher = dynamic_cast<CanvasLauncher*>(m_rightLastClickItemForDouble)) {
            launcher->launch();
        } else if (m_focusedItem == m_rightLastClickItemForDouble) {
            unfocusItem();
        } else if (m_rightLastClickItemForDouble) {
            focusItem(m_rightLastClickItemForDouble);
        }
        return;
    }

    // シングル右クリック：ヒットしたバブルを即アクティブ化するのみ。
    CanvasItem *hitItem=nullptr;
    for (auto it=m_items.rbegin(); it!=m_items.rend(); ++it) {
        if ((*it)->boardRect().contains(cPosR) || (*it)->handleRect().contains(cPosR)) { hitItem=*it; break; }
    }
    if (hitItem) {
        if (!m_selectedItems.isEmpty()) clearSelection();
        if (m_activeItem != hitItem) {
            if (m_activeItem) { m_liftAnim->stop(); m_activeItem->setLiftScale(1.0); }
            m_activeItem = hitItem;
        }
        m_items.removeOne(hitItem); m_items.append(hitItem);
        // 2026-07-13 バグ修正: 左クリック（panning開始）と同様に、今回の
        // クリック位置からリフトの拡大原点（originPoint）を設定する。
        // 従来はここで設定しておらず、前回の左クリック時の原点が残った
        // まま「ぽよん」が伸びる方向だけ引き継がれてしまっていた
        // （例: 前回左上を左クリック→今回右下を右クリックしても
        //   左上原点のまま右下方向へ伸びる）。量子化(3x3)も左と同一。
        {
            const QRectF ir = hitItem->itemRect();
            const double rx = (cPosR.x() - ir.left()) / ir.width();
            const double ry = (cPosR.y() - ir.top())  / ir.height();
            auto q = [](double v){ return v < 0.33 ? 0.0 : v < 0.66 ? 0.5 : 1.0; };
            hitItem->setOriginPoint({q(rx) * ir.width()  / hitItem->scale(),
                                     q(ry) * ir.height() / hitItem->scale()});
        }
        if (!m_focusedItem) startLiftUp();
    }
    m_rightClickTimer->stop();
    m_rightPendingClickItem = hitItem;
    m_rightLastClickItemForDouble = hitItem;
    if (hitItem) m_rightClickTimer->start();

    update();
}

void CanvasWidget::handleMouseMove(QMouseEvent *event)
{
    QPointF pos = event->position();

    // 2026-09-17: 右ドラッグでウィンドウ移動（設定「右ドラッグでウィンドウを移動」）。
    if (m_rightWindowDragPending) {
        if (!(event->buttons() & Qt::RightButton)) {
            // 離したイベントを取りこぼした場合の保険。
            m_rightWindowDragPending = false;
            m_rightWindowDragging = false;
        } else {
            const QPoint globalPos = event->globalPosition().toPoint();
            constexpr int kRightDragThreshold = 6; // px（マンハッタン距離）
            const bool beyondThreshold =
                (globalPos - m_rightWindowDragStartGlobal).manhattanLength() >= kRightDragThreshold;
            // 設定のON/OFFに関係なく、動かしたら右クリック（アクティブ化）は無効。
            if (beyondThreshold)
                m_rightClickCancelledByDrag = true;
            if (!m_rightWindowDragging) {
                if (AppSettings::instance().rightDragWindowMoveEnabled()
                    && m_activeMode.isEmpty()   // 左ボタンのドラッグ等と重ねない
                    && beyondThreshold) {
                    m_rightWindowDragging = true;
                    // ドラッグになったのでダブル右クリック（全画面）の判定は捨てる。
                    m_rightClickTimer->stop();
                    m_rightPendingClickItem = nullptr;
                    m_rightLastClickItemForDouble = nullptr;
                    // 島ドラッグ（DynamicIsland::mouseMoveEvent）と同じく、
                    // 動かした時点でウィンドウ全画面は解除し、自動リサイズ
                    // アニメは手動移動と奪い合わないよう止める。
                    auto *mw = qobject_cast<MainWindow*>(window());
                    if (mw && mw->isWindowFullscreen()) {
                        mw->setWindowFullscreen(false);
                        if (mw->resizeAnim())
                            mw->resizeAnim()->stop();
                        m_rightWindowDragPendingRefit = true;
                    }
                }
            }
            if (m_rightWindowDragging) {
                // 掴んだ位置を「ウィンドウ内の割合」で保つ。全画面解除で
                // サイズが変わってもカーソルがウィンドウの外へ外れない。
                QWidget *w = window();
                const QPoint offset(qRound(m_rightWindowDragAnchor.x() * w->width()),
                                    qRound(m_rightWindowDragAnchor.y() * w->height()));
                // 2026-10-11: DPIの違うモニター間でも追従がずれないよう、
                // 物理ピクセルで移動する（MainWindow::dragWindowNative参照）。
                auto *dragMw = qobject_cast<MainWindow*>(w);
                if (!(dragMw && dragMw->dragWindowNative(m_rightWindowDragAnchor, true)))
                    w->move(globalPos - offset);
                event->accept();
                return;
            }
        }
    }

    // 2026-09-12: リストの行を掴んで並べ替え中。
    if (m_activeMode=="labelRowDrag") {
        if (m_labelRowDragVideo) {
            // 少し動かすまではクリック扱いのまま（指のブレで並べ替えが
            // 始まらないように）。
            if ((pos - m_dragStartScreenPos).manhattanLength() > 6)
                m_labelRowDragVideo->updateRowDrag(pos);
            // 2026-09-23: 右へ引っ張って板の外へ出た → バブルとして取り出す
            if (m_labelRowDragVideo->rowExtractReady(pos)) {
                extractListRowToBubble(m_labelRowDragVideo.data(), pos);
                emit playbackOverlayUpdateRequested();
                update(); event->accept(); return;
            }
        }
        emit playbackOverlayUpdateRequested();
        update(); event->accept(); return;
    }

    // 2026-09-12: ラベルの引っ張り（固定操作）。押した位置からの縦移動量を
    // そのまま渡す（ゴムの減衰はCanvasVideo側で行う）。
    if (m_activeMode=="labelPull") {
        if (m_labelPullVideo) {
            m_labelPullVideo->updateLabelPull(pos.y() - m_dragStartScreenPos.y());
            // 2026-09-23: 押したまま動かしたら長押しはやめる（引っ張り＝固定に譲る）
            const QPointF d = pos - m_dragStartScreenPos;
            m_labelPullVideo->updateLabelHoldPointer(std::hypot(d.x(), d.y()));
        }
        emit playbackOverlayUpdateRequested();
        update(); event->accept(); return;
    }

    if (m_activeMode=="seeking") {
        auto *av=canvasItemAsVideo(m_activeItem);
        if (av && av->seekBarVisible()) {
            const int bx=28,vbx=bx+36,sx=vbx+24,sw=width()-sx-100;
            qint64 dur=av->duration();
            // ドラッグ中は高速（キーフレーム基準）シークを使う。精密シーク
            // (bcSeek)を毎フレーム呼ぶと、対象フレームまでの逐次デコードが
            // 積み重なってかくつく。正確な位置への合わせ直しはドラッグ終了時
            // （handleMouseRelease）で1回だけ行う。
            if (dur>0) av->bcSeekFast(static_cast<qint64>(dur*qBound(0.0,(pos.x()-sx)/sw,1.0)));
        }
        update(); event->accept(); return;
    }
    // 2026-09-20: ダブルクリックの2回目を押したまま動かした → 逆輸入ドラッグ。
    if (m_activeMode=="double_press") {
        if (m_activeItem && m_activeItem == m_lastClickItemForDouble
            && m_items.contains(m_activeItem)
            && (pos-m_dragStartScreenPos).manhattanLength()>8) {
            startBubbleExportDrag(m_activeItem);
        } else {
            updateExportHint();
        }
        event->accept();
        return;
    }
    if (m_activeMode=="panning"&&m_activeItem
     &&(event->modifiers()&Qt::ShiftModifier)
     &&(pos-m_dragStartScreenPos).manhattanLength()>8) {
        startBubbleExportDrag(m_activeItem);
        event->accept();
        return;
    }
    if (m_activeMode=="panning"&&(pos-m_dragStartScreenPos).manhattanLength()>6)
        if (m_singleClickTimer->isActive()) { m_singleClickTimer->stop(); m_pendingClickItem=nullptr; }

    // リサイズ操作もドラッグと同様、クリック（再生/一時停止トグル）判定にしない。
    if (m_activeMode=="item_resizing"&&(pos-m_dragStartScreenPos).manhattanLength()>6)
        if (m_singleClickTimer->isActive()) { m_singleClickTimer->stop(); m_pendingClickItem=nullptr; }

    if (m_activeMode=="camera_pan") {
        const QPointF prevCameraPos = m_cameraPos;
        m_cameraPos=m_startCameraPos-(pos-m_dragStartPos)/m_cameraZoom;
        updateCanvasFromCamera();
        // 2026-09-14 Phase5: カメラが実際に動いたときだけ再描画する。
        // 手動ドラッグはイベント駆動で、掴んでいる間はマウスが1pxも動いて
        // いなくても mouseMoveEvent が届くことがある。そのたびに全画面パスを
        // 1枚焼いていた。見た目が変わらないフレームを飛ばすだけなので、
        // 追従の遅れにはならない（＝慣性と違って間引きではない）。
        if (m_cameraPos != prevCameraPos)
            update();
        // 2026-07-12: 通常キャンバスパンには壁クランプが無いため、実移動量は
        // 常に生のドラッグ量と一致する。それでもprevCameraPosとの差分を渡す
        // 形にしてあるのは、focus_image_pan側と計算経路を揃えておくため
        // （将来どちらかにクランプが増えても、この呼び出し側は変更不要）。
        m_canvasPanInertia->sampleDelta(m_cameraPos - prevCameraPos);
    } else if (m_activeMode=="focus_image_pan") {
        // 全画面中の中ボタンドラッグパン（元々は画像バブル限定・左ドラッグ
        // だったが、動画の「長押しで2倍速」〈左ボタン〉と衝突しないよう
        // 中ボタンへ変更し、動画等にも拡張した）。camera_panと同じ移動
        // 計算のあと、ビューポートがバブル外へはみ出さないようにクランプ
        // する。ドラッグとして成立した（6px以上動いた）時点で、左ボタン側の
        // クリック判定・長押しタイマーが（両ボタン同時押し等で）たまたま
        // 生きていた場合の保険としてキャンセルしておく。
        if ((pos-m_dragStartPos).manhattanLength()>6) {
            if (m_singleClickTimer->isActive()) { m_singleClickTimer->stop(); m_pendingClickItem=nullptr; }
            if (m_longPressTimer->isActive()) {
                m_longPressTimer->stop();
                m_longPressVideo = nullptr;
            }
        }
        CanvasItem *focusTarget = m_focusedItem ? m_focusedItem : m_pendingFocusItem;
        if (focusTarget) {
            const QPointF prevCameraPos = m_cameraPos;
            m_cameraPos = m_startCameraPos-(pos-m_dragStartPos)/m_cameraZoom;
            clampCameraToFocusedImage(focusTarget->itemRect());
            updateCanvasFromCamera();
            // 2026-09-14 Phase5: camera_panと同じく、カメラが実際に動いた
            // ときだけ再描画する。こちらはclampCameraToFocusedImage()が
            // バブルの端でカメラを固定するため、壁に当たっている間は
            // マウスを動かし続けても m_cameraPos が1mmも変わらない。
            // それでも従来は mouseMoveEvent のたびに全画面パスを1枚
            // 焼いていた。見た目が変わらないフレームを飛ばすだけなので
            // 追従の遅れにはならない。
            if (m_cameraPos != prevCameraPos)
                update();

            // 2026-07-12: 慣性用の速度サンプリング。以前はスクリーン座標の
            // 生の移動量から計算していたが、バブルの端まで来て
            // clampCameraToFocusedImage()に止められている間はマウス自体は
            // 動き続けられるため、「画面上は動いていないのに速度だけ残る」
            // というズレがあった。クランプ後の実際のm_cameraPos変化量
            // （prevCameraPosとの差）を渡すことで、壁に当たっている間は
            // 自然と速度がゼロへ近づくようにする（EMA平滑化・間隔異常時の
            // スキップ等はCameraPanInertia側で行う）。
            m_focusPanInertia->sampleDelta(m_cameraPos - prevCameraPos);
        }
    } else if (m_activeMode=="panning"&&m_activeItem) {
        // ドラッグ中は他のバブルを押し出さない（重なったまま追従させる）。
        // 重なり解消は handleMouseRelease() で「もにゅっ」とアニメーションする。
        const bool waitingForExternalDrag =
            (event->modifiers() & Qt::ShiftModifier);
        if (waitingForExternalDrag) {
            // Shiftドラッグは8pxを超えた時点で外部ドラッグへ移る。それまで
            // 通常panningを適用すると、PB上のバブルが数pxずれてしまう。
            // 2026-09-20: 複数選択中も書き出せるようにしたので、選択メンバー
            // 全員をその場に留める。
            if (!m_multiDragStartPositions.isEmpty())
                CanvasSelectionHelper::applyPositionOffset(m_multiDragStartPositions, QPointF());
            else
                m_activeItem->setPos(m_startItemPos);
            update();
            event->accept();
            return;
        }

        const QPointF delta = screenToCanvas(pos)-m_dragStartPos;
        if (!m_multiDragStartPositions.isEmpty()) {
            // 複数選択中：選択メンバー全員を同じ移動量で追従させる。
            CanvasSelectionHelper::applyPositionOffset(m_multiDragStartPositions, delta);
        } else {
            m_activeItem->setPos(m_startItemPos+delta);
            // 2026-09-22: バブル融合。マウスそのままの位置を控え、
            // 吸い寄せ（結合できる状態のとき）は tickBubbleFusion() が足す。
            m_fusionRawDragPos = m_startItemPos + delta;
            m_fusionRawValid = true;
            tickBubbleFusion();
        }
        if (!m_singleClickTimer->isActive()) {
            m_trashZone->move(16, height()-m_trashZone->height()-16);
            m_trashZone->updateProximity(pos);
        }
        update();
    } else if (m_activeMode=="item_resizing"&&m_activeItem) {
        double delta=(screenToCanvas(pos)-m_dragStartPos).x();
        m_activeItem->setScale(
            (m_resizeBaseW*m_startItemScale+delta)/m_resizeBaseW);
        update();
    } else if (m_activeMode=="rect_select") {
        // 空きキャンバスのドラッグ：選択矩形（マーキー）を更新する。
        // ドラッグ中もリアルタイムに選択内容を更新する。
        m_rectSelectCurrentCanvas = QRectF(m_rectSelectStartCanvas, screenToCanvas(pos)).normalized();
        const QList<CanvasItem*> hit = itemsInCanvasRect(m_rectSelectCurrentCanvas);
        if (!(event->modifiers() & Qt::ShiftModifier))
            m_selectedItems.clear();
        for (CanvasItem *item : hit) m_selectedItems.insert(item);
        update();
    }
    event->accept();
}

void CanvasWidget::handleMouseRelease(QMouseEvent *event)
{
    // 2026-09-20: サイドボタン（元に戻す／やり直し）は押した時点で処理済み。
    // 離したときに左ドラッグ等の確定処理へ落とさない。
    if (event->button() == Qt::BackButton || event->button() == Qt::ForwardButton) {
        event->accept();
        return;
    }

    // 2026-09-17: 元に戻す/やり直しのステップ確定。この関数は途中で return する
    // 分岐が多いため、抜けるときに必ず走るようにしておく。確定は、ゴミ箱削除・
    // 押し出しアニメの開始などリリース処理を全部終えた後に行う。
    struct UndoCommitOnExit {
        CanvasWidget *widget;
        bool allReleased;
        ~UndoCommitOnExit() { if (allReleased) widget->commitUndoStep(); }
    } undoCommitOnExit { this, event->buttons() == Qt::NoButton };

    // 2026-07-13 バグ修正: 右クリック（アクティブ化専用ボタン）は
    // handleMousePress側でstartLiftUp()を呼ぶが、リリース側に対応する
    // startLiftDown()が無く、liftScaleが1.025のまま永久に残っていた
    // （左クリックはドラッグ確定パス等で必ず戻る）。描画はliftScaleを
    // 掛けた大きさ（itemScreenRect()）になる一方、全画面フォーカスの
    // ズーム計算はitemRect()基準のため、右クリックでアクティブ化した
    // バブルを全画面にすると「少し拡大されて収まらない」見た目になって
    // いた。右ボタンのリリースで必ずリフトを戻す（ぽよんの往復を完結
    // させる）。左ドラッグ等のモード処理には影響させず、ここで完結する。
    if (event->button() == Qt::RightButton) {
        // 2026-09-17: 右ドラッグでのウィンドウ移動を終える。全画面解除で
        // 打ち切ったアスペクト再フィットはここでやり直す（島ドラッグと同じ）。
        const bool wasWindowDrag = m_rightWindowDragging;
        const bool wasPressed = m_rightWindowDragPending;
        m_rightWindowDragPending = false;
        m_rightWindowDragging = false;
        if (wasWindowDrag && m_rightWindowDragPendingRefit) {
            m_rightWindowDragPendingRefit = false;
            if (auto *mw = qobject_cast<MainWindow*>(window()))
                mw->refitFocusedBubbleAspect();
        }
        // 動かさずに離した → 右クリックとして確定（アクティブ化／ダブルで全画面）。
        if (wasPressed && !m_rightClickCancelledByDrag && !wasWindowDrag) {
            performRightClick(m_rightPressScreenPos);
            // 押した瞬間のリフトアップ→離してリフトダウン、だった「ぽよん」を
            // 離した瞬間に往復させる。すぐ戻すと膨らみが見えないので少し待つ。
            if (m_activeItem && m_activeMode.isEmpty()
                && !m_focusedItem && !m_pendingFocusItem) {
                QPointer<CanvasWidget> self(this);
                CanvasItem *lifted = m_activeItem;
                QTimer::singleShot(150, this, [self, lifted] {
                    if (!self || self->m_activeItem != lifted) return;
                    if (!self->m_activeMode.isEmpty()) return;
                    if (self->m_focusedItem || self->m_pendingFocusItem) return;
                    self->startLiftDown();
                });
            }
        }
        m_rightClickCancelledByDrag = false;
        event->accept();
        return;
    }

    // 動画全画面中の長押し（2倍速）が確定していた場合は、ここで速度を
    // 戻すだけにして、通常のクリック後処理（再生/一時停止トグル等）へは
    // 進ませない。まだ長押し確定前（クイックなクリック）だった場合は
    // タイマーを止めるだけで、以降の通常処理へそのまま進む。
    m_longPressTimer->stop();
    if (m_longPressActive) {
        if (m_longPressVideo)
            m_longPressVideo->setPlaybackSpeed(1.0);
        setLongPressActive(false);
        m_longPressVideo = nullptr;
        emit focusOverlayUpdateRequested();
        event->accept();
        return;
    }
    m_longPressVideo = nullptr;

    // 2026-09-20: ダブルクリックの2回目を離した。動かしていなければここで
    // ダブルクリック（全画面／ランチャー起動）を確定する。動かして逆輸入
    // ドラッグになった場合は mode が external_drag_prepare に変わっている。
    if (m_activeMode=="double_press") {
        m_activeMode.clear();
        const bool moved = (event->position()-m_dragStartScreenPos).manhattanLength()>8;
        if (!moved)
            performLeftDoubleClick(m_lastClickItemForDouble);
        // 全画面化などは各処理が自前で描画を要求する。ここで update() は
        // 積まない（暗転が変わるときだけ updateExportHint() が1回描く）。
        updateExportHint();
        event->accept(); return;
    }
    if (m_activeMode=="focus_double_press") {
        m_activeMode.clear();
        const bool moved = (event->position()-m_dragStartScreenPos).manhattanLength()>8;
        CanvasItem *focusTarget = m_focusedItem ? m_focusedItem : m_pendingFocusItem;
        // 別アイテムへの全画面切り替えはブロック（そのまま何もしない）
        if (!moved && focusTarget && m_lastClickItemForDouble == focusTarget)
            unfocusItem();
        event->accept(); return;
    }

    // 2026-09-12: リストの行を離した。動かしていれば並べ替えを確定、
    // 動かしていなければその動画へ移動する。どちらの場合もここで
    // 消費して返すので、下のバブルの「クリック＝再生/一時停止」には
    // 絶対に落ちない。
    if (m_activeMode=="labelRowDrag") {
        auto *owner = m_labelRowDragVideo.data();
        m_labelRowDragVideo = nullptr;
        m_activeMode.clear();
        if (owner) {
            owner->endRowDrag();
            emit playbackOverlayUpdateRequested();
        }
        // 掴んでいる間に走っていたシングルクリック判定を止める。
        if (m_singleClickTimer)
            m_singleClickTimer->stop();
        update(); event->accept(); return;
    }

    // 2026-09-12: ラベルの引っ張りを離した。しきい値を超えていれば固定／
    // 解除のトグル、超えていなければリストの開閉。
    if (m_activeMode=="labelPull") {
        auto *owner = m_labelPullVideo.data();
        m_labelPullVideo = nullptr;
        m_activeMode.clear();
        if (owner) {
            // 2026-09-23: 長押しが発動していたら、離しても固定・開閉はしない。
            if (owner->labelHoldSwallowRelease()) {
                owner->cancelLabelPull();
                emit playbackOverlayUpdateRequested();
                update(); event->accept(); return;
            }
            if (owner->endLabelPull()) {
                if (owner->labelPinned()) {
                    // 固定は同時に1つだけ。前の固定は外す。
                    if (m_pinnedLabelVideo && m_pinnedLabelVideo != owner)
                        m_pinnedLabelVideo->setLabelPinned(false);
                    m_pinnedLabelVideo = owner;
                } else if (m_pinnedLabelVideo == owner) {
                    m_pinnedLabelVideo = nullptr;
                }
            } else if (owner->labelPanelSupported()) {
                owner->toggleLabelPanel();
            }
            emit playbackOverlayUpdateRequested();
        }
        update(); event->accept(); return;
    }

    if (m_activeMode=="seeking") {
        // ドラッグ中はbcSeekFast()（キーフレーム基準の粗いシーク）だけを
        // 使っていたので、離した瞬間に精密シーク(bcSeek)を1回だけ
        // 呼んで正確な位置へ合わせ直す。
        auto *av=canvasItemAsVideo(m_activeItem);
        if (av && av->seekBarVisible()) {
            const QPointF pos = event->position();
            const int bx=28,vbx=bx+36,sx=vbx+24,sw=width()-sx-100;
            qint64 dur=av->duration();
            if (dur>0) av->bcSeek(static_cast<qint64>(dur*qBound(0.0,(pos.x()-sx)/sw,1.0)));
        }
        m_activeMode.clear(); update(); event->accept(); return;
    }
    if (m_activeItem&&m_activeMode=="panning") {
        const bool isMultiDrag = !m_multiDragStartPositions.isEmpty();
        if (isMultiDrag && m_trashZone->isVisible() && m_trashZone->isHovering(event->position())) {
            // 複数選択中のバブルをまとめてゴミ箱へ。
            deleteSelectedItems();
            m_multiDragStartPositions.clear();
            m_trashZone->hideTrash();
            QTimer::singleShot(300, m_trashZone, &QWidget::hide);
            m_activeMode.clear(); update(); event->accept(); return;
        }
        if (m_trashZone->isVisible()&&m_trashZone->isHovering(event->position())) {
            // Phase 10 Step 4: 後始末一式は CanvasWidget::destroyItem() に集約。
            // GLコンテキストを必要とする releaseGlResources()/delete をその内側で
            // 行うため、makeCurrent()/doneCurrent() でこの1件分を囲む
            // （以前と同じく、doneCurrent()より前に delete が完了する順序を維持）。
            CanvasItem *itemToDelete = m_activeItem;
            makeCurrent();
            destroyItem(itemToDelete);
            doneCurrent();

#ifdef Q_OS_WIN
            // 診断/対策: 動画・音声バブル削除(=MpvPlayer破棄、WASAPI COM絡み)後に
            // メインスレッドのCOM/OLEアパートメントが意図せず破棄されていないか
            // 確認し、必要なら再初期化する。OleInitializeは既に初期化済みなら
            // S_FALSEを返すだけで安全なので何度呼んでも問題ない。
            {
                HRESULT oleHr = OleInitialize(nullptr);
                qDebug() << "[Trash][CanvasWidget] item deleted. OleInitialize hr=0x"
                         << Qt::hex << static_cast<unsigned long>(oleHr) << Qt::dec
                         << (oleHr == S_OK ? "(S_OK: 未初期化状態だった! 再初期化した)"
                             : oleHr == S_FALSE ? "(S_FALSE: 既に初期化済み、問題なし)"
                             : "(その他/エラー)");
                if (auto *mw = qobject_cast<MainWindow*>(window()))
                    mw->refreshDropTargets();
            }
#endif
        } else {
            // ドロップ確定：重なっている他のバブルだけを「もにゅっ」と押し出す。
            // ドロップした本人（複数選択中なら選択メンバー全員）はここでは動かさない。
            if (isMultiDrag) {
                startOverlapPushAnimation(m_selectedItems);
            } else if (beginFusionAbsorbOnRelease()) {
                // 2026-09-22: 融合できる状態で離した → 吸い込みへ。
                // 押し出しもリフトダウンもしない（吸い込みアニメが大きさを持つ）。
            } else {
                startOverlapPushAnimation(m_activeItem);
            }
            if (!m_fusion.absorbing())
                startLiftDown();
        }
        m_fusionRawValid = false;
        m_multiDragStartPositions.clear();
        m_trashZone->hideTrash();
        QTimer::singleShot(300, m_trashZone, &QWidget::hide);
    } else if (m_activeItem && m_activeMode=="item_resizing") {
        // リサイズ確定時も同様に重なりを解消する。
        startOverlapPushAnimation(m_activeItem);
        // リサイズ終了時も掴み上げ状態を解除する（従来はpanningのみで呼ばれており、
        // リサイズ後はliftScaleが1.05のまま戻らずフェードイン等が永久に動かなかった）。
        startLiftDown();
    } else if (m_activeMode=="rect_select") {
        // 範囲選択の確定。選択内容はドラッグ中に随時更新済みなのでここでは何もしない。
    }
    if (m_activeMode == "camera_pan") {
        qDebug("[Focus][camera][PAN-TRACE] camera_pan end camPos=(%.1f,%.1f)",
               m_cameraPos.x(), m_cameraPos.y());

        // 2026-09-20: ホイールのダブルクリック（範囲選択中なら整列）。
        // ホイールは押した瞬間からパンが始まるため、クリックかどうかは
        // 離したときに「動いた量」と「押していた時間」で決める:
        //   クリック   … 移動 kMiddleClickMaxDrag px 以下 かつ kMiddleClickMaxHoldMs 以内
        //   ダブル     … 前回のクリックを離してから kMiddleDoubleClickGapMs 以内に
        //                次を押して、しかも前回の位置から kMiddleDoubleClickMaxDist px 以内
        // それ以外（普通のパン）は従来どおり慣性へ。
        constexpr qreal  kMiddleClickMaxDrag       = 4.0;   // px（マンハッタン距離）
        constexpr qint64 kMiddleClickMaxHoldMs     = 300;
        constexpr qint64 kMiddleDoubleClickGapMs   = 350;
        constexpr qreal  kMiddleDoubleClickMaxDist = 10.0;  // px
        const QPointF releasePos = event->position();
        const bool isClick =
            event->button() == Qt::MiddleButton
            && (releasePos - m_canvasMiddlePressPos).manhattanLength() <= kMiddleClickMaxDrag
            && m_canvasMiddlePressClock.isValid()
            && m_canvasMiddlePressClock.elapsed() <= kMiddleClickMaxHoldMs;
        bool handledDouble = false;
        if (isClick) {
            // 前回クリックを離してから今回押すまでの間隔 = 経過時間 − 押していた時間
            const bool continuesChain =
                m_canvasMiddleClickClock.isValid()
                && (m_canvasMiddleClickClock.elapsed() - m_canvasMiddlePressClock.elapsed())
                       <= kMiddleDoubleClickGapMs
                && (m_canvasMiddlePressPos - m_lastCanvasMiddleClickPos).manhattanLength()
                       <= kMiddleDoubleClickMaxDist;
            m_canvasMiddleClickCount = continuesChain ? m_canvasMiddleClickCount + 1 : 1;
            if (m_canvasMiddleClickCount >= 2) {
                // 2回目＝大きさで整列、3回目＝名前順で並べ直す。
                // トリプルを待ってからダブルを確定すると反応が遅れるので、
                // 2回目で即整列し、3回目が来たらその上から名前順に並べ直す。
                const ArrangeMode mode = m_canvasMiddleClickCount >= 3
                    ? ArrangeMode::ByName : ArrangeMode::BySize;
                if (m_canvasMiddleClickCount >= 3) {
                    m_canvasMiddleClickClock.invalidate();   // 4連打で次を作らない
                    m_canvasMiddleClickCount = 0;
                } else {
                    m_canvasMiddleClickClock.start();
                    m_lastCanvasMiddleClickPos = m_canvasMiddlePressPos;
                }
                // クリック中の数pxのパンは取り消して、押す前の位置へ戻す。
                m_cameraPos = m_startCameraPos;
                updateCanvasFromCamera();
                m_canvasPanInertia->stop();
                if (!arrangeSelectedBubbles(mode))
                    requestFullRender(CanvasRenderReason::Camera);
                handledDouble = true;
            } else {
                m_canvasMiddleClickClock.start();
                m_lastCanvasMiddleClickPos = m_canvasMiddlePressPos;
            }
        } else {
            m_canvasMiddleClickClock.invalidate();       // パンを挟んだら連打は途切れる
            m_canvasMiddleClickCount = 0;
        }

        if (!handledDouble) {
            // 2026-07-12: リリース時点の速度が一定以上なら慣性で動き続ける。
            // 「Camera Pan Inertia」設定がキャンバス側で無効なら開始せず、
            // その場で速度をゼロに戻す。
            if (!isClick && AppSettings::instance().cameraPanInertiaEnabledForCanvas())
                m_canvasPanInertia->endDrag();
            else
                m_canvasPanInertia->stop();
        }
    }
    if (m_activeMode == "focus_image_pan") {
        // 2026-07-12: リリース時点の速度（ドラッグ中にサンプリング済みの
        // EMA平滑化済み値）が一定以上なら、そのまま慣性で動き続ける
        // アニメーションを開始する。ゆっくり離した場合（速度が閾値未満）や
        // 対象が既に無くなっている場合、または「Camera Pan Inertia」設定が
        // フォーカス側で無効な場合は慣性無しでその場で止まる。
        // 「静止してからしばらく経って離したら慣性を無効化する」判定・
        // 閾値そのものはCameraPanInertia::endDrag()側にある。
        CanvasItem *focusTarget = m_focusedItem ? m_focusedItem : m_pendingFocusItem;
        if (focusTarget && !dynamic_cast<CanvasLauncher*>(focusTarget)
                && AppSettings::instance().cameraPanInertiaEnabledForFocus())
            m_focusPanInertia->endDrag();
        else
            m_focusPanInertia->stop();
    }
    m_activeMode.clear(); update(); event->accept();
}

// 画像バブル全画面中のカメラ位置クランプ。ビューポート（画面全体）が
// 画像バブルの矩形からはみ出さない範囲に m_cameraPos を収める。
// ビューポートの方が画像より大きい軸（minZoom未満は通常来ないが保険）は
// 中央固定にする。ホイールズーム・ドラッグパン（focus_image_pan）の両方から呼ぶ。
void CanvasWidget::clampCameraToFocusedImage(const QRectF &itemRect)
{
    if (m_cameraZoom <= 0.0) return;
    const double halfW = width()  / (2.0 * m_cameraZoom);
    const double halfH = height() / (2.0 * m_cameraZoom);
    QPointF p = m_cameraPos;
    if (itemRect.width() <= halfW * 2.0)
        p.setX(itemRect.center().x());
    else
        p.setX(qBound(itemRect.left() + halfW, p.x(), itemRect.right() - halfW));
    if (itemRect.height() <= halfH * 2.0)
        p.setY(itemRect.center().y());
    else
        p.setY(qBound(itemRect.top() + halfH, p.y(), itemRect.bottom() - halfH));
    m_cameraPos = p;
}

// 2026-07-12: 慣性アニメを即座に止める。unfocusItem系、新規パン開始時の
// リセット、ホイールズームでの打ち切りなど、複数箇所から呼ばれるため
// ヘルパー化してある。実体はCameraPanInertia::stop()への薄い転送。
void CanvasWidget::stopFocusPanInertia()
{
    m_focusPanInertia->stop();
}

// 2026-07-12: 中ボタンのダブルクリックで呼ばれる。ホイールズーム/ドラッグ
// パンでずらした表示状態を、バブル全体が画面に収まる「ネイティブ」表示
// （fit状態、余白があれば余白ごと表示・カメラは中央固定）へアニメーション
// で戻す。zoom値の式はhandleWheel()のminZoom計算・
// FocusSessionController::computeFocusZoomLevel()（フォーカスイン時の
// 初期ズーム）と同じ考え方（qMin＝画面に収まる方＝余白ありの表示）に揃えて
// ある。animateCamera()は既存のフォーカスイン/アウトの寄る/引く演出と
// 同じQVariantAnimationを使い回すため、isCameraFocusAnimationRunning()が
// 自動的にこのアニメ中もtrueを返し、ホイールズーム・中ボタンパン・慣性が
// 割り込まないようになる。
void CanvasWidget::resetFocusCameraToNative(CanvasItem *focusTarget)
{
    if (!focusTarget)
        return;

    stopFocusPanInertia();

    const QRectF ir = focusTarget->itemRect();
    const double nativeZoom = qMin(static_cast<double>(width()) / ir.width(),
                                    static_cast<double>(height()) / ir.height());
    constexpr int kResetDurationMs = 260;
    animateCamera(ir.center(), nativeZoom, kResetDurationMs);

    // 2026-07-16: このリセットアニメ中だけ、中間フレームでカメラがアイテム
    // 範囲外を指してフォーカス対象の外側が見えてしまわないよう、毎tick
    // clampCameraToFocusedImage()と同じ規則でクランプする
    // （animateCamera()側でこのフラグを一旦falseにリセットしているため、
    // 呼び出し直後に立てる必要がある。applyCameraAnimationProgress()参照）。
    m_camAnimClampToItemBounds = true;
    m_camAnimClampItem = focusTarget;

    // 演出: リセットが発生したことを視覚的に強調するため、通常より強い
    // 枠グローを即座に発生させる（triggerStrongBorderGlow()参照）。
    triggerStrongBorderGlow();
}

void CanvasWidget::handleWheel(QWheelEvent *event)
{
    // 2026-09-12: ラベルのコントローラ（リスト）の上では、ホイールは
    // リストのスクロールに使う。ここで先に消費しないと、下のキャンバスの
    // 拡大縮小に持っていかれてリストが動かせない。
    if (auto *av = labelOwnerVideo()) {
        if (av->handleLabelPanelWheel(event->position(),
                                      event->angleDelta().y())) {
            emit playbackOverlayUpdateRequested();
            event->accept();
            return;
        }
    }

    CanvasItem *focusTarget=m_focusedItem?m_focusedItem:m_pendingFocusItem;
    if (focusTarget) {
        // 2026-07-12 バグ修正: フォーカスイン/アウト（寄る/引く）のカメラ
        // アニメ中は、中ボタンパン（mousePressEvent側）と同じ理由でホイール
        // ズームも受け付けないようにする。これまではm_pendingFocusItemが
        // セットされている間（＝寄っていくアニメの最中）もfocusTargetが
        // 真になり、ここでm_cameraZoom/m_cameraPosを直接書き換えてしまって
        // いたため、animateCamera()のQVariantAnimationと同じプロパティを
        // 同時に奪い合ってガクガクする不具合になっていた。全画面になり
        // きって（アニメが完了して）からでないと効かないようにする。
        if (isCameraFocusAnimationRunning()) { event->accept(); return; }
        // 2026-07-12: 画像バブル限定だったズーム/パンを、ランチャー（exe/lnk、
        // ダブルクリックがlaunch()に直結しておりそもそもフォーカス状態に
        // ならない）以外の全バブル種別（動画含む）で使えるようにした。
        if (!dynamic_cast<CanvasLauncher*>(focusTarget)) {
            // 2026-07-12: 慣性パン中にホイールズームしたら、その時点で慣性は
            // 打ち切る。慣性で動いている最中にズーム基準（m_cameraZoom）が
            // 変わると「動き続けている画面を無理やりズームする」形になって
            // 気持ち悪いため、ズーム操作を「今の動きを止めて新しく見る場所を
            // 選び直す」意思表示として扱う。
            stopFocusPanInertia();

            // カーソル位置を中心にズームする（以前はir.center()へ毎回リセット
            // していたため、ドラッグパンで動かした表示位置がホイール操作で
            // 元に戻ってしまっていた）。ズーム後はドラッグパンと同じ
            // クランプ処理でビューポートがバブル外へはみ出さないようにする。
            // 2026-07-17: 1コリあたりのズーム量は設定（標準/高感度）で
            // 変えられるようにした（AppSettings::zoomWheelFactorStep()）。
            const double zoomStep = AppSettings::instance().zoomWheelFactorStep();
            double factor=event->angleDelta().y()>0?(1.0+zoomStep):(1.0/(1.0+zoomStep));
            QRectF ir=focusTarget->itemRect();
            // 2026-07-12: 以前はqMax()（バブルが画面全体を覆う=余白が消える
            // ズーム値）を下限にしていたが、フォーカスイン直後の初期ズーム
            // （FocusSessionController::computeFocusZoomLevel()、qMin()＝
            // バブル全体が画面に収まる＝余白ありのズーム値）より下限の方が
            // 大きいというねじれがあった。モニターとバブルのアスペクト比が
            // 合わず余白が出るケース（例: 4:3画面に16:9動画）で、少し
            // 拡大しただけで下限に張り付き、二度と初期の「余白あり」表示へ
            // 戻れなくなる不具合の原因だったため、qMin()に揃える。
            double minZoom=qMin((double)width()/ir.width(),(double)height()/ir.height());
            // 2026-07-12: 上限を絶対値10.0で固定していたため、minZoom自体が
            // ウィンドウ解像度に比例して大きくなるフルスクリーン時ほど
            // 「fit状態から拡大できる余地」が相対的に狭くなってしまって
            // いた（例: 通常ウィンドウではfitの約3.7倍まで拡大できるのに、
            // 高解像度フルスクリーンだと2倍弱までしか拡大できない、など）。
            // ウィンドウサイズによらず拡大範囲の体感を揃えるため、上限も
            // minZoomからの相対倍率にする。
            const double maxZoom = minZoom * 6.0;
            QPointF pos=event->position(), bc=screenToCanvas(pos);
            // 2026-07-17: ホイール1コリごとにm_cameraZoomを即座に書き換える
            // のをやめ、CameraZoomEaseで目標値へなめらかにイージングする
            // ようにした。ただし中ボタンドラッグパン中（focus_image_pan）に
            // ホイールした場合だけは例外で、従来通りその場で即座に反映する。
            // ドラッグ中のパン計算（handleMouseMove）はm_startCameraPos/
            // m_dragStartPosを基準にその都度のm_cameraZoomで割る作りのため、
            // ここでイージングを回してm_cameraZoomが複数フレームにわたって
            // 変化し続けると、ドラッグ側の計算と同じ値を毎フレーム奪い合って
            // 画面が吹っ飛んで見える不具合になる。ドラッグ中は「なめらかさ」
            // より「ドラッグ操作との整合性」を優先し、即時反映のままにする。
            if (m_activeMode==QStringLiteral("focus_image_pan")) {
                if (m_zoomEase->isActive()) m_zoomEase->stop();
                m_targetCameraZoom = qBound(minZoom, m_cameraZoom*factor, maxZoom);
                m_cameraZoom = m_targetCameraZoom;
                updateCanvasFromCamera();
                m_cameraPos -= (pos-canvasToScreen(bc))/m_cameraZoom;
                clampCameraToFocusedImage(ir);
                updateCanvasFromCamera(); update();
                // ドラッグ移動量の基準がズレるため、基準点を現在位置で取り直す。
                m_dragStartPos=pos; m_startCameraPos=m_cameraPos;
            } else {
                if (!m_zoomEase->isActive())
                    m_targetCameraZoom = m_cameraZoom;
                m_targetCameraZoom = qBound(minZoom, m_targetCameraZoom*factor, maxZoom);
                m_zoomAnchorScreenPos = pos;
                m_zoomAnchorCanvasPos = bc;
                m_zoomAnchorHasClampRect = true;
                m_zoomAnchorClampRect = ir;
                m_zoomEase->retarget(m_cameraZoom, m_targetCameraZoom);
            }
        }
        event->accept(); return;
    }
    // フォーカスイン/アウト（寄る/引く）のカメラアニメ中は、ホイールズームでの
    // カメラ操作を受け付けない（中ボタン分岐と同じ理由。mousePressEvent参照）。
    if (isCameraFocusAnimationRunning()) { event->accept(); return; }
    // 2026-07-12: 通常キャンバスの慣性パン中にホイールズームしたら、その
    // 時点で慣性は打ち切る（focus_image_panと同じ理由: 動き続けている
    // 画面を無理やりズームするのは気持ち悪いため、ズーム操作を「今の動きを
    // 止めて新しく見る場所を選び直す」意思表示として扱う）。
    m_canvasPanInertia->stop();
    // 2026-07-17: 1コリあたりのズーム量は設定（標準/高感度）で変えられる
    // ようにした（AppSettings::zoomWheelFactorStep()）。
    const double zoomStep = AppSettings::instance().zoomWheelFactorStep();
    double factor = event->angleDelta().y()>0?(1.0+zoomStep):(1.0/(1.0+zoomStep));
    QPointF pos=event->position(), bc=screenToCanvas(pos);
    // 2026-07-17: フォーカス中の分岐と同じ理由で、ここも即座にm_cameraZoomを
    // 書き換えるのをやめ、CameraZoomEaseで目標値へなめらかにイージングする。
    // ただし中ボタンドラッグパン中（camera_pan）にホイールした場合だけは
    // 例外で、従来通り即座に反映する（イージングとドラッグ側の計算が
    // 同時にm_cameraZoom/m_cameraPosを奪い合って画面が吹っ飛ぶのを防ぐため。
    // 詳しい理由はフォーカス側分岐の同種コメント参照）。
    if (m_activeMode==QStringLiteral("camera_pan")) {
        if (m_zoomEase->isActive()) m_zoomEase->stop();
        m_targetCameraZoom = qBound(0.01, m_cameraZoom*factor, 50.0);
        m_cameraZoom = m_targetCameraZoom;
        updateCanvasFromCamera();
        m_cameraPos -= (pos-canvasToScreen(bc))/m_cameraZoom;
        updateCanvasFromCamera(); update();
        // ドラッグ移動量の基準がズレるため、基準点を現在位置で取り直す。
        m_dragStartPos=pos; m_startCameraPos=m_cameraPos;
    } else {
        if (!m_zoomEase->isActive())
            m_targetCameraZoom = m_cameraZoom;
        m_targetCameraZoom = qBound(0.01, m_targetCameraZoom*factor, 50.0);
        m_zoomAnchorScreenPos = pos;
        m_zoomAnchorCanvasPos = bc;
        m_zoomAnchorHasClampRect = false;
        m_zoomEase->retarget(m_cameraZoom, m_targetCameraZoom);
    }
    event->accept();
}

bool CanvasWidget::canAcceptDropMimeData(const QMimeData *mime, QString *reason) const
{
    return CanvasDropHandler::canAcceptMimeData(mime, isFocusDropBlocked(), reason);
}

void CanvasWidget::logDropMimeData(const char *eventName, const QMimeData *mime,
                                   bool accepted, const QString &reason) const
{
    const CanvasDropHandler::MimeSummary summary = CanvasDropHandler::summarizeMimeData(mime);
    qDebug().noquote()
        << QStringLiteral("[DND][CanvasWidget::%1] accepted=%2 reason=%3 hasUrls=%4 hasText=%5 hasImage=%6 text=\"%7\" urls=[%8]")
              .arg(QString::fromLatin1(eventName))
              .arg(accepted)
              .arg(reason)
              .arg(summary.hasUrls)
              .arg(summary.hasText)
              .arg(summary.hasImage)
              .arg(summary.textPreview)
              .arg(summary.urls.join(QStringLiteral(", ")));
}

// 2026-07-18追加: videoDimensionsReady（実解像度・対比の判明/変化）への
// 接続の共通化。従来はaddMediaSource()内のインラインラムダでのみ接続して
// おり、セッション復元経路（restoreSessionFromJson、CanvasSessionSerializer.cpp）
// で作られたバブルには接続されていなかった。そのため復元バブルでは、
// 連続再生で対比の違う動画へ切り替わっても、フォーカス中のウィンドウ
// 再フィット（refitFocusedWindowForAspectChange）・重なり解消・フレーム拡張
// が一切発火しなかった（「フォーカス中に対比が変わっても収まらない」事象の
// 原因のひとつ）。新規追加・復元の両経路から必ずこの関数で接続する。
void CanvasWidget::connectVideoDimensionSignals(CanvasVideo *video)
{
    // ドロップ/復元時点では実際の解像度がまだ不明なため仮サイズ(480x270等)で
    // 重なりを解消している。実解像度が判明してitemRect()のサイズが変わったら、
    // その場でもう一度だけ重なりを解消し直す（縦長動画などで後から
    // 隣のバブルに食い込むのを防ぐ）。
    connect(video, &CanvasVideo::videoDimensionsReady, this, [this, video]{
        startOverlapPushAnimation(video);
        // 2026-07-18 修正: 連続再生で対比の違う動画に切り替わった時に
        // 次の動画へ進まなくなる不具合が報告された。videoDimensionsReady()は
        // CanvasVideo::onUrlFetched()の内部（まだstartPlayback()を呼ぶ前）で
        // 同期的に発火するため、ここで直接refitFocusedWindowForAspectChange()
        // （ウィンドウの実リサイズ・resizeAnim開始）を呼ぶと、動画の
        // ロード処理の真っ最中にウィンドウリサイズ→resizeGL()の重い連鎖が
        // 割り込む形になり、ロード/連続再生の処理と競合していたと見られる。
        // 呼び出し自体は次のイベントループへ回して、onUrlFetched()一式が
        // 完全に終わってから（＝再生開始処理と競合しない状態で）実行する。
        QPointer<CanvasVideo> guard(video);
        QTimer::singleShot(0, this, [this, guard]{
            if (guard)
                refitFocusedWindowForAspectChange(guard);
        });
    });
}

void CanvasWidget::connectVideoRenderSignals(CanvasVideo *video)
{
    if (!video)
        return;

    connect(video, &CanvasVideo::frameReady, this, [this, video] {
        requestItemRender(video, CanvasRenderReason::VideoFrame);
    });
    connect(video, &CanvasVideo::playbackUiChanged,
            this, [this, video] {
        if (video == canvasItemAsVideo(m_activeItem)
            || video == m_playbarOutgoingVideo) {
            emit playbackOverlayUpdateRequested();
        }
    });
    connect(video, &CanvasVideo::playbackPositionChanged,
            this, [this, video](qint64) {
        if (video != canvasItemAsVideo(m_activeItem))
            return;
        if (!m_playbarVisible && !video->playbackBarAnimActive())
            return;
        constexpr qint64 kPlaybackUiIntervalMs = 100;
        if (m_playbackOverlayUpdateClock.isValid()
            && m_playbackOverlayUpdateClock.elapsed() < kPlaybackUiIntervalMs) {
            return;
        }
        if (m_playbackOverlayUpdateClock.isValid())
            m_playbackOverlayUpdateClock.restart();
        else
            m_playbackOverlayUpdateClock.start();
        emit playbackOverlayUpdateRequested();
    });
}

// 固定ラベルが優先されるか（CanvasWidget.hのm_pinnedLabelVideo参照）。
bool CanvasWidget::pinnedLabelWins() const
{
    if (!m_pinnedLabelVideo || !m_pinnedLabelVideo->labelPinned())
        return false;
    // 別の動画を全画面（フォーカス）にしている間は、そちらのラベルを優先。
    if (auto *focused = canvasItemAsVideo(m_focusedItem))
        return focused == m_pinnedLabelVideo;
    return true;
}

CanvasVideo *CanvasWidget::labelOwnerVideo() const
{
    if (pinnedLabelWins())
        return m_pinnedLabelVideo.data();
    return canvasItemAsVideo(m_activeItem);
}

// いま出ているラベル（＋リスト）の矩形。島の出現判定から除くために
// DynamicIslandController が使う（2026-09-12）。ラベルが出ていない
// ときは空を返すので、島の判定は従来どおりになる。
// 2026-09-16 F-1: いま画面にラベル／リストを「まだ描いている」動画すべての
// 矩形の和。オーバーレイの無効化範囲はこちらを使うこと。
//
// 【なぜ要るか】labelUiRect() が見る labelOwnerVideo() は「いまラベルを
// 持っている動画」で、バブルを非アクティブにした瞬間に null になる。一方で
// 描画側は m_playbarOutgoingVideo のフェードが終わるまでラベルとリストを
// 描き続ける。そのため labelUiRect() だけを無効化範囲にすると、
//   ・ラベルの丸い台（上から96px以内）… フォールバックの帯に救われて消える
//   ・リスト（96pxより下へ長く伸びる）… 誰も無効化しないので画素が残る
// となり、「固定していないバブルを非アクティブにすると、リストが薄いまま
// その場に固まって残る（当たり判定はもう無い）」という症状になっていた。
// D-3で上下の帯をやめるまでは、帯の外接矩形がウィンドウ全体に広がって
// いたため偶然消えていた＝D-3が表に出した潜在バグ。
QRect CanvasWidget::labelUiDamageRect() const
{
    QRectF r = labelUiRect();
    const auto addVideo = [&r](CanvasVideo *v) {
        if (!v)
            return;
        QRectF one = v->labelScreenRect();
        const QRectF panel = v->labelPanelScreenRect();
        if (!panel.isEmpty())
            one = one.isEmpty() ? panel : one.united(panel);
        if (one.isEmpty())
            return;
        one = one.adjusted(-4.0, -4.0, 4.0, 4.0);
        r = r.isEmpty() ? one : r.united(one);
    };
    // フェードアウト中（直前までアクティブだった動画）。
    if (m_playbarOutgoingVideo && m_playbarOutgoingVideo->playbackBarAnimActive())
        addVideo(m_playbarOutgoingVideo);
    // 引っ込み中の固定ラベル。
    if (auto *pinned = m_pinnedLabelVideo.data()) {
        if (pinned->pinnedOverlayFade() > 0.01f)
            addVideo(pinned);
    }
    return r.isEmpty() ? QRect() : r.toAlignedRect();
}

QRect CanvasWidget::labelUiRect() const
{
    auto *owner = labelOwnerVideo();
    if (!owner)
        return QRect();
    QRectF r = owner->labelScreenRect();
    const QRectF panel = owner->labelPanelScreenRect();
    if (!panel.isEmpty())
        r = r.isEmpty() ? panel : r.united(panel);
    if (r.isEmpty())
        return QRect();
    // 縁ぎりぎりで取り合わないよう、少しだけ広げておく。
    return r.adjusted(-4.0, -4.0, 4.0, 4.0).toRect();
}

void CanvasWidget::paintPlaybackOverlay(QPainter &painter,
                                        BackdropBlur::Regions *blurRegions)
{
    auto paintVideo = [this, &painter, blurRegions](CanvasVideo *video) {
        if (!video)
            return;
        painter.save();
        painter.setOpacity(painter.opacity() * m_contentOpacity);
        video->paintPlaybackOverlay(painter, width(), height(), blurRegions);
        painter.restore();
    };

    // 2026-09-12: 固定ラベルが左上を占めている間は、他の動画は自分の
    // ラベルを描かない（同じ場所に2枚出さないため）。再生バーなど他のUIは
    // これまで通り描く。
    CanvasVideo *pinned = pinnedLabelWins() ? m_pinnedLabelVideo.data() : nullptr;
    auto *active = canvasItemAsVideo(m_activeItem);
    if (active)
        active->setLabelSuppressedByPin(pinned && pinned != active);
    // 2026-09-12: 固定ラベルが引っ込むときはフェードさせる（パッと消さない）。
    // 「出すべきか」の指示は syncLogicMedium() 側で毎tick渡している。
    // ここでは、消え切るまで描き続けるために持ち主を控えておくだけ。
    CanvasVideo *pinnedFading = m_pinnedLabelVideo.data();
    if (m_playbarOutgoingVideo)
        m_playbarOutgoingVideo->setLabelSuppressedByPin(
            pinned && pinned != m_playbarOutgoingVideo);

    if (active) {
        const bool wantPlaybar = (m_activeMode != "panning") && m_playbarVisible;
        if (wantPlaybar || active->playbackBarAnimActive())
            paintVideo(active);
    }
    if (m_playbarOutgoingVideo && m_playbarOutgoingVideo->playbackBarAnimActive())
        paintVideo(m_playbarOutgoingVideo);

    // 固定ラベル本体（自分がアクティブなときは上のpaintVideo()に含まれる）。
    // 引っ込む途中（pinnedLabelWins()はもうfalse）でも、フェードが残って
    // いる間は描き続ける。
    if (pinnedFading && pinnedFading != active
        && pinnedFading->pinnedOverlayFade() > 0.01f) {
        painter.save();
        painter.setOpacity(painter.opacity() * m_contentOpacity);
        pinnedFading->paintPinnedLabelOverlay(painter, width(), height(), blurRegions);
        painter.restore();
    }
}

CanvasVideo *CanvasWidget::addMediaSource(const MediaSource &source, const QPointF &canvasPos)
{
    UndoAutoStep undoStep(this);
    qDebug() << "[DND][CanvasWidget] local media source:"
             << mediaKindToString(source.kind) << source.originalPathOrUrl;
    // PluginMedia は originalPathOrUrl がローカルファイルではなく元のページURL
    // （例: 動画サイトのページURL）であることを前提とした種別のため、ローカルファイル
    // チェックの対象外とする。実際のストリーム解決はプラグインが非同期に行う
    // （CanvasVideo::loadMediaSource → resolveViaPlugin）。
    // 2026-10-12: WebVideo も originalPathOrUrl が http(s) のURLなので同様に対象外。
    if (source.kind != MediaKind::PluginMedia && source.kind != MediaKind::WebVideo
        && !QUrl(source.originalPathOrUrl).isLocalFile()) {
        qDebug() << "[DND][CanvasWidget] external media source ignored in core:"
                 << source.originalPathOrUrl;
        return nullptr;
    }
    if (source.kind != MediaKind::LocalVideo && source.kind != MediaKind::LocalAudio
        && source.kind != MediaKind::PluginMedia && source.kind != MediaKind::WebVideo) {
        qDebug() << "[DND][CanvasWidget] unsupported media source kind:"
                 << mediaKindToString(source.kind);
        return nullptr;
    }

    auto *video = new CanvasVideo(this);
    // 新規バブルにも現在のマスターボリュームを即座に効かせる
    // （applyEffectiveVolume()はinitializeMpv()内で呼ばれるため、
    // それより前に設定しておけばよい）。
    video->setMasterGain(m_masterVolume.gain(), false);
    video->setPos(canvasPos);
    video->setScale(source.kind == MediaKind::LocalAudio ? 1.0 : 0.6);
    video->setCenter(canvasPos);
    connectVideoRenderSignals(video);
    // 実解像度判明/変化時の重なり解消・フレーム拡張・フォーカス中の
    // ウィンドウ再フィット（2026-07-18: 復元経路と共通化。ヘルパーの
    // コメント参照）。
    connectVideoDimensionSignals(video);

    video->loadMediaSource(source);
    m_items.append(video);
    // 新規追加時も既存バブルと重なっていたら押し出す（ドロップ第一号対応）。
    startOverlapPushAnimation(video);
    update();

    QPointer<CanvasVideo> guard(video);
    QTimer::singleShot(0, this, [this, guard]() {
        if (!guard || !m_items.contains(guard))
            return;
        makeCurrent();
        guard->initializeMpv();
        guard->warmupPlaybackSurface();
        doneCurrent();
        update();
    });
    return video;
}

CanvasVideo *CanvasWidget::addVideoUrl(const QString &mediaUrl, const QPointF &canvasPos)
{
    MediaSource source;
    source.kind = MediaKind::LocalVideo;
    source.originalPathOrUrl = mediaUrl;
    source.title = QFileInfo(QUrl(mediaUrl).toLocalFile()).fileName();
    return addMediaSource(source, canvasPos);
}

bool CanvasWidget::tryAddPluginMedia(const QString &url, const QPointF &canvasPos)
{
    if (!PluginManager::instance().findHandler(url)) {
        qDebug() << "[Plugin][CanvasWidget] no plugin handler for:" << url;
        return false;
    }

    qDebug() << "[Plugin][CanvasWidget] plugin handler found, adding as PluginMedia:" << url;

    MediaSource source;
    source.kind = MediaKind::PluginMedia;
    source.originalPathOrUrl = url;
    source.title = url;
    return addMediaSource(source, canvasPos) != nullptr;
}

CanvasVideo *CanvasWidget::addWebVideoUrl(const QString &url, const QPointF &canvasPos)
{
    qDebug() << "[DND][CanvasWidget] web video URL dropped:" << url;
    return addMediaSource(mediaSourceFromWebVideoUrl(url), canvasPos);
}

void CanvasWidget::addWebLinkOrProbedVideo(const QString &url, const QPointF &canvasPos)
{
    // 2026-10-12: 拡張子の無いURL（素材サイトの「ダウンロード」リンク等）は、
    // 中身が動画かどうかをサーバーに問い合わせてから決める。動画なら動画バブル、
    // そうでなければ従来どおりWebリンクバブル。トップページや .html 等の明らかな
    // ページは問い合わせずに即Webリンクにする（待ち時間を作らない）。
    if (!WebVideoCache::isWorthProbing(url)) {
        addLauncher(url, canvasPos);
        return;
    }
    // 結果が返るのはドロップの処理が終わった後なので、元に戻すは別の1ステップになる
    // （Web画像の非同期取得と同じ）。
    QPointer<CanvasWidget> guard(this);
    WebVideoCache::probeIsVideo(this, url, [guard, url, canvasPos](bool isVideo) {
        if (!guard)
            return;
        if (isVideo)
            guard->addWebVideoUrl(url, canvasPos);
        else
            guard->addLauncher(url, canvasPos);
    });
}

void CanvasWidget::promptAndInstallPluginZip(const QString &zipFilePath)
{
    // Windows のOSレベルD&D（COM経由）処理がまだ完了していないうちに
    // モーダルダイアログを出すと相性が悪いことがあるため、
    // dropEvent() の処理が完全に終わってから実行する。
    QPointer<CanvasWidget> selfGuard(this);
    QTimer::singleShot(0, this, [selfGuard, zipFilePath]() {
        if (!selfGuard)
            return;

        // 2026-09-13: 確認UIを島（DynamicIsland + IslandPromptPanel）へ移した。
        // 受け手（MainWindow）が接続されていればそちらへ委ね、そうでなければ
        // 従来の QMessageBox 版を使う。島側の見た目を詰めている間、
        // showPluginInstallPrompt() をいつでも呼び戻せるようにしておくための
        // フォールバックで、通常は必ず島側が使われる。
        const QMetaMethod signal =
            QMetaMethod::fromSignal(&CanvasWidget::pluginInstallRequested);
        if (selfGuard->isSignalConnected(signal)) {
            emit selfGuard->pluginInstallRequested(zipFilePath);
            return;
        }
        selfGuard->showPluginInstallPrompt(zipFilePath);
    });
}

void CanvasWidget::showPluginInstallPrompt(const QString &zipFilePath)
{
    const QString fileName = QFileInfo(zipFilePath).fileName();

    const PluginManager::PluginZipPreview preview = PluginManager::instance().previewPluginZip(zipFilePath);

    if (!preview.ok) {
        qWarning() << "[Plugin][CanvasWidget] zip preview failed:" << zipFilePath << preview.errorMessage;
        QMessageBox::warning(this, QStringLiteral("Install Plugin"),
            QStringLiteral("\"%1\" could not be recognized as a plugin.\n%2")
                .arg(fileName, preview.errorMessage));
        return;
    }

    // 確認ダイアログ本文を組み立てる。この時点ではプラグインは
    // まだ一切実行されていない（メタデータの読み取りのみ）。
    QString body = QStringLiteral(
        "Install \"%1\" as a PocketBoard plugin?\n\n"
        "Plugins included:\n").arg(fileName);

    bool anyCompatible = false;
    for (const PluginManager::PluginZipEntry &entry : preview.entries) {
        const QString ids = entry.keys.isEmpty()
            ? QStringLiteral("(unknown ID)")
            : entry.keys.join(QStringLiteral(", "));
        body += QStringLiteral("- %1  [%2]%3\n")
            .arg(entry.fileName, ids,
                 entry.iidMatches ? QString() : QStringLiteral("  * incompatible with this build - will not be loaded"));
        anyCompatible = anyCompatible || entry.iidMatches;
    }

    body += QStringLiteral(
        "\nPlugins run as native code. "
        "Only install plugins obtained from sources you trust.");

    if (!anyCompatible) {
        QMessageBox::warning(this, QStringLiteral("Install Plugin"), body);
        return;
    }

    const auto answer = QMessageBox::question(this, QStringLiteral("Install Plugin"), body,
                                               QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        qDebug() << "[Plugin][CanvasWidget] plugin install cancelled by user:" << zipFilePath;
        return;
    }

    QString err;
    if (PluginManager::instance().installPluginZip(zipFilePath, &err)) {
        qDebug() << "[Plugin][CanvasWidget] plugin package installed:" << zipFilePath;
    } else {
        qWarning() << "[Plugin][CanvasWidget] plugin package install failed:" << zipFilePath << err;
        QMessageBox::warning(this, QStringLiteral("Install Plugin"),
            QStringLiteral("Installation failed.\n%1").arg(err));
    }
}

CanvasImage *CanvasWidget::addImagePixmap(const QPixmap &pixmap, const QPointF &canvasPos,
                                          double scale, bool centerOnPoint)
{
    if (pixmap.isNull())
        return nullptr;

    UndoAutoStep undoStep(this);
    auto *image = new CanvasImage(pixmap, canvasPos, scale);
    if (centerOnPoint)
        image->setCenter(canvasPos);
    m_items.append(image);
    // 新規追加時も既存バブルと重なっていたら押し出す（ドロップ第一号対応）。
    startOverlapPushAnimation(image);
    update();
    return image;
}

CanvasImage *CanvasWidget::addImagePlaceholder(const MediaSource &source, const QPointF &canvasPos,
                                               double scale, bool centerOnPoint)
{
    UndoAutoStep undoStep(this);
    auto *image = new CanvasImage(QPixmap(), canvasPos, scale);
    if (centerOnPoint)
        image->setCenter(canvasPos);
    image->setMediaSource(source);
    image->setLoadFailed(true);
    m_items.append(image);
    startOverlapPushAnimation(image);
    update();
    return image;
}

CanvasImage *CanvasWidget::loadRemoteImageUrl(const QString &imageUrl, const QPointF &canvasPos,
                                              double scale, bool positionIsTopLeft,
                                              const std::function<void(CanvasImage*)> &onAsyncCreated)
{
    // 2026-09-17: 呼ばれた時点で履歴を止めていた（セッション復元中・Undo適用中）
    // なら、非同期で画像が届いて追加されるときも履歴に積まない。
    const bool suppressUndoOnArrival = m_undoSuppressDepth > 0;
    // 2026-07-12: キャッシュ確認はisLikelyImageUrl()のゲートより前に行う。
    // Discord CDN等の期限付きURL対策としてキャッシュファイル名はURL文字列
    // から決定的に導出されるため、初回ドロップ時にダウンロードしたものと
    // 同じパスをここで再計算するだけでヒット判定ができる（専用の保存
    // フィールドを持ち回る必要がない）。ヒットすればネットワーク不要で
    // 即座に表示でき、セッション復元時に元URLが失効していても壊れない。
    // さらに、X(Twitter)のように「再取得可能な元URLが一切存在しない」
    // ケース（WebImageCache::storeCapturedImage()参照）では、imageUrlは
    // 本物のURLではなく画像内容ハッシュベースの合成識別子("pocketboard-
    // capture:...")になっている。これはisLikelyImageUrl()の拡張子判定を
    // 通らないため、ゲートの前でキャッシュを見ておかないと合成識別子の
    // ケースが復元できなくなる。
    const QString cachedPath = WebImageCache::cachedFilePathForUrl(imageUrl);
    if (!cachedPath.isEmpty() && QFileInfo::exists(cachedPath)) {
        QFile cacheFile(cachedPath);
        QPixmap cachedPix;
        if (cacheFile.open(QIODevice::ReadOnly)) {
            // 2026-07-12: 拡張子ではなく内容から形式判定するpixmapFromImageBytes()
            // を使う。hasImage()経路（ビットマップ直接ドロップ）由来のキャッシュは
            // 常にPNG再エンコードで書き込まれるため、元URLの拡張子と実体が
            // 食い違うことがあるが、内容ベースの判定なら問題なく読める。
            cachedPix = pixmapFromImageBytes(cacheFile.readAll());
        }
        if (!cachedPix.isNull()) {
            auto *image = addImagePixmap(cachedPix, canvasPos, scale, !positionIsTopLeft);
            if (image)
                image->setMediaSource(mediaSourceFromWebImageUrl(imageUrl));
            qDebug() << "[DND][CanvasWidget] loaded web image from local cache:" << cachedPath;
            return image;
        }
        qWarning() << "[WebImageCache] cache file exists but failed to decode, refetching:" << cachedPath;
    }

    // ここから先はネットワーク再取得のみを行う経路なので、実際にHTTP(S)で
    // 取得できる見込みのある直接画像URLに対してのみ許可する（合成識別子や
    // ページURL等はここで弾く。ダウンロードできる手段がそもそも無いため）。
    if (!WebImageUrlResolver::isLikelyImageUrl(imageUrl)) {
        qDebug() << "[DND][CanvasWidget] skip non-direct image URL (no cache, not fetchable):" << imageUrl;
        return nullptr;
    }

    auto *nam = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(imageUrl)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("PocketBoard/1.0 image-loader"));

    qDebug() << "[DND][CanvasWidget] probing remote image URL:" << imageUrl;
    QNetworkReply *reply = nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, imageUrl, canvasPos, scale, positionIsTopLeft,
                                                    suppressUndoOnArrival, onAsyncCreated]() {
        std::unique_ptr<UndoSuppressScope> undoSuppress;
        if (suppressUndoOnArrival)
            undoSuppress = std::make_unique<UndoSuppressScope>(this);
        const QByteArray bytes = reply->readAll();
        const QVariant contentTypeHeader = reply->header(QNetworkRequest::ContentTypeHeader);
        const QString contentType = contentTypeHeader.toString().toLower();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        nam->deleteLater();

        QPixmap pix;
        if (networkOk && (contentType.startsWith(QStringLiteral("image/")) || contentType.isEmpty()))
            pix = pixmapFromImageBytes(bytes);
        if (networkOk && pix.isNull())
            pix = pixmapFromImageBytes(bytes);

        if (!pix.isNull()) {
            auto *image = addImagePixmap(pix, canvasPos, scale, !positionIsTopLeft);
            if (image)
                image->setMediaSource(mediaSourceFromWebImageUrl(imageUrl));
            if (image && onAsyncCreated)
                onAsyncCreated(image);
            qDebug() << "[DND][CanvasWidget] remote image loaded:" << imageUrl
                     << pix.size();
            // 2026-07-12: 表示用に取得済みのバイト列をそのままローカル
            // キャッシュへ書いておく（追加のネットワークコストはゼロ）。
            // 30MB超などキャッシュされなかった場合も、表示自体はここまでで
            // 完了しているので影響はない。
            const QString cachedPath = WebImageCache::store(imageUrl, bytes);
            if (!cachedPath.isEmpty())
                qDebug() << "[WebImageCache] cached web image:" << cachedPath;
            return;
        }

        qDebug() << "[DND][CanvasWidget] remote URL was not a decodable image:"
                 << imageUrl << "networkOk=" << networkOk << "contentType=" << contentType;
        CanvasImage *placeholder =
            addImagePlaceholder(mediaSourceFromWebImageUrl(imageUrl), canvasPos, scale, !positionIsTopLeft);
        if (placeholder && onAsyncCreated)
            onAsyncCreated(placeholder);
    });
    return nullptr;
}

CanvasLauncher *CanvasWidget::addLauncher(const QString &path, const QPointF &canvasPos,
                                          double scale, bool centerOnPoint)
{
    if (!CanvasLauncher::validateTarget(path)) {
        qWarning() << "[Launcher] skipped missing target:" << path;
        return nullptr;
    }

    UndoAutoStep undoStep(this);
    auto *launcher = new CanvasLauncher(
        path,
        CanvasLauncher::targetKindForPath(path),
        CanvasLauncher::iconForPath(path),
        canvasPos,
        scale);
    if (centerOnPoint)
        launcher->setCenter(canvasPos);
    m_items.append(launcher);
    startOverlapPushAnimation(launcher);
    update();
    qDebug() << "[Launcher] added:" << path;

    // 2026-10-11: Webリンクのfaviconは非同期取得。届くまではドメイン頭文字の
    // フォールバックアイコン。同じホストのバブルが複数あれば全部まとめて差し替える。
    // 同一ホストの取得が進行中なら重複して通信しない。
    if (launcher->isWebLink() && WebLinkIcon::needsFetch(path)) {
        const QString host = WebLinkIcon::displayHost(path);
        if (!m_webIconPendingHosts.contains(host)) {
            m_webIconPendingHosts.append(host);
            WebLinkIcon::fetch(this, path, [this, host](const QImage &img) {
                m_webIconPendingHosts.removeAll(host);
                if (img.isNull())
                    return;
                const QPixmap pix = QPixmap::fromImage(img);
                for (CanvasItem *it : std::as_const(m_items)) {
                    auto *l = dynamic_cast<CanvasLauncher*>(it);
                    if (l && l->isWebLink()
                        && WebLinkIcon::displayHost(l->path()) == host)
                        l->setIcon(pix);
                }
                update();
            });
        }
    }
    return launcher;
}

bool CanvasWidget::handleDropMimeData(const QMimeData *mime, const QPointF &widgetPos, const QString &source)
{
    QString reason;
    if (!canAcceptDropMimeData(mime, &reason)) {
        logDropMimeData(qPrintable(QStringLiteral("drop:%1").arg(source)), mime, false, reason);
        qDebug() << "[DND][CanvasWidget] unsupported drop after accept:"
                 << source << reason;
        return false;
    }

    logDropMimeData(qPrintable(QStringLiteral("drop:%1").arg(source)), mime, true, reason);
    // 2026-09-17: 1回のドロップで複数ファイルを置いても、元に戻すは1回で済むように
    // ドロップ全体を1ステップにまとめる。
    UndoAutoStep undoStep(this);
    updateCanvasFromCamera();
    QPointF cPos=screenToCanvas(widgetPos);

    if (mime->hasImage()) {
        QImage imageData = qvariant_cast<QImage>(mime->imageData());
        if (!imageData.isNull()) {
            QPixmap pix = QPixmap::fromImage(imageData);
            if (auto *image = addImagePixmap(pix, cPos)) {
                // 2026-07-12: ブラウザ(Google画像検索等)からのドラッグは、ビットマップと
                // 一緒に元のURLも渡してくることが多い。これまでここでは無視していたため
                // sourceUrl/mediaKindが一切記録されず、レイヤー切り替え・セッション復元を
                // またぐと画像が消えてしまうバグの原因になっていた。URLを拾えた場合は
                // WebImageとして記録し、既に手元にあるビットマップを再ダウンロードせず
                // そのままキャッシュへ書いておく。
                const QString webUrl = firstHttpUrlFromMime(mime);
                QString effectiveSourceUrl = webUrl;
                QByteArray bytes;
                QBuffer buffer(&bytes);
                if (buffer.open(QIODevice::WriteOnly) && imageData.save(&buffer, "PNG")) {
                    buffer.close();
                    if (!webUrl.isEmpty()) {
                        image->setMediaSource(mediaSourceFromWebImageUrl(webUrl));
                        const QString cachedPath = WebImageCache::store(webUrl, bytes);
                        if (!cachedPath.isEmpty())
                            qDebug() << "[WebImageCache] cached embedded web image:" << cachedPath;
                    } else {
                        // 2026-07-12: X(Twitter)のように、hasImage()でビットマップは
                        // 得られるものの、mime内のURL/テキストのどれも実際の画像URL
                        // ではない（ブラウザが作った一時ファイルパスや、画像そのもの
                        // ではないページURLしか無い）ケース。再取得可能な元URLが無い
                        // ので、ビットマップの内容ハッシュを合成識別子として自前の
                        // キャッシュへ保存し、それをsourceUrl代わりに使う。ネットワーク
                        // 再取得はできなくなるが、ローカルキャッシュがある限りレイヤー
                        // 切り替え・セッション復元は今までと同様に機能する。
                        const QString capturedKey = WebImageCache::storeCapturedImage(bytes);
                        if (!capturedKey.isEmpty()) {
                            image->setMediaSource(mediaSourceFromWebImageUrl(capturedKey));
                            effectiveSourceUrl = capturedKey;
                            qDebug() << "[WebImageCache] cached embedded image with no source URL, key="
                                     << capturedKey;
                        }
                    }
                }
                qDebug() << "[DND][CanvasWidget] dropped embedded image data:" << pix.size()
                         << "sourceUrl=" << effectiveSourceUrl;
                return true;
            }
        }
    }

    bool droppedLocalFile = false;
    int localDropIndex = 0;
    if (mime->hasUrls()) {
        for (const QUrl &u : mime->urls()) {
            const QString localPath = u.toLocalFile();
            if (localPath.isEmpty())
                continue;

            const QPointF itemPos = cPos + QPointF(localDropIndex * 28.0, localDropIndex * 28.0);
            const CanvasDropHandler::LocalFileKind fileKind =
                CanvasDropHandler::classifyLocalFile(localPath);

            // Phase 8: Plugin API — .zip をドロップしたらプラグインパッケージの
            // インストールとして扱う（Blenderアドオン風のD&D導入）。
            // 実行前に必ず確認ダイアログを挟む（promptAndInstallPluginZip内）。
            if (fileKind == CanvasDropHandler::LocalFileKind::PluginZip) {
                promptAndInstallPluginZip(localPath);
                droppedLocalFile = true;
                ++localDropIndex;
                continue;
            }

            QPixmap pix(localPath);
            if (fileKind == CanvasDropHandler::LocalFileKind::Image && !pix.isNull()) {
                auto *image = addImagePixmap(pix, itemPos);
                if (image)
                    image->setMediaSource(mediaSourceFromLocalFile(localPath, MediaKind::LocalImage));
                droppedLocalFile = true;
                ++localDropIndex;
                qDebug() << "[DND][CanvasWidget] dropped local image:" << localPath << pix.size();
                continue;
            }

            if (fileKind == CanvasDropHandler::LocalFileKind::Video) {
                addMediaSource(mediaSourceFromLocalFile(localPath, MediaKind::LocalVideo), itemPos);
                droppedLocalFile = true;
                ++localDropIndex;
                qDebug() << "[DND][CanvasWidget] dropped local video:" << localPath;
                continue;
            }

            if (fileKind == CanvasDropHandler::LocalFileKind::Audio) {
                addMediaSource(mediaSourceFromLocalFile(localPath, MediaKind::LocalAudio), itemPos);
                droppedLocalFile = true;
                ++localDropIndex;
                qDebug() << "[DND][CanvasWidget] dropped local audio:" << localPath;
                continue;
            }

            if (CanvasLauncher::isLauncherCandidate(localPath)) {
                if (addLauncher(localPath, itemPos) != nullptr) {
                    droppedLocalFile = true;
                    ++localDropIndex;
                    qDebug() << "[DND][CanvasWidget] dropped launcher:" << localPath;
                    continue;
                }
            }

            qDebug() << "[DND][CanvasWidget] unsupported local file after drop:" << localPath;
        }
    }
    if (droppedLocalFile)
        return true;

    bool droppedImage = false;
    bool handledByPlugin = false;
    int webLinkIndex = 0;
    if (mime->hasUrls())
        for (const QUrl &u:mime->urls()) {
            const QString localPath = u.toLocalFile();
            if (localPath.isEmpty()) {
                // 2026-07-12: isLikelyImageUrl()単体ではなくresolveDirectImageUrl()を
                // 使う。Google画像検索のimgres?imgurl=...ラッパーリンクのように、
                // URL自体は拡張子を持たないが中に実画像URLを含むケースを展開するため。
                const QString resolvedUrl = WebImageUrlResolver::resolveDirectImageUrl(u.toString());
                if (!resolvedUrl.isEmpty()) {
                    // 以前はここでログを出すだけで何もしていなかった
                    // （画像URL判定自体は既に動いていたが、実際のダウンロード/
                    // 表示処理が未接続だった）。loadRemoteImageUrl()はキャッシュ
                    // 確認→無ければ非同期ダウンロードを開始する。実際の追加は
                    // 非同期完了後だが、他のドロップ経路（プラグイン解決等）と
                    // 同様、ここでは「処理を開始できたか」でdroppedImageを立てる。
                    loadRemoteImageUrl(resolvedUrl, cPos, 0.5, false);
                    droppedImage = true;
                    qDebug() << "[DND][CanvasWidget] web image URL dropped, fetching:" << resolvedUrl;
                } else if (tryAddPluginMedia(u.toString(), cPos)) {
                    // Phase 8: Plugin API — 対応プラグインがあれば処理を委譲。
                    // （プラグインを先に見るので、動画サイトの扱いは従来と変わらない）
                    handledByPlugin = true;
                } else if (WebVideoCache::isDirectVideoUrl(u.toString(QUrl::FullyEncoded))) {
                    // 2026-10-12: 動画ファイルの直リンク（.mp4 等）は本体で再生する。
                    const QPointF videoPos = cPos + QPointF(webLinkIndex * 28.0, webLinkIndex * 28.0);
                    if (addWebVideoUrl(u.toString(QUrl::FullyEncoded), videoPos) != nullptr) {
                        ++webLinkIndex;
                        handledByPlugin = true;
                    }
                } else if (CanvasLauncher::isWebLinkCandidate(u.toString(QUrl::FullyEncoded))) {
                    // 2026-10-11: 画像でも動画でもない http(s) URL はWebリンクバブルにする。
                    // 動画サイト/動画ファイル直リンクは isWebLinkCandidate() で除外済み。
                    // 2026-10-12: 拡張子の無い動画URLもあるので、中身を確かめてから決める。
                    const QPointF linkPos = cPos + QPointF(webLinkIndex * 28.0, webLinkIndex * 28.0);
                    addWebLinkOrProbedVideo(u.toString(QUrl::FullyEncoded), linkPos);
                    ++webLinkIndex;
                    handledByPlugin = true;
                } else {
                    qDebug() << "[DND][CanvasWidget] skip non-local image URL:" << u;
                }
                continue;
            }
            QPixmap pix(localPath);
            if (isLikelyLocalImageFile(localPath) && !pix.isNull()) {
                double sc=0.5;
                auto *image = new CanvasImage(pix,
                    cPos-QPointF(pix.width()*sc/2,pix.height()*sc/2), sc);
                image->setMediaSource(mediaSourceFromLocalFile(localPath, MediaKind::LocalImage));
                m_items.append(image);
                startOverlapPushAnimation(image);
                droppedImage = true;
            } else {
                qDebug() << "[DND][CanvasWidget] URL was not a loadable image:" << u;
            }
        }

    // Phase 8: Plugin API — URLがテキストとしてドロップ/ペーストされた場合
    // （hasUrls()を持たないブラウザ等からのドラッグ）のフォールバック。
    // 2026-07-12: Discordのリンクをテキストとしてドラッグ/コピペした場合も
    // ここを通るため、プラグイン判定だけでなく画像URL判定も試す。
    if (!droppedImage && !handledByPlugin && !mime->hasUrls() && mime->hasText()) {
        // 2026-10-11: 複数行に並べたURLは1行ずつ処理する。"www."始まりは https:// を補う。
        // 全ての非空行がURLでなければ（普通の文章など）何もしない。
        QStringList lines;
        const QStringList rawLines = mime->text().split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                                         Qt::SkipEmptyParts);
        for (const QString &raw : rawLines) {
            QString line = raw.trimmed();
            if (line.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
                line.prepend(QStringLiteral("https://"));
            lines << line;
        }
        static const QRegularExpression ws(QStringLiteral("\\s"));
        bool allUrls = !lines.isEmpty();
        for (const QString &line : std::as_const(lines)) {
            const QUrl lineUrl(line);
            if (!lineUrl.isValid() || lineUrl.scheme().isEmpty() || line.contains(ws)) {
                allUrls = false;
                break;
            }
        }
        if (allUrls) {
            int textIndex = 0;
            for (const QString &line : std::as_const(lines)) {
                const QPointF itemPos = cPos + QPointF(textIndex * 28.0, textIndex * 28.0);
                const QString resolvedUrl = WebImageUrlResolver::resolveDirectImageUrl(line);
                if (!resolvedUrl.isEmpty()) {
                    loadRemoteImageUrl(resolvedUrl, itemPos, 0.5, false);
                    droppedImage = true;
                    ++textIndex;
                    qDebug() << "[DND][CanvasWidget] web image URL (text drop) fetching:" << resolvedUrl;
                } else if (tryAddPluginMedia(line, itemPos)) {
                    handledByPlugin = true;
                    ++textIndex;
                } else if (WebVideoCache::isDirectVideoUrl(line)) {
                    // 2026-10-12: 動画ファイルの直リンクをテキストで貼った場合。
                    if (addWebVideoUrl(QUrl(line).toString(QUrl::FullyEncoded), itemPos) != nullptr) {
                        handledByPlugin = true;
                        ++textIndex;
                    }
                } else if (CanvasLauncher::isWebLinkCandidate(line)) {
                    // 2026-10-11: URLをテキストでコピー→ペースト/ドロップした場合もWebリンクに。
                    // 2026-10-12: 拡張子の無い動画URLもあるので、中身を確かめてから決める。
                    addWebLinkOrProbedVideo(line, itemPos);
                    handledByPlugin = true;
                    ++textIndex;
                }
            }
        }
    }

    if (!droppedImage && !handledByPlugin)
        qDebug() << "[DND][CanvasWidget] unsupported drop: no media URL or loadable image found";
    update();
    return droppedImage || handledByPlugin;
}

void CanvasWidget::dragEnterEvent(QDragEnterEvent *event)
{
    // このCanvasから始まった逆輸入ドラッグをPB自身へ戻しても、取り込み直して
    // バブルを複製しない。ignore()により、そのままドラッグのキャンセルになる。
    if (event->source() == this) {
        qDebug() << "[BubbleExport] ignoring drag returned to its source canvas";
        event->ignore();
        return;
    }

    // バブルが全画面（フォーカス）表示中は、ドラッグ中のカーソルに禁止マーク
    // （🚫）を出すため、ここでは accept() せずに ignore() する。
    // mimeの内容自体が非対応の場合は、従来どおりドラッグ段階では
    // permissiveにacceptし、実際の可否はdropEvent側で判定する。
    if (isFocusDropBlocked()) {
        // 🚫誤発火の切り分け用に、どのアイテムをフォーカス中と認識して
        // ブロックしたかを必ず残す（このログが出ずに🚫になる場合は
        // フォーカス仕様ではなくOLE/ネイティブ側の問題）。
        logDropMimeData("dragEnterEvent", event->mimeData(), false,
                         QStringLiteral("a bubble is focused/fullscreen; show forbidden cursor (focused=%1 pending=%2)")
                             .arg(reinterpret_cast<quintptr>(m_focusedItem), 0, 16)
                             .arg(reinterpret_cast<quintptr>(m_pendingFocusItem), 0, 16));
        event->ignore();
        return;
    }
    QString reason;
    const bool supported = canAcceptDropMimeData(event->mimeData(), &reason);
    if (!supported)
        reason = QStringLiteral("permissive drag-stage accept; original reason: %1").arg(reason);
    logDropMimeData("dragEnterEvent", event->mimeData(), true, reason);
    CanvasDropHandler::forceAcceptDropEvent(event);
}

void CanvasWidget::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->source() == this) {
        event->ignore();
        return;
    }

    if (isFocusDropBlocked()) {
        logDropMimeData("dragMoveEvent", event->mimeData(), false,
                         QStringLiteral("a bubble is focused/fullscreen; show forbidden cursor"));
        event->ignore();
        return;
    }
    QString reason;
    const bool supported = canAcceptDropMimeData(event->mimeData(), &reason);
    if (!supported)
        reason = QStringLiteral("permissive drag-stage accept; original reason: %1").arg(reason);
    logDropMimeData("dragMoveEvent", event->mimeData(), true, reason);
    CanvasDropHandler::forceAcceptDropEvent(event);
}

// 2026-09-20: クリップボードの中身をドロップと同じ経路で取り込む。
// カーソルがPBの上ならその位置、そうでなければ画面中央に置く。
bool CanvasWidget::pasteFromClipboard()
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime) {
        qDebug() << "[Paste][CanvasWidget] clipboard is empty";
        return false;
    }
    QPoint local;
    const QPointF widgetPos = cursorOverCanvas(&local)
        ? QPointF(local)
        : QPointF(width() / 2.0, height() / 2.0);
    // handleDropMimeData() は同期で mime を読み切る（画像URLの取得は文字列を
    // 控えてから非同期に行う）ので、クリップボードの mimeData をそのまま渡してよい。
    const bool pasted = handleDropMimeData(mime, widgetPos, QStringLiteral("Paste"));
    qDebug() << "[Paste][CanvasWidget] pasted=" << pasted << "at" << widgetPos;
    return pasted;
}

void CanvasWidget::dropEvent(QDropEvent *event)
{
    if (event->source() == this) {
        qDebug() << "[BubbleExport] self-drop cancelled; no bubble imported";
        event->ignore();
        return;
    }

    const bool dropped = handleDropMimeData(event->mimeData(), event->position(), QStringLiteral("CanvasWidget"));
    if (!dropped)
        qDebug() << "[DND][CanvasWidget] drop accepted by event, but content is unsupported";
    CanvasDropHandler::forceAcceptDropEvent(event);
}

void CanvasWidget::handleKeyPress(QKeyEvent *event)
{
    // 2026-09-20: Shiftを押した瞬間に逆輸入ヒントを出す（離したときや
    // PBが非アクティブなときはポーリング側が拾う）。消費はしない。
    if (event->key() == Qt::Key_Shift)
        updateExportHint();

    // 2026-09-17: 元に戻す / やり直し / バブル削除。キーは島の設定パネル「操作」で
    // 変更できる（既定: Ctrl+Z / Ctrl+Y・Ctrl+Shift+Z / Delete・Ctrl+X）。
    // 他のキー操作より先に見る（ユーザーが割り当てたキーを優先する）。
    // 元に戻す/やり直しは押しっぱなしのキーリピートでも続けて効かせるが、
    // 削除はリピートで次々消えないよう最初の1回だけにする。
    if (matchesCanvasShortcut(event, QStringLiteral("undo"))
        || matchesCanvasShortcut(event, QStringLiteral("undoAlt"))) {
        undo();
        event->accept();
        return;
    }
    if (matchesCanvasShortcut(event, QStringLiteral("redo"))
        || matchesCanvasShortcut(event, QStringLiteral("redoAlt"))) {
        redo();
        event->accept();
        return;
    }
    if (matchesCanvasShortcut(event, QStringLiteral("delete"))
        || matchesCanvasShortcut(event, QStringLiteral("deleteAlt"))) {
        if (!event->isAutoRepeat())
            deleteSelectedBubbles();
        event->accept();
        return;
    }

    // 2026-10-12: Shift+L でライト固定の切替（カメラを動かしても光が動かない）。
    if (event->key() == Qt::Key_L && (event->modifiers() & Qt::ShiftModifier)
        && !event->isAutoRepeat()) {
        m_lightLocked = !m_lightLocked;
        qDebug("[Glass] light lock=%d", m_lightLocked ? 1 : 0);
        update();
        event->accept();
        return;
    }
    // 2026-10-12: Shift+I でガラスのまとめ描き（インスタンシング）の切替。計測の比較用。
    if (event->key() == Qt::Key_I && (event->modifiers() & Qt::ShiftModifier)
        && !event->isAutoRepeat()) {
        m_glassHighlight.setBatchingEnabled(!m_glassHighlight.batchingEnabled());
        qDebug("[Glass] batching=%d", m_glassHighlight.batchingEnabled() ? 1 : 0);
        update();
        event->accept();
        return;
    }
    // 2026-10-12: Shift+K で下地の焼き付け（BubbleLayerCache）の切替。計測の比較用。
    if (event->key() == Qt::Key_K && (event->modifiers() & Qt::ShiftModifier)
        && !event->isAutoRepeat()) {
        m_bubbleLayers.setEnabled(!m_bubbleLayers.isEnabled());
        qDebug("[BubbleLayers] enabled=%d", m_bubbleLayers.isEnabled() ? 1 : 0);
        update();
        event->accept();
        return;
    }

    // 2026-10-12: Shift+P で区間ごとのGPU時間・描画回数を左上に表示（GpuProfiler）。
    if (event->key() == Qt::Key_P && (event->modifiers() & Qt::ShiftModifier)
        && !event->isAutoRepeat()) {
        m_perfOverlay = !m_perfOverlay;
        update();
        event->accept();
        return;
    }

    // 2026-07-19: デバッグ用。Shift+Dでガラスのデバッグ表示をサイクル:
    //   0=通常 → 1=実効サーフェス法線 → 2=屈折レイヤー単体 → 0…
    if (event->key() == Qt::Key_D && (event->modifiers() & Qt::ShiftModifier)
        && !event->isAutoRepeat()) {
        m_glassDebugMode = (m_glassDebugMode + 1) % 3;
        qDebug("[GlassDebug] mode=%d (0=normal,1=normalmap,2=refraction-only)",
               m_glassDebugMode);
        update();
        event->accept();
        return;
    }

    // Space で再生/一時停止、左右矢印キーで5秒巻き戻し/早送り。
    // 2026-07-16: 以前は全画面（フォーカス）表示中のバブルにしか効かず、
    // キャンバス上の通常表示バブルでは矢印キーシークの経路自体が
    // 存在しなかった（フォーカス内外で対象を分けていた）。フォーカス
    // 状態によって効いたり効かなかったりする分岐そのものを撤廃し、
    // フォーカス中のバブルが無ければ m_activeItem（キャンバス上で
    // 直近クリック/選択中の単体アイテム。S/Q/C キーと同じ対象）に
    // フォールバックすることで、どこでも同じ経路でシークできるように
    // した。
    // なお、フォーカスイン遷移中（ウィンドウリサイズアニメ完了までの
    // 約450ms）はm_focusedItemがまだnullで、確定待ちの対象は
    // m_pendingFocusItemに入っている（focusItem()参照）。resizeGL()の
    // focusTargetItemと同じ規則でpending側も優先する。
    CanvasItem *seekTarget = m_pendingFocusItem ? m_pendingFocusItem
                            : (m_focusedItem ? m_focusedItem : m_activeItem);
    if (auto *fv = canvasItemAsVideo(seekTarget)) {
        if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
            fv->togglePlayPause();
            event->accept(); return;
        }
        // 2026-07-16: クリック直後(ダブルクリック判定の280ms待ち～mpvが
        // 実際にファイルを開き終えるまで)はプレイヤーがまだ無い/未オープン
        // で、矢印キーシークはBubbleController::seek()側で無言のno-opに
        // なる。そもそも「再生が始まってすらいない動画をシークする」と
        // いう操作自体が意味を持たず、ほぼ誤操作(ミスタップ)でしかない
        // ため、その状態では最初からキー入力を受け付けない(シーク
        // インジケータも出さない)ようにする。Idle/Loading/Sleep/Error
        // はプレイヤーが未接続か既に解放済みの状態。
        const BubbleState st = fv->bubbleState();
        const bool seekable = st != BubbleState::Idle
                            && st != BubbleState::Loading
                            && st != BubbleState::Sleep
                            && st != BubbleState::Error;
        if (seekable) {
            // 2026-09-11: Spaceと違いここは!isAutoRepeat()が無かったため、
            // 矢印キーを押しっぱなしにするとOSのキーリピート間隔(数十ms毎)で
            // 毎回bcSeek()を叩きつつshowSeekIndicator()でフェードアニメを
            // alpha=1.0へ強制リセットし続けていた。フェード(550ms)よりずっと
            // 速い間隔でリセットが連打されるため、インジケータが
            // 「がくがくと点滅」して見え、動画側もシークの連打で不安定に
            // なっていた。押しっぱなし＝リピートは無視し、実際のキー入力
            // (最初の1回)だけシークする。
            if (event->key() == Qt::Key_Left) {
                if (!event->isAutoRepeat()) {
                    qint64 pos = fv->position() - 5000;
                    if (pos < 0) pos = 0;
                    fv->bcSeek(pos);
                    showSeekIndicator(-1);
                }
                event->accept(); return;
            }
            if (event->key() == Qt::Key_Right) {
                if (!event->isAutoRepeat()) {
                    qint64 pos = fv->position() + 5000;
                    const qint64 dur = fv->duration();
                    if (dur > 0 && pos > dur) pos = dur;
                    fv->bcSeek(pos);
                    showSeekIndicator(+1);
                }
                event->accept(); return;
            }
        }
    }

    // 2026-09-14: サラウンドモードを廃止。S キー（グローバルON/OFF）と
    // Q キー（アクティブバブルの個別除外）のハンドラを削除した。
    // どちらのキーも現在は未割り当て。

    if (event->key()==Qt::Key_C&&(event->modifiers()&Qt::ControlModifier)
        && (m_activeItem || m_selectedItems.size() > 1)) {
        // 2026-09-20: 範囲選択中は選択しているバブル全部をコピーする。
        if (m_selectedItems.size() > 1) {
            QSet<CanvasItem*> selected;
            for (CanvasItem *item : std::as_const(m_selectedItems))
                if (item && m_items.contains(item)) selected.insert(item);
            m_bubbleDragExporter->copyToClipboard(CanvasSelectionHelper::readingOrder(selected));
        } else {
            m_bubbleDragExporter->copyToClipboard(m_activeItem);
        }
        event->accept(); return;
    }

    // 2026-09-20: Ctrl+V — PB外でコピーしたもの（スクショ・画像・URL・ファイル）を
    // バブルとして貼り付ける。
    if (event->key()==Qt::Key_V
        && (event->modifiers() & (Qt::ControlModifier|Qt::ShiftModifier|Qt::AltModifier)) == Qt::ControlModifier) {
        if (!event->isAutoRepeat())
            pasteFromClipboard();
        event->accept(); return;
    }

    // 2026-07-12: C（迷子救済機能）— キャンバスを大きくパン/フリックして
    // 見失った時のために、全バブルの座標平均点へカメラを移動する。
    // 移動後にちょうど全バブルが収まる（少し余白を持たせた）ズームに
    // 合わせる。フォーカス中はカメラがそのバブル専用の挙動になっている
    // ため対象外にする。通常キャンバスの慣性が残っていると、今から
    // animateCamera()でセットするカメラ位置と奪い合ってガクつくため
    // 先に止めておく（focusItem()で慣性を止めているのと同じ理由）。
    if (event->key()==Qt::Key_C && !event->isAutoRepeat()
            && !(event->modifiers() & Qt::ControlModifier)
            && !m_focusedItem && !m_pendingFocusItem && !m_items.isEmpty()) {
        QPointF sum(0, 0);
        QRectF bbox;
        bool first = true;
        for (CanvasItem *item : m_items) {
            if (!item)
                continue;
            const QRectF ir = item->itemRect();
            sum += ir.center();
            bbox = first ? ir : bbox.united(ir);
            first = false;
        }
        if (!first) {
            const QPointF avgCenter = sum / static_cast<double>(m_items.size());
            // 全バブルがぴったり収まる余白ゼロのズームだと窮屈なので、
            // 一回り引いた見え方になるよう少しだけ余白を持たせる
            // （focusItem()系のfit計算と同じqMinの考え方: 縦横どちらか
            // 狭い方に合わせて全体が収まるようにする）。
            constexpr double kFitMargin = 1.15;
            const double zoomW = width()  / (bbox.width()  * kFitMargin);
            const double zoomH = height() / (bbox.height() * kFitMargin);
            const double targetZoom = qBound(0.01, qMin(zoomW, zoomH), 50.0);
            m_canvasPanInertia->stop();
            animateCamera(avgCenter, targetZoom, 400);
        }
        event->accept(); return;
    }

    event->ignore();
}

void CanvasWidget::contextMenuEvent(QContextMenuEvent *event)
{
    // 2026-07-08: 右クリックは「バブルをアクティブにする専用ボタン」
    // （handleMousePress/handleMouseRelease側のQt::RightButton分岐）に
    // 用途変更したため、ここで独自メニュー（旧・画像を追加...）は出さない。
    // OSのデフォルトコンテキストメニューも不要なので無視する。
    event->ignore();
}
