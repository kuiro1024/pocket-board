#include "WebVideoCache.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace WebVideoCache
{

namespace {

const QSet<QString> &videoExtensions()
{
    // CanvasDropHandler::isLikelyLocalVideoFile() と同じ並び＋HLS。
    static const QSet<QString> exts = {
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"),
        QStringLiteral("mkv"), QStringLiteral("webm"), QStringLiteral("avi"),
        QStringLiteral("wmv"), QStringLiteral("flv"), QStringLiteral("mpeg"),
        QStringLiteral("mpg"), QStringLiteral("ts"), QStringLiteral("m2ts"),
        QStringLiteral("3gp"), QStringLiteral("m3u8"),
    };
    return exts;
}

bool isHttpUrl(const QUrl &u)
{
    const QString scheme = u.scheme().toLower();
    return u.isValid() && !u.host().isEmpty()
        && (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"));
}

QString pathSuffix(const QUrl &u)
{
    return QFileInfo(u.path()).suffix().toLower();
}

const char *kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) PocketBoard/1.0";

QNetworkAccessManager *networkManager()
{
    static QPointer<QNetworkAccessManager> nam;
    if (!nam) {
        nam = new QNetworkAccessManager(QCoreApplication::instance());
    }
    return nam;
}

QNetworkRequest makeRequest(const QString &url)
{
    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    return request;
}

// 進行中のダウンロード（URL → reply）。同じURLを二重にダウンロードしないため。
QHash<QString, QPointer<QNetworkReply>> &activeDownloads()
{
    static QHash<QString, QPointer<QNetworkReply>> downloads;
    return downloads;
}

int httpStatus(QNetworkReply *reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

bool isRedirectStatus(int status)
{
    return status >= 300 && status < 400;
}

QString contentTypeOf(QNetworkReply *reply)
{
    QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    const int semicolon = type.indexOf(QLatin1Char(';'));
    if (semicolon >= 0)
        type = type.left(semicolon);
    return type.trimmed();
}

bool isVideoContentType(const QString &contentType, const QUrl &finalUrl)
{
    if (contentType.startsWith(QStringLiteral("video/")))
        return true;
    if (contentType == QStringLiteral("application/vnd.apple.mpegurl")
        || contentType == QStringLiteral("application/x-mpegurl"))
        return true;
    // 種類を名乗らないサーバー（octet-stream 等）は、リダイレクト後の
    // 最終URLの拡張子で判断する。
    if ((contentType.isEmpty() || contentType == QStringLiteral("application/octet-stream")
         || contentType == QStringLiteral("binary/octet-stream"))
        && videoExtensions().contains(pathSuffix(finalUrl)))
        return true;
    return false;
}

} // namespace

bool isDirectVideoUrl(const QString &url)
{
    const QUrl u(url);
    return isHttpUrl(u) && videoExtensions().contains(pathSuffix(u));
}

bool isStreamingPlaylistUrl(const QString &url)
{
    return pathSuffix(QUrl(url)) == QStringLiteral("m3u8");
}

bool isWorthProbing(const QString &url)
{
    const QUrl u(url);
    if (!isHttpUrl(u))
        return false;
    const QString path = u.path();
    if (path.isEmpty() || path == QStringLiteral("/"))
        return false; // トップページ
    static const QSet<QString> pageExts = {
        QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("shtml"),
        QStringLiteral("php"), QStringLiteral("asp"), QStringLiteral("aspx"),
        QStringLiteral("jsp"), QStringLiteral("pdf"), QStringLiteral("zip"),
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("svg"),
    };
    return !pageExts.contains(pathSuffix(u));
}

QString titleForUrl(const QString &url)
{
    const QUrl u(url);
    // 末尾が "/" のURL（例: .../download/video/12345/）でも最後の区切りを使う。
    const QStringList parts = u.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty())
        return parts.last();
    return u.host();
}

QString cacheDirPath()
{
    const QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(baseDir).filePath(QStringLiteral("webvideo"));
}

QString cachedFilePathForUrl(const QString &url)
{
    if (url.isEmpty())
        return {};
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    // 拡張子は主にフォルダを人が見たときの分かりやすさのため（mpv は中身で判定する）。
    QString ext = pathSuffix(QUrl(url));
    if (!videoExtensions().contains(ext) || ext == QStringLiteral("m3u8"))
        ext = QStringLiteral("video");
    return QDir(cacheDirPath()).filePath(QString::fromLatin1(hash) + QLatin1Char('.') + ext);
}

QString completedCacheFile(const QString &url)
{
    const QString path = cachedFilePathForUrl(url);
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    if (!info.exists() || info.size() <= 0)
        return {};
    return path;
}

QString playableUrlFor(const QString &url)
{
    const QString cached = completedCacheFile(url);
    if (!cached.isEmpty()) {
        qDebug() << "[WebVideoCache] play from local cache:" << cached;
        return QUrl::fromLocalFile(cached).toString();
    }
    return url;
}

void ensureCached(const QString &url)
{
    if (url.isEmpty() || !isHttpUrl(QUrl(url)) || isStreamingPlaylistUrl(url))
        return;
    const QString path = cachedFilePathForUrl(url);
    if (path.isEmpty() || !completedCacheFile(url).isEmpty())
        return;

    auto &active = activeDownloads();
    const auto existing = active.constFind(url);
    if (existing != active.constEnd() && existing.value())
        return;

    QDir dir(cacheDirPath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        qWarning() << "[WebVideoCache] failed to create cache directory:" << dir.absolutePath();
        return;
    }

    const QString partPath = path + QStringLiteral(".part");
    auto file = std::make_shared<QFile>(partPath);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[WebVideoCache] failed to open part file:" << partPath << file->errorString();
        return;
    }

    QNetworkReply *reply = networkManager()->get(makeRequest(url));
    active.insert(url, reply);
    auto failed = std::make_shared<bool>(false);
    qDebug() << "[WebVideoCache] download started:" << url;

    // アプリ終了時はダウンロードを打ち切る（.part は次回起動時の掃除で消える）。
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                     reply, [reply, failed]() {
        *failed = true;
        reply->abort();
    });

    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, failed, url]() {
        if (*failed)
            return;
        const int status = httpStatus(reply);
        if (isRedirectStatus(status))
            return;
        if (status >= 400) {
            qDebug() << "[WebVideoCache] download refused, status" << status << url;
            *failed = true;
            reply->abort();
            return;
        }
        if (contentTypeOf(reply).startsWith(QStringLiteral("text/html"))) {
            qDebug() << "[WebVideoCache] download is an HTML page, not a video:" << url;
            *failed = true;
            reply->abort();
            return;
        }
        const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (length > kMaxCacheableBytes) {
            qDebug() << "[WebVideoCache] skip caching, over size limit:" << length << url;
            *failed = true;
            reply->abort();
        }
    });

    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, file, failed, url]() {
        if (*failed)
            return;
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;
        if (file->size() + chunk.size() > kMaxCacheableBytes) {
            qDebug() << "[WebVideoCache] skip caching, grew over size limit:" << url;
            *failed = true;
            reply->abort();
            return;
        }
        if (file->write(chunk) != chunk.size()) {
            qWarning() << "[WebVideoCache] write failed:" << file->errorString();
            *failed = true;
            reply->abort();
        }
    });

    QObject::connect(reply, &QNetworkReply::finished, reply,
                     [reply, file, failed, url, path, partPath]() {
        activeDownloads().remove(url);
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        if (!*failed && networkOk) {
            const QByteArray rest = reply->readAll();
            if (!rest.isEmpty() && file->write(rest) != rest.size())
                *failed = true;
        }
        const qint64 written = file->size();
        file->close();
        reply->deleteLater();

        if (*failed || !networkOk || written <= 0) {
            if (!*failed)
                qDebug() << "[WebVideoCache] download failed:" << reply->errorString() << url;
            QFile::remove(partPath);
            return;
        }
        QFile::remove(path);
        if (!QFile::rename(partPath, path)) {
            qWarning() << "[WebVideoCache] failed to finalize cache file:" << path;
            QFile::remove(partPath);
            return;
        }
        qDebug() << "[WebVideoCache] cached web video:" << path << written << "bytes";
    });
}

void probeIsVideo(QObject *context, const QString &url,
                  const std::function<void(bool isVideo)> &callback)
{
    if (!isHttpUrl(QUrl(url))) {
        if (callback)
            callback(false);
        return;
    }

    QNetworkReply *reply = networkManager()->get(makeRequest(url));
    QPointer<QObject> guard(context);
    auto done = std::make_shared<bool>(false);
    // 結論が出たら1回だけ呼ぶ。中身（ページ本体や動画本体）は要らないので打ち切る。
    auto finish = std::make_shared<std::function<void(bool)>>();
    *finish = [reply, guard, done, callback, url](bool isVideo) {
        if (*done)
            return;
        *done = true;
        qDebug() << "[WebVideoCache] probe result:" << (isVideo ? "video" : "not video") << url;
        reply->abort();
        reply->deleteLater();
        if (guard && callback)
            callback(isVideo);
    };

    QTimer::singleShot(kProbeTimeoutMs, reply, [finish]() {
        (*finish)(false);
    });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, finish]() {
        const int status = httpStatus(reply);
        if (isRedirectStatus(status))
            return;
        if (status >= 400) {
            (*finish)(false);
            return;
        }
        (*finish)(isVideoContentType(contentTypeOf(reply), reply->url()));
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [finish]() {
        (*finish)(false);
    });
}

void sweepOrphans(const QStringList &referencedUrls)
{
    QDir dir(cacheDirPath());
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
            qDebug() << "[WebVideoCache] removed orphan cache file:" << fullPath;
        else
            qWarning() << "[WebVideoCache] failed to remove orphan cache file:" << fullPath;
    }
}

} // namespace WebVideoCache
