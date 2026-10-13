#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QObject;

// 2026-10-12: Web動画（素材サイト等にある「動画ファイルそのもの」へのURL）を
// ドロップで再生できるようにするための小さな独立ユーティリティ。
// WebImageCache（webimage/）の動画版で、%LOCALAPPDATA%/PocketBoard/webvideo/ に保存する。
//
// 方針:
//  - 再生はキャッシュを待たない。mpv にURLをそのまま渡してダウンロードしながら
//    再生し、それと並行してここで同じURLをファイルへ保存する。
//  - 保存が終わったら、次回以降（セッション復元・再読み込み）はファイルから再生する。
//    素材サイトの期限付きURLが切れても壊れない。
//  - キャッシュファイル名は WebImageCache と同じく URL 文字列から決定的に作る
//    （SHA1）。セッションJSONに専用フィールドは増やさない。
//  - 動画サイトの「ページのURL」はここでは一切扱わない（プラグインの担当）。
//    ここが扱うのは、URLが直接動画ファイルを指しているものだけ。
namespace WebVideoCache
{
    // 1本あたりの保存上限。これを超える動画は保存だけを諦める
    // （再生はURLからのストリーミングで普通に続く）。
    constexpr qint64 kMaxCacheableBytes = 1024LL * 1024 * 1024; // 1GiB

    // 拡張子のないURLが動画かどうかを問い合わせるときの待ち時間の上限。
    constexpr int kProbeTimeoutMs = 4000;

    // http(s) で、パスの拡張子が動画（.mp4 / .webm / .m3u8 等）のURLか。
    bool isDirectVideoUrl(const QString &url);

    // HLS（.m3u8）のように、1本のファイルとして保存できない形式か。
    // 再生はできるが保存はしない。
    bool isStreamingPlaylistUrl(const QString &url);

    // 中身を問い合わせる価値のあるURLか（http(s) で、トップページや .html 等の
    // 明らかなWebページではないもの）。false のURLは問い合わせずにWebリンクにする。
    bool isWorthProbing(const QString &url);

    // URLから表示用のファイル名を作る（無ければホスト名）。
    QString titleForUrl(const QString &url);

    // %LOCALAPPDATA%/PocketBoard/webvideo/ のパス。
    QString cacheDirPath();

    // URLに対応する決定的なキャッシュファイルパス（存在するかは問わない）。
    QString cachedFilePathForUrl(const QString &url);

    // 保存が完了していればそのファイルパス、まだなら空文字。
    QString completedCacheFile(const QString &url);

    // 実際に mpv へ渡すURL。保存済みならローカルファイル（file:///...）、
    // まだならWebのURLそのもの。
    QString playableUrlFor(const QString &url);

    // まだ保存されていなければ裏でダウンロードを始める（同じURLの二重ダウンロードはしない）。
    void ensureCached(const QString &url);

    // 拡張子のないURL（例: 素材サイトの「ダウンロード」リンク）が動画かどうかを、
    // サーバーの返す Content-Type で確かめる。ヘッダーを受け取った時点で通信は打ち切る。
    // callback は context が生きている場合だけ、メインスレッドで1回だけ呼ばれる。
    void probeIsVideo(QObject *context, const QString &url,
                      const std::function<void(bool isVideo)> &callback);

    // webvideo/ 内で referencedUrls のどれからも導出されないファイル
    // （と、途中で終わった .part）を削除する（起動時の掃除用）。
    void sweepOrphans(const QStringList &referencedUrls);
}
