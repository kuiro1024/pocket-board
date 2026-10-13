#include "WebLinkIcon.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <memory>

namespace {

constexpr qint64 kMaxHtmlBytes = 512 * 1024;
constexpr qint64 kMaxIconBytes = 2 * 1024 * 1024;
constexpr int    kTimeoutMs    = 8000;
constexpr int    kMaxCandidates = 4;

QString faviconDir()
{
    // WebImageCache（.../webimage）と同じ AppLocalDataLocation 配下に置く。
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        base = QDir::homePath();
    return QDir(base).filePath(QStringLiteral("favicon"));
}

QString keyForHost(const QString &host)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(host.toLower().toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString hostOf(const QString &url)
{
    return WebLinkIcon::displayHost(url);
}

QString cachePathFor(const QString &url)
{
    const QString host = hostOf(url);
    if (host.isEmpty())
        return {};
    return QDir(faviconDir()).filePath(keyForHost(host) + QStringLiteral(".png"));
}

QString failMarkerPathFor(const QString &url)
{
    const QString host = hostOf(url);
    if (host.isEmpty())
        return {};
    return QDir(faviconDir()).filePath(keyForHost(host) + QStringLiteral(".none"));
}

struct Candidate {
    QUrl url;
    int  score = 0;
};

// HTMLの <link ...> からアイコン候補を抽出（スコア降順）。
QList<Candidate> iconCandidatesFromHtml(const QString &html, const QUrl &base)
{
    QList<Candidate> out;
    static const QRegularExpression linkRe(
        QStringLiteral("<link\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attrRe(
        QStringLiteral("([a-zA-Z\\-]+)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))"));

    auto it = linkRe.globalMatch(html);
    while (it.hasNext()) {
        const QString tag = it.next().captured(0);
        QString rel, href, sizes, type;
        auto ai = attrRe.globalMatch(tag);
        while (ai.hasNext()) {
            const auto m = ai.next();
            const QString name = m.captured(1).toLower();
            QString value = m.captured(2);
            if (value.isEmpty()) value = m.captured(3);
            if (value.isEmpty()) value = m.captured(4);
            if (name == QLatin1String("rel")) rel = value.toLower();
            else if (name == QLatin1String("href")) href = value.trimmed();
            else if (name == QLatin1String("sizes")) sizes = value.toLower();
            else if (name == QLatin1String("type")) type = value.toLower();
        }
        if (href.isEmpty() || !rel.contains(QLatin1String("icon")))
            continue;
        if (rel.contains(QLatin1String("mask-icon")))
            continue;

        const QUrl resolved = base.resolved(QUrl(href));
        const QString scheme = resolved.scheme().toLower();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
            continue;

        int score = 10;
        if (rel.contains(QLatin1String("apple-touch-icon")))
            score += 30;
        // sizes="32x32" / "180x180" 等。大きいほど優先（128pxを超えても縮小で使える）。
        const int x = sizes.indexOf(QLatin1Char('x'));
        if (x > 0) {
            bool ok = false;
            const int w = sizes.left(x).toInt(&ok);
            if (ok)
                score += qMin(w, 256) / 8;
        }
        // SVGはデコーダ依存なので優先度を下げる。
        if (type.contains(QLatin1String("svg"))
            || resolved.path().endsWith(QLatin1String(".svg"), Qt::CaseInsensitive))
            score -= 25;
        out.append({resolved, score});
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
    return out;
}

QImage normalizedIcon(const QImage &src)
{
    if (src.isNull() || src.width() < 16 || src.height() < 16)
        return {};
    const int size = WebLinkIcon::kIconSize;
    QImage scaled = src.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QImage out(size, size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);
    p.end();
    return out;
}

// 1回の取得作業。ownerの子として生き、終わったらdeleteLaterする。
class FaviconJob : public QObject
{
public:
    FaviconJob(QObject *owner, const QString &pageUrl,
               std::function<void(const QImage &)> done)
        : QObject(owner)
        , m_pageUrl(pageUrl)
        , m_done(std::move(done))
        , m_nam(new QNetworkAccessManager(this))
    {
    }

    void start()
    {
        QNetworkReply *reply = m_nam->get(makeRequest(QUrl(m_pageUrl)));
        auto buffer = std::make_shared<QByteArray>();
        connect(reply, &QNetworkReply::readyRead, this, [reply, buffer]() {
            buffer->append(reply->readAll());
            if (buffer->size() > kMaxHtmlBytes)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, buffer]() {
            const QUrl finalUrl = reply->url().isValid() ? reply->url() : QUrl(m_pageUrl);
            reply->deleteLater();
            QList<Candidate> cands;
            if (!buffer->isEmpty())
                cands = iconCandidatesFromHtml(QString::fromUtf8(*buffer), finalUrl);
            // 最後の砦として /favicon.ico。
            QUrl root = finalUrl;
            root.setPath(QStringLiteral("/favicon.ico"));
            root.setQuery(QString());
            root.setFragment(QString());
            cands.append({root, 0});
            // 重複除去＋上限。
            QList<QUrl> urls;
            for (const Candidate &c : cands) {
                if (!urls.contains(c.url))
                    urls.append(c.url);
                if (urls.size() >= kMaxCandidates)
                    break;
            }
            m_queue = urls;
            tryNext();
        });
    }

private:
    QNetworkRequest makeRequest(const QUrl &url) const
    {
        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(kTimeoutMs);
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 PocketBoard/1.0 favicon-loader"));
        return req;
    }

    void tryNext()
    {
        if (m_queue.isEmpty()) {
            finish(QImage());
            return;
        }
        const QUrl url = m_queue.takeFirst();
        QNetworkReply *reply = m_nam->get(makeRequest(url));
        auto buffer = std::make_shared<QByteArray>();
        connect(reply, &QNetworkReply::readyRead, this, [reply, buffer]() {
            buffer->append(reply->readAll());
            if (buffer->size() > kMaxIconBytes)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, buffer]() {
            const bool ok = reply->error() == QNetworkReply::NoError;
            reply->deleteLater();
            if (ok) {
                const QImage img = normalizedIcon(QImage::fromData(*buffer));
                if (!img.isNull()) {
                    finish(img);
                    return;
                }
            }
            tryNext();
        });
    }

    void finish(const QImage &img)
    {
        if (m_done)
            m_done(img);
        deleteLater();
    }

    QString m_pageUrl;
    std::function<void(const QImage &)> m_done;
    QNetworkAccessManager *m_nam = nullptr;
    QList<QUrl> m_queue;
};

} // namespace

namespace WebLinkIcon {

QString displayHost(const QString &url)
{
    const QUrl u(url);
    const QString scheme = u.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return {};
    QString host = u.host().toLower();
    if (host.startsWith(QLatin1String("www.")))
        host.remove(0, 4);
    return host;
}

QPixmap loadCached(const QString &url)
{
    const QString path = cachePathFor(url);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return {};
    QPixmap pix;
    if (!pix.load(path))
        return {};
    return pix;
}

QPixmap fallbackIcon(const QString &url, int size)
{
    const QString host = displayHost(url);
    const QChar letter = host.isEmpty() ? QChar(u'?') : host.at(0).toUpper();
    const uint h = qHash(host);
    const QColor bg = QColor::fromHsl(static_cast<int>(h % 360), 150, 105);

    QPixmap pix(size, size);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawEllipse(QRectF(size * 0.06, size * 0.06, size * 0.88, size * 0.88));
    QFont f(QStringLiteral("Arial"));
    f.setBold(true);
    f.setPixelSize(static_cast<int>(size * 0.5));
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, 235));
    p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, QString(letter));
    p.end();
    return pix;
}

bool needsFetch(const QString &url)
{
    const QString cache = cachePathFor(url);
    if (cache.isEmpty())
        return false;
    if (QFileInfo::exists(cache))
        return false;
    const QFileInfo marker(failMarkerPathFor(url));
    if (marker.exists()
        && marker.lastModified().daysTo(QDateTime::currentDateTime()) < kRetryDays)
        return false;
    return true;
}

void fetch(QObject *owner, const QString &url,
           const std::function<void(const QImage &)> &done)
{
    const QString cache = cachePathFor(url);
    const QString marker = failMarkerPathFor(url);
    if (!owner || cache.isEmpty()) {
        if (done)
            done(QImage());
        return;
    }
    QDir().mkpath(faviconDir());

    auto *job = new FaviconJob(owner, url,
        [cache, marker, done](const QImage &img) {
            if (!img.isNull()) {
                img.save(cache, "PNG");
                QFile::remove(marker);
            } else {
                QFile f(marker);
                if (f.open(QIODevice::WriteOnly))
                    f.close();
            }
            if (done)
                done(img);
        });
    job->start();
}

} // namespace WebLinkIcon
