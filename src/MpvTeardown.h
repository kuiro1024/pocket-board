#pragma once

class QObject;
class MpvPlayer;

// MpvPlayerの破棄と、それに伴うOLEドロップターゲット登録の修復（🚫バグ対策）を
// 必ずセットで行うための一元化ポイント。
//
// 経緯（2026-07-15）: mpv_terminate_destroy()（~MpvPlayer()が呼ぶ）は
// WASAPI関連のCOM破棄処理を内包しており、メインスレッドのOLEアパートメント
// （RegisterDragDropの登録元）を巻き添えにしてD&Dが🚫固定になることがある。
// 従来はMpvPlayerを破棄する箇所ごとに個別でrefreshDropTargets()を呼ぶ運用に
// なっており、実際に呼び忘れ（CanvasVideo::drainPlayersAwaitingGlDelete()）
// が発生した。今後MpvPlayerを破棄する経路を追加・変更する場合は、直接
// `delete player` するのではなく、必ずこの関数を経由すること。
//
// owner: ログ・修復処理に渡すオーナー（CanvasVideo*であることを想定。
//        owner->parent() を辿って CanvasWidget → MainWindow を見つけ、
//        MainWindow::refreshDropTargets() を呼ぶ）。
// player: 破棄対象への参照。呼び出し後は必ず nullptr になる。
//         *player が既に nullptr なら何もしない（多重呼び出し安全）。
// releaseGlBeforeDelete: true の場合、delete の前に
//         player->releaseVideoResources() を呼ぶ。GLコンテキストが
//         current であることを呼び出し元が保証できる場所
//         （paintGL() 内など）でのみ true にすること。
void destroyMpvPlayer(QObject *owner, MpvPlayer *&player, bool releaseGlBeforeDelete = false);
