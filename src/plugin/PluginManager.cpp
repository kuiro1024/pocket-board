#include "PluginManager.h"
#include "PbControlApi.h"
#include "PluginConfigStore.h"
#include "IslandControlRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLibrary>
#include <QPluginLoader>
#include <QProcess>
#include <QTimer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>
#include <QDebug>
#include <utility>

#include <qt_windows.h>

namespace {

// プラグインへ渡す PbControlHost 実装（IPbControlProvider 1.2）。
// sectionId（=pluginId）を固定で埋め込むことで、プラグインは自分の
// セクションのコントロールしか操作できない（他プラグイン・本体項目の
// 状態は変更不可）。実処理は IslandControlRegistry へ委譲するだけ。
class PluginControlHostAdapter : public PbControlHost
{
public:
    explicit PluginControlHostAdapter(const QString &sectionId)
        : m_sectionId(sectionId)
    {
    }

    void setControlEnabled(const QString &controlId, bool enabled) override
    {
        IslandControlRegistry::instance().setControlEnabled(m_sectionId, controlId, enabled);
    }

    void setControlLabel(const QString &controlId, const QString &label) override
    {
        IslandControlRegistry::instance().setControlLabel(m_sectionId, controlId, label);
    }

private:
    QString m_sectionId;
};

// PowerShell の Expand-Archive でzipをdestDirへ展開する共通ヘルパー。
// installPluginZip() と previewPluginZip() の両方から使う。
bool extractZipViaPowerShell(const QString &zipFilePath, const QString &destDir, QString *errorMessage)
{
    auto fail = [errorMessage](const QString &msg) {
        if (errorMessage)
            *errorMessage = msg;
        return false;
    };

    if (!QDir().mkpath(destDir))
        return fail(QStringLiteral("failed to create directory: %1").arg(destDir));

    auto psQuote = [](QString s) {
        return s.replace(QLatin1Char('\''), QStringLiteral("''"));
    };
    const QString command = QStringLiteral("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
        .arg(psQuote(zipFilePath), psQuote(destDir));

    QProcess proc;
    proc.start(QStringLiteral("powershell"), {
        QStringLiteral("-NoProfile"),
        QStringLiteral("-NonInteractive"),
        QStringLiteral("-Command"),
        command
    });
    if (!proc.waitForStarted(5000))
        return fail(QStringLiteral("failed to start powershell (Expand-Archive)"));
    if (!proc.waitForFinished(15000)) {
        proc.kill();
        return fail(QStringLiteral("plugin extraction timed out"));
    }
    if (proc.exitCode() != 0) {
        const QString stderrText = QString::fromLocal8Bit(proc.readAllStandardError());
        return fail(QStringLiteral("Expand-Archive failed: %1").arg(stderrText.left(300)));
    }
    return true;
}

// 上の非同期版。UIスレッドを止めずに展開する。
//
// done は必ずメインスレッドで、かつ**ちょうど1回**呼ばれる。
// finished / errorOccurred / タイムアウトのどれが先に来ても、最初の1回
// だけが通るよう done をムーブで奪ってから呼ぶ（QProcessは失敗時に
// errorOccurred と finished の両方を出すことがある）。
// context が破棄された場合は呼ばれない（Qt::AutoConnection + context）。
void extractZipViaPowerShellAsync(const QString &zipFilePath, const QString &destDir,
                                  QObject *context,
                                  std::function<void(bool, const QString &)> done)
{
    const auto finishNow = [](std::shared_ptr<std::function<void(bool, const QString &)>> slot,
                              bool ok, const QString &err) {
        if (!slot || !*slot)
            return;
        auto callback = std::move(*slot);
        *slot = nullptr;            // 2回目以降は素通し
        callback(ok, err);
    };

    auto slot = std::make_shared<std::function<void(bool, const QString &)>>(std::move(done));

    if (!QDir().mkpath(destDir)) {
        finishNow(slot, false,
                  QStringLiteral("failed to create directory: %1").arg(destDir));
        return;
    }

    auto psQuote = [](QString str) {
        return str.replace(QLatin1Char('\''), QStringLiteral("''"));
    };
    const QString command = QStringLiteral("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
        .arg(psQuote(zipFilePath), psQuote(destDir));

    // 親を context にしておくと、context が先に消えたときプロセスオブジェクトも
    // 一緒に片付く（残ったQProcessがデストラクタで待ちに入るのを避ける）。
    auto *proc = new QProcess(context);
    auto *timeout = new QTimer(proc);
    timeout->setSingleShot(true);
    timeout->setInterval(15000);   // 同期版と同じ上限

    QObject::connect(timeout, &QTimer::timeout, proc, [proc, slot, finishNow]() {
        proc->kill();
        finishNow(slot, false, QStringLiteral("plugin extraction timed out"));
        proc->deleteLater();
    });

    QObject::connect(proc, &QProcess::errorOccurred, context,
                     [proc, slot, finishNow](QProcess::ProcessError) {
        finishNow(slot, false,
                  QStringLiteral("failed to start powershell (Expand-Archive)"));
        proc->deleteLater();
    });

    QObject::connect(proc, &QProcess::finished, context,
                     [proc, slot, finishNow](int exitCode, QProcess::ExitStatus) {
        if (exitCode != 0) {
            const QString stderrText =
                QString::fromLocal8Bit(proc->readAllStandardError());
            finishNow(slot, false,
                      QStringLiteral("Expand-Archive failed: %1").arg(stderrText.left(300)));
        } else {
            finishNow(slot, true, QString());
        }
        proc->deleteLater();
    });

    timeout->start();
    proc->start(QStringLiteral("powershell"), {
        QStringLiteral("-NoProfile"),
        QStringLiteral("-NonInteractive"),
        QStringLiteral("-Command"),
        command
    });
}

// zipファイル名からプラグインフォルダ名を作る。Windowsのファイル名として
// 不正な文字・前後の空白/ドットを除去する（zip名は基本的にそのまま通る想定。
// 全滅した場合のみ "plugin" にフォールバック）。
QString sanitizedPluginFolderName(const QString &zipFilePath)
{
    QString name = QFileInfo(zipFilePath).completeBaseName();
    name.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
    name = name.trimmed();
    while (name.endsWith(QLatin1Char('.')))
        name.chop(1);
    if (name.isEmpty())
        name = QStringLiteral("plugin");
    return name;
}

} // namespace

PluginManager &PluginManager::instance()
{
    static PluginManager s_instance;
    return s_instance;
}

PluginManager::PluginManager(QObject *parent)
    : QObject(parent)
{
}

QString PluginManager::userPluginsDir()
{
    // QStandardPaths::AppDataLocation は組織名/アプリ名
    // （main.cpp で共に "PocketBoard" に設定済み）に基づいた
    // 書き込み可能なユーザー領域を返す（例:
    // C:/Users/<user>/AppData/Roaming/PocketBoard/PocketBoard）。
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("Plugins"));
}

void PluginManager::loadPlugins()
{
    const QString exeDir  = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Plugins"));
    const QString userDir = userPluginsDir();

    // 旧形式（userDir直下の裸DLL）をフォルダ単位管理へ移行してから走査する。
    // exe横のPlugins/（開発用・手動配置）は移行対象外（勝手に動かさない）。
    migrateFlatUserPlugins();

    // 2026-08-22: userDir（Roaming、ユーザーがZIPドロップでインストール/
    // 更新した版）を exeDir（exe横、開発用の手動配置や配布物への同梱分）
    // より先に読み込む。loadPluginFile() 側の「同一プラグインIDは先に
    // 読み込んだ方を正とし、後から来た同IDは重複スキップする」仕組みは
    // そのまま活かし、走査の順序だけを入れ替えることで
    // 「ユーザーが入れたプラグインは常に優先される」を実現する。
    // Roaming側に該当プラグインが無ければ、従来通りexeDir側が使われる。
    loadPluginsFromDir(userDir);
    loadPluginsFromDir(exeDir);

    qDebug() << "[Plugin] total plugins loaded:" << m_plugins.size();
}

void PluginManager::migrateFlatUserPlugins()
{
    QDir root(userPluginsDir());
    if (!root.exists())
        return;

    const QFileInfoList flatDlls =
        root.entryInfoList(QStringList{QStringLiteral("*.dll")}, QDir::Files);
    for (const QFileInfo &fi : flatDlls) {
        const QString folderName = fi.completeBaseName();
        if (folderName.isEmpty())
            continue;
        if (!root.mkpath(folderName)) {
            qWarning().noquote()
                << QStringLiteral("[Plugin] migration: failed to create folder %1, "
                                  "loading %2 from flat location")
                       .arg(root.filePath(folderName), fi.fileName());
            continue;
        }
        const QString newPath = QDir(root.filePath(folderName)).filePath(fi.fileName());
        if (QFile::exists(newPath)) {
            // 移行先に同名DLLが既にある（新形式で再インストール済み等）。
            // 直下の旧DLLを消すのはリスクがある（ロック・ユーザー資産）ため
            // 触らずログのみ。m_loadedPathsの先勝ちにより新形式側が先に
            // ロードされるよう、walk順は loadPluginsFromDir() の再帰任せに
            // せず、ここでは何もしない。
            qWarning().noquote()
                << QStringLiteral("[Plugin] migration: %1 already exists, leaving flat DLL as-is")
                       .arg(newPath);
            continue;
        }
        if (QFile::rename(fi.absoluteFilePath(), newPath)) {
            qDebug().noquote()
                << QStringLiteral("[Plugin] migrated to folder layout: %1 -> %2")
                       .arg(fi.fileName(), newPath);
        } else {
            // ロック中・アクセス権等。従来どおり直下からロードされるだけ
            // なので安全（次回起動時に再試行される）。
            qWarning().noquote()
                << QStringLiteral("[Plugin] migration: failed to move %1 (locked?), "
                                  "loading from flat location")
                       .arg(fi.fileName());
        }
    }
}

void PluginManager::loadPluginsFromDir(const QString &dir)
{
    QDir d(dir);
    if (!d.exists()) {
        qDebug().noquote() << QStringLiteral("[Plugin] directory not found, skip: %1").arg(dir);
        return;
    }

    // サブフォルダも含めて再帰的に走査する（zip展開で
    // Plugins/SomePlugin/SomePlugin.dll のようなネストが生じても拾える）。
    QDirIterator it(dir, QStringList{QStringLiteral("*.dll")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        loadPluginFile(QDir::cleanPath(it.next()));
}

bool PluginManager::loadPluginFile(const QString &filePath)
{
    if (m_loadedPaths.contains(filePath))
        return false; // 既に読み込み済み

    if (!QLibrary::isLibrary(filePath))
        return false;

    auto *loader = new QPluginLoader(filePath, this);

    // Q_PLUGIN_METADATA can be inspected without constructing the plugin.
    // Skip a declared duplicate before instance() so an older duplicate DLL
    // cannot start sidecars, network requests, or other constructor work that
    // its destructor may not know how to stop safely.
    const QJsonObject loaderMetaData = loader->metaData();
    const QJsonArray declaredKeys =
        loaderMetaData.value(QStringLiteral("MetaData")).toObject()
            .value(QStringLiteral("Keys")).toArray();
    for (const QJsonValue &declaredKeyValue : declaredKeys) {
        const QString declaredKey = declaredKeyValue.toString();
        if (declaredKey.isEmpty())
            continue;
        for (const LoadedPlugin &loaded : std::as_const(m_plugins)) {
            if (!loaded.plugin || loaded.plugin->pluginId() != declaredKey)
                continue;
            qWarning().noquote() << QStringLiteral(
                "[Plugin] duplicate metadata key skipped before construction: "
                "%1 from %2 (already loaded from %3)")
                    .arg(declaredKey, filePath, loaded.filePath);
            loader->deleteLater();
            m_loadedPaths.insert(filePath);
            return false;
        }
    }

    // 2026-07-13 バグ修正: プラグインが追加のQtモジュール等（例:
    // ScreenSharePluginのQt6WebSockets.dll）に依存している場合、
    // QPluginLoaderは内部で素のLoadLibrary()を使うため（qlibrary_win.cpp）、
    // 依存DLLは「exeフォルダ→System32→PATH」からしか解決されず、
    // プラグインDLL自身のフォルダは検索されない。開発環境ではPATHに
    // C:\msys64\mingw64\bin が入っているため偶然解決されて気づけないが、
    // Setupインストール環境ではロード自体が失敗し「プラグインが黙って
    // 存在しない」状態になっていた。SetDllDirectory()でプラグインの
    // フォルダをロード中だけ標準検索順へ差し込み、依存DLLをプラグイン
    // フォルダへ同梱すれば自己完結で動くようにする（本体配布物へ
    // プラグイン専用DLLを混ぜない方針を守るため、本体側でこの1点だけ
    // 面倒を見る）。
    const QString depDir = QDir::toNativeSeparators(QFileInfo(filePath).absolutePath());
    SetDllDirectoryW(reinterpret_cast<const wchar_t *>(depDir.utf16()));
    QObject *instance = loader->instance();
    SetDllDirectoryW(nullptr); // 検索順を元へ戻す（呼び出しごとに都度設定する）

    if (!instance) {
        qWarning().noquote() << QStringLiteral("[Plugin] failed to load: %1 (%2)")
                                    .arg(filePath, loader->errorString());
        loader->deleteLater();
        return false;
    }

    auto *plugin = qobject_cast<IPbMediaPlugin *>(instance);
    if (!plugin) {
        qWarning().noquote() << QStringLiteral("[Plugin] does not implement IPbMediaPlugin: %1").arg(filePath);
        loader->unload();
        loader->deleteLater();
        return false;
    }

    // 開発用のexe横Pluginsとユーザー領域Pluginsに同じpluginIdのDLLが
    // 同時に存在する場合、両方を常駐させるとprovider・QProcess・
    // QNetworkAccessManager等まで二重生成される。走査順はexe横→ユーザー
    // 領域なので、先にロードした実装を優先し、後続の重複を安全に破棄する。
    for (const LoadedPlugin &loaded : std::as_const(m_plugins)) {
        if (loaded.plugin && loaded.plugin->pluginId() == plugin->pluginId()) {
            qWarning().noquote() << QStringLiteral(
                "[Plugin] duplicate pluginId skipped: %1 from %2 (already loaded from %3)")
                    .arg(plugin->pluginId(), filePath, loaded.filePath);
            loader->unload();
            loader->deleteLater();
            m_loadedPaths.insert(filePath);
            return false;
        }
    }

    qDebug().noquote() << QStringLiteral("[Plugin] loaded: %1 (%2 v%3) from %4")
                              .arg(plugin->pluginId(), plugin->displayName(),
                                   plugin->pluginVersion(), filePath);

    LoadedPlugin lp;
    lp.loader = loader;
    lp.plugin = plugin;
    lp.thumbnailProvider = qobject_cast<IPbThumbnailProvider *>(instance);
    // ラベルのコントローラ（IPbPlaylistNavigator）も任意実装。
    // IIDが一致しない古いプラグインは「コントローラ無し」として安全に
    // 無視される（メディア解決自体はIPbMediaPlugin_iidだけで動く）。
    lp.playlistNavigator = qobject_cast<IPbPlaylistNavigator *>(instance);
    // 2026-09-22: バブル融合の可否（IPbFusionPolicyProvider）も任意実装。
    lp.fusionPolicy = qobject_cast<IPbFusionPolicyProvider *>(instance);
    // 2026-09-23: ラベルの長押し（IPbLabelHoldActionProvider）も任意実装。
    lp.labelHold = qobject_cast<IPbLabelHoldActionProvider *>(instance);
    lp.labelPreview = qobject_cast<IPbLabelPreviewProvider *>(instance);
    lp.filePath = filePath;
    lp.pluginDir = QFileInfo(filePath).absolutePath();

    // 島設定パネルへのコントロール提供（IPbControlProvider）は任意実装。
    // qobject_cast は Q_INTERFACES のIID文字列（IPbControlProvider_iid）が
    // 本体側と完全一致した場合のみ成功するため、古いレイアウトで
    // ビルドされたプラグインは「コントロール無し」として安全に無視される
    // （メディア解決機能はIPbMediaPlugin_iidが合っていればそのまま動く）。
    lp.controls = qobject_cast<IPbControlProvider *>(instance);
    if (lp.controls)
        registerPluginControls(lp);

    // 2026-09-23: 任意インターフェースがどれだけ噛み合ったかを起動時に1行で出す。
    // 噛み合わない＝そのプラグインが古いヘッダーでビルドされている（iidが違う）。
    // 「長押しが出ない」等の切り分けを、これ1行でできるようにするため。
    qInfo().noquote() << QStringLiteral(
        "[Plugin] %1 optional interfaces: thumbnail=%2 playlist=%3 fusion=%4 "
        "labelHold=%5 controls=%6 labelPreview=%7")
        .arg(lp.plugin ? lp.plugin->pluginId() : QStringLiteral("(unknown)"),
             lp.thumbnailProvider ? QStringLiteral("yes") : QStringLiteral("no"),
             lp.playlistNavigator ? QStringLiteral("yes") : QStringLiteral("no"),
             lp.fusionPolicy ? QStringLiteral("yes") : QStringLiteral("no"),
             lp.labelHold ? QStringLiteral("yes") : QStringLiteral("no"),
             lp.controls ? QStringLiteral("yes") : QStringLiteral("no"),
             lp.labelPreview ? QStringLiteral("yes") : QStringLiteral("no"));

    m_plugins.append(lp);
    m_loadedPaths.insert(filePath);
    return true;
}

void PluginManager::registerPluginControls(LoadedPlugin &lp)
{
    const QString pluginId = lp.plugin->pluginId();
    auto store = std::make_shared<PluginConfigStore>(lp.pluginDir, pluginId);
    lp.configStore = store;

    IPbControlProvider *provider = lp.controls;
    const QList<PbControlSpec> specs = provider->controlSpecs();

    IslandControlSection section;
    section.id = pluginId;
    section.title = lp.plugin->displayName();

    for (const PbControlSpec &spec : specs) {
        IslandControlEntry entry;
        entry.spec = spec;
        const QString controlId = spec.id;
        if (spec.kind == PbControlKind::Button) {
            entry.onTriggered = [provider, controlId] {
                provider->onControlTriggered(controlId);
            };
        } else if (spec.kind != PbControlKind::Label) {
            // 値持ちコントロール: 現在値はplugin_config.jsonの保存値
            // （無ければdefaultValue。defaultValueはRegistry登録時に
            // normalizeされるが、store読み出しのfallbackにはspecの生値を
            // 渡す。invalidな場合もRegistryが補完済みのdefaultValueを
            // UI構築側で使うため問題ない）。
            entry.currentValue = [store, spec] {
                return store->value(spec.id, spec.defaultValue);
            };
            entry.onValueChanged = [provider, store, controlId](const QVariant &v) {
                store->setValue(controlId, v);
                provider->onControlValueChanged(controlId, v);
            };
        }
        section.entries.append(entry);
    }

    // registerSection() 内で不正specの除外・上限適用・defaultValue補完が
    // 行われる。復元通知は「実際に登録されたentry」に対してのみ行う。
    if (!IslandControlRegistry::instance().registerSection(section))
        return;

    const QList<IslandControlSection> sections =
        IslandControlRegistry::instance().sections();
    for (const IslandControlSection &registered : sections) {
        if (registered.id != pluginId)
            continue;
        for (const IslandControlEntry &entry : registered.entries) {
            if (entry.spec.kind == PbControlKind::Button
                || entry.spec.kind == PbControlKind::Label)
                continue;
            provider->onControlValueRestored(
                entry.spec.id, store->value(entry.spec.id, entry.spec.defaultValue));
        }
        break;
    }

    // 登録・復元が完了した後に、動的状態変更の窓口を渡す
    // （IPbControlProvider 1.2。プラグインはここで初期の有効/無効・
    // 状態Label文言を設定できる。旧規約のプラグインはこの呼び出しを
    // 既定実装＝no-opで無視するだけ）。
    lp.controlHost = std::make_shared<PluginControlHostAdapter>(pluginId);
    provider->onControlHostReady(lp.controlHost.get());
}

PluginManager::PluginZipPreview PluginManager::previewPluginZip(const QString &zipFilePath)
{
    PluginZipPreview preview;

    const QFileInfo zipInfo(zipFilePath);
    if (!zipInfo.exists() || zipInfo.suffix().compare(QStringLiteral("zip"), Qt::CaseInsensitive) != 0) {
        preview.errorMessage = QStringLiteral("not a .zip file: %1").arg(zipFilePath);
        return preview;
    }

    const QString stagingDir = QDir(QDir::tempPath()).filePath(
        QStringLiteral("PocketBoardPluginPreview_%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    QString extractError;
    if (!extractZipViaPowerShell(zipFilePath, stagingDir, &extractError)) {
        preview.errorMessage = extractError;
        QDir(stagingDir).removeRecursively();
        return preview;
    }

    scanExtractedPluginZip(stagingDir, &preview);
    return preview;
}

// 展開済みフォルダから .dll のメタデータだけを読む（同期版・非同期版で共有）。
// 読み終わったら stagingDir ごと消す。
//
// QPluginLoader::metaData() は Q_PLUGIN_METADATA に埋め込まれたJSONを
// 読み取るだけで、instance()（＝プラグインの実際のコンストラクタ呼び出し）
// は行わない。そのためこの時点ではプラグイン作者のコードは実行されない
// （ただしOSがDLLファイルを開いてメタデータ領域を読む動作自体は発生する）。
void PluginManager::scanExtractedPluginZip(const QString &stagingDir,
                                           PluginZipPreview *preview)
{
    QDirIterator it(stagingDir, QStringList{QStringLiteral("*.dll")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString filePath = it.next();
        if (!QLibrary::isLibrary(filePath))
            continue;

        QPluginLoader loader(filePath);
        const QJsonObject meta = loader.metaData();

        PluginZipEntry entry;
        entry.fileName = QFileInfo(filePath).fileName();
        entry.iid = meta.value(QStringLiteral("IID")).toString();
        const QJsonObject metaDataObj = meta.value(QStringLiteral("MetaData")).toObject();
        for (const QJsonValue &k : metaDataObj.value(QStringLiteral("Keys")).toArray())
            entry.keys << k.toString();
        entry.iidMatches = (entry.iid == QStringLiteral(IPbMediaPlugin_iid));
        preview->entries.append(entry);
    }

    QDir(stagingDir).removeRecursively();

    preview->ok = !preview->entries.isEmpty();
    if (!preview->ok && preview->errorMessage.isEmpty())
        preview->errorMessage = QStringLiteral("no .dll found in zip");
}

void PluginManager::previewPluginZipAsync(const QString &zipFilePath, QObject *context,
                                          std::function<void(const PluginZipPreview &)> done)
{
    PluginZipPreview preview;

    const QFileInfo zipInfo(zipFilePath);
    if (!zipInfo.exists() || zipInfo.suffix().compare(QStringLiteral("zip"), Qt::CaseInsensitive) != 0) {
        preview.errorMessage = QStringLiteral("not a .zip file: %1").arg(zipFilePath);
        if (done)
            done(preview);
        return;
    }

    const QString stagingDir = QDir(QDir::tempPath()).filePath(
        QStringLiteral("PocketBoardPluginPreview_%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    extractZipViaPowerShellAsync(zipFilePath, stagingDir, context,
                                 [stagingDir, done](bool ok, const QString &err) {
        PluginZipPreview result;
        if (!ok) {
            result.errorMessage = err;
            QDir(stagingDir).removeRecursively();
        } else {
            scanExtractedPluginZip(stagingDir, &result);
        }
        if (done)
            done(result);
    });
}

bool PluginManager::installPluginZip(const QString &zipFilePath, QString *errorMessage)
{
    auto fail = [errorMessage](const QString &msg) {
        if (errorMessage)
            *errorMessage = msg;
        qWarning().noquote() << QStringLiteral("[Plugin] install failed: %1").arg(msg);
        return false;
    };

    const QFileInfo zipInfo(zipFilePath);
    if (!zipInfo.exists() || zipInfo.suffix().compare(QStringLiteral("zip"), Qt::CaseInsensitive) != 0)
        return fail(QStringLiteral("not a .zip file: %1").arg(zipFilePath));

    // 2026-07-13: フォルダ単位管理。Plugins/直下ではなく Plugins/<zip名>/ へ
    // 展開し、各プラグインが自分のフォルダ（＝plugin_config.jsonの置き場所）
    // を持つ。同名フォルダが既にあれば「更新」扱いの上書き展開
    // （Expand-Archive -Force は追加・上書きのみで削除はしないため、
    // 既存の plugin_config.json は保持される）。
    const QString destDir =
        QDir(userPluginsDir()).filePath(sanitizedPluginFolderName(zipFilePath));
    qDebug().noquote() << QStringLiteral("[Plugin] installing package: %1 -> %2").arg(zipFilePath, destDir);

    // 追加の外部展開ライブラリ(QuaZip等)への依存を避けるため、
    // Windows標準のPowerShell Expand-Archiveでzipを展開する。
    QString extractError;
    if (!extractZipViaPowerShell(zipFilePath, destDir, &extractError))
        return fail(extractError);

    qDebug().noquote() << QStringLiteral("[Plugin] package extracted, reloading plugins from %1").arg(destDir);
    loadPluginsFromDir(destDir);
    qDebug() << "[Plugin] total plugins loaded:" << m_plugins.size();
    return true;
}

void PluginManager::installPluginZipAsync(const QString &zipFilePath, QObject *context,
                                          std::function<void(bool, const QString &)> done)
{
    const QFileInfo zipInfo(zipFilePath);
    if (!zipInfo.exists() || zipInfo.suffix().compare(QStringLiteral("zip"), Qt::CaseInsensitive) != 0) {
        if (done)
            done(false, QStringLiteral("not a .zip file: %1").arg(zipFilePath));
        return;
    }

    const QString destDir =
        QDir(userPluginsDir()).filePath(sanitizedPluginFolderName(zipFilePath));
    qDebug().noquote() << QStringLiteral("[Plugin] installing package: %1 -> %2").arg(zipFilePath, destDir);

    extractZipViaPowerShellAsync(zipFilePath, destDir, context,
                                 [this, destDir, done](bool ok, const QString &err) {
        if (!ok) {
            qWarning().noquote() << QStringLiteral("[Plugin] install failed: %1").arg(err);
            if (done)
                done(false, err);
            return;
        }
        // ここはメインスレッド。DLLの読み込み・プラグインの生成・
        // IslandControlRegistry への登録は必ずこちらで行う。
        qDebug().noquote() << QStringLiteral("[Plugin] package extracted, reloading plugins from %1").arg(destDir);
        loadPluginsFromDir(destDir);
        qDebug() << "[Plugin] total plugins loaded:" << m_plugins.size();
        if (done)
            done(true, QString());
    });
}

QList<IPbMediaPlugin *> PluginManager::plugins() const
{
    QList<IPbMediaPlugin *> result;
    result.reserve(m_plugins.size());
    for (const auto &p : m_plugins)
        result.append(p.plugin);
    return result;
}

IPbMediaPlugin *PluginManager::findHandler(const QString &url) const
{
    for (const auto &p : m_plugins) {
        if (p.plugin && p.plugin->canHandle(url))
            return p.plugin;
    }
    return nullptr;
}

IPbThumbnailProvider *PluginManager::findThumbnailProvider(const QString &url) const
{
    for (const auto &p : m_plugins) {
        // The first canHandle() match is the media resolver selected by
        // findHandler(). Do not skip a legacy handler and accidentally borrow
        // a thumbnail provider from a later, different plugin.
        if (p.plugin && p.plugin->canHandle(url))
            return p.thumbnailProvider;
    }
    return nullptr;
}

IPbPlaylistNavigator *PluginManager::findPlaylistNavigator(const QString &url) const
{
    for (const auto &p : m_plugins) {
        // findThumbnailProvider()と同じ理由で、最初のcanHandle()一致
        // （＝findHandler()が選ぶプラグイン）だけを見る。別プラグインの
        // navigatorを借りると、担当外のURLの前後情報を答えてしまう。
        if (p.plugin && p.plugin->canHandle(url))
            return p.playlistNavigator;
    }
    return nullptr;
}

PbPlaylistContext PluginManager::playlistContext(const QString &url,
                                                 int itemsBefore, int itemsAfter) const
{
    IPbPlaylistNavigator *nav = findPlaylistNavigator(url);
    if (!nav)
        return PbPlaylistContext();
    return nav->playlistContext(url, itemsBefore, itemsAfter);
}

PbFusionPolicy PluginManager::fusionPolicy(const QString &url) const
{
    for (const auto &p : m_plugins) {
        // findPlaylistNavigator()と同じく、最初のcanHandle()一致
        // （＝このURLを解決したプラグイン）だけに聞く。
        if (p.plugin && p.plugin->canHandle(url))
            return p.fusionPolicy ? p.fusionPolicy->fusionPolicy(url) : PbFusionPolicy();
    }
    return PbFusionPolicy();
}

PbLabelHoldAction PluginManager::labelHoldAction(const QString &url) const
{
    for (const auto &p : m_plugins) {
        // fusionPolicy() と同じく、最初の canHandle() 一致だけに聞く。
        if (p.plugin && p.plugin->canHandle(url))
            return p.labelHold ? p.labelHold->labelHoldAction(url) : PbLabelHoldAction();
    }
    return PbLabelHoldAction();
}

PbLabelPreview PluginManager::labelPreview(const QString &url) const
{
    for (const auto &p : m_plugins) {
        if (p.plugin && p.plugin->canHandle(url))
            return p.labelPreview ? p.labelPreview->labelPreview(url) : PbLabelPreview();
    }
    return PbLabelPreview();
}

bool PluginManager::runLabelHoldAction(const QString &url,
                                       const PbLabelHoldCallback &callback) const
{
    for (const auto &p : m_plugins) {
        if (p.plugin && p.plugin->canHandle(url)) {
            if (!p.labelHold)
                return false;
            p.labelHold->runLabelHoldAction(url, callback);
            return true;
        }
    }
    return false;
}

bool PluginManager::resolve(const QString &url, int targetHeight, const PbResolveCallback &callback) const
{
    IPbMediaPlugin *plugin = findHandler(url);
    if (!plugin)
        return false;

    qDebug().noquote() << QStringLiteral("[Plugin] resolving via %1: %2 (targetHeight=%3)")
                              .arg(plugin->pluginId(), url).arg(targetHeight);
    plugin->resolve(url, targetHeight, callback);
    return true;
}
