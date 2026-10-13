#pragma once

#include <QString>

// ============================================================
//  StartupRegistration
//
//  「Windows起動時に自動で起動」（スタートアップ）の登録／解除。
//
//  登録先は HKCU\Software\Microsoft\Windows\CurrentVersion\Run の
//  値 "PocketBoard"（中身: "<exeのフルパス>" --background）。
//  インストーラ（installer/PocketBoard.iss の startup タスク）も
//  同じ値を書くので、オンオフの状態は自前の設定ファイルに持たず
//  毎回レジストリを読んで判定する（どこで切り替えても表示がずれない）。
//
//  Windowsの設定やタスクマネージャーで「無効」にされた場合は Run の値は
//  残ったまま StartupApproved\Run 側に無効フラグが立つので、それも見る。
// ============================================================
namespace StartupRegistration
{
    // 登録済み かつ Windows側で無効化されていないとき true。
    bool isEnabled();

    // true: 現在のexeで登録（Windows側の無効フラグも解除）。
    // false: 登録を削除。成功したら true を返す。
    bool setEnabled(bool enabled);

    // 登録済みで、登録先のexeがもう存在しない場合だけ今のexeへ付け替える
    // （ポータブル版を移動した等の保険。存在するなら触らない）。
    void refreshPathIfRegistered();
}
