#pragma once

// マスターボリューム(0〜100、既定50)の値保持とホイール入力の変換のみを行う
// 純粋なデータ+計算クラス（FocusSessionController/VideoSleepCoordinatorと
// 同様、CanvasWidgetの責務を切り出した小さなヘルパー）。
//
// 各バブル(CanvasVideo)への実際の反映（setMasterGain()呼び出し・
// mpvへのvolumeプロパティ送信）はCanvasWidget側が行う。複数バブルへの
// ループ処理は CanvasWidget::applyMasterGainToAllVideos() に置く方針
// （このクラスは CanvasItem/CanvasVideo を一切知らない）。
//
// 個別バブル音量との関係: 乗算方式。CanvasVideo::applyEffectiveVolume()が
// 「m_userVolume × m_masterGain」を計算してmpvへ渡す
// （2026-09-14: サラウンドゲインの層は廃止した）。
// マスターを動かしても各バブルの個別音量設定(m_userVolume)自体は変化しない。
class MasterVolumeController
{
public:
    // 現在値 (0-100)。
    int value() const { return m_value; }
    // CanvasVideo::setMasterGain()へ渡す倍率 (0.0-1.0)。
    float gain() const { return m_value / 100.0f; }

    // セッション復元時などに直接値を設定する。0-100へクランプする。
    // 実際に値が変化した場合のみtrueを返す（呼び出し側はtrueの時だけ
    // 全バブルへ反映すればよい）。
    bool setValue(int v);

    // 島の+ボタン上でのホイール操作を反映する。ホイール1クレンジ=1%。
    // steps: 正なら音量アップ方向、負ならダウン方向のクレンジ数
    //（通常は±1を渡す想定）。0-100を超えないようクランプする。
    // 実際に値が変化した場合のみtrueを返す。
    bool applyWheelSteps(int steps);

private:
    int m_value {50};
};
