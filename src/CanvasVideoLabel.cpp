#include "CanvasVideoLabel.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>

namespace {
// レイアウト定数。paint()とmeasure()で同じ値を使う必要があるため、
// 関数内のconstexprではなくここに集約する。
// 台（ガラスの板）の外側マージン。2026-09-12: 以前は「中身の位置」を16pxに
// 固定して台をそこから外へ広げていたため、台の左端(5px)と上端(10px)で
// ウィンドウ端からの距離がばらついていた。台自身の位置を基準にして、
// 左も上も同じ距離になるようにする。
constexpr double kPlateMargin = 12.0;
constexpr double kIconSize    = 20.0;
constexpr double kGap         = 6.0;
// 台の余白。2026-09-12: 左右を同じにすると、左端に来るのが「円のロゴ」、
// 右端に来るのが「テキスト」で見え方が揃わず、ロゴ側だけ余白が広く見えた。
// 円は角が無いぶん詰めてよいので、左を狭く・右を広くして視覚的に釣り合わせる。
constexpr double kPillPaddingLeft  = 7.0;
constexpr double kPillPaddingRight = 12.0;
constexpr double kPillPaddingY     = 6.0;

// ヘッダで公開している「台の左端からロゴの中心まで」と実体がずれていないか。
// ずれるとリストの行のピンとロゴの縦線が合わなくなる（CanvasVideoLabel.h）。
static_assert(kPillPaddingLeft + kIconSize * 0.5
                  == CanvasVideoLabelPainter::kIconCenterFromPillLeft,
              "kIconCenterFromPillLeft がレイアウト定数と一致していません");
static_assert(kIconSize == CanvasVideoLabelPainter::kIconSizePx,
              "kIconSizePx がレイアウト定数と一致していません");

// 2026-09-12: 明るい背景や白っぽい映像の上だと、白文字がほぼ読めなくなる。
// 文字の後ろへ半透明の黒を1pxだけずらして敷き、輪郭を作る（ぼかした影を
// 作るとQPainterでは毎フレームのコストが高いので、1pxのオフセット2回で
// 「にじみ」ではなく「縁」として効かせる）。CanvasVideo.cpp側にも同名の
// ヘルパーがある（あちらはパネル用。共有ヘッダを増やすほどの処理量では
// ないため、それぞれの翻訳単位に閉じたまま置いている）。
void drawShadowedText(QPainter &painter, const QRectF &rect, int flags,
                      const QString &text, const QColor &color, double alpha)
{
    if (text.isEmpty() || alpha <= 0.01)
        return;
    const int shadowAlpha = qBound(0, qRound(170 * alpha), 255);
    painter.save();
    painter.setPen(QColor(0, 0, 0, shadowAlpha));
    painter.drawText(rect.translated(1.0, 1.0), flags, text);
    painter.drawText(rect.translated(0.0, 1.0), flags, text);
    painter.setPen(color);
    painter.drawText(rect, flags, text);
    painter.restore();
}
} // namespace

void CanvasVideoLabelPainter::ensureIconImage(const QByteArray &svgBytes)
{
    if (svgBytes == m_iconSourceBytes)
        return;

    m_iconSourceBytes = svgBytes;
    m_iconImage = QImage();
    if (svgBytes.isEmpty())
        return;

    QSvgRenderer renderer(svgBytes);
    if (!renderer.isValid())
        return;

    m_iconImage = QImage(64, 64, QImage::Format_ARGB32_Premultiplied);
    m_iconImage.fill(Qt::transparent);
    QPainter iconPainter(&m_iconImage);
    iconPainter.setRenderHint(QPainter::Antialiasing, true);
    iconPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&iconPainter, QRectF(0, 0, 64, 64));
}

double CanvasVideoLabelPainter::compactPillWidth(double fullWidth,
                                                 double pillHeight,
                                                 double compactAnim)
{
    // 最小形は「高さ＝幅」の真円。ロゴだけが乗る丸い台になる。
    const double t = qBound(0.0, compactAnim, 1.0);
    return fullWidth + (pillHeight - fullWidth) * t;
}

double CanvasVideoLabelPainter::pillLeftRadius(double pillHeight)
{
    // 2026-09-12: ロゴを角丸四角から円（svgLogoForSite()のrx=32）へ変更した
    // のに合わせ、台も左右対称のカプセルへ戻した。左だけアイコンの角丸と
    // 同心にしていたのは、四角いロゴと台の角の曲率を揃えるためだったので、
    // ロゴが円になった今は左右とも高さの半分でよい。
    return pillHeight * 0.5;
}

CanvasVideoLabelPainter::Layout
CanvasVideoLabelPainter::computeLayout(int width, const QByteArray &labelIconSvg,
                                       const QString &labelText,
                                       const QString &morphText)
{
    Layout out;
    if (labelIconSvg.isEmpty() && labelText.isEmpty())
        return out;

    ensureIconImage(labelIconSvg);
    out.hasIcon = !m_iconImage.isNull() && !labelIconSvg.isEmpty();
    out.hasText = !labelText.isEmpty();
    if (!out.hasIcon && !out.hasText)
        return out;

    const QFont font(QStringLiteral("Arial"), 10, QFont::DemiBold);
    const QFontMetrics fm(font);
    const double maxTextWidth = qMax(
        40.0, width - kPlateMargin * 2.0 - kPillPaddingLeft - kPillPaddingRight
                  - (out.hasIcon ? kIconSize + kGap : 0.0) - 24.0);
    out.elidedText = out.hasText
        ? fm.elidedText(labelText, Qt::ElideRight, static_cast<int>(maxTextWidth))
        : QString();
    out.textWidth = out.hasText ? fm.horizontalAdvance(out.elidedText) : 0.0;
    out.elidedMorphText = morphText.isEmpty()
        ? QString()
        : fm.elidedText(morphText, Qt::ElideRight, static_cast<int>(maxTextWidth));

    // 2026-09-23: 短い語（Solo / List / Mix / Live …）で台の大きさが変わらない
    // ようにする。文字を置く枠を、本体の既定の語（"Solo"）の幅を下限として
    // 固定し、それより長い語のときだけ広げる。長押しで語が入れ替わる最中も
    // 形が動かないよう、入れ替え先の語も含めて最大を採る。
    const double fixedTextWidth = fm.horizontalAdvance(QStringLiteral("Solo"));
    out.textBoxWidth = out.hasText
        ? qMax(fixedTextWidth,
               qMax(out.textWidth,
                    morphText.isEmpty()
                        ? 0.0 : fm.horizontalAdvance(out.elidedMorphText)))
        : 0.0;

    const double contentWidth = (out.hasIcon ? kIconSize : 0.0)
        + (out.hasIcon && out.hasText ? kGap : 0.0) + out.textBoxWidth;
    const double pillHeight = qMax(kIconSize, static_cast<double>(fm.height()))
        + kPillPaddingY * 2.0;

    out.pillRect = QRectF(kPlateMargin, kPlateMargin,
                          contentWidth + kPillPaddingLeft + kPillPaddingRight,
                          pillHeight);
    out.iconSize = kIconSize;
    out.gap      = kGap;
    // 中身の開始x。
    out.margin   = out.pillRect.left() + kPillPaddingLeft;
    out.valid    = true;
    return out;
}

QRectF CanvasVideoLabelPainter::measure(int width, int height,
                                        const QByteArray &labelIconSvg,
                                        const QString &labelText,
                                        const QString &morphText)
{
    Q_UNUSED(height);
    const Layout layout = computeLayout(width, labelIconSvg, labelText, morphText);
    return layout.valid ? layout.pillRect : QRectF();
}

QRectF CanvasVideoLabelPainter::paint(QPainter &painter, int width, int height,
                                      float fadeAlpha,
                                      const QByteArray &labelIconSvg,
                                      const QString &labelText,
                                      bool drawPill, double textShiftX,
                                      double compactAnim,
                                      const QString &morphText,
                                      double morphAmount)
{
    Q_UNUSED(height);
    m_lastPaintedRect = QRectF();
    m_lastIconRect = QRectF();
    if (fadeAlpha <= 0.001f)
        return QRectF();

    const Layout layout = computeLayout(width, labelIconSvg, labelText, morphText);
    if (!layout.valid)
        return QRectF();

    // 縮退（島に場所を譲る最小形）。台の幅を高さ＝真円まで縮め、ロゴを
    // その中心へ寄せ、テキストは台の中へクリップしながら消す。
    const double compact = qBound(0.0, compactAnim, 1.0);
    const double pillHeight = layout.pillRect.height();
    QRectF pillRect = layout.pillRect;
    if (compact > 0.0001) {
        pillRect.setWidth(compactPillWidth(layout.pillRect.width(),
                                           pillHeight, compact));
    }

    // ヒット判定用。台を描かない場合もロゴを内包する矩形としてpillRectを
    // そのまま使う（pillRectはロゴ単体でも常にそれを含む）。
    m_lastPaintedRect = pillRect;

    painter.save();
    const qreal inheritedOpacity = painter.opacity();
    // 【2026-09-13 検討済み・差し戻し】ロゴだけのラベルにも台を描くよう
    // 変えたが、元に戻した。「ロゴだけ／ロゴ＋テキスト」の2通りは本体が
    // 用意している選択肢で、どちらにするかはプラグインが決める話。
    // 通常の動画にも台が欲しいなら、本体が勝手に台を足すのではなく、
    // プラグインがテキストを返せばよい（動画サイト用プラグインなら Live / List /
    // Solo を返す）。本体側の選択肢はそのまま残してある。
    // 2026-09-13: 縮退中はテキストが無くなるが、台（丸い皿）は残す。
    // 「ロゴだけのラベルには台を描かない」という既定の選択肢はそのままで、
    // これは「テキスト付きラベルが一時的に縮んでいる」状態なので台が要る。
    if ((layout.hasText || compact > 0.0001) && drawPill) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(10, 10, 20, qRound(150 * fadeAlpha)));
        // 左右対称のカプセル（pillLeftRadius()のコメント参照）。左右で
        // 半径を分けられる形のまま残してあるのは、CanvasVideo側の「台が
        // カードへ伸びる」演出が同じ描き方を使うため。
        const double rightRadius = pillHeight * 0.5;
        const double leftRadius  = pillLeftRadius(pillHeight);
        const double x0 = pillRect.left();
        const double x1 = pillRect.right();
        const double y0 = pillRect.top();
        const double y1 = pillRect.bottom();
        QPainterPath path;
        path.moveTo(x0 + leftRadius, y0);
        path.lineTo(x1 - rightRadius, y0);
        path.arcTo(QRectF(x1 - rightRadius * 2.0, y0,
                          rightRadius * 2.0, rightRadius * 2.0), 90.0, -180.0);
        path.lineTo(x0 + leftRadius, y1);
        path.arcTo(QRectF(x0, y1 - leftRadius * 2.0,
                          leftRadius * 2.0, leftRadius * 2.0), 270.0, -90.0);
        path.lineTo(x0, y0 + leftRadius);
        path.arcTo(QRectF(x0, y0, leftRadius * 2.0, leftRadius * 2.0),
                   180.0, -90.0);
        path.closeSubpath();
        painter.drawPath(path);
    }

    // ロゴのx。縮退すると台の中心へ寄る。
    const double iconLeftNormal  = layout.margin;
    const double iconLeftCompact = pillRect.center().x() - layout.iconSize * 0.5;
    double cursorX = iconLeftNormal
                   + (iconLeftCompact - iconLeftNormal) * compact;
    const double centerY = pillRect.center().y();
    if (layout.hasIcon) {
        painter.setOpacity(inheritedOpacity * fadeAlpha);
        m_lastIconRect = QRectF(cursorX, centerY - layout.iconSize * 0.5,
                                layout.iconSize, layout.iconSize);
        // 2026-09-12: ロゴの「形」は本体が決めてよい要素なので円に統一する。
        // ただし切り抜く（＝形を与える）だけで、絵そのものには一切手を
        // 加えない。何を表示するかはプラグインの領分であり、本体が縁の効果
        // などを被せると、渡された絵と無関係な装飾が乗ってしまうため
        // （PbPluginApi.h の labelIconSvg のコメント参照）。
        painter.save();
        QPainterPath iconClip;
        iconClip.addEllipse(m_lastIconRect);
        painter.setClipPath(iconClip, Qt::IntersectClip);
        painter.drawImage(m_lastIconRect, m_iconImage);
        painter.restore();
        painter.setOpacity(inheritedOpacity);
    }

    // テキストは縮退で消える。(1-t)^2 にして台が縮み切るより早く抜くことで、
    // 狭くなった台から文字がはみ出したまま残る瞬間を作らない。念のため
    // 台の内側でクリップもする（縮退の途中で右へはみ出すのを防ぐ）。
    const double textAlpha = fadeAlpha * (1.0 - compact) * (1.0 - compact);
    if (layout.hasText && textAlpha > 0.004) {
        const QFont font(QStringLiteral("Arial"), 10, QFont::DemiBold);
        const QFontMetrics fm(font);
        painter.setFont(font);
        // 文字は「語の長さによらない一定の枠」の中央に置く（枠の幅は
        // computeLayout が決める。台の大きさを揃えるため）。
        const QRectF textRect(cursorX + layout.iconSize + layout.gap
                                  + qMax(0.0, textShiftX)
                                  - (layout.hasIcon ? 0.0 : layout.iconSize + layout.gap),
                              centerY - fm.height() * 0.5,
                              layout.textBoxWidth + 2.0, fm.height());
        painter.save();
        if (compact > 0.0001)
            painter.setClipRect(pillRect, Qt::IntersectClip);
        const double morph = qBound(0.0, morphAmount, 1.0);
        if (morph > 0.001 && !layout.elidedMorphText.isEmpty()) {
            // 入れ替え: 今の語は上へ抜けながら薄く、次の語は下から入ってくる。
            // 文字の高さぶんは動かさない（枠からはみ出すと台の外に出るため）。
            const double slide = fm.height() * 0.42;
            painter.save();
            painter.setClipRect(pillRect, Qt::IntersectClip);
            drawShadowedText(painter, textRect.translated(0.0, -slide * morph),
                             Qt::AlignHCenter | Qt::AlignVCenter, layout.elidedText,
                             QColor(255, 255, 255,
                                    qRound(230 * textAlpha * (1.0 - morph))),
                             textAlpha * (1.0 - morph));
            drawShadowedText(painter, textRect.translated(0.0, slide * (1.0 - morph)),
                             Qt::AlignHCenter | Qt::AlignVCenter,
                             layout.elidedMorphText,
                             QColor(255, 255, 255, qRound(230 * textAlpha * morph)),
                             textAlpha * morph);
            painter.restore();
        } else {
            drawShadowedText(painter, textRect, Qt::AlignHCenter | Qt::AlignVCenter,
                             layout.elidedText,
                             QColor(255, 255, 255, qRound(230 * textAlpha)),
                             textAlpha);
        }
        painter.restore();
    }
    painter.restore();
    // ピルを描いたときだけ、その矩形を返す（アイコンのみ／台を描かない
    // 場合は背景が無いのでぼかし対象にしない）。
    return ((layout.hasText || compact > 0.0001) && drawPill) ? pillRect : QRectF();
}
