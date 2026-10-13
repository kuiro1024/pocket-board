#pragma once

#include <QtPlugin>
#include <QString>
#include <QVariant>
#include <QList>

#include "PbControlSpec.h"

// ============================================================
//  PbControlApi.h
//
//  島（DynamicIsland）設定パネルへコントロールを追加したいプラグイン用の
//  追加インターフェース。
//
//  【IPbMediaPluginとの関係】
//    IPbMediaPlugin（PbPluginApi.h）とは意図的に別インターフェース・
//    別IIDにしてある。理由:
//      - メディア解決のみの既存プラグイン（動画サイト用プラグイン等）は
//        本ヘッダを知らないまま無改修・再ビルド不要で動き続ける
//      - PbControlSpec のレイアウト変更時は IPbControlProvider_iid だけを
//        上げればよく、IPbMediaPlugin_iid（現在1.6）に影響しない
//    コントロールを持ちたいプラグインは、IPbMediaPlugin と多重継承し
//    Q_INTERFACES に両方を書く（実装例: plugins/DummyTestPlugin）。
//
//  【安全性】
//    プラグインが渡せるのは PbControlSpec の宣言データのみ。本体側は
//    登録時に IslandControlRegistry::validateSpec() で検証し、不正な
//    specは理由をログに出してスキップする。ウィジェット生成・描画・
//    永続化はすべて本体側の責務。
//
//  【永続化】
//    値は本体が各プラグインフォルダ内の plugin_config.json に保存する
//    （PluginConfigStore）。プラグインがこのファイルを直接触る必要は
//    ない（触らないこと。自前の追加ファイルを同フォルダに置くのは自由）。
//
//  【スレッド】
//    以下のコールバックはすべて本体がQtメインスレッド（GUIスレッド）
//    から呼び出す。実装内でブロッキング処理（ネットワーク・重いI/O）を
//    行うと島のUIが固まるため、必要なら実装側で非同期化すること。
// ============================================================
// ============================================================
//  PbControlHost（IID 1.2で追加）
//
//  プラグイン→本体方向の「登録済みコントロールの動的状態変更」窓口。
//  プラグインはホストの内部クラス（IslandControlRegistry等）へ直接
//  アクセスできない（リンクしていない）ため、本体がこの抽象クラスの
//  実装を IPbControlProvider::onControlHostReady() で渡す。
//
//  【できること（意図的にこれだけ）】
//    - コントロールの有効/無効切り替え（例: 処理中はボタンを無効化）
//    - Label / Button の表示文言変更（例: 状態表示 "停止中"→"接続中…"）
//  ウィジェットの追加・削除・種類変更はできない（宣言は
//  controlSpecs() の一度きり、という既存方針は変えない）。
//
//  【規約】
//    - 必ずQtメインスレッドから呼ぶこと（本体側コールバックと同じ規約）
//    - controlId は自プラグインの controlSpecs() で宣言したもののみ有効。
//      未知のIDは本体側が警告ログを出して無視する
//    - 文言は PbControlSpec::label と同じ長さ制限（48文字）。超過分は
//      本体側で切り詰められる
//    - ポインタの寿命は本体側が管理する（プラグインのロード中は常に
//      有効。delete しないこと）
// ============================================================
class PbControlHost
{
public:
    virtual ~PbControlHost() = default;

    // コントロールの有効/無効を切り替える（全kind対象）。
    virtual void setControlEnabled(const QString &controlId, bool enabled) = 0;

    // Label / Button の表示文言を変更する（他kindは本体側で無視される）。
    virtual void setControlLabel(const QString &controlId, const QString &label) = 0;
};

class IPbControlProvider
{
public:
    virtual ~IPbControlProvider() = default;

    // 島に並べたいコントロールの宣言リスト。プラグインのロード時に
    // 一度だけ呼ばれる（動的な増減は現状未対応）。
    virtual QList<PbControlSpec> controlSpecs() const = 0;

    // 値持ちコントロール（Toggle/Slider/SpinBox/Choice）の値がUI操作で
    // 変更された。value の型は PbControlSpec::defaultValue と同じ規約。
    // 呼び出し時点で plugin_config.json への保存は本体側で完了している。
    virtual void onControlValueChanged(const QString &controlId,
                                       const QVariant &value) = 0;

    // Buttonが押された。
    virtual void onControlTriggered(const QString &controlId) = 0;

    // 保存済み設定の復元通知。ロード直後（controlSpecs()の後）に、
    // 値持ちコントロールごとに1回ずつ呼ばれる。値は plugin_config.json の
    // 保存値、無ければ defaultValue（本体側で補完済み）。
    virtual void onControlValueRestored(const QString &controlId,
                                        const QVariant &value) = 0;

    // （IID 1.2で追加）セクション登録・値復元が完了した後に一度だけ
    // 呼ばれ、動的状態変更の窓口（PbControlHost）を渡す。ポインタは
    // プラグインのロード中は常に有効（本体側が所有。deleteしないこと）。
    // 動的更新が不要なプラグインはオーバーライド不要（既定実装は何もしない）。
    virtual void onControlHostReady(PbControlHost *host) { Q_UNUSED(host) }
};

// 【重要・事故防止】PbControlSpec / PbControlOption / 本インターフェースの
// レイアウトを変更するたびに必ず末尾の番号を上げること。qobject_cast は
// プラグイン側 Q_INTERFACES の登録文字列と本体側のこの文字列が完全一致した
// 場合のみ成功するため、番号を上げておけば古いレイアウトのプラグインは
// 「コントロール無し」として安全に無視されるだけになる（メディア解決機能は
// IPbMediaPlugin_iid が一致していればそのまま動く）。
// 改訂履歴:
//   1.0 (2026-07-13): 初版（Toggle/Slider/Button/SpinBox/Choice/Label）
//   1.1 (2026-07-13): PbControlKind::TextInput 追加、PbControlSpec に
//                     placeholder / maxLength フィールド追加
//   1.2 (2026-07-13): PbControlHost（動的な有効/無効・Label/Button文言変更）
//                     追加、IPbControlProvider::onControlHostReady() 追加
//   (1.2のまま) 2026-09-16: PbControlKind::KeyInput を末尾に追加。レイアウトは
//                     不変なので番号は上げていない（既存プラグインを作り直さずに済む）
#define IPbControlProvider_iid "com.pocketboard.IPbControlProvider/1.2"
Q_DECLARE_INTERFACE(IPbControlProvider, IPbControlProvider_iid)
