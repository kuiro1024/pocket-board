#pragma once

#include <QObject>
#include <QImage>

class QNetworkAccessManager;

// CanvasVideo::loadThumbnail() から切り出した、サムネイル画像の
// ダウンロード・デコードだけを担当するクラス（Phase 7 Step 1）。
//
// 元の CanvasVideo::loadThumbnail() が持っていた QNetworkAccessManager の
// 生成・リクエスト発行・完了時のデコード/縮小処理をそのままこちらへ移した
// だけで、動作・ログ文言・エラーメッセージは一切変えていない。
// GLリソース（サムネイルテクスチャ）や CanvasVideo 側の
// m_thumbnailImage/m_thumbnailPending の保持は引き続き CanvasVideo が行う
// （このクラスはデコード済み QImage を signal で渡すだけで、GLには一切
// 触れない。paintGL() 内でのテクスチャ生成は従来通り CanvasVideo 側）。
class ThumbnailLoader : public QObject
{
    Q_OBJECT
public:
    explicit ThumbnailLoader(QObject *parent = nullptr);

    // url からサムネイル画像を非同期ダウンロードし、デコード・縮小後に
    // thumbnailReady() を発火する。CanvasVideo::loadThumbnail() が持っていた
    // 既存のネットワーク取得ロジックと完全に同一（失敗時は何もしないで
    // 終わる点も含めて同一）。
    void requestThumbnail(const QString &url);

    // 2026-07-08: ローカル動画ファイルのサムネイルを、Windows Explorerが
    // ファイル一覧で表示しているのと同じサムネイル
    // （IShellItemImageFactory::GetImage、Windows専用のシェルAPI）から
    // 取得する。動画を一切デコードせずOS側のサムネイルキャッシュ
    // （thumbcache_*.db）を利用するため高速。取得はバックグラウンドスレッド
    // （QThreadPool）で行い、完了時に requestThumbnail() と同じ
    // thumbnailReady() シグナルで結果を返す（呼び出し側は取得元の違いを
    // 意識しなくてよい）。取得失敗時（対応するサムネイルプロバイダが無い
    // 等）は何も発火しない。Windows以外のビルドでは常に何もしない。
    void requestLocalThumbnail(const QString &filePath);

    // サムネイルの長辺が上限を超えている場合のみ縮小する。
    // CanvasVideo::setRestoredThumbnailImage()（セッション復元時）からも
    // 同じ上限を適用するために呼ばれる、ダウンロード経路と共有のロジック。
    static QImage downscaleThumbnailIfNeeded(const QImage &src);

    // 2026-09-22: requestLocalThumbnail() と同じ取得（Explorerのサムネイル）を
    // 呼び出し元のスレッドでそのまま行う。ブロッキングなので必ずワーカー
    // スレッドから呼ぶこと（ラベルの一覧の行のサムネイル用。CanvasVideo::
    // requestPanelThumbnail）。失敗時・Windows以外は null を返す。
    static QImage fetchLocalThumbnailBlocking(const QString &filePath);

signals:
    // デコード・縮小・Format_RGBA8888変換済みの画像。
    // 失敗時（DL失敗・パース失敗）は発火しない。
    void thumbnailReady(const QImage &image);

private:
    QNetworkAccessManager *m_nam {nullptr};

#ifdef Q_OS_WIN
    // バックグラウンドスレッドで実行する実処理本体。QObjectの状態（m_nam等）
    // には一切触れないstatic関数にしてあるため、スレッドセーフに直接呼べる。
    static QImage fetchWindowsShellThumbnail(const QString &filePath);
#endif
};
