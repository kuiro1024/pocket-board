#pragma once

#include <QString>
#include <QList>
#include <QVariant>

// ============================================================
//  PbControlSpec.h
//
//  島（DynamicIsland）設定パネルに並べる「コントロール」の宣言構造体。
//  本体組み込みの設定項目と、プラグイン（IPbControlProvider）由来の
//  項目の両方がこの型で表現される。
//
//  【設計方針】
//    - プラグインはこの「宣言」を返すだけで、QWidgetの生成・スタイル・
//      配置・永続化はすべて本体側（IslandSettingsPanel /
//      IslandControlRegistry / PluginConfigStore）が行う。
//      プラグインが任意のウィジェットやUIコードを注入することはできない
//      （安全性と見た目の一貫性の担保）。
//    - PbPluginApi.h と同様、このヘッダは Qt6::Core のみに依存する
//      （QWidget/QColor等のGui/Widgets型は使わない）。
//
//  【ABI注意（PbResolvedMediaと同じ運用ルール）】
//    この構造体はプラグインDLLと本体の間でメモリレイアウトを直接
//    やり取りする型＝IPbControlProvider_iid が保証するABIの一部。
//    フィールドを追加・削除・型変更・並び替えする場合は、必ず
//    PbControlApi.h の IPbControlProvider_iid のバージョン番号を上げること
//    （例: "1.0" → "1.1"）。番号を上げ忘れた場合の症状・理由は
//    PbPluginApi.h 末尾のIID定義部コメントを参照。
// ============================================================

// コントロールの型。本体が用意したこの型でしかコントロールは作れない。
// 将来値を追加する場合は既存の値を変更せず末尾に追加すること
// （PbResolveErrorCode と同じルール。並び順が変わると古いプラグインの
// kind値の意味が変わってしまう）。
enum class PbControlKind
{
    Toggle,     // チェックボックス（値: bool）
    Slider,     // 水平スライダー（値: int、minValue/maxValue/step）
    Button,     // ただのボタン（値なし。押下イベントのみ）
    SpinBox,    // 数値入力（値: int、minValue/maxValue/step。▲▼ステッパー付き）
    Choice,     // コンボボックス（値: 選択された PbControlOption::id 文字列）
    Label,      // 表示のみのテキスト（値なし。説明・注記用）
    TextInput,  // 1行テキスト入力（値: QString。placeholder/maxLength対応。
                //   確定はEnterまたはフォーカスアウト時。IID 1.1で追加）
    KeyInput,   // キー入力欄（値: int＝Windows仮想キーコード、空欄は0。見た目は
                //   TextInput と同じで、欄を押してからキーを1つ押すとそのキーが入る。
                //   Backspace/Delete で空、Esc で入力終了。2026-09-16追加。
                //   構造体のレイアウトは変わらないので IID は据え置き。この値を
                //   知らない古い本体では、このコントロールだけが登録時に弾かれる）
};

// Choice用の選択肢1件。
struct PbControlOption
{
    QString id;     // 保存に使う安定ID（plugin_config.json にはこれが入る）
    QString label;  // 表示名
};

// コントロール1件の宣言。
struct PbControlSpec
{
    // セクション（プラグイン）内で一意なID。plugin_config.json の
    // 保存キーになるため、プラグインのバージョンを跨いで安定させること。
    QString id;

    // 表示名（英語推奨。島UIは文字を控えめにする方針のため短く）。
    QString label;

    PbControlKind kind = PbControlKind::Toggle;

    // 初期値。kindごとの期待型:
    //   Toggle: bool / Slider・SpinBox: int / Choice: option id (QString)
    //   Button・Label: 不使用
    // 未設定（invalid）の場合は本体側が既定値を補う
    // （Toggle=false、Slider/SpinBox=minValue、Choice=先頭のoption id）。
    QVariant defaultValue;

    // Slider / SpinBox 用の範囲。
    int minValue = 0;
    int maxValue = 100;
    int step     = 1;

    // Choice 用の選択肢（Choiceでは1件以上必須）。
    QList<PbControlOption> options;

    // ツールチップ（省略可）。
    QString tooltip;

    // trueにすると直前のコントロールと同じ行に配置される
    // （例: "Show Grid" チェックボックスの右に "Grid Size" スピンボックス、
    //  TextInput の右に "Connect" ボタン、ボタン3つを1行に等分割）。
    // 連結できるのは1行あたり最大3個まで。4連続以上を指定した場合、
    // 4つ目は自動的に次の行の先頭になる。行の先頭コントロールでは
    // このフラグは無視される。列幅は等分割。
    bool sameRowAsPrevious = false;

    // ---- TextInput 用（IID 1.1で追加） ----

    // 未入力時に薄く表示されるヒント文字列（例: "192.168.1.5"）。
    QString placeholder;

    // 最大入力文字数。0（既定）は無制限。
    int maxLength = 0;
};
