#include "WebImageUrlResolver.h"

#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

namespace WebImageUrlResolver
{

namespace {

// 2026-07-12: Google画像検索の検索結果グリッドから直接ドラッグすると、
// 実際の画像URLではなく https://www.google.com/imgres?imgurl=<実URL>&imgrefurl=...
// という「検索結果ページへのラッパーリンク」が渡ってくることが多い。
// これは.path()が"/imgres"で終わるだけで拡張子を持たないため、
// isLikelyImageUrl()の単純な拡張子判定では弾かれてしまい、WebImageとしての
// 記録/キャッシュが一切発火しない（＝レイヤー切り替えで消える）原因になる。
// よく使われるクエリパラメータ名（imgurl/mediaurl/url）の中身を覗いて、
// そちらが直接画像URLならそれを実URLとして採用する。
QString directImageUrlFromWrapper(const QString &rawUrl)
{
    const QUrl url(rawUrl);
    if (!url.isValid())
        return {};

    const QUrlQuery query(url);
    static const QStringList candidateKeys = {
        QStringLiteral("imgurl"), QStringLiteral("mediaurl"), QStringLiteral("url")
    };
    for (const QString &key : candidateKeys) {
        if (!query.hasQueryItem(key))
            continue;
        const QString inner = QUrl::fromPercentEncoding(
            query.queryItemValue(key, QUrl::FullyEncoded).toUtf8());
        if (isLikelyImageUrl(inner))
            return inner;
    }
    return {};
}

// 2026-07-12: TwitterことXの画像CDN（pbs.twimg.com）は
// https://pbs.twimg.com/media/XXXXXXXX?format=jpg&name=large のように、
// 拡張子をURLのパスではなく format= クエリパラメータで指定してくる
// （パス部分には拡張子が一切現れない）。これもisLikelyImageUrl()の
// パス拡張子判定では検出できず、Google imgresのラッパーとも違う
// パターン（"実URLはこのURL自身"で、クエリに拡張子ヒントがあるだけ）
// なので別関数として判定する。
bool hasImageFormatQueryHint(const QUrl &url)
{
    const QUrlQuery query(url);
    static const QStringList formatKeys = {
        QStringLiteral("format"), QStringLiteral("fm")
    };
    static const QSet<QString> knownFormats = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("webp")
    };
    for (const QString &key : formatKeys) {
        if (query.hasQueryItem(key) && knownFormats.contains(query.queryItemValue(key).toLower()))
            return true;
    }
    return false;
}

} // namespace

bool isLikelyImageUrl(const QString &rawUrl)
{
    const QUrl url(rawUrl);
    const QString path = url.path().toLower();
    static const QStringList imageExts = {
        QStringLiteral(".png"),
        QStringLiteral(".jpg"),
        QStringLiteral(".jpeg"),
        QStringLiteral(".gif"),
        QStringLiteral(".webp")
    };
    for (const QString &ext : imageExts) {
        if (path.endsWith(ext))
            return true;
    }
    return false;
}

QString resolveDirectImageUrl(const QString &rawUrl)
{
    if (isLikelyImageUrl(rawUrl))
        return rawUrl;

    const QString wrapped = directImageUrlFromWrapper(rawUrl);
    if (!wrapped.isEmpty())
        return wrapped;

    const QUrl url(rawUrl);
    if (url.isValid() && hasImageFormatQueryHint(url))
        return rawUrl;

    return {};
}

} // namespace WebImageUrlResolver
