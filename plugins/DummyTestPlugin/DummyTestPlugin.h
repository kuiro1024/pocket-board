#pragma once

#include <QObject>

#include "plugin/PbPluginApi.h"
#include "plugin/PbControlApi.h"

// ============================================================
//  DummyTestPlugin
//
//  Phase 8 Plugin API の動作確認専用のダミー実装。
//
//  実サービス（動画サイト等）は一切解決しない。
//  独自の "pbtest://" スキームのみを canHandle() し、
//  非同期遅延の後にダミーの解決結果を返す。
//
//  これにより以下を検証できる：
//    1. QPluginLoader が Plugins/ フォルダの DLL を正しく読み込めるか
//    2. qobject_cast<IPbMediaPlugin*> が成功するか
//    3. CanvasWidget::tryAddPluginMedia() → CanvasVideo::resolveViaPlugin()
//       → PluginManager::resolve() → 本プラグインの resolve() という
//       一連の非同期解決フローが機能するか
//
//  2026-07-13: プラグインコントロールAPI（IPbControlProvider）の疎通
//  確認用に、本体が用意する全コントロール型（Toggle/Slider/SpinBox/
//  Choice/Button）を1つずつ宣言する実装を追加。値の変更・復元・
//  ボタン押下はすべて qDebug ログに出るだけ（実機能なし）。
//  これにより以下を検証できる：
//    4. qobject_cast<IPbControlProvider*> が成功するか
//    5. 島設定パネルにプラグインセクション（見出し＋コントロール）が
//       自動レイアウトで表示され、高さが自動拡張／スクロールするか
//    6. 値が Plugins/<フォルダ>/plugin_config.json へ保存され、
//       再起動後に onControlValueRestored() で復元されるか
// ============================================================
class DummyTestPlugin : public QObject, public IPbMediaPlugin, public IPbControlProvider
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID IPbMediaPlugin_iid FILE "DummyTestPlugin.json")
    Q_INTERFACES(IPbMediaPlugin IPbControlProvider)

public:
    explicit DummyTestPlugin(QObject *parent = nullptr);

    // ---- IPbMediaPlugin ----
    QString pluginId() const override;
    QString displayName() const override;
    QString pluginVersion() const override;
    bool canHandle(const QString &url) const override;
    void resolve(const QString &url, int targetHeight, const PbResolveCallback &callback) override;

    // ---- IPbControlProvider ----
    QList<PbControlSpec> controlSpecs() const override;
    void onControlValueChanged(const QString &controlId, const QVariant &value) override;
    void onControlTriggered(const QString &controlId) override;
    void onControlValueRestored(const QString &controlId, const QVariant &value) override;
};
