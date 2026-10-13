#include "CanvasDropHandler.h"

#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QSet>
#include <QStringList>
#include <QUrl>

bool CanvasDropHandler::canAcceptMimeData(const QMimeData *mime,
                                          bool focusDropBlocked,
                                          QString *reason)
{
    if (!mime) {
        if (reason)
            *reason = QStringLiteral("mimeData is null");
        return false;
    }

    if (focusDropBlocked) {
        if (reason)
            *reason = QStringLiteral("a bubble is currently focused/fullscreen; drop disabled");
        return false;
    }

    if (mime->hasUrls() || mime->hasText() || mime->hasImage()) {
        if (reason)
            *reason = QStringLiteral("supported mime: urls/text/image");
        return true;
    }

    if (reason)
        *reason = QStringLiteral("no urls, text, or image in mimeData");
    return false;
}

void CanvasDropHandler::forceAcceptDropEvent(QDropEvent *event)
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

CanvasDropHandler::MimeSummary CanvasDropHandler::summarizeMimeData(const QMimeData *mime)
{
    MimeSummary summary;
    if (!mime)
        return summary;

    summary.hasUrls = mime->hasUrls();
    summary.hasText = mime->hasText();
    summary.hasImage = mime->hasImage();
    summary.textPreview = mime->text().left(200);

    for (const QUrl &url : mime->urls())
        summary.urls << url.toString();

    return summary;
}

CanvasDropHandler::LocalFileKind CanvasDropHandler::classifyLocalFile(const QString &path)
{
    if (isPluginZipPath(path))
        return LocalFileKind::PluginZip;
    if (isLikelyLocalImageFile(path))
        return LocalFileKind::Image;
    if (isLikelyLocalVideoFile(path))
        return LocalFileKind::Video;
    if (isLikelyLocalAudioFile(path))
        return LocalFileKind::Audio;
    return LocalFileKind::Unsupported;
}

QStringList CanvasDropHandler::localFilePaths(const QMimeData *mime)
{
    QStringList paths;
    if (!mime || !mime->hasUrls())
        return paths;

    for (const QUrl &url : mime->urls()) {
        const QString localPath = url.toLocalFile();
        if (!localPath.isEmpty())
            paths.append(localPath);
    }
    return paths;
}

bool CanvasDropHandler::isLikelyLocalVideoFile(const QString &path)
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

bool CanvasDropHandler::isLikelyLocalAudioFile(const QString &path)
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

bool CanvasDropHandler::isLikelyLocalImageFile(const QString &path)
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

bool CanvasDropHandler::isPluginZipPath(const QString &path)
{
    return path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive);
}

