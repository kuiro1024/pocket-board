#pragma once

#include <QObject>
#include <QList>
#include <QSet>
#include <QString>
#include <functional>
#include <memory>

#include "PbPluginApi.h"
#include "PbThumbnailApi.h"
#include "PbPlaylistApi.h"
#include "PbFusionApi.h"
#include "PbLabelActionApi.h"

class QPluginLoader;
class IPbControlProvider;
class PbControlHost;
class PluginConfigStore;

// ============================================================
//  PluginManager
//
//  IPbMediaPlugin 実装 DLL を読み込み、URLに応じたハンドラを
//  検索・呼び出すホスト側の窓口。
//
//  本体コードは PluginManager を通じてのみプラグインと通信し、
//  個々のプラグイン実装（特定の動画サイト用など）を直接知らない。
//  （＝本体は MediaSource だけを扱い、サービス固有処理は知らない）
//
//  【走査対象フォルダ（2箇所・再帰的にスキャン）】
//    1. 実行ファイルと同階層の "Plugins"（開発時・手動配置用）
//    2. %APPDATA%/PocketBoard/PocketBoard/Plugins（ユーザー領域。
//       Program Files配布時など書き込み権限が無い場所でも、
//       .zip ドロップでのプラグイン導入先として使える）
//
//  【フォルダ単位管理（2026-07-13〜）】
//    zipドロップでのインストールは Plugins/<zip名>/ へ展開され、
//    各プラグインが自分のフォルダを持つ。設定値（島コントロール、
//    IPbControlProvider実装プラグインのみ）は同フォルダ内の
//    plugin_config.json に保存される（PluginConfigStore参照）。
//    旧形式（Plugins/直下の裸DLL）は起動時に <DLL名>/ フォルダへ
//    自動移行される（移動失敗時はそのままの場所からロード＝安全側）。
// ============================================================
class PluginManager : public QObject
{
    Q_OBJECT

public:
    // zip内の .dll について、実際にプラグインとして実行(instance()呼び出し)
    // する前に確認できる最低限の情報。
    struct PluginZipEntry
    {
        QString fileName;   // zip内でのファイル名
        QString iid;        // Q_PLUGIN_METADATA(IID ...) に書かれた値
        QStringList keys;   // プラグイン側JSONの "Keys"（通常 pluginId が入る）
        bool iidMatches = false; // 本体が期待する IPbMediaPlugin_iid と一致するか
    };
    struct PluginZipPreview
    {
        bool ok = false;
        QString errorMessage;
        QList<PluginZipEntry> entries;
    };

    static PluginManager &instance();

    // 上記2箇所の Plugins フォルダを再帰的に走査し、未読み込みの .dll を
    // すべて読み込む。既に読み込み済みのファイルはスキップされるため、
    // 何度呼んでも安全（installPluginZip() 後の再読み込みにも使う）。
    void loadPlugins();

    // ユーザー領域のプラグインインストール先
    // （%APPDATA%/PocketBoard/PocketBoard/Plugins）。
    static QString userPluginsDir();

    // zipFilePath を一時フォルダへ展開し、中の .dll のメタデータ（IID・Keys）
    // のみを読み取る。QPluginLoader::metaData() はプラグインをインスタンス化
    // しない（＝プラグイン作者の実コードは実行されない）ため、
    // installPluginZip() で実際に読み込む前の確認用途に使える。
    // ただしOSレベルでのDLLファイルの展開・アクセス自体は発生するため、
    // 完全なサンドボックスではないことに注意。
    static PluginZipPreview previewPluginZip(const QString &zipFilePath);

    // previewPluginZip() の非同期版。
    //
    // 【なぜ非同期版が要るか】展開は PowerShell の Expand-Archive を
    // QProcess で起動して待つ実装で、PowerShellの起動だけで数百ms〜1秒以上
    // かかる。同期版はその間UIスレッドを完全に止めるため、zipをドロップ
    // してから確認UI（島のプロンプト）が出るまで画面が固まり、承認後は
    // 島の閉じアニメーションが途中で止まって見えた。
    //
    // done はメインスレッドで呼ばれる。context が破棄されると呼ばれない
    // （＝コールバック内で context のメンバへ触ってよい）。
    static void previewPluginZipAsync(const QString &zipFilePath, QObject *context,
                                      std::function<void(const PluginZipPreview &)> done);

    // zipFilePath を userPluginsDir()/<zip名>/ へ展開し、直後に読み込む。
    // 成功時は true。失敗時は false を返し、errorMessage があれば詳細を格納する。
    // 呼び出し側（CanvasWidget）は先に previewPluginZip() の内容をユーザーに
    // 提示し、承認を得てからこれを呼ぶこと。
    //
    // 同名フォルダが既にある場合は「更新」扱いで上書き展開する
    // （Expand-Archive -Force は追加・上書きのみで削除はしないため、
    // 既存の plugin_config.json は保持される）。ただし既にロード済みの
    // DLLはWindowsがファイルロックしているため上書きに失敗しうる。
    // その場合はエラーを返す（アプリ再起動後に再ドロップで更新可能）。
    bool installPluginZip(const QString &zipFilePath, QString *errorMessage = nullptr);

    // installPluginZip() の非同期版（理由は previewPluginZipAsync() 参照）。
    // 展開だけが非同期で、DLLの読み込み（loadPluginsFromDir）はメイン
    // スレッドのコールバック内で行う。プラグインのコンストラクタや
    // IslandControlRegistry への登録がワーカースレッドから走らないよう、
    // 非同期にするのは外部プロセスの待ちだけに限っている。
    void installPluginZipAsync(const QString &zipFilePath, QObject *context,
                               std::function<void(bool, const QString &)> done);

    // 読み込み済みプラグイン一覧。
    QList<IPbMediaPlugin *> plugins() const;

    // url を処理できる最初のプラグインを返す（無ければ nullptr）。
    IPbMediaPlugin *findHandler(const QString &url) const;

    // URL担当のメディアプラグインが任意サムネイルAPIも実装している場合に
    // そのproviderを返す。未対応プラグインではnullptr（従来経路を使用）。
    IPbThumbnailProvider *findThumbnailProvider(const QString &url) const;

    // URL担当のメディアプラグインが任意のプレイリストAPI
    // （PbPlaylistApi.h）も実装している場合にそのnavigatorを返す。
    // 未対応プラグインではnullptr（＝ラベルのコントローラは出ない）。
    IPbPlaylistNavigator *findPlaylistNavigator(const QString &url) const;

    // findPlaylistNavigator() の薄いラッパー。navigatorが居なければ
    // valid=false の空コンテキストを返すので、呼び出し側（CanvasVideo）は
    // プラグインの有無を気にしなくてよい。
    PbPlaylistContext playlistContext(const QString &url,
                                      int itemsBefore, int itemsAfter) const;

    // 2026-09-22: バブル融合の可否（PbFusionApi.h）。URL担当のプラグインが
    // IPbFusionPolicyProvider を実装していなければ既定値（すべて受け付ける）。
    PbFusionPolicy fusionPolicy(const QString &url) const;

    // 2026-09-23: ラベルの長押し（PbLabelActionApi.h）。URL担当のプラグインが
    // IPbLabelHoldActionProvider を実装していなければ available=false。
    PbLabelHoldAction labelHoldAction(const QString &url) const;
    // 担当が居なければ false（callback は呼ばない）。
    bool runLabelHoldAction(const QString &url, const PbLabelHoldCallback &callback) const;
    // 2026-09-23: URL担当のプラグインに、戻したラベルの見た目を聞く。
    // 非実装・担当なしなら known=false。
    PbLabelPreview labelPreview(const QString &url) const;

    // url を処理できるプラグインを探し、見つかれば resolve() を呼ぶ。
    // 戻り値は「対応プラグインが見つかり resolve() を呼び出したか」であり、
    // 解決（ストリーム取得）そのものの成否ではない。
    // 成否は callback に渡される PbResolvedMedia::success で判定すること。
    // targetHeight: 0=最高画質、>0ならその値以下で最も高い画質を選ぶ
    // （IPbMediaPlugin::resolve() 参照）。
    bool resolve(const QString &url, int targetHeight, const PbResolveCallback &callback) const;

private:
    // 展開済みフォルダから .dll のメタデータだけを読み、読み終わったら
    // そのフォルダごと消す。previewPluginZip() と previewPluginZipAsync()
    // が共有する（展開が同期か非同期かの違いしかないため）。
    static void scanExtractedPluginZip(const QString &stagingDir,
                                       PluginZipPreview *preview);

    explicit PluginManager(QObject *parent = nullptr);
    PluginManager(const PluginManager &) = delete;
    PluginManager &operator=(const PluginManager &) = delete;

    // dir 以下を再帰的に走査し、見つかった .dll をすべて loadPluginFile() する。
    void loadPluginsFromDir(const QString &dir);

    // 単一の .dll をロードする。既にロード済み・IPbMediaPlugin非実装なら false。
    bool loadPluginFile(const QString &filePath);

    // 旧形式（userPluginsDir()直下の裸DLL）を <DLL名>/ フォルダへ移動する
    // 一回きりのマイグレーション。移動失敗（ロック中等）時はそのまま
    // 残し、従来どおり直下からロードされる（再帰走査のため互換）。
    void migrateFlatUserPlugins();

    struct LoadedPlugin
    {
        QPluginLoader  *loader = nullptr;
        IPbMediaPlugin *plugin = nullptr;
        IPbThumbnailProvider *thumbnailProvider = nullptr;
        // ラベルのコントローラ用（任意実装）。非実装なら nullptr。
        IPbPlaylistNavigator *playlistNavigator = nullptr;
        // バブル融合の可否（任意実装、PbFusionApi.h）。非実装なら nullptr。
        IPbFusionPolicyProvider *fusionPolicy = nullptr;
        // ラベルの長押し（任意実装、PbLabelActionApi.h）。非実装なら nullptr。
        IPbLabelHoldActionProvider *labelHold = nullptr;
        // セッションから戻したラベルの見た目（任意実装、PbLabelActionApi.h）。
        IPbLabelPreviewProvider *labelPreview = nullptr;
        // 島設定パネルへのコントロール提供（任意実装）。非実装なら nullptr。
        IPbControlProvider *controls = nullptr;
        QString filePath;
        // DLLの置かれているフォルダ（= plugin_config.json の置き場所）。
        QString pluginDir;
        // controls != nullptr の場合のみ生成される設定ストア。
        // Registry登録コールバック（ラムダ）が値キャプチャで共有するため
        // shared_ptr で保持する。
        std::shared_ptr<PluginConfigStore> configStore;
        // controls != nullptr の場合のみ生成される、プラグイン→本体方向の
        // 動的状態変更窓口（PbControlHost実装。IPbControlProvider 1.2）。
        // 生ポインタをonControlHostReady()でプラグインへ渡すため、
        // プラグインのロード中（＝アプリ終了まで）所有し続ける。
        std::shared_ptr<PbControlHost> controlHost;
    };

    // IPbControlProvider 実装プラグインのコントロール宣言を検証し、
    // IslandControlRegistry へセクション登録＋保存値の復元通知を行う。
    void registerPluginControls(LoadedPlugin &lp);

    QList<LoadedPlugin> m_plugins;
    QSet<QString> m_loadedPaths;
};
