#pragma once

#include <QString>
#include <QtPlugin>

// ============================================================
//  PbFusionApi.h （2026-09-22追加）
//
//  バブル融合（バブルを別のバブルに重ねて離すと、相手のリストに入る。
//  本体の BubbleFusion.h）を、プラグインが「このメディアでは受け付けない」
//  と断るための任意実装インターフェース。
//
//  【なぜ IPbMediaPlugin / PbResolvedMedia を拡張しないのか】
//  PbPlaylistApi.h / PbThumbnailApi.h と同じ理由。PbResolvedMedia へ
//  フィールドを足すと IPbMediaPlugin_iid を上げる必要があり、既存の
//  プラグインがすべて読み込み拒否になる。独立した任意インターフェース
//  にしておけば、未実装のプラグインは「すべて受け付ける」として
//  そのまま動く。
//
//  【設計方針】
//  本体は理由を知らない。「Mix だから」「配信だから」といったサービス
//  固有の判断はすべてプラグインの中で完結させ、本体には可否だけを返す。
//  断られたときの見せ方（重ねても反応しない等）は本体が決める。
//
//  【ABI注意】PbFusionPolicy のフィールドを追加・削除・型変更・並び替え
//  する場合は、必ず末尾の IPbFusionPolicyProvider_iid の番号を上げること。
// ============================================================

struct PbFusionPolicy
{
    // 他のバブルを重ねて、このバブルのリストへ入れてよいか（受け側として）。
    // 例: 件数や中身をサービスが自動で決めるリスト（Mix）は、利用者が
    // 足した動画と混ざると意味が変わるので false にする。
    bool acceptsIncoming = true;

    // このバブルを他のバブルへ重ねて、相手のリストへ入れてよいか
    // （吸われる側として）。
    // 例: 終わりの無い配信（Live）はリストの1項目として並べられないので false。
    bool canJoinOthers = true;
};

// ============================================================
//  IPbFusionPolicyProvider
//
//  IPbMediaPlugin 実装クラスが追加で継承する任意インターフェース。
// ============================================================
class IPbFusionPolicyProvider
{
public:
    virtual ~IPbFusionPolicyProvider() = default;

    // 【最重要・この呼び出しは同期かつノンブロッキングであること】
    //
    // 本体はバブルを重ねている間、UIスレッドからこれを毎フレーム呼ぶ
    // ことがある。ネットワークアクセス・外部プロセス起動・ファイルI/Oは
    // 絶対に行わず、URLの形と手元のキャッシュだけで即座に返すこと。
    //
    // url: そのバブルが今再生している元のURL（PbResolvedMedia を得たときの
    // originalPathOrUrl。リストを辿って別の動画へ移っていれば移動先のURL）。
    // 分からない場合は既定値（すべて受け付ける）を返してよい。
    virtual PbFusionPolicy fusionPolicy(const QString &url) const = 0;
};

#define IPbFusionPolicyProvider_iid "com.pocketboard.IPbFusionPolicyProvider/1.0"
Q_DECLARE_INTERFACE(IPbFusionPolicyProvider, IPbFusionPolicyProvider_iid)
