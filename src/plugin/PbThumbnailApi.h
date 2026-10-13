#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QtGlobal>
#include <QtPlugin>
#include <functional>

// Optional thumbnail capability implemented alongside IPbMediaPlugin.
// The media-plugin ABI remains unchanged; hosts must query this interface
// independently and keep the legacy thumbnailUrl path as a fallback.
struct PbThumbnailRequest
{
    quint64 requestId = 0;
    QString sourceUrl;
    // Media resolve/session restore may already know the concrete image URL.
    // Providers should prefer this hint over resolving sourceUrl again.
    QString thumbnailUrlHint;
    int maxDimension = 640;
};

struct PbThumbnailResult
{
    quint64 requestId = 0;
    bool success = false;
    QByteArray encodedImage;
    QString mimeType;
    QSize pixelSize;
    QString errorMessage;
};

// Implementations may perform file and network work asynchronously, but must
// invoke the callback exactly once on Qt's main thread unless the request was
// cancelled. Hosts defensively ignore stale request IDs and destroyed owners.
using PbThumbnailCallback = std::function<void(const PbThumbnailResult &)>;

class IPbThumbnailProvider
{
public:
    virtual ~IPbThumbnailProvider() = default;

    virtual void requestThumbnail(const PbThumbnailRequest &request,
                                  const PbThumbnailCallback &callback) = 0;

    // Cancellation is best-effort. The default no-op keeps the optional
    // interface easy to implement; hosts must still tolerate a late result.
    virtual void cancelThumbnailRequest(quint64 requestId)
    {
        Q_UNUSED(requestId)
    }
};

#define IPbThumbnailProvider_iid "com.pocketboard.IPbThumbnailProvider/1.0"
Q_DECLARE_INTERFACE(IPbThumbnailProvider, IPbThumbnailProvider_iid)
