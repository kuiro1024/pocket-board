#pragma once

#include <QString>
#include <QStringList>

class QMimeData;
class QDropEvent;

class CanvasDropHandler
{
public:
    enum class LocalFileKind {
        Unsupported,
        PluginZip,
        Image,
        Video,
        Audio,
    };

    struct MimeSummary {
        bool hasUrls = false;
        bool hasText = false;
        bool hasImage = false;
        QString textPreview;
        QStringList urls;
    };

    static bool canAcceptMimeData(const QMimeData *mime,
                                  bool focusDropBlocked,
                                  QString *reason = nullptr);

    static void forceAcceptDropEvent(QDropEvent *event);
    static MimeSummary summarizeMimeData(const QMimeData *mime);
    static LocalFileKind classifyLocalFile(const QString &path);
    static QStringList localFilePaths(const QMimeData *mime);
    static bool isLikelyLocalVideoFile(const QString &path);
    static bool isLikelyLocalAudioFile(const QString &path);
    static bool isLikelyLocalImageFile(const QString &path);
    static bool isPluginZipPath(const QString &path);
};
