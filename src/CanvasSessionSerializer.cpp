#include "CanvasWidget.h"
#include "CanvasImage.h"
#include "CanvasLauncher.h"
#include "CameraPanInertia.h"
#include "MainWindow.h"

#include <QBuffer>
#include <QCache>
#include <QDebug>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPixmap>
#include <QStringList>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#include <ole2.h>
#endif

namespace {

// 2026-07-11: レイヤー切り替えのたびにrestoreSessionFromJson()が
// ローカル画像/動画サムネイルを毎回ゼロからデコードし直していたのが
// 体感の重さの主因の一つだったため、プロセス内キャッシュを追加する。
// 「同じレイヤーへ何度も出入りする」使い方が主なので、2回目以降の
// 訪問ではデコードをまるごと省略できる。ファイルパスをキーにする
// 素朴な方式で、ファイル内容が実行中に変わるケースは想定しない
// （キャッシュ無効化ロジックは持たせない。最初の一手として最小限）。

// ローカル画像バブル用。フルサイズのQPixmapをパスごとにキャッシュする。
QPixmap cachedLocalImagePixmap(const QString &path)
{
    // CanvasImage itself keeps the native-resolution pixmap while it is displayed.
    // This cache only avoids decoding the same file again when revisiting a layer,
    // so evicting its oldest references does not alter image quality or source data.
    constexpr int kCacheLimitKiB = 128 * 1024;
    static QCache<QString, QPixmap> cache(kCacheLimitKiB);
    if (QPixmap *cached = cache.object(path))
        return *cached;

    QPixmap pix(path);
    if (!pix.isNull()) {
        const qint64 bytes = qint64(pix.width()) * qint64(pix.height())
                           * qMax(1, pix.depth()) / 8;
        const int costKiB = static_cast<int>(
            qMin<qint64>(kCacheLimitKiB + 1LL,
                         qMax<qint64>(1, (bytes + 1023) / 1024)));
        cache.insert(path, new QPixmap(pix), costKiB);
    }
    return pix;
}

// 動画バブルのサムネイル用。session_N.jsonに埋め込まれたbase64文字列を
// キーに、PNGデコード直後（ダウンスケール・フォーマット変換前）の
// QImageをキャッシュする。CanvasVideo::setRestoredThumbnailImage()側の
// ダウンスケール/convertToFormat自体は都度呼ぶが、これはCOWで安価な上
// 既に正しいフォーマットなら早期リターンするため、重いのは専ら
// PNGデコード（loadFromData）の方だけをここで省略する。
QImage cachedDecodedThumbnail(const QString &base64)
{
    constexpr int kCacheLimitKiB = 64 * 1024;
    static QCache<QString, QImage> cache(kCacheLimitKiB);
    if (QImage *cached = cache.object(base64))
        return *cached;
    QImage decoded;
    const QByteArray bytes = QByteArray::fromBase64(base64.toLatin1());
    if (!bytes.isEmpty())
        decoded.loadFromData(bytes);
    if (!decoded.isNull()) {
        const qint64 bytesCost = decoded.sizeInBytes();
        const int costKiB = static_cast<int>(
            qMin<qint64>(kCacheLimitKiB + 1LL,
                         qMax<qint64>(1, (bytesCost + 1023) / 1024)));
        cache.insert(base64, new QImage(decoded), costKiB);
    }
    return decoded;
}

bool isSessionLocalFileUrlString(const QString &url)
{
    return QUrl(url).isLocalFile();
}

MediaSource sessionMediaSourceFromLocalFile(const QString &path, MediaKind kind)
{
    MediaSource source;
    source.kind = kind;
    source.originalPathOrUrl = QUrl::fromLocalFile(path).toString();
    source.title = QFileInfo(path).fileName();
    return source;
}

} // namespace
// ============================================================
// 2026-09-17: Undo/Redo で削除したバブルを作り直すため、1アイテム分の
// 書き出しを sessionToJson() から切り出した（中身は従来のループ本体そのまま）。
// 保存対象外のアイテムは空オブジェクトを返す。
QJsonObject CanvasWidget::sessionEntryForItem(CanvasItem *item) const
{
    QJsonObject entry;
    entry.insert(QStringLiteral("x"), item->pos().x());
    entry.insert(QStringLiteral("y"), item->pos().y());
    entry.insert(QStringLiteral("scale"), item->scale());
    const QRectF rect = item->itemRect();
    entry.insert(QStringLiteral("width"), rect.width());
    entry.insert(QStringLiteral("height"), rect.height());

    if (auto *video = canvasItemAsVideo(item)) {
        // Phase 8: Plugin API — PluginMedia は originalPathOrUrl が
        // ローカルファイルではなく元ページURL（例: 動画サイトのページURL）のため、
        // ローカルファイル判定の対象外として保存を許可する。
        // 保存するのは「元ページURL」のみで、解決済みストリームURLや
        // HTTPヘッダーは保存しない（一時的な値であり、次回はプラグインに
        // 再解決させる設計のため）。
        const bool isPluginMedia = video->mediaKind() == MediaKind::PluginMedia;
        if (!isPluginMedia && !isSessionLocalFileUrlString(video->sourceUrl()))
            return QJsonObject();
        entry.insert(QStringLiteral("type"), QStringLiteral("video"));
        QJsonObject videoJson;
        // ---- MediaSource 中心の共通項目 ----
        videoJson.insert(QStringLiteral("kind"), mediaKindToString(video->mediaKind()));
        videoJson.insert(QStringLiteral("sourceUrl"), video->sourceUrl());
        videoJson.insert(QStringLiteral("title"), video->title());
        // どのプラグインが解決したかの任意ヒント（本体は分岐に使わない。
        // 復元時に対応プラグインが見つからなかった場合のエラー表示用）。
        if (isPluginMedia && !video->resolverPluginId().isEmpty())
            videoJson.insert(QStringLiteral("pluginId"), video->resolverPluginId());
        // ---- 動画固有の再生状態 ----
        // 音声バブル（LocalAudio）は再生位置を保存しない（常に最初から
        // 再生する方針のため）。動画バブルのみ保存する。
        //
        // 【2026-07-03追加】バブルが最後まで再生し終わっている
        // （BubbleState::Ended）場合は、position()がduration付近の値の
        // ままになっている。これをそのまま保存すると、次回復元時に
        // mpvの"start="としてほぼ終端の位置が渡され、開いた瞬間に
        // 再度EOFへ達してEndedへ戻ってしまい、何度再生しても実質何も
        // 再生されないまま終わる（＝再生開始できないように見える）
        // 不具合になる。Ended状態のバブルは0として保存し、次回は
        // 停止サムネイル表示（先頭）から復元されるようにする。
        if (video->mediaKind() != MediaKind::LocalAudio) {
            const bool hasEnded = video->bubbleState() == BubbleState::Ended;
            // 2026-07-11 根本原因調査で発見: video->position()は
            // 「今まさにmpvへロードされている位置」しか返さない
            // （BubbleController::position() → m_player->position()）。
            // セッション復元直後、まだ一度もクリックして再生していない
            // バブル（restoreLazy()で受け取ったplaybackPositionを
            // m_resumePositionMsに保持したまま、実際のmpvロードは
            // 未着手＝m_mpvPlayerがまだ存在しない）はposition()が
            // 常に0を返す。この値をそのまま保存すると、レイヤーを
            // 切り替えるだけで「一度も触っていない」バブルの再生位置が
            // 問答無用で0に上書きされてしまっていた
            // （＝ユーザー報告の「動画再生位置がリセットされる」の
            // 主因と思われる。BubbleController::position()側で
            // 対応したdeep focus sleep/一時停止アイドル解放のケースより
            // こちらの方がずっと頻発する）。
            // CanvasVideo::savedResumePosition()（m_resumePositionMsの
            // getter。従来は宣言されているだけで未使用だった）を
            // フォールバックとして使い、「一度も再生されておらず
            // position()が0を返しているだけ」の場合は、復元時点の
            // 元の値をそのまま保存し直す。実際に0秒地点から再生された
            // 場合もm_resumePositionMsは消費時に0へ戻るため、
            // その場合は素直に0のまま保存され、区別を誤ることはない。
            qint64 posMs = video->position();
            if (posMs <= 0)
                posMs = video->savedResumePosition();
            videoJson.insert(QStringLiteral("playbackPosition"),
                              hasEnded ? 0.0 : posMs / 1000.0);
        }
        videoJson.insert(QStringLiteral("volume"), video->volume());
        // 保存時点で実際に再生中だったかどうか。
        // 復元時はセッション全体を安全のため一時停止状態でロードするため
        // 現状は参照されないが、将来の用途（UI表示・Plugin API等）のため保存対象とする。
        videoJson.insert(QStringLiteral("paused"), !video->isPlaying());
        videoJson.insert(QStringLiteral("thumbnailUrl"), video->thumbnailUrl());
        // 2026-07-11: PNGエンコードはCanvasVideo::thumbnailPngBase64ForSession()
        // 側でキャッシュされている（復元時に種を仕込んでおくため、
        // サムネイルが復元後に変わっていなければ再エンコードなしで
        // 済む。詳細はCanvasVideo.h参照）。
        // Provider media normally returns an empty payload. During
        // one-time migration from an old session, however, CanvasVideo
        // retains the old Base64 as an offline fallback until the provider
        // has successfully populated its own cache.
        const QString thumbBase64ForSave =
            video->thumbnailPngBase64ForSession();
        if (!thumbBase64ForSave.isEmpty())
            videoJson.insert(QStringLiteral("thumbnailPngBase64"), thumbBase64ForSave);
        // 2026-09-12: ラベルのリスト（前後に並ぶ項目の列）のスナップ
        // ショットと、ユーザーが手で決めた並び順。次回起動時に通信ゼロで
        // 即座にリストを出すために持つ。保存するのはプラグインが言った
        // こと（url / タイトル / 長さ / サムネURL）だけで、サムネイル
        // 画像そのものは保存しない。約50件で10KB程度。
        //
        // 2026-09-23: 本体のリスト（バブル融合で作ったもの）。並び全体と今の
        // 位置を持つ。今の動画は上の sourceUrl 等にもそのまま入っているので、
        // この項目を知らない古い版で開いても「今の動画1本」として開ける。
        // 本体のリストがあるバブルの一覧は本体の並びが優先なので、プラグインの
        // 一覧（panelSnapshot）は保存しない。
        // 2026-09-23: プラグインのラベル。再生（＝解決）する前から Solo / List /
        // Mix / Live を出せるように、最後の答えをそのまま覚えておく。
        const QJsonObject pluginLabel = video->pluginLabelToJson();
        if (!pluginLabel.isEmpty())
            videoJson.insert(QStringLiteral("pluginLabel"), pluginLabel);
        const QJsonObject coreList = video->coreListToJson();
        if (!coreList.isEmpty()) {
            videoJson.insert(QStringLiteral("coreList"), coreList);
        } else {
            const QJsonObject panelSnapshot = video->panelSnapshotToJson();
            if (!panelSnapshot.isEmpty())
                videoJson.insert(QStringLiteral("panelSnapshot"), panelSnapshot);
        }
        entry.insert(QStringLiteral("video"), videoJson);
    } else if (auto *image = dynamic_cast<CanvasImage*>(item)) {
        entry.insert(QStringLiteral("type"), QStringLiteral("image"));
        QJsonObject imageJson;
        // ---- MediaSource 中心の共通項目 ----
        imageJson.insert(QStringLiteral("kind"), mediaKindToString(image->mediaKind()));
        imageJson.insert(QStringLiteral("sourceUrl"), image->sourceUrl());
        imageJson.insert(QStringLiteral("title"), image->title());
        imageJson.insert(QStringLiteral("loadFailed"), image->loadFailed());
        entry.insert(QStringLiteral("image"), imageJson);
    } else if (auto *launcher = dynamic_cast<CanvasLauncher*>(item)) {
        if (!launcher->isLaunchable())
            return QJsonObject();
        entry.insert(QStringLiteral("type"), QStringLiteral("launcher"));
        QJsonObject launcherJson;
        launcherJson.insert(QStringLiteral("path"), launcher->path());
        switch (launcher->targetKind()) {
        case CanvasLauncher::TargetKind::Directory:
            launcherJson.insert(QStringLiteral("kind"), QStringLiteral("directory"));
            break;
        case CanvasLauncher::TargetKind::Shortcut:
            launcherJson.insert(QStringLiteral("kind"), QStringLiteral("shortcut"));
            break;
        case CanvasLauncher::TargetKind::Web:
            launcherJson.insert(QStringLiteral("kind"), QStringLiteral("web"));
            break;
        case CanvasLauncher::TargetKind::File:
        default:
            launcherJson.insert(QStringLiteral("kind"), QStringLiteral("file"));
            break;
        }
        entry.insert(QStringLiteral("launcher"), launcherJson);
    } else {
        entry.insert(QStringLiteral("type"), QStringLiteral("unknown"));
    }

    return entry;
}

// 2026-09-17: Undo/Redo で削除したバブルを作り直すため、1エントリ分の
// 復元を restoreSessionFromJson() から切り出した（中身は従来のループ本体そのまま）。
// 作ったアイテムを返す。Web画像でキャッシュが無く非同期取得になった場合は
// nullptr を返し、取得後に onAsyncCreated を呼ぶ。
CanvasItem *CanvasWidget::createItemFromSessionEntry(
    const QJsonObject &entry,
    const std::function<void(CanvasItem*)> &onAsyncCreated)
{
    const QString type = entry.value(QStringLiteral("type")).toString();
    const QPointF pos(entry.value(QStringLiteral("x")).toDouble(),
                      entry.value(QStringLiteral("y")).toDouble());
    const double scale = entry.value(QStringLiteral("scale")).toDouble(1.0);
    const double savedWidth =
        entry.value(QStringLiteral("width")).toDouble();
    const double savedHeight =
        entry.value(QStringLiteral("height")).toDouble();
    const bool hasSavedSize = savedWidth > 0.0 && savedHeight > 0.0;
    const QPointF savedCenter =
        pos + QPointF(savedWidth * 0.5, savedHeight * 0.5);

    if (type == QStringLiteral("video")) {
        const QJsonObject videoJson = entry.value(QStringLiteral("video")).toObject();
        // sourceUrl が Phase 6 以降の正式キー。youtubeUrl は旧形式の読み込み互換のみ。
        const QString savedUrl = videoJson.value(QStringLiteral("sourceUrl")).toString(
            videoJson.value(QStringLiteral("youtubeUrl")).toString());
        MediaKind kind = mediaKindFromString(videoJson.value(QStringLiteral("kind")).toString());
        if (kind == MediaKind::Unknown)
            kind = MediaKind::LocalVideo;
        // Phase 8: Plugin API — PluginMedia は元ページURL（非ローカル）で
        // 保存されているのが正常なので、ローカルファイル判定の対象外とする。
        // 2026-10-12: WebVideo も http(s) のURLで保存されているのが正常。
        if (kind != MediaKind::PluginMedia && kind != MediaKind::WebVideo
            && !isSessionLocalFileUrlString(savedUrl))
            return nullptr;
        // playbackPosition が Phase 6 以降の正式キー。positionSeconds は旧形式の読み込み互換のみ。
        // 音声バブル（LocalAudio）は常に最初から再生する方針のため、
        // 古いセッションファイルに再生位置が残っていても無視する。
        const double playbackPositionSeconds = (kind == MediaKind::LocalAudio)
            ? 0.0
            : (videoJson.contains(QStringLiteral("playbackPosition"))
                   ? videoJson.value(QStringLiteral("playbackPosition")).toDouble()
                   : videoJson.value(QStringLiteral("positionSeconds")).toDouble());
        // NOTE: paused は保存対象だが、セッション全体は安全のため常に一時停止状態
        // （遅延ロード）で復元する方針のため、復元時には意図的に参照しない。
        auto *video = new CanvasVideo(this);
        // 復元直後のバブルにも、（この直前に復元済みの）マスター
        // ボリュームを即座に効かせる（initializeMpv()より前でよい）。
        video->setMasterGain(m_masterVolume.gain(), false);
        video->setPos(pos);
        video->restoreLazy(
            savedUrl,
            videoJson.value(QStringLiteral("thumbnailUrl")).toString(),
            static_cast<qint64>(playbackPositionSeconds * 1000.0),
            static_cast<float>(videoJson.value(QStringLiteral("volume")).toDouble(1.0)),
            qRound(entry.value(QStringLiteral("width")).toDouble(480.0) / qMax(0.001, scale)),
            qRound(entry.value(QStringLiteral("height")).toDouble(270.0) / qMax(0.001, scale)),
            kind,
            videoJson.value(QStringLiteral("title")).toString(),
            videoJson.value(QStringLiteral("pluginId")).toString());
        // 2026-09-12: ラベルのリストのスナップショットを先に戻しておく。
        // これがあると、復元直後にラベルを開いた時点でプラグインへ問い
        // 合わせる前から一覧が出る（プラグインの答えが返った時点で
        // マージされる）。無い古いセッションでは空のまま＝従来どおり。
        {
            const QJsonObject panelSnapshot =
                videoJson.value(QStringLiteral("panelSnapshot")).toObject();
            if (!panelSnapshot.isEmpty())
                video->restorePanelSnapshotFromJson(panelSnapshot);
        }
        // 2026-09-23: プラグインのラベル（再生前からラベルと一覧を出すため）
        {
            const QJsonObject pluginLabel =
                videoJson.value(QStringLiteral("pluginLabel")).toObject();
            if (!pluginLabel.isEmpty())
                video->restorePluginLabelFromJson(pluginLabel);
        }
        // 2026-09-23: 本体のリスト。restoreLazy() で今の動画が決まった後に戻す
        // （位置の確かめに今の動画のURLを使うため）。
        {
            const QJsonObject coreList =
                videoJson.value(QStringLiteral("coreList")).toObject();
            if (!coreList.isEmpty())
                video->restoreCoreListFromJson(coreList);
        }
        // restoreLazy()で元寸法が確定してから最小短辺を適用する。
        video->setScale(scale);
        if (hasSavedSize)
            video->setCenter(savedCenter);
        // 2026-07-11: PNGデコードはcachedDecodedThumbnail()でキャッシュされる
        // （同じレイヤーへ再訪した際、同じbase64文字列なら再デコードしない）。
        // seedThumbnailPngBase64()で「このbase64から来た」ことを記録しておき、
        // 保存側（thumbnailPngBase64ForSession()）が再エンコードを省略できる
        // ようにする。
        const QString thumbBase64ForRestore =
            videoJson.value(QStringLiteral("thumbnailPngBase64")).toString();
        if (!thumbBase64ForRestore.isEmpty()) {
            const QImage thumbnail = cachedDecodedThumbnail(thumbBase64ForRestore);
            if (!thumbnail.isNull()) {
                if (video->hasThumbnailProvider()) {
                    video->setRestoredProviderThumbnailFallback(
                        thumbnail, thumbBase64ForRestore);
                } else {
                    video->setRestoredThumbnailImage(thumbnail);
                    video->seedThumbnailPngBase64(thumbBase64ForRestore);
                }
            }
        }
        // 2026-07-11: thumbnailPngBase64が埋め込まれていなかった場合
        // （保存時点でまだサムネイル未取得だった等）のフォールバック。
        // restoreLazy()内で自動的に再取得を試みるのはローカル動画/音声の
        // みで、リモート/プラグイン動画（動画サイト等）のサムネイルURLは
        // 復元時に保持されるだけで実際のダウンロードはされない。
        // これによりレイヤー切り替えを跨いだ際、base64キャッシュが
        // 無いリモート動画のサムネイルが永久に表示されなくなっていた。
        // ここでthumbnailUrlが分かっていれば明示的に取得し直す。
        if (!video->hasThumbnailProvider() && video->thumbnailImage().isNull()) {
            const QString restoredThumbUrl =
                videoJson.value(QStringLiteral("thumbnailUrl")).toString();
            if (!restoredThumbUrl.isEmpty())
                video->loadThumbnail(restoredThumbUrl);
        }
        connectVideoRenderSignals(video);
        // 2026-07-18追加: 従来この復元経路ではvideoDimensionsReadyを
        // 接続しておらず、復元バブルは連続再生（nextUrl）で対比の違う
        // 動画へ切り替わっても、フォーカス中のウィンドウ再フィット
        // （refitFocusedWindowForAspectChange）・重なり解消・フレーム
        // 拡張が一切働かなかった。addMediaSource()と共通のヘルパーで
        // 必ず接続する（ヘルパーのコメント参照）。
        connectVideoDimensionSignals(video);
        m_items.append(video);
        return video;
    } else if (type == QStringLiteral("image")) {
        const QJsonObject imageJson = entry.value(QStringLiteral("image")).toObject();
        // sourceType / filePath / url は旧形式の読み込み互換のみ。
        // Phase 6 以降は kind と sourceUrl が正式キー。
        const QString sourceType = imageJson.value(QStringLiteral("sourceType")).toString();
        MediaKind kind = mediaKindFromString(imageJson.value(QStringLiteral("kind")).toString());
        if (kind == MediaKind::Unknown)
            kind = (sourceType == QStringLiteral("web")) ? MediaKind::WebImage : MediaKind::LocalImage;

        if (kind == MediaKind::LocalImage) {
            QString path = QUrl(imageJson.value(QStringLiteral("sourceUrl")).toString()).toLocalFile();
            if (path.isEmpty())
                path = imageJson.value(QStringLiteral("filePath")).toString();
            MediaSource source = sessionMediaSourceFromLocalFile(path, MediaKind::LocalImage);
            source.title = imageJson.value(QStringLiteral("title")).toString(source.title);
            if (path.isEmpty() || !QFileInfo::exists(path)) {
                qWarning() << "[Session] Local image missing:" << path;
                return addImagePlaceholder(
                        source,
                        hasSavedSize ? savedCenter : pos,
                        scale,
                        hasSavedSize);
            }
            // 2026-07-11: cachedLocalImagePixmap()で同一パスの再デコードを
            // 省略する（同じレイヤーへ再訪した際の重さ対策）。
            QPixmap pix = cachedLocalImagePixmap(path);
            if (pix.isNull()) {
                qWarning() << "[Session] Local image failed to load:" << path;
                return addImagePlaceholder(
                        source,
                        hasSavedSize ? savedCenter : pos,
                        scale,
                        hasSavedSize);
            }
            auto *image = new CanvasImage(pix, pos, scale);
            if (hasSavedSize)
                image->setCenter(savedCenter);
            image->setMediaSource(source);
            image->setLoadFailed(imageJson.value(QStringLiteral("loadFailed")).toBool(false));
            m_items.append(image);
            return image;
        } else {
            const QString url = imageJson.value(QStringLiteral("sourceUrl")).toString(
                imageJson.value(QStringLiteral("url")).toString());
            // 2026-07-12: 以前はここでisSessionLikelyImageUrl()（拡張子だけを見る
            // 簡易判定）を通した上でloadRemoteImageUrl()を呼んでいたが、これは
            // loadRemoteImageUrl()側のisLikelyImageUrl()判定と重複している上、
            // より厳しく先に弾いてしまっていた。X(Twitter)のように再取得可能な
            // URLが無く画像内容ハッシュの合成識別子("pocketboard-capture:...")を
            // sourceUrlとして使っているケースは拡張子を持たないため、この重複
            // ゲートのせいでキャッシュに実体があるにもかかわらず復元がスキップ
            // されていた。loadRemoteImageUrl()側は既にキャッシュ確認を拡張子判定
            // より先に行うよう直してあるので、ここでは空文字チェックのみ行い、
            // 実際の判定はloadRemoteImageUrl()に一本化する。
            if (!url.isEmpty()) {
                std::function<void(CanvasImage*)> asyncCallback;
                if (onAsyncCreated)
                    asyncCallback = [onAsyncCreated](CanvasImage *image) { onAsyncCreated(image); };
                return loadRemoteImageUrl(
                    url,
                    hasSavedSize ? savedCenter : pos,
                    scale,
                    !hasSavedSize,
                    asyncCallback);
            }
            qWarning() << "[Session] Web image URL missing, skipped.";
        }
    } else if (type == QStringLiteral("launcher")) {
        const QJsonObject launcherJson = entry.value(QStringLiteral("launcher")).toObject();
        const QString path = launcherJson.value(QStringLiteral("path")).toString();
        if (!CanvasLauncher::validateTarget(path)) {
            qWarning() << "[Session] Launcher target missing, skipped:" << path;
            return nullptr;
        }
        return addLauncher(
                path,
                hasSavedSize ? savedCenter : pos,
                scale,
                hasSavedSize);
    }
    return nullptr;
}

QJsonObject CanvasWidget::sessionToJson() const
{
    QJsonObject canvas;
    QJsonArray items;
    int z = 0;
    for (CanvasItem *item : m_items) {
        const QString itemId = QStringLiteral("item-%1").arg(z);
        QJsonObject entry = sessionEntryForItem(item);
        if (entry.isEmpty())
            continue;
        entry.insert(QStringLiteral("id"), itemId);
        entry.insert(QStringLiteral("zOrder"), z++);
        items.append(entry);
    }

    canvas.insert(QStringLiteral("items"), items);

    QJsonObject cameraJson;
    cameraJson.insert(QStringLiteral("x"), m_cameraPos.x());
    cameraJson.insert(QStringLiteral("y"), m_cameraPos.y());
    cameraJson.insert(QStringLiteral("zoom"), m_cameraZoom);
    canvas.insert(QStringLiteral("camera"), cameraJson);

    // ---- マスターボリューム(0-100) ----
    canvas.insert(QStringLiteral("masterVolume"), m_masterVolume.value());

    // ---- バブルフレーム（グループ） ----
    // 2026-09-23: フレーム機能は本体から外した（old_fn/frames）。読み込んだ
    // ときのフレーム情報（矩形とタイトル）だけをそのまま書き戻し、昔の
    // セッションから消えないようにしておく。Flowのつなぎ線（flowLinks）は
    // 保存しない（Flowも本体から外した。old_fn/flow）。
    if (!m_legacyFramesJson.isEmpty())
        canvas.insert(QStringLiteral("frames"), m_legacyFramesJson);

    return canvas;
}

// Phase 6: 新形式（sourceUrl / playbackPosition 等）を正として読み込みつつ、
// 旧形式（youtubeUrl / filePath / sourceType / positionSeconds）のセッションファイルも
// 引き続き読み込めるよう、各値はフォールバック付きで取得する。
void CanvasWidget::restoreSessionFromJson(const QJsonObject &canvas)
{
    // 2026-09-17: キャンバスの中身がまるごと入れ替わるので、元に戻す/やり直しの
    // 履歴はここで捨てる（旧アイテムのポインタも以下で一括 delete される）。
    // 復元のためのバブル追加は履歴に積まない。
    clearUndoHistory();
    UndoSuppressScope undoSuppress(this);
    // 2026-07-12: レイヤー切り替え（LayerSwitcher::loadLayerIntoCanvas()経由、
    // レイヤー切替/新規作成/削除の全パスがここを通る）の度に必ず呼ばれる
    // ため、ここが「前のレイヤーで走っていた慣性を確実に止める」ための
    // 一番確実な場所になる。これが無いと、あるレイヤーで中ボタンを勢いよく
    // フリックした直後に別レイヤーへ切り替えた場合、これから復元される
    // 新しいm_cameraPos（下で上書きされる）が古いレイヤーの慣性速度で
    // そのまま動き続けてしまい、「切り替えた瞬間に知らない場所へ勝手に
    // パンしていく」という不具合になる。
    stopFocusPanInertia();
    m_canvasPanInertia->stop();

    // カメラ位置・ズームの復元。アイテムが1つも無いセッションでも適用したいので、
    // items が空でも return してしまう下の分岐より前に処理する。
    if (canvas.contains(QStringLiteral("camera"))) {
        const QJsonObject cameraJson = canvas.value(QStringLiteral("camera")).toObject();
        m_cameraPos = QPointF(cameraJson.value(QStringLiteral("x")).toDouble(m_cameraPos.x()),
                              cameraJson.value(QStringLiteral("y")).toDouble(m_cameraPos.y()));
        m_cameraZoom = qBound(0.01, cameraJson.value(QStringLiteral("zoom")).toDouble(m_cameraZoom), 50.0);
        m_preFocusCameraPos  = m_cameraPos;
        m_preFocusCameraZoom = m_cameraZoom;
        updateCanvasFromCamera();
    }

    // マスターボリュームの復元。キーが無い（旧セッションファイル）場合は
    // MasterVolumeControllerの既定値(50)のまま変更しない。
    if (canvas.contains(QStringLiteral("masterVolume"))) {
        m_masterVolume.setValue(canvas.value(QStringLiteral("masterVolume")).toInt(m_masterVolume.value()));
    }

    // ---- バブルフレーム（グループ） ----
    // 2026-09-23: フレーム機能は本体から外した（old_fn/frames）。作らずに
    // 矩形とタイトルだけ保持し、保存時にそのまま書き戻す。members（アイテムの
    // 並び順での番号）はバブルの増減で別のバブルを指してしまうので外す。
    {
        m_legacyFramesJson = QJsonArray();
        const QJsonArray framesJson = canvas.value(QStringLiteral("frames")).toArray();
        for (const QJsonValue &v : framesJson) {
            QJsonObject fj = v.toObject();
            fj.remove(QStringLiteral("members"));
            m_legacyFramesJson.append(fj);
        }
    }
    clearSelection();
    m_multiDragStartPositions.clear();

    const QJsonArray items = canvas.value(QStringLiteral("items")).toArray();
    // 2026-07-10 レイヤー機能(Layer.md Phase B)対応: 以前はitemsが空配列
    // （またはitemsキー無し）の場合ここで早期returnし、既存アイテムの
    // teardown（GL/mpv解放・qDeleteAll）を一切行っていなかった。
    // 「空のitems: []を持つセッションを読み込む」＝新規/空レイヤーへの
    // 切り替えという扱いが成立しなくなるため、items空でも必ず
    // teardown〜再構築処理まで通す（この先のループ本体はitemsが空なら
    // 何もしないだけなので、それ以降の処理はitems空でも問題なく動作する）。
    cancelAllOverlapAnims();
    cancelAllFadeInAnims();
    // 2026-07-11 🚫（D&D禁止固定）バグ修正: 以前はこの関数は
    // releaseGlResources() → doneCurrent() → qDeleteAll(m_items) の順だった。
    // しかし ~CanvasVideo() は無条件で m_mpvPlayer を delete し、~MpvPlayer()
    // の mpv_render_context_free() は有効なGLコンテキストがcurrentであることを
    // 要求する（destroyItem() の同趣旨コメント参照。「動画/音声バブルを
    // ゴミ箱に2回入れるとD&Dが🚫固定になる」バグと同一の根本原因）。
    // レイヤー切り替え(LayerSwitcher)はこの関数を毎回通るため、動画バブルを
    // 含むレイヤーを切り替えるたびにOLE/D&Dを壊す危険があった。
    // destroyItem()・~CanvasWidget()と同様、deleteまで含めて
    // makeCurrent()〜doneCurrent()の内側で行う。
    {
        const bool glReady = (context() != nullptr);
        if (glReady) makeCurrent();
        for (CanvasItem *item : m_items) {
            if (auto *vid = canvasItemAsVideo(item)) {
                vid->releaseGlResources(m_renderer);
            } else if (glReady) {
                if (auto *image = dynamic_cast<CanvasImage*>(item))
                    image->releaseGlResources(m_renderer);
            }
        }
        qDeleteAll(m_items);
        m_items.clear();
        if (glReady) doneCurrent();
    }
    m_activeItem = nullptr;
    m_focusedItem = nullptr;
    m_pendingFocusItem = nullptr;

#ifdef Q_OS_WIN
    // ゴミ箱削除経路（handleMouseReleaseのtrash分岐・deleteSelectedItems）と
    // 同様、mpv一括破棄の直後にOLEの生存確認とドロップターゲットの再登録を
    // 行う（レイヤー切り替えはこの一括破棄を頻繁に行うため）。
    {
        const HRESULT oleHr = OleInitialize(nullptr);
        qDebug() << "[Layer][CanvasWidget] session teardown done. OleInitialize hr=0x"
                 << Qt::hex << static_cast<unsigned long>(oleHr) << Qt::dec;
        if (auto *mw = qobject_cast<MainWindow*>(window()))
            mw->refreshDropTargets();
    }
#endif

    for (const QJsonValue &value : items)
        createItemFromSessionEntry(value.toObject());

    // 2026-07-11 レイヤー機能バグ修正（初版）: CanvasWidget::initializeGL()は
    // Qtによってアプリ起動時に一度しか呼ばれない。そのループが
    // 「m_itemsに載っている動画バブルのMpvPlayerを作る」役目を担って
    // いたため、起動時の最初のrestoreSessionFromJson()（initializeGL()より
    // 前に走る）で読み込まれたバブルは、後からinitializeGL()側のループで
    // 拾われて初期化されていた。しかしレイヤー切り替え等でGLコンテキスト
    // 生成後に再度restoreSessionFromJson()が呼ばれるケースでは、
    // initializeGL()は二度と呼ばれないため、ここで新しく生まれた動画
    // バブルはMpvPlayerが一度も作られず、再生操作をしても反応しない
    // （＝「レイヤー切り替え後に動画が再生できなくなる」不具合の原因）。
    // 対策として、復元直後にここで全バブル分initializeMpv()を同期的に
    // 呼んでいた。
    //
    // 2026-07-11 追記2〜4（撤回・再設計）: 上記の「全部先に初期化」方式は
    // 動画バブルがN個あるレイヤーへ切り替えるたびにmpv_create()/
    // mpv_initialize()/mpv_render_context_create()をN回連続でUIスレッド上に
    // 走らせることになり、バブル数に比例して切り替えが重くなっていた
    // （追記2でユーザー報告・確認済み）。QTimer::singleShot()チェーンで
    // 1個ずつ遅延実行する案も試したが、結局N個分すべてを最終的には
    // 呼ぶ以上トータルの重さは変わらず、しかも遅延中にユーザーが再生操作
    // すると再生位置リセットの退行を引き起こした（追記3）ため撤回した。
    //
    // 最終的な対策: このループそのものを廃止し、根本原因である
    // 「initializeMpv()を呼ぶ主体がinitializeGL()の一度きりのループしか
    // 無かったこと」をCanvasVideo::startPlayback()側で直接解消した。
    // startPlayback()の「mpvがまだ無い」分岐（m_mpvInitializationAttempted
    // ==false）で、親のCanvasWidget（new CanvasVideo(this)で常に親として
    // 渡される）経由でmakeCurrent()/initializeMpv()/doneCurrent()をその場で
    // 呼ぶようにしたため、実際に再生されようとしたバブル1個分だけがその
    // 瞬間に初期化される。これによりレイヤー切り替え自体はバブル数に
    // 依存しないO(1)になり、mpv起動コストは「本当に再生されるバブル」にしか
    // 発生しなくなる（Layer.mdが本来想定していた遅延読み込みの形）。
    // CanvasVideo::paintGL()はm_mpvPlayerが無くてもサムネイル表示に
    // フォールバックする設計なので、未再生バブルの見た目には影響しない。
    update();
}
