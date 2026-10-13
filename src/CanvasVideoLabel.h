#pragma once

#include <QByteArray>
#include <QImage>
#include <QRectF>
#include <QString>

class QPainter;

// Paints the optional media-provider label in the window-space QWidget
// overlay. The rasterized SVG is retained per video and rebuilt only when the
// provider changes it.
class CanvasVideoLabelPainter
{
public:
    // 描いたピル(背景)の矩形を返す。ピルを描かなかった場合は空の矩形。
    // 呼び出し側がその背後をぼかすために使う。
    // drawPill=false にすると台（背景）を描かず、ロゴとテキストだけを
    // 描く。ラベルがコントローラのカードへ伸びていく演出のとき、台は
    // CanvasVideo側が「伸びる1枚の板」として描くため、二重に描かないよう
    // にするための切り替え。
    // textShiftX: テキストだけを右へずらす量(px)。ロゴは動かさない。
    // リストが開くとき、テキスト（と鍵マーク）を板の右端へ寄せて中央を
    // 空け、そこへ簡易コントロールを置くために使う（CanvasVideo側が
    // 「板の右端 − ラベルの右端」を渡すので、伸びる動きにそのまま追従する）。
    // compactAnim: 0=通常（ロゴ＋テキスト）、1=最小（ロゴと丸い台だけ）。
    // ウィンドウが狭くて島（DynamicIsland）とぶつかるとき、ラベルに場所を
    // 譲らせるための縮退。台の幅が高さと同じ＝真円まで縮み、テキストは
    // 台からはみ出さないようクリップされつつ消える。
    // 台そのものは CanvasVideo 側が描くことが多い（drawPill=false）ので、
    // あちらへ渡す矩形にも同じ式で縮めた幅を使うこと（CanvasVideo.cpp の
    // labelRect 参照）。ずれると台とロゴの位置が合わなくなる。
    static double compactPillWidth(double fullWidth, double pillHeight,
                                   double compactAnim);

    // morphText/morphAmount: 2026-09-23。長押しでゲージをためている間、
    // 今の語（labelText）から morphText へ文字を入れ替えていく。
    // 0=今の語だけ、1=入れ替え後の語だけ。台の幅は両方が収まる幅で固定する
    // ので、入れ替えの最中に形が動かない。
    QRectF paint(QPainter &painter, int width, int height, float fadeAlpha,
                 const QByteArray &labelIconSvg, const QString &labelText,
                 bool drawPill = true, double textShiftX = 0.0,
                 double compactAnim = 0.0,
                 const QString &morphText = QString(), double morphAmount = 0.0);

    // 描かずにラベル（台）の矩形だけを求める。伸びる板の開始形状として
    // 必要なため、paint()より前に呼べる形で切り出してある。
    QRectF measure(int width, int height,
                   const QByteArray &labelIconSvg, const QString &labelText,
                   const QString &morphText = QString());

    // 台の左側の角丸半径（アイコンの角丸と同心にした値）。伸びる板が
    // 開始時にラベルと完全に一致するよう、CanvasVideo側が参照する。
    static double pillLeftRadius(double pillHeight);

    // 台の左端からロゴ（円）の中心までの距離。2026-09-12: リストの行の
    // ピンをこのロゴの中心と同じ縦線上に置くため、CanvasVideo側が参照する。
    // 実体の定数（kPillPaddingLeft / kIconSize）は .cpp 側にあるので、
    // 値がずれたらビルドが止まるよう .cpp で static_assert している。
    static constexpr double kIconCenterFromPillLeft = 17.0;

    // ロゴ（円）の直径。行のサムネイルの左端をロゴの右端に揃えるために
    // CanvasVideo側が参照する。こちらも .cpp で static_assert している。
    static constexpr double kIconSizePx = 20.0;

    // 直近のpaint()が実際に描いたラベル全体（ロゴ＋テキスト＋台の余白）の
    // 画面座標。クリックのヒット判定に使う。paint()の戻り値と違い、台を
    // 描かなかった（ロゴだけの）場合もロゴを含む矩形を返す。何も描かな
    // かった場合は空。
    QRectF lastPaintedRect() const { return m_lastPaintedRect; }

    // 直近のpaint()が描いたロゴの矩形（画面座標）。描いていなければ空。
    // CanvasVideo側が、この上へ台と同じガラスの縁を重ねて質感を揃える。
    QRectF lastIconRect() const { return m_lastIconRect; }

private:
    void ensureIconImage(const QByteArray &svgBytes);

    // paint()とmeasure()で共有するレイアウト計算結果。
    struct Layout
    {
        bool    valid = false;
        bool    hasIcon = false;
        bool    hasText = false;
        QRectF  pillRect;
        QString elidedText;
        QString elidedMorphText;
        double  textWidth = 0.0;      // 文字の実寸
        double  textBoxWidth = 0.0;   // 文字を置く枠（語の長さによらず一定）
        double  iconSize = 0.0;
        double  gap = 0.0;
        double  margin = 0.0;
    };
    Layout computeLayout(int width, const QByteArray &labelIconSvg,
                         const QString &labelText,
                         const QString &morphText = QString());

    QImage m_iconImage;
    QByteArray m_iconSourceBytes;
    QRectF m_lastPaintedRect;
    QRectF m_lastIconRect;
};
