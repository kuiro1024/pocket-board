#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariant>
#include <functional>

#include "plugin/PbControlSpec.h"

// ============================================================
//  IslandControlRegistry
//
//  島（DynamicIsland）設定パネルに表示するコントロールの中央登録所。
//  本体組み込みの設定項目（MainWindow::registerIslandSettingsControls()）と
//  プラグイン由来の項目（PluginManager::registerPluginControls()）の
//  両方が「セクション」単位でここに登録され、IslandSettingsPanel が
//  sections() を読んでUIを組み立てる。
//
//  登録・変更があると sectionsChanged() が発火し、パネルが開いていれば
//  再構築＋高さ再計算される（DynamicIsland::ensureSettingsPanel() 参照）。
//
//  【値の流れ】
//    UI操作 → IslandControlEntry::onValueChanged(value)
//      - 本体項目: AppSettings / CanvasWidget のsetterへ（従来と同じ）
//      - プラグイン項目: PluginConfigStore::setValue() で保存
//        → IPbControlProvider::onControlValueChanged() へ通知
//    UI構築時の現在値は currentValue()（無ければ spec.defaultValue）。
// ============================================================

// コントロール1件 = 宣言(spec) + 本体側の接続コールバック。
struct IslandControlEntry
{
    PbControlSpec spec;

    // UI構築時に現在値を得る。null可（その場合 spec.defaultValue を使う）。
    std::function<QVariant()> currentValue;

    // 値持ちコントロール（Toggle/Slider/SpinBox/Choice）の変更通知先。
    std::function<void(const QVariant &)> onValueChanged;

    // Button の押下通知先。
    std::function<void()> onTriggered;

    // 2026-09-16: Button の「ボタンに書く文字」。設定パネルは項目名（spec.label）を
    // 行の左、ボタンを右に置くようになったので、両者を別の文言にできる
    // （例: 項目名「呼出しショートカットキー」／ボタン「ショートカット設定」）。
    // 空なら既定の文言（「設定…」）。本体側だけの項目で、プラグインの宣言
    // （PbControlSpec）には影響しない。
    QString buttonText;

    // 2026-09-16: TextInput / KeyInput を横に並べる欄の数（本体側だけの項目）。
    // 2以上なら値は QVariantList（欄ごとに TextInput は QString、KeyInput は int）。
    int columns = 1;

    // 2026-09-16: KeyInput の欄に出すキーの名前（仮想キーコード → 表示名）。
    // null なら IslandControls::defaultKeyName()（英字・数字・F1〜・修飾キー等）。
    std::function<QString(int)> keyName;
};

// 2026-09-15: 設定画面のカテゴリ。島の設定パネルをウィンドウ全体へ展開する
// UI（カテゴリごとに項目をまとめて表示）へ移行するための分類。
// 並び順＝表示順。プラグイン由来のセクションは常に Plugins。
enum class IslandControlCategory
{
    Appearance,  // 見た目（グラフィック）
    Behavior,    // 操作・動作
    Other,       // その他
    // 2026-09-18: 操作ガイド専用のタブ。設定項目（セクション）は持たず、
    // IslandHelpGuide のデータを表示するだけ。ここに登録するものは無い。
    Help,        // ヘルプ
    Plugins,     // プラグイン
};

struct IslandControlSection
{
    // 本体: "host.<name>"（hostSectionId() で作る） / プラグイン: pluginId
    QString id;
    QString title;  // セクション見出し（空なら見出しなし）
    IslandControlCategory category = IslandControlCategory::Plugins;
    QList<IslandControlEntry> entries;
};

// 2026-07-13 (PbControlHost / IPbControlProvider 1.2):
// 登録済みコントロール1件に対する「実行時の状態上書き」。
// spec（宣言）自体は不変のまま、有効/無効と表示文言だけを後から
// 変更できるようにする（例: プラグインの状態表示Label、処理中の
// ボタン無効化）。値はここ（Registry）が保持するため、パネルを
// 閉じて開き直しても（rebuild()されても）最後の状態が反映される。
struct IslandControlState
{
    bool    hasEnabled = false;
    bool    enabled    = true;
    bool    hasLabel   = false;
    QString label;
    // 2026-09-16: 行ごと隠す（例: 「Windowsのカラー設定を使用」がオンの間は
    // 「カラー設定」を出さない）。
    bool    hasVisible = false;
    bool    visible    = true;
};

class IslandControlRegistry : public QObject
{
    Q_OBJECT

public:
    // 本体セクションの予約ID。2026-09-15: 本体項目をカテゴリ別の複数
    // セクションへ分けたため、"host.<name>" の形で名前を付ける。
    // "host" で始まるIDは本体予約（isHostSectionId()）。
    static QString hostSectionId(const QString &name)
    { return QStringLiteral("host.") + name; }
    static bool isHostSectionId(const QString &id)
    { return id == QLatin1String("host") || id.startsWith(QLatin1String("host.")); }

    // 1セクションあたりのコントロール数上限（プラグインの暴走対策。
    // 超過分は登録時に警告ログを出して切り捨てる）。
    static constexpr int maxControlsPerSection() { return 12; }

    static IslandControlRegistry &instance();

    // specの静的検証。falseの場合 why に理由（ログ用・英語）を格納する。
    // 未知のkind・min>max・Choiceでoptions空・id/label不正などを弾く。
    static bool validateSpec(const PbControlSpec &spec, QString *why = nullptr);

    // セクションを登録する。同じidのセクションが既にあれば置き換える
    // （プラグイン再インストール時など）。不正なentryは警告ログを出して
    // スキップし、有効なentryが1つも無ければ登録せず false を返す。
    // defaultValue が未設定のentryはここで既定値を補完する
    // （Toggle=false / Slider・SpinBox=minValue / Choice=先頭option id）。
    bool registerSection(IslandControlSection section);

    // セクションを取り除く（現状呼び出し箇所なし。プラグインの
    // アンインストール機能を作る際に使う想定）。
    void unregisterSection(const QString &sectionId);

    // 表示順のセクション一覧。本体("host.*")が常に先頭（本体同士は登録順）、
    // 以降はプラグインの登録順。
    QList<IslandControlSection> sections() const { return m_sections; }

    // ---- 実行時の状態上書き（PbControlHost経由。2026-07-13追加）----
    // 対象コントロールが未登録の場合は警告ログを出して無視する。
    // label は PbControlSpec::label と同じ48文字制限へ切り詰める。
    // メインスレッドから呼ぶこと（Registry全体の既存前提と同じ）。
    void setControlEnabled(const QString &sectionId, const QString &controlId, bool enabled);
    void setControlLabel(const QString &sectionId, const QString &controlId, const QString &label);
    void setControlVisible(const QString &sectionId, const QString &controlId, bool visible);

    // 2026-09-16: 表示中の部品の値だけを差し替える（例: 色のH/S/Vスライダーと
    // カラーコード欄を連動させる）。部品の onValueChanged は呼ばない（連動の往復を防ぐ）。
    // 値そのものは保持しない。パネルを作り直すときは各 entry の currentValue が正。
    void pushControlValue(const QString &sectionId, const QString &controlId, const QVariant &value);

    // 現在の状態上書き（無ければ既定値＝上書きなし）。
    // IslandSettingsPanel がウィジェット構築時と controlStateChanged 受信時に読む。
    IslandControlState controlState(const QString &sectionId, const QString &controlId) const;

signals:
    // セクションの登録・置換・削除があった（パネル再構築のトリガー）。
    void sectionsChanged();

    // 登録済みコントロールの実行時状態（有効/無効・文言）が変わった。
    // パネルが開いていれば該当ウィジェットのみをその場更新する
    // （rebuild()はしない。クリック処理中のボタン破棄やスクロール位置の
    // リセットを避けるため）。
    void controlStateChanged(const QString &sectionId, const QString &controlId);

    // pushControlValue() の通知。開いているパネルの該当部品だけを差し替える。
    void controlValuePushed(const QString &sectionId, const QString &controlId, const QVariant &value);

private:
    explicit IslandControlRegistry(QObject *parent = nullptr);
    IslandControlRegistry(const IslandControlRegistry &) = delete;
    IslandControlRegistry &operator=(const IslandControlRegistry &) = delete;

    // sectionId+controlId が登録済みか（状態上書きの対象検証用）。
    bool hasControl(const QString &sectionId, const QString &controlId) const;

    QList<IslandControlSection> m_sections;

    // 実行時の状態上書き。キー: sectionId → (controlId → state)。
    // registerSection() で同一セクションが置き換えられた時はリセットする。
    QHash<QString, QHash<QString, IslandControlState>> m_controlStates;
};
