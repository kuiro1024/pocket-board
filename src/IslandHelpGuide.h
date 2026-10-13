#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QFontMetrics;

// ============================================================
//  IslandHelpGuide（2026-09-18）
//
//  設定の「ヘルプ」タブに出す操作ガイド。設定項目の並び（●／項目名／
//  右端の部品）の規則には従わない、独立した1枚の図として描く。
//
//  構成は2種類:
//
//   1) 引き出し線（特例・最上段）
//      島の＋ボタンから括弧状の線を引いて「このボタンにこの機能がある」と
//      直接示す。画面に実際に写っている物にしか使えない書き方なので、
//      ＋ボタンの2項目だけがこれ。
//
//        ┌─ [マウス図] ↕ [結果]  ホイール上下で音量調整
//        └─ [マウス図]   [結果]  左クリックで設定を開く
//
//   2) ・で区切ったグループ（それ以降）
//
//        ● キャンバス ───────────────
//          ・ [マウス図] ＋ [結果]  ホイールドラッグで移動
//          ・ …
//
//  縦の揃え（ユーザー指定）:
//    - グループ見出しの ● … ＋ボタンの中心（markerX）を通る
//    - 各項目の ・      … ＋ボタンの中心とタブの左端のちょうど中間
//    - マウスの図の左端 … タブの左端（labelX）
//    - 文字は右基準（controlRight へ寄せる）。ウィンドウが狭くなったときに
//      右から文字が消えていかないようにするため。
//
//  マウスの図は「押す場所 × クリック回数」の組み合わせが多く、SVGを
//  その数だけ用意すると破綻するので QPainter で直接描く。補足記号
//  （↕・十字・ドラッグ・長押し）と結果アイコンだけ :/icons/ のSVG。
//
//  記号の約束:
//    青い塗り  = 押す場所（左／右／両方／ホイール）
//    放射1本   = クリック
//    放射2本（大＋小。角度を開ける） = ダブルクリック
//    時計      = 押しっぱなし
//
//  フレーム更新への配慮:
//    完全な静止画。アニメーションもタイマーも持たない。再描画は
//    スクロール・リサイズで Qt が出す paintEvent だけ。アイコンの
//    ラスタは PbSvgIcon がキャッシュするので、描画は貼るだけ。
// ============================================================
namespace IslandHelpGuide {

// マウスのどこを押すか
enum class Press
{
    None,    // 押さない（ホイールを回すだけ等）
    Left,
    Right,
    Both,    // 左右どちらでも可
    Wheel,
};

struct Entry
{
    Press   press  = Press::None;
    int     clicks = 0;          // 0=押す絵を出さない / 1=クリック / 2=ダブルクリック / 3=トリプルクリック
    QString modIcon;             // 入力の補足（:/icons/help_updown.svg 等）。空なら無し
    QString resultIcon;          // 操作の結果（:/icons/res_*.svg）。空なら無し
    QString text;
    // キーボードの行のときだけ使う。1要素＝1つの組み合わせで、
    // 要素内は "+" 区切り、要素どうしは "/" で並べる。
    // 例: { "Ctrl+Shift+Z", "Ctrl+Y" }
    QStringList keys;
    // マウス以外の絵（今は "resize" のみ: バブルの右下のリサイズマーク）
    QString customGlyph;
};

struct Group
{
    QString title;
    QVector<Entry> entries;
};

// ＋ボタンから引き出し線で説明する項目（特例・最上段）
QVector<Entry> plusButtonEntries();
// ・で区切って並べるグループ
QVector<Group> groups();

} // namespace IslandHelpGuide

// ============================================================
//  IslandHelpGuideWidget
// ============================================================
class IslandHelpGuideWidget : public QWidget
{
public:
    explicit IslandHelpGuideWidget(QWidget *parent = nullptr);

    // IslandSettingsPanel::relayoutRows() から渡される揃え線（行ローカル座標）。
    //   markerX      … ＋ボタンの中心
    //   labelX       … タブの左端
    //   controlRight … ×ボタンの中心（右端）
    void setGuides(int markerX, int labelX, int controlRight);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int  contentHeight() const;
    // 文字の列の幅（右基準の列。いちばん長い項目に合わせる）
    int  textColumnWidth(const QFontMetrics &fm, int leftEnd) const;
    void updateHeight();
    // 1行ぶん描く。y は行の上端、h は行の高さ。戻り値は次の行の上端。
    int  paintEntry(QPainter &p, const IslandHelpGuide::Entry &e, int y, int h);

    QVector<IslandHelpGuide::Entry> m_plus;
    QVector<IslandHelpGuide::Group> m_groups;
    int m_markerX = 21;
    int m_labelX = 47;
    int m_controlRight = 320;
    mutable int m_textColumnW = 0;   // textColumnWidth() のキャッシュ
};
