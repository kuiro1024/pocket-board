#pragma once

#include <QString>

// 2026-07-12: Web画像URLの「直接画像として取得できるURLかどうか」の判定を
// まとめた小さな独立ユーティリティ。元々はCanvasWidget.cpp内の匿名名前空間に
// 直書きしていたが、サイトごとの癖（Discordの拡張子付きURL、Google画像検索の
// imgres?imgurl=...ラッパー、Twitter/Xのformat=クエリ等）に対応するたびに
// 関数が増えていく性質があり、CanvasWidget.cpp自体は既にchordbrushup計画
// （md/chordbrushup.md）で分割対象になっている巨大ファイルなので、ここへ
// 切り出した（WebImageCache.h/cppと同じ方針）。
//
// 新しいサイトの特殊ケースが見つかった場合は、このファイルに判定を追加
// していく想定。
namespace WebImageUrlResolver
{
    // URLのパス部分の拡張子だけを見た単純な直接画像URL判定
    // （.png/.jpg/.jpeg/.gif/.webp）。
    bool isLikelyImageUrl(const QString &rawUrl);

    // isLikelyImageUrl()に加えて、以下も試す統合判定:
    // - ラッパーリンク展開（Google画像検索の imgres?imgurl=... 等、
    //   クエリパラメータの中に実URLが埋め込まれているパターン）
    // - format=/fm= クエリヒント（Twitter/X の pbs.twimg.com 等、
    //   拡張子がパスではなくクエリパラメータで示されるパターン。
    //   この場合はURL自体が既に実URLなのでそのまま返す）
    // 直接画像として扱えるURLが得られればそれを返し、得られなければ空文字
    // （＝画像URLとしては扱わない）。
    QString resolveDirectImageUrl(const QString &rawUrl);
}
