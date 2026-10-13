#pragma once

#include <QString>
#include <functional>

class IPbMediaPlugin;
struct PbResolvedMedia;

// CanvasVideo::resolveViaPlugin() が保持する、プラグイン解決失敗時の状態
// （Phase 7 Step 2）。元は個別メンバ変数(m_pluginResolveFailed 等)だった
// ものを1箇所にまとめただけで、意味・初期値は変更していない。
// 実際の PluginManager::resolve() 呼び出し自体はまだ CanvasVideo 側に
// 残っている（このファイルは今のところデータ置き場のみ。Phase 7 Step 3で
// resolve()呼び出しのwrapperをここへ追加する予定）。
struct PluginResolveFailure
{
    bool failed {false};
    // ログ用の詳細メッセージ。画面にはこのソフトの方針上出さず、
    // 3桁エラーコードのみ表示する（CanvasVideo::paintResolveFailedOverlay()参照）。
    QString message;
    int errorCode {0};

    // 新しい解決を始める前に呼ぶ（前回の失敗表示をクリアする）。
    void clear()
    {
        failed = false;
        message.clear();
        errorCode = 0;
    }

    // 解決失敗時に呼ぶ。
    void set(const QString &msg, int code)
    {
        failed = true;
        message = msg;
        errorCode = code;
    }
};

// CanvasVideo::resolveViaPlugin() が使っていた PluginManager 呼び出しの
// ラッパー（Phase 7 Step 3）。findHandler()/resolve() の実装自体は
// PluginManager / IPbMediaPlugin 側にそのまま残っており、ここは呼び出しを
// 1段間接化しているだけ。コールバックが呼ばれるスレッドの前提
// （呼び出し元と同じスレッド、Plugin ABI 側の既存契約）は一切変えていない。
// onUrlFetched() へ合流させる結果処理そのものは、計画書の方針
// （「onUrlFetchedは最後までCanvasVideo側に残してよい」）通り、
// 引き続き CanvasVideo::resolveViaPlugin() 側のコールバックが担当する。
class PluginMediaResolver
{
public:
    // PluginManager::instance().findHandler(url) をそのまま呼ぶだけ。
    static IPbMediaPlugin *findHandler(const QString &url);

    // plugin->resolve(url, quality, callback) をそのまま呼ぶだけ。
    static void resolve(IPbMediaPlugin *plugin, const QString &url, int quality,
                        const std::function<void(const PbResolvedMedia &)> &callback);
};
