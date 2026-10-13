#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>
#include <QtPlugin>

// ============================================================
//  PbPlaylistApi.h （2026-09-12追加）
//
//  フォーカスラベル（PbResolvedMedia::labelIconSvg/labelText）を
//  クリックしたときに開く「前後の動画コントローラ」へ、プラグインが
//  中身を供給するための任意実装インターフェース。
//
//  【なぜ IPbMediaPlugin を拡張しないのか】
//  PbResolvedMedia へフィールドを足すと IPbMediaPlugin_iid を上げる
//  必要があり、既存プラグインが全て読み込み拒否になる。サムネイル
//  API（PbThumbnailApi.h）と同じく、独立した任意インターフェースと
//  して足すことで、メディア解決の ABI には一切触れずに済む
//  （未実装のプラグインでは単にコントローラが出ないだけ）。
//
//  【設計方針は PbControlSpec.h と同じ】
//  プラグインは「何を並べるか」だけを返し、描画・レイアウト・
//  クリック判定・アニメーションはすべて本体が行う。プラグインが
//  任意のウィジェットやUIコードを注入することはできない。
//  本体は相変わらず「再生リスト」「Mix」「関連動画」といった
//  サービス固有の概念を一切知らず、「前後に並ぶ項目の列」としか
//  解釈しない。
//
//  【ABI注意】PbResolvedMedia と同じ運用ルール。この構造体の
//  フィールドを追加・削除・型変更・並び替えする場合は、必ず
//  末尾の IPbPlaylistNavigator_iid のバージョン番号を上げること。
// ============================================================

// コントローラに1行として並ぶ項目。
struct PbPlaylistItem
{
    // この項目を再生するためのURL。本体はこれを新しい
    // originalPathOrUrl として resolve() し直すだけで、中身の意味は
    // 解釈しない（PbResolvedMedia::nextUrl と同じ扱い）。
    QString url;

    // 表示用タイトル。空なら本体はURLの一部を代わりに出す。
    QString title;

    // サムネイル画像の取得元URL。画像そのものはここには載せない
    // （50件分の画像を抱え込むとメモリも取得時間も無駄になるため）。
    // 本体はコントローラを開いて実際に画面へ出す数件だけを、
    // 必要になった時点で取りに行く。空なら画像なしで描画する。
    QString thumbnailUrl;

    // 長さ（秒）。0 なら不明（配信等）として表示を省略する。
    int durationSec = 0;
};

// 「今再生しているURLは、どんな並びのどこにいるのか」の回答。
struct PbPlaylistContext
{
    // このURLを並びの一部として認識できたか。false なら本体は
    // コントローラを開かない（ラベルのクリックは何も起こさない）。
    bool valid = false;

    // 並び全体の名前（再生リスト名等）。空でもよい。
    QString listTitle;

    // items の中で「今再生中の項目」が何番目か。-1 は
    // 「前後は分かるが現在位置は特定できていない」。
    int currentIndex = -1;

    // items に載せた範囲の前後に、まだ項目が存在するか
    // （コントローラの「これ以上は無い」表示用。件数は問わない）。
    bool moreBefore = false;
    bool moreAfter  = false;

    // 今は答えられないが、裏取得が進めば後で答えが変わりうるか
    // （PbResolvedMedia::nextUrlPending と同じ考え方）。true の間、
    // 本体はコントローラを開いたまま控えめな間隔で聞き直す。
    bool pending = false;

    // 要求された範囲（itemsBefore/itemsAfter）で切り出した項目の列。
    // 現在再生中の項目自身も含む（currentIndex で指す）。
    QList<PbPlaylistItem> items;
};

// ============================================================
//  IPbPlaylistNavigator
//
//  IPbMediaPlugin 実装クラスが追加で継承する任意インターフェース。
// ============================================================
class IPbPlaylistNavigator
{
public:
    virtual ~IPbPlaylistNavigator() = default;

    // 【最重要・この呼び出しは同期かつノンブロッキングであること】
    //
    // 本体はコントローラを開いている間、UIスレッドからこれを繰り返し
    // 呼ぶ。ネットワークアクセス・外部プロセス起動・ファイルI/Oの
    // 待ち合わせは絶対に行わず、既に手元にあるキャッシュだけで即座に
    // 返すこと（ここで数秒ブロックすると、その間アプリ全体のUIが
    // 固まる）。
    //
    // まだ分からない場合は valid=false / pending=true を返し、実際の
    // 取得はプラグイン側の都合のよいタイミングで裏で進めればよい。
    // 本体は pending の間だけ控えめに聞き直す。
    //
    // itemsBefore / itemsAfter: 現在の項目から前後それぞれ何件まで
    // 欲しいか。手元にそれだけ無ければ、有る分だけ返してよい。
    virtual PbPlaylistContext playlistContext(const QString &url,
                                              int itemsBefore,
                                              int itemsAfter) const = 0;
};

#define IPbPlaylistNavigator_iid "com.pocketboard.IPbPlaylistNavigator/1.0"
Q_DECLARE_INTERFACE(IPbPlaylistNavigator, IPbPlaylistNavigator_iid)
