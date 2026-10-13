#pragma once

#include <QImage>
#include <QPixmap>
#include <QString>
#include <functional>

class QObject;

// 2026-10-11: Webリンクバブル用のサイトアイコン（favicon）取得・キャッシュ。
//
// - ページHTMLの <link rel="icon"/"apple-touch-icon"> を解析し、無ければ
//   /favicon.ico を試す。第三者サービス（Googleのfavicon API等）は使わない。
// - 取得したアイコンは %LOCALAPPDATA%/PocketBoard/favicon/ に128px PNGで
//   ホスト単位でキャッシュする。取得失敗は「失敗マーカー」を残し、一定期間
//   （kRetryDays）は再取得しない（セッション復元のたびに通信しないため）。
// - 取得できない間は、ホスト名の頭文字を描いたフォールバックアイコンを使う。
namespace WebLinkIcon
{
    constexpr int kIconSize = 128;
    constexpr int kRetryDays = 7;

    // http/https のURLからホスト名を返す（先頭の "www." は除く）。
    QString displayHost(const QString &url);

    // キャッシュ済みfaviconを返す。無ければnull。
    QPixmap loadCached(const QString &url);

    // ホスト名の頭文字を描いたフォールバックアイコン。
    QPixmap fallbackIcon(const QString &url, int size = kIconSize);

    // faviconの取得が必要か（キャッシュ無し、かつ失敗マーカーが新しくない）。
    bool needsFetch(const QString &url);

    // 非同期にfaviconを取得してキャッシュへ保存する。完了時にdoneを呼ぶ
    // （失敗時は空のQImage）。ownerが破棄されると通信も中断されdoneは呼ばれない。
    void fetch(QObject *owner, const QString &url,
               const std::function<void(const QImage &)> &done);
}
