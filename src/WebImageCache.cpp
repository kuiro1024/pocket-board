#include "WebImageCache.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

namespace WebImageCache
{

QString cacheDirPath()
{
    const QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(baseDir).filePath(QStringLiteral("webimage"));
}

QString cachedFilePathForUrl(const QString &url)
{
    if (url.isEmpty())
        return {};

    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();

    // 拡張子はURLのパス部分から抽出する。既存のisLikelyImageUrl()と
    // 同じ発想（未知/取得不能な場合は汎用の"img"にフォールバック。
    // ファイル自体はQPixmapが中身から判定して開ければよく、拡張子は
    // 主にキャッシュフォルダを人間が見たときの可読性のためのもの）。
    QString ext = QFileInfo(QUrl(url).path()).suffix().toLower();
    static const QSet<QString> knownExts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("webp")
    };
    if (!knownExts.contains(ext))
        ext = QStringLiteral("img");

    return QDir(cacheDirPath()).filePath(QString::fromLatin1(hash) + QStringLiteral(".") + ext);
}

QString store(const QString &url, const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return {};

    if (bytes.size() > kMaxCacheableBytes) {
        qDebug() << "[WebImageCache] skip caching, over size limit:" << bytes.size() << url;
        return {};
    }

    const QString path = cachedFilePathForUrl(url);
    if (path.isEmpty())
        return {};

    QDir dir(QFileInfo(path).absolutePath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        qWarning() << "[WebImageCache] failed to create cache directory:" << dir.absolutePath();
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[WebImageCache] failed to open cache file for write:" << file.errorString();
        return {};
    }
    if (file.write(bytes) != bytes.size()) {
        qWarning() << "[WebImageCache] short write to cache file:" << path;
        file.remove();
        return {};
    }
    return path;
}

QString storeCapturedImage(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return {};

    const QByteArray contentHash =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex();
    const QString key = QStringLiteral("pocketboard-capture:") + QString::fromLatin1(contentHash);

    const QString path = store(key, bytes);
    if (path.isEmpty())
        return {};
    return key;
}

void removeCachedFile(const QString &cachedPath)
{
    if (cachedPath.isEmpty())
        return;
    QFile file(cachedPath);
    if (file.exists() && !file.remove())
        qWarning() << "[WebImageCache] failed to remove cache file:" << cachedPath;
}

void sweepOrphans(const QStringList &referencedUrls)
{
    const QString dirPath = cacheDirPath();
    QDir dir(dirPath);
    if (!dir.exists())
        return;

    QSet<QString> keep;
    for (const QString &url : referencedUrls) {
        const QString path = cachedFilePathForUrl(url);
        if (!path.isEmpty())
            keep.insert(QFileInfo(path).fileName());
    }

    const QStringList entries = dir.entryList(QDir::Files);
    for (const QString &name : entries) {
        if (keep.contains(name))
            continue;
        const QString fullPath = dir.filePath(name);
        if (QFile::remove(fullPath))
            qDebug() << "[WebImageCache] removed orphan cache file:" << fullPath;
        else
            qWarning() << "[WebImageCache] failed to remove orphan cache file:" << fullPath;
    }
}

} // namespace WebImageCache
