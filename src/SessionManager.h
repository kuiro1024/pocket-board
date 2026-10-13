#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class CanvasWidget;

// ---- SessionManager --------------------------------------------
// レイヤー機能（Layer.md Phase A）: 保存ファイルは常に`session_N.json`
// という統一形式で扱う（`session.json`という無印ファイルは廃止）。
// Nは「一度割り当てたら使い捨て」のレイヤー番号（LayerManifest参照）。
// ここでは番号を受け取って該当ファイルを読み書きするだけで、番号自体の
// 割り当て・並び順・名前の一覧管理はLayerManifestの責務。
// ------------------------------------------------------------------
class SessionManager
{
public:
    static QString sessionFilePath(int layerNumber);
    // layerNameはLayerManifest側のマニフェストと二重に保存する
    // （マニフェストだけ壊れても名前が失われないようにするための冗長化）。
    static bool save(CanvasWidget *canvas, int layerNumber, const QString &layerName);
    // 2026-10-12: 自動保存用。JSONの組み立てだけ呼び出し元（GUIスレッド）で
    // 行い、文字列化とディスク書き込みは専用の1本のワーカーで行う。
    // 中身が前回書いたものと同じなら書かない。save()（同期）と混ざっても、
    // 後から要求された方が必ず勝つ（世代番号で古い書き込みを捨てる）。
    static void saveAsync(CanvasWidget *canvas, int layerNumber, const QString &layerName);
    static bool restore(CanvasWidget *canvas, int layerNumber);
    // session_N.json内に二重保存された"layerName"を読み出す。
    // ファイルが無い/キーが無い場合は空文字を返す
    // （マニフェスト破損時のフォールバック復旧用）。
    static QString readLayerName(int layerNumber);
    // 指定レイヤーのセッションファイルを削除する（レイヤー削除操作用）。
    static bool removeSessionFile(int layerNumber);
    // session_N.json内の二重保存分"layerName"だけを更新する（canvas本体には
    // 触れない）。レイヤー名変更操作で、対象が非アクティブレイヤー（＝canvas
    // 側の最新状態を持っていない）場合に使う。ファイルがまだ存在しない
    // （一度も保存されていない新規レイヤー）場合は何もせずtrueを返す。
    static bool updateLayerNameInFile(int layerNumber, const QString &layerName);

    // 2026-07-11: 起動時に全レイヤーのsession_N.jsonをまとめて読み込み、
    // メモリキャッシュへ乗せておく（Layer.md）。以後のsave()/restore()等は
    // このキャッシュを介するため、レイヤー切り替えのたびにディスクI/O＋
    // JSONパースが走らなくなる。実際にCanvasItem/サムネイル等の「重い」
    // 処理を組み立てる部分（restoreSessionFromJson側）は従来通り切り替え
    // 時に行われる——ここで減らすのはあくまでファイルI/O＋パース分のみ。
    // 既にキャッシュ済みの番号はスキップする（何度呼んでも安全）。
    static void preloadAllLayers(const QList<int> &layerNumbers);

    // 2026-07-12: webimageキャッシュの起動時ガベージコレクション用。
    // 全レイヤーのsession_N.json（既にpreloadAllLayers()でキャッシュ済み
    // なので追加のディスクI/Oは無い）からtype=="image"かつkind=="webImage"の
    // sourceUrlだけを集めて返す。
    static QStringList collectAllWebImageUrls(const QList<int> &layerNumbers);

    // 2026-10-12: webvideoキャッシュの起動時ガベージコレクション用。
    // type=="video" かつ kind=="webVideo" の sourceUrl と、動画バブルの
    // 本体のリスト（coreList）に入っている webVideo のURLを集めて返す。
    static QStringList collectAllWebVideoUrls(const QList<int> &layerNumbers);

private:
    static QJsonObject loadSessionObject(int layerNumber);
    static bool writeSessionObject(int layerNumber, const QJsonObject &root);
};
