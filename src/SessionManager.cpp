#include "SessionManager.h"
#include "CanvasWidget.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QSaveFile>
#include <QMutex>
#include <QThreadPool>
#include <QCryptographicHash>
#include <QDebug>
#include <atomic>

namespace {
// 2026-07-11: layerNumber → session_N.jsonのルートJSONオブジェクトの
// プロセス内キャッシュ。preloadAllLayers()での一括先読みと、その後の
// save()/restore()等がすべてここを介する。関数内staticにすることで
// SessionManager.hをQHash込みで汚さずに済ませている。
QHash<int, QJsonObject> &sessionObjectCache()
{
    static QHash<int, QJsonObject> cache;
    return cache;
}

// 2026-10-12: 書き込みの順序づけ。保存要求ごとにGUIスレッドで世代番号を
// 振り、実際の書き込みは s_writeMutex の内側で「自分より新しい世代が
// 既に書かれていたら何もしない」とする。非同期の自動保存が、後から
// 同期で行った保存（トレイ格納・終了時）を古い内容で上書きしないため。
std::atomic<quint64> s_nextGeneration {0};
QMutex s_writeMutex;
quint64 s_lastWrittenGeneration = 0;      // s_writeMutex で保護
QHash<int, QByteArray> s_lastWrittenHash; // 同上。同じ中身なら書かない

QThreadPool &sessionWriterPool()
{
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(30000);
        return true;
    }();
    Q_UNUSED(configured)
    return pool;
}

// QSaveFile で一時ファイルへ書いてから置き換える。途中で落ちても
// 前回の session_N.json はそのまま残る（以前は Truncate で直接書いて
// いたため、書き込み中に落ちると空や途中までのファイルになり得た）。
bool writeBytesAtomically(int layerNumber, const QString &path,
                          const QByteArray &bytes, quint64 generation)
{
    QMutexLocker lock(&s_writeMutex);
    if (generation < s_lastWrittenGeneration)
        return true; // より新しい内容が既に書かれている
    const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1);
    if (s_lastWrittenHash.value(layerNumber) == hash && QFileInfo::exists(path)) {
        s_lastWrittenGeneration = generation;
        return true; // 中身が変わっていない
    }

    QDir dir(QFileInfo(path).absolutePath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        qWarning() << "[Session] Failed to create session directory:" << dir.absolutePath();
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[Session] Failed to open session for write:" << file.errorString();
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        qWarning() << "[Session] Failed to write session:" << file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        qWarning() << "[Session] Failed to commit session:" << file.errorString();
        return false;
    }
    s_lastWrittenGeneration = generation;
    s_lastWrittenHash.insert(layerNumber, hash);
    return true;
}

} // namespace

QString SessionManager::sessionFilePath(int layerNumber)
{
    const QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(baseDir).filePath(QStringLiteral("session_%1.json").arg(layerNumber));
}

// version 1: 旧形式（youtubeUrl / filePath / sourceType / positionSeconds / "z" が正式キー）
// version 2: Phase 6 以降。MediaSource 中心（kind / sourceUrl / title）+
//            position(x,y) / size(width,height) / zOrder / playbackPosition / volume / paused。
//            旧キーは CanvasWidget::restoreSessionFromJson() が読み込み互換として引き続き受理する。
bool SessionManager::save(CanvasWidget *canvas, int layerNumber, const QString &layerName)
{
    if (!canvas)
        return false;

    QJsonObject root;
    root.insert(QStringLiteral("version"), 2);
    // LayerManifest側のマニフェストと二重に持たせる（Layer.md Phase A）。
    root.insert(QStringLiteral("layerName"), layerName);
    root.insert(QStringLiteral("canvas"), canvas->sessionToJson());
    return writeSessionObject(layerNumber, root);
}

void SessionManager::saveAsync(CanvasWidget *canvas, int layerNumber, const QString &layerName)
{
    if (!canvas)
        return;

    QJsonObject root;
    root.insert(QStringLiteral("version"), 2);
    root.insert(QStringLiteral("layerName"), layerName);
    root.insert(QStringLiteral("canvas"), canvas->sessionToJson());
    sessionObjectCache().insert(layerNumber, root);

    const quint64 generation = ++s_nextGeneration;
    const QString path = sessionFilePath(layerNumber);
    // QJsonObject は暗黙共有（参照カウントはアトミック）。ワーカー側は
    // 読むだけなので、コピーを渡せばGUIスレッド側の変更とは干渉しない。
    sessionWriterPool().start([layerNumber, path, root, generation] {
        const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
        writeBytesAtomically(layerNumber, path, bytes, generation);
    });
}

bool SessionManager::restore(CanvasWidget *canvas, int layerNumber)
{
    if (!canvas)
        return false;

    const QJsonObject root = loadSessionObject(layerNumber);
    if (root.isEmpty())
        return false;

    canvas->restoreSessionFromJson(root.value(QStringLiteral("canvas")).toObject());
    return true;
}

QString SessionManager::readLayerName(int layerNumber)
{
    const QJsonObject root = loadSessionObject(layerNumber);
    if (root.isEmpty())
        return QString();
    return root.value(QStringLiteral("layerName")).toString();
}

bool SessionManager::removeSessionFile(int layerNumber)
{
    sessionObjectCache().remove(layerNumber);
    QFile file(sessionFilePath(layerNumber));
    if (!file.exists())
        return true;
    return file.remove();
}

void SessionManager::preloadAllLayers(const QList<int> &layerNumbers)
{
    auto &cache = sessionObjectCache();
    for (int layerNumber : layerNumbers) {
        if (cache.contains(layerNumber))
            continue;

        QFile file(sessionFilePath(layerNumber));
        if (!file.exists())
            continue; // まだ一度も保存されていない新規レイヤー。restore側が空扱いにする。
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "[Session] Failed to open session for preload:" << file.errorString();
            continue;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "[Session] Failed to parse session during preload:" << parseError.errorString();
            continue;
        }
        cache.insert(layerNumber, doc.object());
    }
}

bool SessionManager::updateLayerNameInFile(int layerNumber, const QString &layerName)
{
    QJsonObject root = loadSessionObject(layerNumber);
    if (root.isEmpty()) {
        // まだ一度も保存されていない新規レイヤー。canvas本体が無いので
        // ここでは何もしない（次回save()時にlayerNameごと正しく書かれる）。
        return true;
    }
    root.insert(QStringLiteral("layerName"), layerName);
    return writeSessionObject(layerNumber, root);
}

QStringList SessionManager::collectAllWebImageUrls(const QList<int> &layerNumbers)
{
    QStringList urls;
    for (int layerNumber : layerNumbers) {
        const QJsonObject root = loadSessionObject(layerNumber);
        if (root.isEmpty())
            continue;

        const QJsonObject canvasJson = root.value(QStringLiteral("canvas")).toObject();
        const QJsonArray items = canvasJson.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : items) {
            const QJsonObject entry = v.toObject();
            if (entry.value(QStringLiteral("type")).toString() != QStringLiteral("image"))
                continue;
            const QJsonObject imageJson = entry.value(QStringLiteral("image")).toObject();
            if (imageJson.value(QStringLiteral("kind")).toString() != QStringLiteral("webImage"))
                continue;
            const QString url = imageJson.value(QStringLiteral("sourceUrl")).toString();
            if (!url.isEmpty())
                urls << url;
        }
    }
    return urls;
}

QStringList SessionManager::collectAllWebVideoUrls(const QList<int> &layerNumbers)
{
    const QString webVideoKind = QStringLiteral("webVideo");
    QStringList urls;
    for (int layerNumber : layerNumbers) {
        const QJsonObject root = loadSessionObject(layerNumber);
        if (root.isEmpty())
            continue;

        const QJsonObject canvasJson = root.value(QStringLiteral("canvas")).toObject();
        const QJsonArray items = canvasJson.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : items) {
            const QJsonObject entry = v.toObject();
            if (entry.value(QStringLiteral("type")).toString() != QStringLiteral("video"))
                continue;
            const QJsonObject videoJson = entry.value(QStringLiteral("video")).toObject();
            if (videoJson.value(QStringLiteral("kind")).toString() == webVideoKind) {
                const QString url = videoJson.value(QStringLiteral("sourceUrl")).toString();
                if (!url.isEmpty())
                    urls << url;
            }
            // 本体のリスト（くっつけたバブル）の中のWeb動画も残す。
            const QJsonArray listItems = videoJson.value(QStringLiteral("coreList")).toObject()
                                             .value(QStringLiteral("items")).toArray();
            for (const QJsonValue &lv : listItems) {
                const QJsonObject o = lv.toObject();
                if (o.value(QStringLiteral("k")).toString() != webVideoKind)
                    continue;
                const QString url = o.value(QStringLiteral("u")).toString();
                if (!url.isEmpty())
                    urls << url;
            }
        }
    }
    return urls;
}

QJsonObject SessionManager::loadSessionObject(int layerNumber)
{
    auto &cache = sessionObjectCache();
    const auto cached = cache.constFind(layerNumber);
    if (cached != cache.constEnd())
        return cached.value();

    // 通常はpreloadAllLayers()で起動時に埋まっているはずだが、
    // 新規レイヤー作成直後など未キャッシュのケースに備えたフォールバック。
    QFile file(sessionFilePath(layerNumber));
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[Session] Failed to open session for read:" << file.errorString();
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[Session] Failed to parse session:" << parseError.errorString();
        return {};
    }
    const QJsonObject root = doc.object();
    cache.insert(layerNumber, root);
    return root;
}

bool SessionManager::writeSessionObject(int layerNumber, const QJsonObject &root)
{
    // ディスクと同時にキャッシュも更新しておく。次回このレイヤーへ
    // switchToLayer()で戻ってきた際、restore()がディスクを読み直さず
    // 今書いたばかりのこの内容をそのまま使えるようにするため。
    sessionObjectCache().insert(layerNumber, root);
    const quint64 generation = ++s_nextGeneration;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return writeBytesAtomically(layerNumber, sessionFilePath(layerNumber), bytes, generation);
}
