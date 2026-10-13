#pragma once

#include <QString>
#include <QByteArray>
#include <QtPlugin>
#include <functional>

// ============================================================
//  PbLabelActionApi.h （2026-09-23追加）
//
//  ラベルの長押し。ラベルを押し続けると、台の縁に沿って時計回りに
//  ゲージがたまり、たまりきると「何か」が起きる。何が起きるかは
//  そのメディアの持ち主が決める:
//    本体（ローカル動画） … 同じフォルダの動画を並べて List にする
//    プラグイン           … 例: 単発の動画を関連動画のリストに切り替える
//
//  本体は長押しの操作・ゲージ・失敗時の戻り・元に戻すを受け持ち、
//  中身（どんなURLへ変わるか・なぜ変わるか）は一切知らない。
//
//  【なぜ IPbMediaPlugin を拡張しないのか】
//  PbFusionApi.h と同じ理由。独立した任意インターフェースにしておけば、
//  未実装のプラグインは「長押しは無し」としてそのまま動く。
//
//  【ABI注意】構造体のフィールドを追加・削除・型変更・並び替えする場合は、
//  必ず末尾の IPbLabelHoldActionProvider_iid の番号を上げること。
// ============================================================

// 2026-09-23: 長押しが押しきられたとき、誰が動作を持つか。
// 本体は「長押しという操作」と「自分の動作を1つ」だけを持ち、どれを使うかは
// 担当プラグインが決める（プラグインごとに扱いを変えない。本体はサービスを
// 一切知らない）。担当プラグインがいないメディア（ローカルのファイル）は、
// 聞く相手がいないので本体の動作になる。
enum class PbLabelHoldActionKind
{
    None = 0,   // 長押しを出さない
    Core,       // 本体が持っている動作を使う（今は「同じフォルダのものを並べる」）
    Plugin      // このプラグインの動作を使う（runLabelHoldAction が呼ばれる）
};

struct PbLabelHoldAction
{
    PbLabelHoldActionKind kind = PbLabelHoldActionKind::None;
    // カーソルを置いたときに、ラベルの下へ小さく出す言葉（例: "Mix にする"）。
    // 空なら本体が自分の言葉を出す（kind=Core のときは常に本体の言葉）。
    QString hint;
    // 2026-09-23: 押しきったら、このラベルが何に変わるか（例: "Mix"）。
    // ゲージをためている間、ラベルの文字が今の語からこの語へ入れ替わっていく
    // （途中でやめれば戻る）。空なら入れ替えの演出をしない。
    // ここに入れるのは変わった後に実際に出る語と同じものにすること。
    QString targetLabel;
};

struct PbLabelHoldResult
{
    bool success = false;
    // 成功したとき、このバブルで開き直すURL。本体は今の再生位置を保ったまま
    // このURLへ切り替える（元に戻すと元のURLへ戻る）。
    QString newUrl;
    // 失敗したときの理由（ログ用。本体は画面に文章は出さず、ゲージを
    // 震わせて戻すだけ）。
    QString errorMessage;

    // 2026-09-23: 再生を止めずにURLだけ差し替えてよいか。
    // 「同じものを再生したまま、意味づけ（前後の並び）だけが変わる」場合に
    // true。例: 単発 → 関連動画のリスト は、再生しているストリームは同じ動画の
    // ままで、後ろに続きが付くだけ。false（既定）なら本体はそのURLを開き直す
    // （今の再生位置は保つが、読み込みのぶん一瞬止まる）。
    bool keepPlaying = false;

    // keepPlaying=true のときに本体へ渡す新しいラベル。意味は
    // PbResolvedMedia の同名フィールドと同じ（開き直さないので、解決の
    // 答えとしてラベルを渡す機会がここしか無い）。
    QByteArray labelIconSvg;
    QString    labelText;
    bool       labelHoverable = false;
    bool       labelPinnable  = false;
    bool       labelProgress  = false;
};

using PbLabelHoldCallback = std::function<void(const PbLabelHoldResult &)>;

// ============================================================
//  IPbLabelHoldActionProvider
//
//  IPbMediaPlugin 実装クラスが追加で継承する任意インターフェース。
// ============================================================
class IPbLabelHoldActionProvider
{
public:
    virtual ~IPbLabelHoldActionProvider() = default;

    // 【同期・ノンブロッキング】ラベルを描くたび・押した瞬間に呼ばれる。
    // kind=Core を返せば本体の動作をそのまま使える（自分では何もしなくてよい）。
    // ネットワークアクセス・外部プロセス起動・ファイルI/Oは行わず、URLの形と
    // 手元のキャッシュだけで即座に返すこと。
    virtual PbLabelHoldAction labelHoldAction(const QString &url) const = 0;

    // ゲージがたまりきったときに1回だけ呼ばれる。時間がかかってよい
    // （その間ゲージは満タンのまま待つ）。callback は必ず1回、UIスレッドで
    // 呼ぶこと。本体は一定時間で待つのをやめるので、遅れて呼ばれても害は無い。
    virtual void runLabelHoldAction(const QString &url,
                                    const PbLabelHoldCallback &callback) = 0;
};

// 【ABI注意】構造体を変えたら必ず番号を上げること。古いヘッダーでビルドされた
// プラグインは構造体の大きさが違い、そのまま呼ぶとメモリを壊す（実機でクラッシュ）。
// 番号を上げておけば qobject_cast が一致せず「長押し非対応のプラグイン」として
// 安全に無視される。
//   1.0 → 1.1  PbLabelHoldResult に keepPlaying とラベルの各項目（2026-09-23）
//   1.1 → 1.2  PbLabelHoldAction::available を kind（None/Core/Plugin）へ（2026-09-23）
//   1.2 → 1.3  PbLabelHoldAction::targetLabel を追加（2026-09-23）
#define IPbLabelHoldActionProvider_iid "com.pocketboard.IPbLabelHoldActionProvider/1.3"
Q_DECLARE_INTERFACE(IPbLabelHoldActionProvider, IPbLabelHoldActionProvider_iid)

// ============================================================
//  IPbLabelPreviewProvider（2026-09-23追加、任意実装）
//
//  セッションから戻したバブルのラベルを、解決（再生）を待たずに今の
//  プラグインの見た目に合わせるためのもの。本体はラベルのアイコンを
//  セッションへ保存して起動直後に出しているが、それは保存した時点の
//  プラグインの絵なので、プラグインがアイコンを変えても一度再生するまで
//  古い絵のままになっていた（例: Mix のアイコンを ▶ → ♪ に変えたとき）。
//
//  URLの形だけで分かる範囲の見た目を同期で返す。分からなければ
//  known=false（本体は保存していた絵をそのまま使う）。
//  テキスト（Live など解決しないと分からないもの）はここでは扱わない。
// ============================================================
struct PbLabelPreview
{
    bool known = false;
    QByteArray labelIconSvg;
};

class IPbLabelPreviewProvider
{
public:
    virtual ~IPbLabelPreviewProvider() = default;
    // 【同期・ノンブロッキング】URLの形と手元のキャッシュだけで答えること。
    virtual PbLabelPreview labelPreview(const QString &url) const = 0;
};

// 【ABI注意】構造体を変えたら必ず番号を上げること。
#define IPbLabelPreviewProvider_iid "com.pocketboard.IPbLabelPreviewProvider/1.0"
Q_DECLARE_INTERFACE(IPbLabelPreviewProvider, IPbLabelPreviewProvider_iid)
