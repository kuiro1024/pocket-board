#pragma once

#include <QString>
#include <QStringList>

class QByteArray;

// 2026-07-12: Web画像バブル（Discord CDNの期限付きURL等）を
// %LOCALAPPDATA%/PocketBoard/webimage/ へキャッシュするための
// 小さな独立ユーティリティ。
//
// 設計方針: MediaSource / セッションJSONのスキーマは変更しない。
// キャッシュファイル名は「URL文字列そのもの」から決定的に導出する
// （SHA1ハッシュ）ため、「このURLのキャッシュが存在するか」は
// 既存の sourceUrl だけから毎回再計算でき、専用フィールドを
// 持ち回る必要がない。復元時は cachedFilePathForUrl() が返すパスに
// ファイルが存在するかを見るだけでよい。
//
// Discordの署名付きURL（?ex=...&is=...&hm=...）はURL全体が
// 一度限りの発行物なので、これで問題なく一意にキャッシュできる。
namespace WebImageCache
{
    // 1ファイルあたりのキャッシュ上限。これを超えるダウンロードは
    // キャッシュへの保存だけをスキップする（その場の表示自体は
    // 通常通り継続してよい。次回セッション復元時は元URLへの
    // 再取得を試み、失敗すればプレースホルダーになる＝従来の挙動）。
    constexpr qint64 kMaxCacheableBytes = 30LL * 1024 * 1024;

    // %LOCALAPPDATA%/PocketBoard/webimage/ のパス。
    QString cacheDirPath();

    // 与えられたURLに対応する決定的なキャッシュファイルパスを返す。
    // ファイルが実際に存在するかどうかは問わない
    // （呼び出し側で QFileInfo::exists() すること）。
    QString cachedFilePathForUrl(const QString &url);

    // ダウンロード済みバイト列をキャッシュへ保存する。
    // kMaxCacheableBytesを超える場合や書き込み失敗時は空文字を返す。
    QString store(const QString &url, const QByteArray &bytes);

    // 2026-07-12: X(Twitter)のように、hasImage()でビットマップは得られる
    // ものの、mimeデータ内のURLがどれも再取得に使えない（ブラウザが作った
    // 一時ファイルパスや、画像そのものではないページURLしか無い）ケース用。
    // 「再取得可能な元URL」が存在しないので、代わりにバイト列自体の
    // ハッシュを合成識別子（"pocketboard-capture:<hash>"）として発行し、
    // それをキーにキャッシュへ保存する。呼び出し側はこの識別子を通常の
    // sourceUrlと同じようにMediaSourceへ保持しておけばよい
    // （cachedFilePathForUrl()等は文字列のハッシュを取るだけなので、
    // 本物のURLである必要はない）。保存に失敗した場合は空文字を返す。
    QString storeCapturedImage(const QByteArray &bytes);

    // 指定パスのキャッシュファイルを削除する（存在しなければ何もしない）。
    void removeCachedFile(const QString &cachedPath);

    // webimage/ フォルダ内で、referencedUrls のいずれからも
    // 導出されないファイルを削除する（起動時ガベージコレクション用。
    // クラッシュ等でバブル削除時の削除処理を経由できなかった
    // 孤児ファイルの掃除）。
    void sweepOrphans(const QStringList &referencedUrls);
}
