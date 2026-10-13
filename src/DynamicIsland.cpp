#include "DynamicIsland.h"
#include "CanvasWidget.h"
#include "MainWindow.h"
#include "AppSettings.h"
#include "IslandControlRegistry.h"
#include "IslandSettingsPanel.h"
#include "IslandControls.h"
#include "IslandTabGoo.h"
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QRadialGradient>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QCursor>
#include <QEasingCurve>
#include <QPixmap>
#include <QSpinBox>
#include <QToolButton>
#include <QSvgRenderer>
#include <QHash>
#include <QDebug>
#include <windows.h>
#include <winreg.h>

// ---- Windowsアクセントカラー ----
QColor getWindowsAccentColor()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\DWM",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return QColor(30, 30, 30, 240);
    DWORD value = 0, size = sizeof(value);
    RegQueryValueExW(key, L"ColorizationColor", nullptr, nullptr,
                     reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return QColor((value>>16)&0xFF, (value>>8)&0xFF, value&0xFF, 230);
}

// ---- アプリのアクセントカラー（2026-09-16）----
// 設定「Windowsのカラー設定を使用」がオンなら Windows のアクセントカラー、
// オフなら設定のカラーコード（"#RRGGBB"）。不正な値なら Windows の色に戻す。
// α は Windows の色と揃える（230。塗るときは kIslandBodyAlpha に落とす）。
// 以前は島だけがこれを使い、動画UI等は getWindowsAccentColor() を直接
// 呼んでいたため、設定の色が島にしか効かなかった（DynamicIsland.h 参照）。
QColor appAccentColor()
{
    const AppSettings &cfg = AppSettings::instance();
    if (!cfg.useWindowsAccentColor()) {
        QColor c(cfg.customAccentColor());
        if (c.isValid()) {
            c.setAlpha(230);
            return c;
        }
    }
    return getWindowsAccentColor();
}

// ---- マスターボリュームアイコン(SVG)描画ヘルパー ----
// image/svg/ico_Sound_0〜3.svg（.qrc経由で :/icons/ にバンドル）を、
// 島の他のアイコン（"+"等）と統一感を出すため白でティントしてから描画する。
// QSvgRendererはパース済みDOMを保持するため、アイコン名ごとにキャッシュして
// 毎フレームの再パースを避ける。
namespace {
QPixmap tintedSvgPixmap(const QString &resourcePath, const QSize &size, const QColor &color)
{
    static QHash<QString, QSvgRenderer*> s_rendererCache;
    QSvgRenderer *renderer = s_rendererCache.value(resourcePath, nullptr);
    if (!renderer) {
        renderer = new QSvgRenderer(resourcePath);
        s_rendererCache.insert(resourcePath, renderer);
    }
    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    renderer->render(&p);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pixmap.rect(), color);
    p.end();
    return pixmap;
}

// アイコンSVGを「十分に高い解像度で一度だけ」ラスタライズしてキャッシュする。
//
// 2026-09-11: 直前の版では、毎フレームその時点のホバー拡大率に合わせた
// 整数サイズ(baseIconPx×totalScale×DPRを四捨五入)でSVGを焼き直していた。
// 拡大率は連続的に変わるのにラスタのピクセル数は整数でしか変われないため、
// アニメーション中はアイコンだけが1px単位で太さ・アンチエイリアスを変え、
// 連続的に拡大するベクタの円背景と歩調が合わずガクガク/チカチカして見えた
// （ユーザー指摘）。
// ここでは想定最大倍率ぶんの余裕を持った固定サイズで先に焼いておき、描画側は
// 円と同じpainter変換へそのまま乗せる。常に縮小方向のスケーリングになるため
// 拡大時もボケず、かつ毎フレームの再ラスタライズも無くなる。
const QPixmap &cachedTintedSvgPixmap(const QString &resourcePath, int logicalPx,
                                     qreal dpr, const QColor &color)
{
    // ホバー(1.15倍)と出現アニメのオーバーシュートを見込んでも足りるよう4倍。
    constexpr int kSuperSample = 4;
    const int physPx = qMax(1, qRound(logicalPx * dpr * kSuperSample));
    const QString key = resourcePath + QLatin1Char('|') + QString::number(physPx)
                      + QLatin1Char('|') + color.name(QColor::HexArgb);
    static QHash<QString, QPixmap> s_pixmapCache;
    auto it = s_pixmapCache.find(key);
    if (it == s_pixmapCache.end())
        it = s_pixmapCache.insert(key, tintedSvgPixmap(resourcePath, QSize(physPx, physPx), color));
    return it.value();
}

// アイコン種別文字列 → リソースパス。
// "sound0"〜"sound3" はマスターボリューム段階表示、
// "x"/"square"/"-" は島の閉じる/全画面/最小化ボタン（元は自前パス描画の
// ×・□・－だったが、image/svg/exit・fullscreen・min.svg に差し替え済み）。
QString soundIconResourcePath(const QString &iconType)
{
    if (iconType == QStringLiteral("sound0")) return QStringLiteral(":/icons/ico_Sound_0.svg");
    if (iconType == QStringLiteral("sound1")) return QStringLiteral(":/icons/ico_Sound_1.svg");
    if (iconType == QStringLiteral("sound2")) return QStringLiteral(":/icons/ico_Sound_2.svg");
    if (iconType == QStringLiteral("sound3")) return QStringLiteral(":/icons/ico_Sound_3.svg");
    if (iconType == QStringLiteral("x"))      return QStringLiteral(":/icons/exit.svg");
    if (iconType == QStringLiteral("square")) return QStringLiteral(":/icons/fullscreen.svg");
    if (iconType == QStringLiteral("-"))      return QStringLiteral(":/icons/min.svg");
    return QString();
}
// 島の背後をぼかす半径(論理px)。
constexpr qreal kBackdropBlurRadius = 18.0;
// 島本体の塗りの不透明度。下げるほど背後のぼけた内容が透ける。
// 2026-09-11: この値がそのまま背後の明るさの上限を決める(寄与は1-α)。
// 150だと背後が真っ白でも島の中は49%グレー止まりで、バブルのガラス枠のような
// 明らかに光っているものまで暗く沈んで見えていた。115へ下げて寄与を55%に。
// 失われる色味と輝きは、ぼかし側のトーン調整(Blur.cpp kVibrancy /
// kHighlightKnee / kHighlightGain)で補っているので、暗い背景の上での
// 見た目はほぼ従来どおりになる。
//
// 2026-09-12: 逆に115だと明るい背景（白い画像など）の上で島の色が背景に
// 引きずられすぎ、アクセント色がほとんど残らなくなっていた。素の色を
// もう少し保てるところまで戻す（寄与を約35%へ）。
constexpr int kIslandBodyAlpha = 165;

} // namespace

// ---- AnimatedButton ----
// 物理バネ振動イージング（Python版 _physics_logic の移植）
qreal AnimatedButton::physicsEasing(qreal t)
{
    const double DAMPING = 5.0, PERIOD = 0.4, INTENSITY = 0.4, PEAK_DELAY = 2.02;
    if (t <= 0 || t >= 1) return 1.0;
    double decay = std::exp(-DAMPING * t);
    double angle = (2 * M_PI / PERIOD) * (t + PEAK_DELAY);
    return 1.0 + (INTENSITY * decay * std::sin(angle));
}

bool islandGlassOnGpu()
{
    static const bool s_on =
        qEnvironmentVariable("PB_ISLAND_GL_GLASS", QStringLiteral("1"))
        != QLatin1String("0");
    return s_on;
}

AnimatedButton::AnimatedButton(const QString &color, const QString &iconType, QWidget *parent)
    : QPushButton(parent), m_color(color), m_iconType(iconType)
{
    // 円のガラスは島（親）の上のGL側で描く。座標は島のローカル座標で登録する。
    if (parent && islandGlassOnGpu())
        m_glass.attach(parent);
    setFixedSize(36, 36);
    setFlat(true);
    setCursor(Qt::PointingHandCursor);
    // キーボードフォーカスを受け取らない（Space/Enterでの誤クリック防止）
    setFocusPolicy(Qt::NoFocus);

    // 出現アニメ（OutBack, amplitude=2.0）
    m_appearAnim = new QPropertyAnimation(this, "visualScale", this);
    m_appearAnim->setDuration(600);
    m_appearAnim->setStartValue(0.0f);
    m_appearAnim->setEndValue(1.0f);
    QEasingCurve appearCurve(QEasingCurve::OutBack);
    appearCurve.setAmplitude(2.0);
    m_appearAnim->setEasingCurve(appearCurve);
    connect(m_appearAnim, &QPropertyAnimation::finished, this, [this]{
        m_fullyAppeared = true;
    });

    // ホバーイン: シンプルに拡大（OutQuad, 1.0→1.15）
    m_hoverInAnim = new QPropertyAnimation(this, "bounceScale", this);
    m_hoverInAnim->setDuration(150);
    m_hoverInAnim->setEasingCurve(QEasingCurve::OutQuad);

    // ホバーアウト: 少し小さくなりすぎてから元に戻る
    // QVariantAnimationでt:0→1を直接bounceScaleに変換して渡す
    m_hoverOutAnim = new QVariantAnimation(this);
    m_hoverOutAnim->setDuration(400);
    m_hoverOutAnim->setStartValue(0.0f);
    m_hoverOutAnim->setEndValue(1.0f);
    connect(m_hoverOutAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        float t = v.toFloat();
        float scale;
        if (t < 0.4f) scale = 1.15f - t * (1.15f - 0.9f) / 0.4f;
        else          scale = 0.9f  + (t - 0.4f) * (1.0f - 0.9f) / 0.6f;
        setBounceScale(scale);
    });
}

void AnimatedButton::startAppear()
{
    m_fullyAppeared = false;
    m_appearAnim->stop();
    m_appearAnim->start();
}

void AnimatedButton::resetState()
{
    m_fullyAppeared = false;
    m_appearAnim->stop();
    m_hoverInAnim->stop();
    m_hoverOutAnim->stop();
    setVisualScale(0.0f);
    setBounceScale(1.0f);
}

void AnimatedButton::setDimmed(bool dimmed)
{
    if (m_dimmed == dimmed)
        return;
    m_dimmed = dimmed;
    if (dimmed) {
        // グレーアウトに入る瞬間にホバー拡大が残っていると、縮んだ灰色の
        // 円が拡大したまま固まって見えるので等倍へ戻す。
        m_hoverInAnim->stop();
        m_hoverOutAnim->stop();
        setBounceScale(1.0f);
    }
    update();
    syncGlass();
}

void AnimatedButton::enterEvent(QEnterEvent *)
{
    emit hoverEntered();
    // グレーアウト中はホバーバウンスを出さない（押せないボタンが
    // 反応すると押せるように見える）。
    if (!m_fullyAppeared || m_dimmed) return;
    m_hoverOutAnim->stop();
    m_hoverInAnim->stop();
    m_hoverInAnim->setStartValue(m_bounceScale);
    m_hoverInAnim->setEndValue(1.15f);
    m_hoverInAnim->start();
}

void AnimatedButton::leaveEvent(QEvent *)
{
    emit hoverLeft();
    if (!m_fullyAppeared || m_dimmed) return;
    m_hoverInAnim->stop();
    m_hoverOutAnim->stop();
    m_hoverOutAnim->start(); // t:0→1で自動的に1.15→0.9→1.0の軌跡を辿る
}

void AnimatedButton::glassColors(QColor &tint, QColor &shade) const
{
    // paintEvent() の baseColor / tintColor / shadeColor と同じ導出。
    const QColor baseColor = [&]() -> QColor {
        const QColor c(m_color);
        if (!m_dimmed)
            return c;
        const QColor hsv = c.toHsv();
        return QColor::fromHsv(qMax(0, hsv.hue()),
                               static_cast<int>(hsv.saturation() * 0.18),
                               static_cast<int>(hsv.value() * 0.50));
    }();
    {
        const QColor hsv = baseColor.toHsv();
        shade = QColor::fromHsv(qMax(0, hsv.hue()),
                                qMin(255, static_cast<int>(hsv.saturation() * 1.10)),
                                static_cast<int>(hsv.value() * 0.52));
    }
    {
        constexpr double kSparkleValue = 1.20;
        const QColor hsv = baseColor.toHsv();
        tint = QColor::fromHsv(qMax(0, hsv.hue()),
                               qBound(0, static_cast<int>(hsv.saturation() * 1.04), 255),
                               qBound(0, static_cast<int>(hsv.value() * kSparkleValue + 10), 255));
        tint.setAlpha(192);   // kTintAlpha
    }
}

void AnimatedButton::syncGlass()
{
    if (!islandGlassOnGpu())
        return;
    const QWidget *island = parentWidget();
    BackdropBlur::GlassPlateStyle glass;
    qreal opacity = 0.0;
    if (island && isVisible() && !m_bodyHidden && m_visualScale >= 0.01f) {
        const float totalScale = m_visualScale * m_bounceScale;
        const float cx = width() / 2.0f;
        const float cy = height() / 2.0f;
        glass.enabled = true;
        glass.shape = BackdropBlur::GlassPlateStyle::Shape::Disc;
        glassColors(glass.body, glass.shade);
        // params: 0 cx / 1 cy / 2 r / 3 scale / 4 shadowR / 5 shadowCy
        glass.params[0] = cx;
        glass.params[1] = cy;
        glass.params[2] = 12.0f * totalScale;
        glass.params[3] = totalScale;
        glass.params[4] = 12.0f * 1.28f * m_visualScale;
        glass.params[5] = cy + 12.0f * 0.16f * m_visualScale;
        const auto *fx = qobject_cast<QGraphicsOpacityEffect *>(island->graphicsEffect());
        opacity = fx ? fx->opacity() : 1.0;
    }
    m_glass.setGlassStyle(glass);
    m_glass.setShape(glass.enabled ? QRectF(geometry()) : QRectF(), 0.0, 0.0, opacity);
}

void AnimatedButton::moveEvent(QMoveEvent *event)
{
    QPushButton::moveEvent(event);
    syncGlass();
}

void AnimatedButton::showEvent(QShowEvent *event)
{
    QPushButton::showEvent(event);
    syncGlass();
}

void AnimatedButton::hideEvent(QHideEvent *event)
{
    QPushButton::hideEvent(event);
    syncGlass();
}

void AnimatedButton::paintEvent(QPaintEvent *)
{
    syncGlass();
    if (m_visualScale < 0.01f) return;
    // 2026-09-16: GL版では円（落ち影〜縁の光）はGL側が描く。ここはアイコンだけ。
    const bool bodyOnGpu = islandGlassOnGpu();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const float totalScale = m_visualScale * m_bounceScale;
    const qreal baseR = 12.0;
    const qreal cx = width() / 2.0, cy = height() / 2.0;

    // ---- 落ち影（ホバーでは大きさを変えない） ----
    // 2026-09-11: バブルのGlassHighlightEffectと違い、ライト方向追従などの
    // 動的計算は一切しない「完全に固定の絵」としてガラスの陰影を描く。
    // ホバーのバウンス(最大1.15倍)まで影に掛けると36pxのウィジェット矩形を
    // はみ出して四角く切れるため、影は出現アニメ(visualScale)にだけ追従させる。
    if (!m_bodyHidden && !bodyOnGpu) {
        const qreal shadowR = baseR * 1.28 * m_visualScale;      // 最大15.4px(枠18pxの内側)
        const QPointF shadowCenter(cx, cy + baseR * 0.16 * m_visualScale);
        QRadialGradient shadow(shadowCenter, shadowR);
        shadow.setColorAt(0.00, QColor(0, 0, 0, 66));
        shadow.setColorAt(0.60, QColor(0, 0, 0, 28));
        shadow.setColorAt(1.00, QColor(0, 0, 0, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(shadow);
        painter.drawEllipse(shadowCenter, shadowR, shadowR);
    }

    painter.save();
    painter.translate(cx, cy);
    painter.scale(totalScale, totalScale);
    painter.translate(-cx, -cy);

    // 2026-09-13: グレーアウト中は彩度をほぼ抜き、明度も落とす。
    // 以降の色（tintColor/shadeColor）はすべてこのbaseColorから導出される
    // ので、ここ1箇所だけで body 全体が灰色になる。
    const QColor baseColor = [&]() -> QColor {
        const QColor c(m_color);
        if (!m_dimmed)
            return c;
        const QColor hsv = c.toHsv();
        return QColor::fromHsv(qMax(0, hsv.hue()),
                               static_cast<int>(hsv.saturation() * 0.18),
                               static_cast<int>(hsv.value() * 0.50));
    }();
    const auto withAlpha = [](QColor c, int a) { c.setAlpha(a); return c; };
    const QRectF bodyRect(cx - baseR, cy - baseR, baseR * 2.0, baseR * 2.0);
    painter.setPen(Qt::NoPen);

    // 暗部に使う色。黒を重ねると彩度が落ちて「灰色いススけた影」になるため、
    // 色相をそのままに彩度を少し上げ、明度だけを落とした暗色を作り、
    // CompositionMode_Multiplyで掛ける。こうすると黄色は琥珀、赤は深紅、
    // 緑は深緑と、ボタンごとの色に馴染んだ暗さになる。
    // 明度係数0.42は暗すぎたため0.52へ(2026-09-11)。
    const QColor shadeColor = [&] {
        const QColor hsv = baseColor.toHsv();
        return QColor::fromHsv(qMax(0, hsv.hue()),
                               qMin(255, static_cast<int>(hsv.saturation() * 1.10)),
                               static_cast<int>(hsv.value() * 0.52));
    }();

    // ---- ガラス表現 ----------------------------------------------------
    // 方針(2026-09-11): 球体のような立体表現は作らない。「平らな半透明の
    // ガラス板」を下地とし、その上に
    //   ・光が当たって輝いている部分 = 加算(CompositionMode_Plus)のハイライト
    //   ・光が当たらず沈んでいる部分 = 通常合成の暗い翳り／輪郭線
    // の2種類だけを重ねる。中央は素通しのまま何も描かない(ここに
    // グラデーションを入れると途端にプラスチック/ゲルの質感になる)。
    //
    // 見た目の調整はまずこの2つの濃度から触るとよい:
    //   kFrostAlpha … 光を散らす白の下敷き。大きいほど乳白色のすりガラス寄り
    //   kTintAlpha  … ボタン色の濃さ。大きいほどベタ塗りのボタン寄り
    //
    // 2026-09-11: 26/92では島の下地(Windowsアクセントカラー)が透けすぎて
    // 色が眠く、特に黄色が白っぽく・赤が紫寄りに濁っていた(ユーザー指摘)。
    // フロストを薄くしてティントを濃くし、色をはっきり出す方向へ振り直す。
    // ハイライトと影は縁にしか乗っていないので、ここを濃くしてもガラスの
    // 質感自体は保たれる。
    constexpr int kFrostAlpha = 10;
    // 2026-09-12: 光量を動画UIへ揃えたあと、ティントがわずかに重く感じた
    // ため少しだけ薄くした（210→192）。下地がほんの少し透ける程度の差。
    constexpr int kTintAlpha  = 192;

    // 2026-09-12: 色そのものを一段きらめかせる。縁の光を強くすると
    // （元の96/86のように）ガラスが主張しすぎるので、触るのは「色」の方。
    //   ・明度を上げて彩度も気持ち上げた濃い発色をティントに使う
    //   ・面全体へ、斜めから差す弱い加算の艶を1枚だけ重ねる
    // 調整はこの2つの数字から: kSparkleValue（明るさ）/ kSheenAlpha（艶）。
    constexpr double kSparkleValue = 1.20;
    constexpr int    kSheenAlpha   = 13;
    const QColor tintColor = [&] {
        const QColor hsv = baseColor.toHsv();
        return QColor::fromHsv(qMax(0, hsv.hue()),
                               qBound(0, static_cast<int>(hsv.saturation() * 1.04), 255),
                               qBound(0, static_cast<int>(hsv.value() * kSparkleValue + 10), 255));
    }();

    if (!m_bodyHidden && !bodyOnGpu) {
        QPainterPath clip;
        clip.addEllipse(QPointF(cx, cy), baseR, baseR);
        painter.save();
        painter.setClipPath(clip);

        // (1) ガラス板そのもの。フロスト層＋色のティントで、どちらも平坦。
        painter.fillRect(bodyRect, QColor(255, 255, 255, kFrostAlpha));
        painter.fillRect(bodyRect, withAlpha(tintColor, kTintAlpha));

        // 斜めの艶。左上から右下へ抜ける弱い加算を面全体へ1枚だけ。
        // 中央に濃いグラデーションを置くとプラスチック/ゲルに見えるので、
        // 端から端まで緩やかに落ちるだけの薄さに留める。
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
        QLinearGradient sheen(QPointF(cx - baseR, cy - baseR),
                              QPointF(cx + baseR, cy + baseR));
        sheen.setColorAt(0.00, QColor(255, 255, 255, kSheenAlpha));
        sheen.setColorAt(0.55, QColor(255, 255, 255, kSheenAlpha / 4));
        sheen.setColorAt(1.00, QColor(255, 255, 255, 0));
        painter.fillRect(bodyRect, sheen);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        // (2) 加算ハイライト: 縁の内側へにじむリムグロー(全周)。
        //     2026-09-11: 立ち上がりが急で「縁と内側の境界線」に見えたため、
        //     中間のストップを増やして内側へ緩やかに減衰させる。
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
        QRadialGradient ring(QPointF(cx, cy), baseR);
        ring.setColorAt(0.00, QColor(255, 255, 255, 0));
        ring.setColorAt(0.38, QColor(255, 255, 255, 0));   // 中央は素通し
        // 2026-09-12: 光量を動画UI（CanvasVideoのガラス円）と揃えた。
        // 島のボタンだけが強く光っていると、同じ画面に並ぶ動画UI・ラベルと
        // 別の素材に見えるため。
        ring.setColorAt(0.68, QColor(255, 255, 255, 4));
        ring.setColorAt(0.86, QColor(255, 255, 255, 12));
        ring.setColorAt(0.96, QColor(255, 255, 255, 22));
        ring.setColorAt(1.00, QColor(255, 255, 255, 18));
        painter.fillRect(bodyRect, ring);

        // (3) 加算ハイライト: 光の入る上端と、抜ける下端をもう一段明るく。
        QRadialGradient topGlow(QPointF(cx, cy - baseR * 1.02), baseR * 0.95);
        topGlow.setColorAt(0.00, QColor(255, 255, 255, 22));
        topGlow.setColorAt(0.45, QColor(255, 255, 255, 7));
        topGlow.setColorAt(1.00, QColor(255, 255, 255, 0));
        painter.fillRect(bodyRect, topGlow);

        QRadialGradient bottomGlow(QPointF(cx, cy + baseR * 1.06), baseR * 0.80);
        bottomGlow.setColorAt(0.00, QColor(255, 255, 255, 16));
        bottomGlow.setColorAt(0.45, QColor(255, 255, 255, 5));
        bottomGlow.setColorAt(1.00, QColor(255, 255, 255, 0));
        painter.fillRect(bodyRect, bottomGlow);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        // (4) 影: 上下と違って光の乗らない左右を沈める(乗算)。
        //     2026-09-11: 以前は一定幅(2.6px)のストロークで帯を描いていたが、
        //     ペンの内側がそのまま段差になり「縁と内側の間の境界線」として
        //     見えてしまっていた(ユーザー指摘)。上下のハイライトと同じく、
        //     縁に中心を置いた放射グラデーションへ変更し、内側へ向かって
        //     滑らかに消えるようにする。
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        for (const int side : {-1, 1}) {
            const QPointF shadeCenter(cx + side * baseR * 0.92, cy);
            QRadialGradient sideShade(shadeCenter, baseR * 0.95);
            sideShade.setColorAt(0.00, withAlpha(shadeColor, 70));
            sideShade.setColorAt(0.45, withAlpha(shadeColor, 26));
            sideShade.setColorAt(1.00, withAlpha(shadeColor, 0));
            painter.fillRect(bodyRect, sideShade);
        }
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        painter.restore(); // クリップ解除
    }

    // 2026-09-15: 分裂アニメ中（m_bodyHidden）は輪郭と縁の光も描かない。
    if (!m_bodyHidden && !bodyOnGpu) {
        // (5) 輪郭線: ガラス板の切り口。これが無いと下地に溶けて板に見えない。
        //     こちらも乗算。円の外へわずかにはみ出す部分は下地が透明なので、
        //     乗算でも通常合成と同じ「暗色の線」として出る(シルエットの縁取り)。
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        QLinearGradient contour(QPointF(cx, cy - baseR), QPointF(cx, cy + baseR));
        contour.setColorAt(0.00, withAlpha(shadeColor, 120));
        contour.setColorAt(0.50, withAlpha(shadeColor, 230));
        contour.setColorAt(1.00, withAlpha(shadeColor, 190));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QBrush(contour), 0.9));
        painter.drawEllipse(QPointF(cx, cy), baseR - 0.35, baseR - 0.35);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        // (6) 加算ハイライト: 縁を走る細い光の線。上端が最も強く、左右は消える。
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
        QLinearGradient edgeLight(QPointF(cx, cy - baseR), QPointF(cx, cy + baseR));
        edgeLight.setColorAt(0.00, QColor(255, 255, 255, 30));
        edgeLight.setColorAt(0.32, QColor(255, 255, 255, 11));
        edgeLight.setColorAt(0.68, QColor(255, 255, 255, 10));
        edgeLight.setColorAt(1.00, QColor(255, 255, 255, 24));
        painter.setPen(QPen(QBrush(edgeLight), 0.9));
        painter.drawEllipse(QPointF(cx, cy), baseR - 0.85, baseR - 0.85);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }

    // 2026-09-15: 雫に引っ張られた円の寄りにアイコンも合わせる。
    // painter はすでに中心基準で totalScale 倍されているので、その分を割り戻す。
    if (m_iconOffsetX != 0.0)
        painter.translate(m_iconOffsetX / qMax(0.01f, totalScale), 0.0);

    // アイコン（"+" のみ自前パス描画。それ以外は image/svg/*.svg をSVGで描画）
    if (m_visualScale > 0.8f) {
        // グレーアウト中はアイコンも白から灰色へ。円だけ灰色でアイコンが
        // 白のままだと、そこだけ浮いて「押せそう」に見える。
        const QColor iconColor = m_dimmed ? QColor(190, 190, 190)
                                          : QColor(255, 255, 255);
        const QString svgRes = soundIconResourcePath(m_iconType);
        if (!svgRes.isEmpty()) {
            // 2026-09-11: アイコンは円背景とまったく同じpainter変換の中で描く。
            // 「円はベクタで連続拡大／アイコンは毎フレーム整数サイズで焼き直し」
            // と別々に動かすと、拡大率が連続なのにアイコンのピクセル数だけが
            // 1px単位で飛ぶため、アニメ中にアイコンがガクガク・チカチカする。
            // cachedTintedSvgPixmap()が4倍解像度で一度だけ焼いたものを返すので、
            // 変換に乗せても常に縮小方向のスムーススケーリングになりボケない。
            // 閉じる/全画面/最小化は視認性のため他アイコンより1.5倍大きく表示する。
            const bool isWindowControlIcon =
                (m_iconType == QStringLiteral("x") || m_iconType == QStringLiteral("square")
                 || m_iconType == QStringLiteral("-"));
            const int baseIconPx = isWindowControlIcon ? 30 : 16;
            const QPixmap &pm = cachedTintedSvgPixmap(svgRes, baseIconPx,
                                                      devicePixelRatioF(), iconColor);
            painter.drawPixmap(QRectF(cx - baseIconPx / 2.0, cy - baseIconPx / 2.0,
                                       baseIconPx, baseIconPx),
                                pm, pm.rect());
            painter.restore();
            return;
        }

        painter.setPen(QPen(iconColor, 2.0,
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath path;
        const float s = 8.0f;

        if (m_iconType == "+") {
            path.moveTo(cx - s/2, cy); path.lineTo(cx + s/2, cy);
            path.moveTo(cx, cy - s/2); path.lineTo(cx, cy + s/2);
        }
        painter.drawPath(path);
    }

    painter.restore();
}

// ---- DynamicIsland ----
DynamicIsland::DynamicIsland(QWidget *parent)
    : QFrame(parent)
{
    setAcceptDrops(true);
    m_backdropBlur.attach(this);
    m_accentColor  = appAccentColor();
    // 2026-09-16: 設定パネルの部品も島と同じ色から作る
    IslandControls::setAccentColor(m_accentColor);
    m_islandWidth  = m_baseH; // 最初は丸（幅=高さ）

    // 透明度エフェクト
    m_opacity = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(m_opacity);
    m_opacity->setOpacity(0.0);

    // ボタン（Python版と同じ色・アイコン）
    btnSettings = new AnimatedButton("#4A90E2", "+",      this);
    btnClose    = new AnimatedButton("#FF5F57", "x",      this);
    btnMax      = new AnimatedButton("#FFBD2E", "square", this);
    btnMin      = new AnimatedButton("#28C940", "-",      this);
    for (auto *btn : {btnSettings, btnClose, btnMax, btnMin}) {
        btn->setAcceptDrops(true);
        btn->show();
        btn->setEnabled(false);
    }

    connect(btnSettings, &QPushButton::clicked, this, []{ /* 後でメニュー接続 */ });

    // ---- マスターボリューム(+ボタン ホイール操作) ----
    // +ボタンからカーソルが外れたら音量シーク表示をフェードアウトし、
    // 外れる前に戻ってきた場合はフェードアウトを止めて表示を維持する。
    connect(btnSettings, &AnimatedButton::hoverEntered, this, &DynamicIsland::onMasterVolumeButtonHoverEntered);
    connect(btnSettings, &AnimatedButton::hoverLeft,    this, &DynamicIsland::onMasterVolumeButtonHoverLeft);

    // カラーアニメ
    m_colorAnim = new QVariantAnimation(this);
    m_colorAnim->setDuration(500);
    connect(m_colorAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        m_accentColor = v.value<QColor>(); update();
        IslandControls::setAccentColor(m_accentColor);
    });

    setFixedHeight(m_baseH);
    setIslandWidth(m_baseH);

    // 高さアニメ（設定パネル展開用）
    // maximumHeight/minimumHeightを同時に動かしてガクつきを防ぐ
    m_heightAnim = new QVariantAnimation(this);
    m_heightAnim->setDuration(300);
    m_heightAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_heightAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        int h = v.toInt();
        setMinimumHeight(h);
        setMaximumHeight(h);
    });

    // 2026-09-15: 設定の開閉（幅＋高さ）。toggleSettingsPanel() 参照。
    m_settingsSizeAnim = new QVariantAnimation(this);
    m_settingsSizeAnim->setStartValue(0.0);
    m_settingsSizeAnim->setEndValue(1.0);
    // 2026-09-16: 開閉アニメが終わったら、画像から本物のパネルへ戻す。
    // （閉じるときの「終わったらパネルをhide」の単発接続より先に繋いでおく）
    connect(m_settingsSizeAnim, &QVariantAnimation::finished,
            this, &DynamicIsland::endPanelSnapshot);
    connect(m_settingsSizeAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        const double t = v.toDouble();
        const auto lerp = [t](int a, int b) { return qRound(a + (b - a) * t); };
        applySettingsIslandSize(QSize(lerp(m_settingsAnimFrom.width(),  m_settingsAnimTo.width()),
                                      lerp(m_settingsAnimFrom.height(), m_settingsAnimTo.height())));
    });
}

void DynamicIsland::setIslandWidth(int w)
{
    if (w == m_islandWidth && width() == w) return;

    m_islandWidth = w;
    setFixedWidth(w);

    // Python版と同じボタン配置
    // 各ボタンの中心X: [31, w-31, w-68, w-105]
    int bh = btnClose->height();
    int bw = btnClose->width();
    int yOff = m_baseH / 2 - bh / 2;
    int dynamicx = 10;
    btnSettings->move(31         - bw/2 - dynamicx, yOff);
    btnClose->move(   w - 31     - bw/2 + dynamicx, yOff);
    btnMax->move(     w - 68     - bw/2 + dynamicx, yOff);
    btnMin->move(     w - 105    - bw/2 + dynamicx, yOff);

    bool interactive = (w > 120);
    if (interactive != m_buttonsInteractive) {
        m_buttonsInteractive = interactive;
        // 2026-09-13: 確認プロンプト中は幅が閾値をまたいでも有効化しない。
        // プロンプトは畳まれた状態から出ることがあり、その展開途中で
        // ここを通ると、抑止したはずのボタンが復活してしまう。
        for (auto *btn : {btnSettings, btnClose, btnMax, btnMin})
            btn->setEnabled(interactive && !m_controlsSuppressed);
    }

    emit islandWidthChanged();
    update();
}

void DynamicIsland::prepareForFirstAnimation()
{
    ensurePolished();
    for (auto *btn : {btnSettings, btnClose, btnMax, btnMin})
        btn->ensurePolished();

    const int originalWidth = m_islandWidth;
    const QSize originalSize = size();
    const bool originalUpdates = updatesEnabled();
    const qreal originalOpacity = m_opacity ? m_opacity->opacity() : 1.0;
    setUpdatesEnabled(false);
    if (m_opacity) m_opacity->setOpacity(1.0);

    // Warm up QWidget polish, backing-store paths, opacity-effect buffering, and
    // child button paint paths before the first visible hover animation.
    for (int warmWidth : {m_baseH, 240}) {
        setIslandWidth(warmWidth);
        resize(warmWidth, height());
        QPixmap pixmap(size());
        pixmap.fill(Qt::transparent);
        render(&pixmap);

        for (auto *btn : {btnSettings, btnClose, btnMax, btnMin}) {
            btn->setVisualScale(1.0f);
            btn->setBounceScale(1.0f);
            QPixmap buttonPixmap(btn->size());
            buttonPixmap.fill(Qt::transparent);
            btn->render(&buttonPixmap);
            btn->resetState();
        }
    }

    resize(originalSize);
    setIslandWidth(originalWidth);
    if (m_opacity) m_opacity->setOpacity(originalOpacity);
    setUpdatesEnabled(originalUpdates);
    update();
}

void DynamicIsland::refreshAccentColor()
{
    QColor newColor = appAccentColor();
    if (newColor.rgba() == m_accentColor.rgba()) return;
    m_colorAnim->stop();
    m_colorAnim->setStartValue(m_accentColor);
    m_colorAnim->setEndValue(newColor);
    m_colorAnim->start();
    // 動画UI等の描き直し用（島自身は上のアニメで追従する）
    emit accentColorChanged(newColor);
}

namespace {
// 設定パネル展開用カスタムイージング。
// 従来のOutBack(amplitude=0.4)は終端の「っぴょん」感は良いが、t=0での
// 速度が線形の約3.7倍と急峻で、静止状態からいきなり高速で伸び始めるため
// 最初の1〜2フレームが「がくっ」と見えていた。時間軸をsmoothstep
// (3t²-2t³)で歪めてからOutBackへ渡すことで、開始速度ゼロ（ゆっくり
// 動き出す）〜終端のオーバーシュートは従来通り、という軌跡にする。
qreal settingsExpandEasing(qreal t)
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    const qreal warped = t * t * (3.0 - 2.0 * t);   // smoothstep: 始点速度0
    const qreal s = 1.70158 * 0.4;                  // OutBack amplitude=0.4相当
    const qreal u = warped - 1.0;
    return 1.0 + (s + 1.0) * u * u * u + s * u * u; // easeOutBack(warped)
}

// 2026-09-15: ウィンドウ全体へ展開するとき用。上の OutBack は行き過ぎが
// 「サイズの約4%」なので、全画面級の大きさだと数十pxになり、ウィンドウの
// 端（余白15px）を突き抜けてしまう。始点速度ゼロは同じにしたまま、行き過ぎの
// 無い OutCubic へ渡す。
qreal settingsFullExpandEasing(qreal t)
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    const qreal warped = t * t * (3.0 - 2.0 * t);   // smoothstep: 始点速度0
    const qreal u = 1.0 - warped;
    return 1.0 - u * u * u;                          // easeOutCubic(warped)
}
} // namespace

void DynamicIsland::toggleSettingsPanel(CanvasWidget *canvas)
{
    // 2026-07-13: 旧実装（PANEL_H=220固定＋move()手動座標配置＋項目追加の
    // たびに高さを手計算）は IslandSettingsPanel（自動レイアウト＋
    // ウィンドウ縦サイズ上限＋溢れたらスクロール）へ置き換えた。
    // 設定項目そのもの（本体のShow Grid等・プラグイン由来コントロール）は
    // IslandControlRegistry 経由で登録される（本体項目の登録は
    // MainWindow::registerIslandSettingsControls() 参照）。
    //
    // 2026-09-15: 下へ伸びるだけだったのを、幅・高さともウィンドウ全体
    // （端に kSettingsInset の余白）へ広げる形にした。
    //   - 幅は setIslandWidth() 経由で動かすので、中央寄せ（MainWindow::
    //     layoutIsland()）とウィンドウ操作ボタンの右寄せはそのまま追従する。
    //   - 幅を動かすのは本来 DynamicIslandController の役目なので、開閉アニメ中は
    //     コントローラが手を出さないようにしてある（isSettingsResizing()）。
    //   - キャンバス上のUI（CanvasOverlayWidget）は settingsOpenChanged で
    //     MainWindow が隠す。
    Q_UNUSED(canvas);
    m_settingsOpen = !m_settingsOpen;

    // 閉じアニメ途中の開き直しに備え、閉じ側が張った
    // 「finished→パネルhide」の単発接続を必ず切ってから開始する
    // （残したままだと開きアニメ完了時にパネルがhideされ、
    // 島は展開しているのに設定項目が見えなくなる）。
    disconnect(m_heightAnimFinishedConn);
    m_heightAnim->stop();
    m_settingsSizeAnim->stop();
    m_settingsAnimFrom = QSize(m_islandWidth, height());

    if (m_settingsOpen) {
        // アイコンを「−」に
        btnSettings->setIconType("-");

        ensureSettingsPanel();

        const QSize target = settingsExpandedSize();
        m_settingsPanel->applyPanelGeometry(target.width(), target.height());
        m_settingsPanel->setVisibleWidth(m_islandWidth);
        m_settingsPanel->lower();
        m_settingsPanel->show();
        // タブバーは分裂が終わるまで隠す（下の setTabBarSuppressed）。画像にも
        // 写らないよう、先に隠してから撮る。
        m_settingsPanel->setTabBarSuppressed(true);
        beginPanelSnapshot();

        // 2026-09-15: タブは＋ボタンから分裂して現れる。静止用のタブは
        // 分裂が終わるまで隠しておく（IslandTabGooLayer のコメント参照）。
        ensureTabGoo();
        m_settingsPanel->setTabBarSuppressed(true);
        m_tabGoo->startOpen();

        // 始点速度ゼロ・行き過ぎ無し（settingsFullExpandEasing()参照）。
        m_settingsAnimTo = target;
        m_settingsSizeAnim->setDuration(420);
        QEasingCurve curve;
        curve.setCustomType(settingsFullExpandEasing);
        m_settingsSizeAnim->setEasingCurve(curve);
        m_settingsSizeAnim->start();
        emit settingsOpenChanged(true);
    } else {
        // アイコンを「+」に戻す
        btnSettings->setIconType("+");

        // 2026-09-15: タブは＋ボタンへ吸い込まれて消える。
        if (m_settingsPanel)
            m_settingsPanel->setTabBarSuppressed(true);
        ensureTabGoo();
        m_tabGoo->startClose();
        if (m_settingsPanel && m_settingsPanel->isVisible())
            beginPanelSnapshot();

        // 閉じる。戻り先は「出ている島」の通常の姿（幅240の
        // ピル）。カーソルが離れていれば、アニメ後にコントローラが畳む。
        m_settingsAnimTo = QSize(240, m_baseH);
        // 2026-09-15: 開くときと同じ速さ・同じカーブに揃えた（タブの吸い込み
        // IslandTabGoo::kCloseMs も同じ長さ）。
        m_settingsSizeAnim->setDuration(420);
        {
            QEasingCurve curve;
            curve.setCustomType(settingsFullExpandEasing);
            m_settingsSizeAnim->setEasingCurve(curve);
        }
        // 接続ハンドルを保持し、開き直し時に確実に切れるようにする。
        // ラムダ内でもm_settingsOpenを再確認し、万一発火が漏れても
        // 「開いているのにhide」だけは起きないよう二重に防ぐ。
        m_heightAnimFinishedConn =
            connect(m_settingsSizeAnim, &QVariantAnimation::finished, this, [this](){
                if (m_settingsPanel && !m_settingsOpen) m_settingsPanel->hide();
            }, Qt::SingleShotConnection);
        m_settingsSizeAnim->start();
        // 畳み始めと同時に戻し始める（isRetracting() と同じ考え方）。
        emit settingsOpenChanged(false);
    }
}

void DynamicIsland::beginPanelSnapshot()
{
    static const bool s_enabled =
        qEnvironmentVariable("PB_PANEL_SNAPSHOT", QStringLiteral("1"))
        != QLatin1String("0");
    if (!s_enabled || !m_settingsPanel)
        return;

    // 開き直しなどで既に画像を出している場合は、いったん本物へ戻してから撮る
    // （描き直さないと、前回撮った古い状態が残る）。
    const bool wasSnapshot = m_panelSnapshot && !m_panelSnapshot->isHidden();
    if (wasSnapshot) {
        m_panelSnapshot->hide();
        m_settingsPanel->show();
    }

    const QRect geo = m_settingsPanel->geometry();
    if (geo.width() <= 0 || geo.height() <= 0)
        return;
    const qreal dpr = devicePixelRatioF();
    QPixmap image(qMax(1, qRound(geo.width() * dpr)), qMax(1, qRound(geo.height() * dpr)));
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    // 背景は描かせない（パネルは透明。本体の色はGL側の板）。子だけを描く。
    m_settingsPanel->render(&image, QPoint(), QRegion(), QWidget::DrawChildren);

    if (!m_panelSnapshot) {
        m_panelSnapshot = new QLabel(this);
        m_panelSnapshot->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_panelSnapshot->setAttribute(Qt::WA_TranslucentBackground, true);
        m_panelSnapshot->setStyleSheet(QStringLiteral("background: transparent;"));
        m_panelSnapshot->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    }
    m_panelSnapshot->setPixmap(image);
    m_panelSnapshot->setGeometry(geo);
    // パネルと同じ重なり順（島のボタン・液体のレイヤーより下）に置く。
    m_panelSnapshot->stackUnder(m_settingsPanel);
    m_panelSnapshot->show();
    m_settingsPanel->hide();
}

void DynamicIsland::endPanelSnapshot()
{
    if (!m_panelSnapshot || m_panelSnapshot->isHidden())
        return;
    m_panelSnapshot->hide();
    m_panelSnapshot->clear();   // 画像のメモリを返す
    // 開いているときだけ本物を戻す。閉じたときは閉じ側の処理がパネルを隠す。
    if (m_settingsPanel && m_settingsOpen) {
        m_settingsPanel->show();
        m_settingsPanel->lower();
    }
}

bool DynamicIsland::isSettingsResizing() const
{
    if (m_settingsSizeAnim && m_settingsSizeAnim->state() == QAbstractAnimation::Running)
        return true;
    // 2026-09-15: 吸い込みアニメ（島の幅240へ戻った後もしばらく続く）の間も
    // コントローラに島を畳ませない。畳み始めると雫の行き先（島の幅から決まる
    // タブの右端）が一緒に縮んで形が崩れるため。
    return m_tabGoo && m_tabGoo->mode() == IslandTabGooLayer::Mode::Close;
}

void DynamicIsland::ensureTabGoo()
{
    if (m_tabGoo)
        return;
    m_tabGoo = new IslandTabGooLayer(btnSettings, m_baseH, this);
    {
        // IslandSettingsPanel::setTabBarInsets と同じ値（ensureSettingsPanel参照）
        constexpr int kTabGap = 8;
        const int left = btnSettings->x() + btnSettings->width() + kTabGap;
        const int rightSpace = (m_islandWidth - btnMin->x()) + kTabGap;
        m_tabGoo->setTabInsets(left, rightSpace);
    }
    m_tabGoo->tabContentsProvider = [this](QVector<IslandTabGoo::TabItem> &tabs, int &current) {
        if (!m_settingsPanel)
            return;
        tabs = m_settingsPanel->tabItems();
        current = m_settingsPanel->currentTabIndex();
    };
    m_tabGoo->onFinished = [this](IslandTabGooLayer::Mode finished) {
        // 開き切ったら静止用のタブへ引き継ぐ。閉じ終わりはパネルごと隠れている。
        if (finished == IslandTabGooLayer::Mode::Open && m_settingsPanel && m_settingsOpen)
            m_settingsPanel->setTabBarSuppressed(false);
        if (finished == IslandTabGooLayer::Mode::Close && m_settingsPanel)
            m_settingsPanel->setTabBarSuppressed(false);
    };
}

void DynamicIsland::applySettingsIslandSize(const QSize &size)
{
    setIslandWidth(size.width());
    if (minimumHeight() != size.height() || maximumHeight() != size.height()) {
        setMinimumHeight(size.height());
        setMaximumHeight(size.height());
    }
    if (m_settingsPanel && m_settingsPanel->isVisible())
        m_settingsPanel->setVisibleWidth(size.width());
}

void DynamicIsland::ensureSettingsPanel()
{
    if (m_settingsPanel)
        return;

    m_settingsPanel = new IslandSettingsPanel(m_islandWidth, m_baseH, this);
    m_settingsPanel->rebuild();

    // 2026-09-15: 上端のタブは設定ボタンと最小化ボタンの間に置く。
    // ボタンは setIslandWidth() で両端に寄せて置かれるので、端からの距離は
    // 島の幅によらず一定。今のボタン位置から一度だけ求めて渡す。
    {
        constexpr int kTabGap = 8;
        const int left = btnSettings->x() + btnSettings->width() + kTabGap;
        const int rightSpace = (m_islandWidth - btnMin->x()) + kTabGap;
        m_settingsPanel->setTabBarInsets(left, rightSpace);

        // 2026-09-16: 設定項目の行の揃え線。行頭の点は＋ボタンの中心、項目名は
        // タブの左端、部品の右端は×ボタンの中心に揃える（島の上端と縦に通る）。
        const int markerX = btnSettings->x() + btnSettings->width() / 2;
        const int closeCenterX = btnClose->x() + btnClose->width() / 2;
        m_settingsPanel->setRowGuides(markerX, left, m_islandWidth - closeCenterX);
    }

    // プラグインのインストール等でコントロールが増減したら、パネルを
    // 再構築する（2026-09-15: 高さは項目数ではなくウィンドウで決まるので、
    // ジオメトリを当て直すだけ）。
    connect(&IslandControlRegistry::instance(), &IslandControlRegistry::sectionsChanged,
            this, [this]{
        if (!m_settingsPanel)
            return;
        m_settingsPanel->rebuild();
        refreshSettingsPanelGeometry();
    });

    // 初回生成直後にオフスクリーンで一度描画しておく
    // （prepareForFirstAnimation()と同じウォームアップ手法）。
    // スタイルシートのpolishや子ウィジェット群の初回paint経路の
    // 準備がアニメ開始後に走ると、その間も壁時計ベースの
    // QVariantAnimationの時間だけが進み、最初の可視フレームが
    // 数十ms分先へ飛んで「がくっ」と見える原因になる。
    // 2026-09-15: 描くのは項目の列の範囲だけで足りる（列の外は透明で、
    // ウィンドウサイズのピクスマップを確保する意味が無い）。
    m_settingsPanel->ensurePolished();
    const QSize warm = settingsExpandedSize();
    m_settingsPanel->applyPanelGeometry(warm.width(), warm.height());
    const int columnW = m_settingsPanel->columnWidth();
    QPixmap warmPixmap(columnW, warm.height());
    warmPixmap.fill(Qt::transparent);
    m_settingsPanel->render(&warmPixmap, QPoint(-(warm.width() - columnW) / 2, 0),
                            QRegion((warm.width() - columnW) / 2, 0, columnW, warm.height()));
}

int DynamicIsland::settingsPanelMaxHeight() const
{
    // 島はウィンドウ上端から15pxの位置に置かれる（MainWindow::
    // layoutIsland()）。下側にも余白を残し、ウィンドウが極端に低い場合
    // でも最低1行分は確保する。
    const QWidget *host = window();
    const int hostH = host ? host->height() : 600;
    return qMax(m_baseH + 48, hostH - 15 - 20);
}

QSize DynamicIsland::settingsExpandedSize() const
{
    // 上端は島の定位置(15px)、左右と下は kSettingsInset。
    // 極端に小さいウィンドウでも、通常の展開幅(240)と最低1行分は確保する。
    const QWidget *host = window();
    const int hostW = host ? host->width()  : 800;
    const int hostH = host ? host->height() : 600;
    // 2026-09-16: 大きいウィンドウでは kSettingsMaxWidth/Height で頭打ち。
    return QSize(qMax(240, qMin(kSettingsMaxWidth, hostW - kSettingsInset * 2)),
                 qMax(m_baseH + 48, qMin(kSettingsMaxHeight, hostH - 15 - kSettingsInset)));
}

void DynamicIsland::refreshSettingsPanelGeometry()
{
    if (!m_settingsPanel || !m_settingsOpen)
        return;
    const QSize target = settingsExpandedSize();
    m_settingsPanel->applyPanelGeometry(target.width(), target.height());

    // 開きアニメ中なら行き先だけ差し替える（止めると展開が途中で切れる）。
    // 補間は毎フレーム m_settingsAnimTo を読むので、次のフレームから新しい
    // サイズへ向かう。
    if (isSettingsResizing()) {
        m_settingsAnimTo = target;
        m_settingsPanel->setVisibleWidth(m_islandWidth);
        // パネルの寸法が変わったので、撮っておいた画像は使えない。
        endPanelSnapshot();
        return;
    }
    applySettingsIslandSize(target);
}

// ============================================================
//  確認プロンプト（2026-09-13）
//  設計メモ: 中身の描画は paintEvent() ではなく IslandPromptPanel
//  （島の子ウィジェット）が受け持つ。設定パネルと同じ構造なので、
//  高さアニメ（m_heightAnim）と上限計算（settingsPanelMaxHeight）を
//  そのまま共有できる。
// ============================================================
void DynamicIsland::showPrompt(const IslandPromptContent &content,
                               std::function<void(bool)> answered)
{
    // 既にプロンプトが出ている場合は「拒否」として打ち切ってから差し替える。
    // 通常は呼び出し側（MainWindow）がキューで直列化しているので通らない。
    if (m_promptOpen) {
        auto previous = m_promptCallback;
        m_promptCallback = nullptr;
        m_promptOpen = false;
        if (previous)
            previous(false);
    }

    // 設定パネルとは排他。開いていたら畳まずに状態だけ戻す
    // （高さアニメはこの直後にプロンプト用で上書きするため）。
    if (m_settingsOpen) {
        m_settingsOpen = false;
        btnSettings->setIconType(QStringLiteral("+"));
        disconnect(m_heightAnimFinishedConn);
        // 2026-09-15: 設定の幅＋高さアニメも止める。幅はこの後コントローラの
        // openPromptView() が、高さは下の m_heightAnim が今の値から引き継ぐ。
        m_settingsSizeAnim->stop();
        if (m_tabGoo)
            m_tabGoo->stop();   // ＋ボタンの本体表示も元に戻る
        if (m_settingsPanel)
            m_settingsPanel->hide();
        endPanelSnapshot();
        emit settingsOpenChanged(false);
    }

    if (!m_promptPanel) {
        m_promptPanel = new IslandPromptPanel(m_islandWidth, m_baseH, this);
        connect(m_promptPanel, &IslandPromptPanel::accepted,
                this, [this]{ finishPrompt(true); });
        connect(m_promptPanel, &IslandPromptPanel::rejected,
                this, [this]{ finishPrompt(false); });

        // 幅アニメーション（DynamicIslandController::m_islandAnim）に
        // パネルを追従させる。これが無いと、最終幅でレイアウトされた
        // ボタン行が島にクリップされ、右端のNOだけが島の右端の動き
        // （OutBackの行き過ぎと戻り）に合わせて伸び縮みして見える。
        connect(this, &DynamicIsland::islandWidthChanged, this, [this]{
            if (m_promptOpen && m_promptPanel)
                m_promptPanel->setVisibleWidth(m_islandWidth);
        });
    }

    // 設定の閉じアニメが走っている最中にプロンプトが来た場合も、そちらを止めて
    // からプロンプトの幅・高さアニメに引き継ぐ（二重に幅を動かさない）。
    if (m_tabGoo)
        m_tabGoo->stop();
    if (m_settingsSizeAnim->state() == QAbstractAnimation::Running) {
        m_settingsSizeAnim->stop();
        disconnect(m_heightAnimFinishedConn);
        if (m_settingsPanel && !m_settingsOpen)
            m_settingsPanel->hide();
        endPanelSnapshot();
    }

    m_promptCallback = std::move(answered);
    m_promptOpen = true;
    m_promptPanel->setContent(content);

    // 幅は島本体のアニメーション（DynamicIslandController::openPromptView）が
    // 広げるが、高さ計算には最終的な幅が要る（文言の折り返し行数が変わる）。
    // ここでは目標幅で先に計算しておく。
    const int targetW = promptIslandWidth();
    const int targetH = m_promptPanel->preferredPanelHeight(targetW, settingsPanelMaxHeight());
    m_promptPanel->applyPanelGeometry(targetW, targetH);
    // レイアウトは最終幅で確定させた上で、表示は今の島幅から始める
    // （以降は islandWidthChanged で毎フレーム追従する）。
    m_promptPanel->setVisibleWidth(m_islandWidth);
    // 島上部の丸ボタンより下に敷く（設定パネルと同じ）。
    m_promptPanel->lower();
    m_promptPanel->show();

    setControlsSuppressed(true);

    // 展開アニメは設定パネルと同じカーブ（開始速度ゼロ→終端オーバーシュート）。
    disconnect(m_promptAnimFinishedConn);
    disconnect(m_heightAnimFinishedConn);
    m_heightAnim->stop();
    m_heightAnim->setDuration(450);
    QEasingCurve bounceCurve;
    bounceCurve.setCustomType(settingsExpandEasing);
    m_heightAnim->setEasingCurve(bounceCurve);
    m_heightAnim->setStartValue(height());
    m_heightAnim->setEndValue(targetH);
    m_heightAnim->start();
}

void DynamicIsland::finishPrompt(bool answer)
{
    if (!m_promptOpen)
        return;

    m_promptOpen = false;
    setControlsSuppressed(false);

    m_heightAnim->stop();
    m_heightAnim->setDuration(250);
    m_heightAnim->setEasingCurve(QEasingCurve::OutCubic);
    m_heightAnim->setStartValue(height());
    m_heightAnim->setEndValue(m_baseH);
    disconnect(m_promptAnimFinishedConn);
    m_promptAnimFinishedConn =
        connect(m_heightAnim, &QVariantAnimation::finished, this, [this]{
            if (m_promptPanel && !m_promptOpen)
                m_promptPanel->hide();
        }, Qt::SingleShotConnection);
    m_heightAnim->start();

    // コールバックは最後に、かつメンバから外してから呼ぶ。
    // コールバックの中から次のプロンプトが開かれても（＝連続ドロップの
    // キュー処理）、こちらの後始末とは干渉しない。
    auto callback = m_promptCallback;
    m_promptCallback = nullptr;
    if (callback)
        callback(answer);
}

void DynamicIsland::setControlsSuppressed(bool suppressed)
{
    if (m_controlsSuppressed == suppressed)
        return;
    m_controlsSuppressed = suppressed;
    for (auto *btn : {btnSettings, btnClose, btnMax, btnMin}) {
        btn->setDimmed(suppressed);
        btn->setEnabled(!suppressed && m_buttonsInteractive);
    }
}

void DynamicIsland::notifyHostWindowResized()
{
    // プロンプト表示中は、ウィンドウが縮んで上限へ当たったときだけ
    // 高さを詰め直す（文言の折り返しは幅固定なので変わらない）。
    if (m_promptOpen && m_promptPanel) {
        const int targetH = m_promptPanel->preferredPanelHeight(promptIslandWidth(),
                                                                settingsPanelMaxHeight());
        if (m_heightAnim->state() == QAbstractAnimation::Running
            && m_heightAnim->endValue().toInt() == targetH)
            return;
        if (m_heightAnim->state() != QAbstractAnimation::Running
            && maximumHeight() == targetH)
            return;
        m_promptPanel->applyPanelGeometry(promptIslandWidth(), targetH);
        m_promptPanel->setVisibleWidth(m_islandWidth);
        m_heightAnim->stop();
        setMinimumHeight(targetH);
        setMaximumHeight(targetH);
        return;
    }

    if (!m_settingsPanel || !m_settingsOpen)
        return;

    // 2026-09-15: 設定はウィンドウ全体へ展開するので、幅・高さとも追従させる。
    // ウィンドウのリサイズアニメ（animateToAspect等）中は resizeEvent が
    // 毎フレーム来るため、変化が無ければ何もしない。開きアニメ中は止めずに
    // 行き先だけ差し替える（refreshSettingsPanelGeometry()）。
    const QSize target = settingsExpandedSize();
    if (isSettingsResizing()) {
        if (m_settingsAnimTo == target)
            return;
    } else if (m_islandWidth == target.width() && maximumHeight() == target.height()) {
        return;
    }
    refreshSettingsPanelGeometry();
}

void DynamicIsland::updateMasterVolumeIcon(int value)
{
    // 指定は1〜33/34〜66/67〜100の3等分、0はミュート。
    if (value <= 0)         btnSettings->setIconType(QStringLiteral("sound0"));
    else if (value <= 33)   btnSettings->setIconType(QStringLiteral("sound1"));
    else if (value <= 66)   btnSettings->setIconType(QStringLiteral("sound2"));
    else                    btnSettings->setIconType(QStringLiteral("sound3"));
}

void DynamicIsland::notifyMasterVolumeChanged(int value)
{
    m_masterVolumeValue = qBound(0, value, 100);
    updateMasterVolumeIcon(m_masterVolumeValue);
    // シークバー境界(filledW)が動くので必ず再描画する。
    // 以前はフェードアニメを毎回再スタートしていたためvalueChanged経由の
    // update()に暗黙に依存していたが、α=1.0到達後はアニメを起動しなく
    // なったため、ここで明示的に要求する必要がある。
    update();

    if (!m_masterVolumeFadeAnim) {
        m_masterVolumeFadeAnim = new QVariantAnimation(this);
        connect(m_masterVolumeFadeAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
            m_masterVolumeGestureAlpha = v.toDouble();
            update();
        });
    }
    // 途中でフェードアウト中だった場合も、今のアルファ値から1.0へ
    // なめらかに繋ぎ直す（フェードアウト完了時のアイコン復帰接続は
    // stop()では発火しないため、明示的に切っておく）。
    // start/endは必ずdoubleで統一すること（型が食い違うと
    // QVariantAnimationの補間器が正しく機能せず、値が実質固定されたまま
    // アニメーションしているように見えなくなる不具合になる）。
    disconnect(m_masterVolumeFadeFinishedConn);
    // 既に1.0へ向かってフェードイン中なら再スタートしない。
    // ホイールのたびに150msのOutCubicを最初からやり直すと、連続操作中は
    // αが漸近的にしか上がらず（実測で1.0到達まで数百ms）、その間ずっと
    // 「本来より薄い暗色」が見えてしまう。表示済み(α=1.0)の場合も
    // start()し直す意味はないので、フェードアウト中→イン切り替えと
    // 初回表示のときだけ開始する。
    const bool fadingIn =
        (m_masterVolumeFadeAnim->state() == QAbstractAnimation::Running
         && m_masterVolumeFadeAnim->endValue().toDouble() > 0.5);
    if (!fadingIn && m_masterVolumeGestureAlpha < 0.999) {
        m_masterVolumeFadeAnim->stop();
        m_masterVolumeFadeAnim->setDuration(150);
        m_masterVolumeFadeAnim->setEasingCurve(QEasingCurve::OutCubic);
        m_masterVolumeFadeAnim->setStartValue(m_masterVolumeGestureAlpha);
        m_masterVolumeFadeAnim->setEndValue(1.0);
        m_masterVolumeFadeAnim->start();
    }

    // タイムアウトでは消さない。+ボタンからカーソルが外れるまでは
    // 音量シーク表示のまま維持する（フェードアウトはhoverLeft経由のみ）。
}

void DynamicIsland::onMasterVolumeButtonHoverEntered()
{
    // フェードアウト中に戻ってきた場合は、その場で止めて表示を維持する
    // （ホイールしていなくても、ボタン上にいる間は音量シーク表示のまま）。
    if (!m_masterVolumeFadeAnim)
        return;
    if (m_masterVolumeFadeAnim->state() == QAbstractAnimation::Running
        && m_masterVolumeFadeAnim->endValue().toDouble() < 0.5) {
        disconnect(m_masterVolumeFadeFinishedConn);
        m_masterVolumeFadeAnim->stop();
    }
}

void DynamicIsland::onMasterVolumeButtonHoverLeft()
{
    // まだ一度もホイール操作していない（音量シーク表示がそもそも
    // 出ていない）場合は何もしない。
    if (m_masterVolumeGestureAlpha <= 0.001f && !m_masterVolumeFadeAnim)
        return;
    if (!m_masterVolumeFadeAnim)
        return;

    disconnect(m_masterVolumeFadeFinishedConn);
    m_masterVolumeFadeAnim->stop();
    m_masterVolumeFadeAnim->setDuration(350);
    m_masterVolumeFadeAnim->setEasingCurve(QEasingCurve::InCubic);
    m_masterVolumeFadeAnim->setStartValue(m_masterVolumeGestureAlpha);
    m_masterVolumeFadeAnim->setEndValue(0.0);
    m_masterVolumeFadeFinishedConn = connect(m_masterVolumeFadeAnim, &QVariantAnimation::finished, this, [this]{
        // 設定パネル開閉中の"-"表示を上書きしないよう、現在の開閉状態に
        // 合わせて+ボタンのアイコンを戻す（toggleSettingsPanel()と同じ規則）。
        btnSettings->setIconType(m_settingsOpen ? QStringLiteral("-") : QStringLiteral("+"));
    });
    m_masterVolumeFadeAnim->start();
}

// 2026-09-14: 島の本体の絵を決めている値を1つにまとめたもの（paintEvent()
// のキャッシュキー）。ここが前回と同じなら、描き直しても1ピクセルも
// 変わらない。
//
// 不透明度は入れない: QGraphicsOpacityEffect が外から掛けるものなので、
// paintEvent() が描く絵そのものは不透明度では変わらない。
//
// 【追加するときの注意】paintEvent() の本体が新しく参照するようになった
// 状態は、必ずここにも足すこと。忘れると「値は変わったのにキャッシュが
// 効いたまま」で、絵が更新されない静かな不具合になる。
quint64 DynamicIsland::islandBodyCacheKey() const
{
    const auto mix = [](quint64 h, quint64 v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    };
    quint64 h = 0xcbf29ce484222325ULL;
    h = mix(h, static_cast<quint64>(width()));
    h = mix(h, static_cast<quint64>(height()));
    h = mix(h, static_cast<quint64>(m_baseH));
    h = mix(h, static_cast<quint64>(m_islandWidth));
    h = mix(h, static_cast<quint64>(m_accentColor.rgba()));
    h = mix(h, static_cast<quint64>(m_masterVolumeValue));
    h = mix(h, static_cast<quint64>(
                   qRound(m_masterVolumeGestureAlpha * 255.0f)));
    return h;
}

#ifndef NDEBUG
void DynamicIsland::reportIslandStat()
{
    if (!m_statWindow.isValid()) {
        m_statWindow.start();
        return;
    }
    if (m_statWindow.elapsed() < 2000)
        return;
    const double perBuild = m_statBuilds > 0 ? 1.0 / m_statBuilds : 0.0;
    qDebug("[Island][stat] paints=%d sameKey=%d hidden=%d builds=%d "
           "paint=%.3fms(max %.3f) build=%.3fms size=%dx%d",
           m_statPaints, m_statPaintsSame, m_statPaintsHidden, m_statBuilds,
           m_statPaints > 0 ? m_statPaintMsSum / m_statPaints : 0.0,
           m_statPaintMsMax,
           m_statBuilds > 0 ? m_statBuildMsSum / m_statBuilds : 0.0,
           width(), height());
    if (m_statBuilds > 0) {
        // 1ビルドあたりの内訳。plus= が縁の光（加算4種）ぶん。
        // strokes= が出ていれば bandRect クリップ入りの版が動いている
        // （反映確認用。この行自体が2026-09-14の変更で新しく増えたもの）。
        // v= はどの版が動いているかの目印。[BUILD] のタイムスタンプは
        // DynamicIsland.cpp を直しても更新されないため当てにならない。
        qDebug("[Island][build] v=a6 base=%.3f frost=%.3f outline=%.3f plus=%.3f "
               "shade=%.3f contour=%.3f end=%.3f strokes=%.1f "
               "(ms/build, builds=%d size=%dx%d)",
               m_statBuildBaseMs * perBuild, m_statBuildFrostMs * perBuild,
               m_statBuildOutlineMs * perBuild,
               m_statBuildPlusMs * perBuild, m_statBuildShadeMs * perBuild,
               m_statBuildContourMs * perBuild, m_statBuildEndMs * perBuild,
               m_statBuildStrokes * perBuild,
               m_statBuilds, width(), height());
    }
    m_statWindow.restart();
    m_statPaints = m_statPaintsSame = m_statPaintsHidden = 0;
    m_statBuilds = 0;
    m_statBuildMsSum = 0.0;
    m_statBuildBaseMs = m_statBuildFrostMs = m_statBuildPlusMs = 0.0;
    m_statBuildShadeMs = m_statBuildContourMs = m_statBuildEndMs = 0.0;
    m_statBuildOutlineMs = 0.0;
    m_statBuildStrokes = 0;
    m_statPaintMsSum = m_statPaintMsMax = 0.0;
}
#endif

// ---- 島の本体のGL登録（2026-09-22 切り出し）--------------------------
// 以前はこの登録を paintEvent() の中だけで行っていた。ところが Qt は1回の
// 再描画で「下の QOpenGLWidget（キャンバス）の paintGL → 上に乗るウィジェットの
// paintEvent」の順に描くため、島の幅が変わったフレームでは
//   - 丸ボタンのGL登録 … moveEvent で即座に新しい位置へ（＝このフレームに間に合う）
//   - 島の本体のGL登録 … paintEvent で更新（＝paintGL の後。次のフレームまで古い幅）
// となり、GL側では「本体は1フレーム前の幅・右のボタンは今の位置」で描かれていた。
// 島は中央寄せなので幅が増えると左端は追従し右端だけが足りなくなり、
// 右側のボタンが本体から少しはみ出して見えた（左の＋ボタンは左端基準なので無事）。
// キャンバスの再描画が詰まるとこのズレが2フレーム以上になり、展開中の
// 「ガクッ」とした揺れにも見えていた。
// サイズが変わった瞬間（resizeEvent）にも登録し直し、ボタンと同じフレームに揃える。
void DynamicIsland::syncBodyGlass()
{
    const int W = width();
    const int H = height();
    // paintEvent() の path と同じ寸法（kStrokeMargin と角丸半径の決め方）。
    constexpr qreal kStrokeMargin = 1.0;
    const qreal left   = kStrokeMargin;
    const qreal top    = kStrokeMargin;
    const qreal right  = W - kStrokeMargin;
    const qreal bottom = H - kStrokeMargin;
    const qreal r = qMin(qMin(m_baseH / 2.0, (bottom - top) / 2.0),
                         (right - left) / 2.0);

    const qreal islandOpacity = m_opacity ? m_opacity->opacity() : 1.0;

    // 2026-09-16: 島の本体とガラス表現（縁の光・翳り・輪郭線）をGL側へ移した。
    // ぼかしと同じ形・同じ不透明度で、BackdropBlurが直後にシェーダーで描く
    // （Blur.h の GlassPlateStyle に理由と方式）。展開アニメーション中に
    // 毎フレーム走っていたQPainterの約57本のストローク描画が無くなる。
    // 見比べ用に PB_ISLAND_GL_GLASS=0 で従来のQPainter描画へ戻せる。
    const bool kGlassOnGpu = islandGlassOnGpu();
    {
        BackdropBlur::GlassPlateStyle glass;
        if (kGlassOnGpu) {
            glass.enabled = true;
            QColor body = m_accentColor;
            body.setAlpha(kIslandBodyAlpha);
            glass.body = body;
            // 旧コードの shadeColor と同じ式（彩度×1.10・明度×0.42）。
            const QColor hsv = m_accentColor.toHsv();
            glass.shade = QColor::fromHsv(
                qMax(0, hsv.hue()),
                qMin(255, static_cast<int>(hsv.saturation() * 1.10)),
                static_cast<int>(hsv.value() * 0.42));
            glass.baseH = m_baseH;
        }
        m_backdropBlur.setGlassStyle(glass);
    }
    m_backdropBlur.setShape(QRectF(left, top, right - left, bottom - top),
                            r, kBackdropBlurRadius, islandOpacity);
    // 丸ボタンと設定タブの液体アニメもGL側で描いているので、島の不透明度の
    // 変化をあちらへも伝える（値が変わっていなければ何も起きない）。
    if (kGlassOnGpu) {
        for (auto *btn : {btnSettings, btnClose, btnMax, btnMin}) {
            if (btn)
                btn->syncGlass();
        }
        if (m_tabGoo)
            m_tabGoo->syncGlass();
    }
    // 2026-09-22: 静止しているタブの面もGL側（IslandSettingsTabBar::syncGlass）。
    if (m_settingsPanel)
        m_settingsPanel->syncTabGlass();
}

void DynamicIsland::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    syncBodyGlass();
}

void DynamicIsland::paintEvent(QPaintEvent *)
{
#ifndef NDEBUG
    QElapsedTimer statTimer;
    statTimer.start();
    const quint64 statKey = islandBodyCacheKey();
    const bool statSameAsLast = m_statContentKeyInit && statKey == m_statContentKey;
    m_statContentKey = statKey;
    m_statContentKeyInit = true;
#endif

    int W = width();
    int H = height();

    // 2026-07-12 バグ修正: 輪郭をなぞるストローク（当時はカーソル追従グロー、
    // 現在はガラス表現の輪郭線）を
    // このpathぴったりの輪郭でdrawPath()すると、ペンの太さの半分が
    // 輪郭の外側にはみ出す。以前はpathが座標(0,0)〜(W,H)ちょうどにタッチ
    // していたため、そのはみ出し分がウィジェット境界の外＝クリップ対象に
    // なり、特に左端・上端の丸みが「途切れて」見えていた（右端・下端も
    // 本来同じだけ切れるが、影や余白の付き方の違いで目立ちにくかった）。
    // ペンの半幅より少し余裕を持たせた1pxぶん内側にpath自体を縮めることで、
    // ストロークの外側半分もウィジェット内に収まるようにする。
    constexpr qreal kStrokeMargin = 1.0;
    const qreal left   = kStrokeMargin;
    const qreal top    = kStrokeMargin;
    const qreal right  = W - kStrokeMargin;
    const qreal bottom = H - kStrokeMargin;

    // 角丸半径。基本高さの半分(=21px)をそのまま使っていたが、pathは上下
    // kStrokeMarginぶん内側にあるため実際の高さは40pxしかなく、半径21pxの
    // 弧が上下辺をはみ出して両端に段差(小さな突起)ができていた。
    // 2026-09-11: pathの実寸から半径を決め、非展開時はきれいなピルにする。
    // 展開時は高さが増えるのでm_baseH/2側で頭打ちになり、従来と同じ角丸になる。
    const qreal r = qMin(qMin(m_baseH / 2.0, (bottom - top) / 2.0),
                         (right - left) / 2.0);

    // 上半円の矩形（上半分）: y=top..top+r
    // 下半円の矩形（下半分）: y=(bottom-r)..bottom
    // 中央ストレッチ: y=(top+r)..(bottom-r)

    QPainterPath path;
    // 左上の角から時計回りに描画
    // 上辺（左→右）
    path.moveTo(left + r, top);
    path.lineTo(right - r, top);
    // 右上角丸
    path.arcTo(right - 2*r, top, 2*r, 2*r, 90, -90);
    // 右辺（上→下）
    path.lineTo(right, bottom - r);
    // 右下角丸
    path.arcTo(right - 2*r, bottom - 2*r, 2*r, 2*r, 0, -90);
    // 下辺（右→左）
    path.lineTo(left + r, bottom);
    // 左下角丸
    path.arcTo(left, bottom - 2*r, 2*r, 2*r, 270, -90);
    // 左辺（下→上）
    path.lineTo(left, top + r);
    // 左上角丸
    path.arcTo(left, top, 2*r, 2*r, 180, -90);
    path.closeSubpath();

    // 背後のぼかし範囲を、いま描こうとしている形そのもので登録する。
    // 値が変わったときだけキャンバスへ再描画が要求されるので毎回呼んでよい。
    // (実際のぼかしはCanvasWidget::paintGL()のGLパス側。設計はBlur.h参照)
    //
    // 2026-09-11: 効き具合には自分の不透明度をそのまま渡す。設定パネル展開中に
    // キャンバスを操作すると、DynamicIslandController::tick()が
    // setVisible()ではなくQGraphicsOpacityEffectの不透明度を0へフェードして
    // 島を一時的に隠す。その間ブラー側の登録値は何も変わらないため、
    // 「値が変わったら再描画」の判定に引っかからず、操作をやめて島が戻っても
    // ブラーが掛け直されないままになっていた(動かすと直るのは、別の理由で
    // キャンバスが再描画されるから)。不透明度を登録値に含めれば、
    // フェードのたびに再描画が要求され、動画UIと同じ挙動になる。
    // 2026-09-22: 背後のぼかしとGLのガラス板の登録は syncBodyGlass() へ切り出した
    // （サイズが変わった瞬間＝resizeEvent からも呼ぶため。理由はそちらのコメント）。
    syncBodyGlass();
    const bool kGlassOnGpu = islandGlassOnGpu();

    // ---- 島の本体の描画（結果を QPixmap へキャッシュ）------------------
    //
    // 2026-09-14: ここから下の描画は「幅・高さ・アクセント色・マスター
    // ボリュームの表示」だけで決まる。カーソル位置にも不透明度にも依存しない。
    // それなのに実測では毎秒25〜50回走っており、[Island][stat] の sameKey が
    // そのほぼ全数を占めていた（＝毎回まったく同じ絵を描き直していた）。
    //
    // 原因はこちらが出す update() ではなく、外からの再合成:
    //   - 島には QGraphicsOpacityEffect が掛かっているため、子ボタンが1つ
    //     再描画されるだけでもエフェクトの元画像＝島全体が描き直される
    //   - 下の QOpenGLWidget が present すると兄弟ウィジェットごと再合成される
    // どちらもこちらからは止めにくい。なので「描く回数」ではなく
    // 「1回あたりの値段」を下げる。ラベルのパネル（CanvasVideo の
    // m_panelCache）と同じ手。
    //
    // 不透明度はキーに入れない。QGraphicsOpacityEffect は描き上がった絵に
    // 外から掛かるものなので、paintEvent() が描く絵そのものは不透明度では
    // 変わらない。フェード中も同じキャッシュをそのまま使い回してよい。
    //
    // 透明な画像へ描いてから貼っても結果が変わらない根拠: QGraphicsEffect は
    // もともと元画像を透明な QPixmap へ描いてから効果を掛ける。つまり下の
    // 加算・乗算合成は今でも透明な下地に対して行われている。
    // マスターボリューム操作中の表示。GL版では島の本体がGL側にあるため、
    // ウィジェットの上へ直接描く（旧来は本体の直後・縁の光の手前に描いていた。
    // 縁から seekInset だけ内側に収まる薄い帯なので、重なり順の差はほぼ出ない）。
    const auto paintMasterVolumeSeek = [&](QPainter &painter) {
        if (m_masterVolumeGestureAlpha > 0.001f) {
            painter.save();
            painter.setClipPath(path);
            painter.setOpacity(m_masterVolumeGestureAlpha);
            const int filledW = qRound(W * (m_masterVolumeValue / 100.0));
            // 2026-09-11: α=230だと空き側がほぼ塗り潰しになり、ガラス表現ごと
            // 白い板で覆ってしまっていた。レベルの差が分かる程度の薄さに留める。
            const int luminance = qGray(m_accentColor.rgb());
            const QColor seekColor = luminance < 128
                ? QColor(255, 255, 255, 80)
                : QColor(0, 0, 0, 80);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

            // 2026-09-11: 境目を単純な垂直の切り口(fillRect)にすると、島自身の
            // 縁のリムライトまで真っ二つに断ち切られて平坦に見える。境目の上下端に
            // 丸みを付けて、一段乗っているように見せる。
            //
            // 丸める向きは「音量のある左側を、右2隅が丸いパネルにする」。その
            // パネルを帯から引くことで、空き側(灰色)が丸く回り込む形になる。
            // 逆向き(空き側の左2隅を丸める)だと、音量側が出っ張って見えてしまう。
            // 上下はseekInsetだけ内側へ寄せ、縁のリムライトには被せない。
            constexpr qreal seekRound = 5.0;
            constexpr qreal seekInset = 2.0;
            const QRectF seekBand(left, top + seekInset,
                                  right - left, (bottom - top) - seekInset * 2.0);
            QPainterPath filledPanel;
            filledPanel.addRoundedRect(
                QRectF(seekBand.left() - seekRound, seekBand.top(),
                       (filledW - seekBand.left()) + seekRound, seekBand.height()),
                seekRound, seekRound);
            QPainterPath bandPath;
            bandPath.addRect(seekBand);
            painter.setPen(Qt::NoPen);
            painter.fillPath(bandPath.subtracted(filledPanel), seekColor);
            painter.restore();
        }

    };

    if (kGlassOnGpu) {
        if (!m_bodyCache.isNull())
            m_bodyCache = QPixmap();   // 使わないのでメモリを返す
        if (m_masterVolumeGestureAlpha > 0.001f) {
            QPainter seekPainter(this);
            seekPainter.setRenderHint(QPainter::Antialiasing);
            paintMasterVolumeSeek(seekPainter);
        }
#ifndef NDEBUG
        ++m_statPaints;
        if (statSameAsLast)
            ++m_statPaintsSame;
        if (m_opacity && m_opacity->opacity() <= 0.01)
            ++m_statPaintsHidden;
        const double statMsGpu = statTimer.nsecsElapsed() / 1e6;
        m_statPaintMsSum += statMsGpu;
        m_statPaintMsMax = qMax(m_statPaintMsMax, statMsGpu);
#endif
        return;
    }

    const qreal dpr = devicePixelRatioF();
    const QSize cachePx(qMax(1, qRound(W * dpr)), qMax(1, qRound(H * dpr)));
    const quint64 cacheKey = islandBodyCacheKey();
    if (m_bodyCache.isNull() || m_bodyCacheKey != cacheKey
        || m_bodyCache.size() != cachePx
        || !qFuzzyCompare(m_bodyCacheDpr + 1.0, dpr + 1.0)) {
#ifndef NDEBUG
        QElapsedTimer buildTimer;
        buildTimer.start();
#endif
        m_bodyCache = QPixmap(cachePx);
        m_bodyCache.setDevicePixelRatio(dpr);
        m_bodyCache.fill(Qt::transparent);
        m_bodyCacheKey = cacheKey;
        m_bodyCacheDpr = dpr;

        QPainter painter(&m_bodyCache);
        painter.setRenderHint(QPainter::Antialiasing);

#ifndef NDEBUG
        // 2026-09-14: build が 240x42 で約2.9ms、240x362 で6〜8ms。設定パネルの
        // 開閉アニメーション中は高さが毎フレーム変わるためキャッシュが必ず
        // 外れ、ここがそのまま毎フレームのコストになる。
        // 犯人は下の縁の減衰（従来は 12+12+12+12+8 = 56本の drawPath）だと
        // 判明済み。区間ごとの所要時間を出す。
        QElapsedTimer sectionTimer;
        sectionTimer.start();
        double secPrev = 0.0;
        const auto secMark = [&](double &sink) {
            const double now = sectionTimer.nsecsElapsed() / 1e6;
            sink += now - secPrev;
            secPrev = now;
        };
#endif

        // アクセントカラー塗り。
        // 2026-09-11: 背後をぼかすようになったため、ここを不透明のままにすると
        // せっかくのぼけた下地が完全に隠れてしまう。ガラスとして成立する程度まで
        // アルファを落とす(Windowsのアクセント色は既定でα=230)。
        QColor bodyColor = m_accentColor;
        bodyColor.setAlpha(kIslandBodyAlpha);
        painter.setBrush(bodyColor);
        painter.setPen(Qt::NoPen);
        painter.drawPath(path);

        // マスターボリューム操作中（ホイール中〜直後）だけ、島全体を
        // 「左0・右100」のシークバー状に一時表示する。
        // 音量がない右側は、現在のアクセント色に対してコントラストが高くなる
        // 白または黒を自動選択して描く。表示/非表示は
        // m_masterVolumeGestureAlphaでなめらかにフェードする。
        paintMasterVolumeSeek(painter);

#ifndef NDEBUG
        secMark(m_statBuildBaseMs);
#endif

        // ---- ガラス表現 -------------------------------------------------------
        // 2026-09-11: AnimatedButtonと同じ言語(加算で輝く縁 / 乗算で沈む縁)を
        // 島にも適用する。形は常に上で組み立てた現在のpathへ追従するので、
        // +ボタンで下へ伸びても角丸のまま破綻しない。
        //
        // ボタンとの唯一の違いは、上辺・下辺が光る範囲を「高さの割合」ではなく
        // m_baseH基準の絶対距離で決めている点。割合にすると展開時に光が縦へ
        // 間延びして効果が消えてしまうため、展開しても上下の光り方は保たれ、
        // 伸びた左右の縁はそのぶん翳りの区間が長くなるだけになる。
        {
            const auto withAlpha = [](QColor c, int a) { c.setAlpha(a); return c; };
            const QColor shadeColor = [&] {
                const QColor hsv = m_accentColor.toHsv();
                return QColor::fromHsv(qMax(0, hsv.hue()),
                                       qMin(255, static_cast<int>(hsv.saturation() * 1.10)),
                                       static_cast<int>(hsv.value() * 0.42));
            }();
            const qreal innerH = qMax(1.0, bottom - top);
            // 上下の縁が光る距離。展開時は m_baseH 基準の絶対距離で一定に保つが、
            // 畳んだ状態(高さ≒42px)では絶対距離のままだと上下の光が島の全高を
            // 覆い尽くし、暗い部分が一切残らなくなる(ユーザー指摘)。高さに対する
            // 割合でも頭打ちさせ、中央に必ず暗部が残るようにする。
            const qreal lightSpan = qMin(m_baseH * 0.95, innerH * 0.36);
            // 左右の翳りが乗る区間。上の光が届かない中央帯をここで沈める。
            const qreal s1 = qMin(0.34, (m_baseH * 0.62) / innerH);
            const qreal s2 = 1.0 - s1;

            // 縁から内側への減衰は、太さを変えたストロークの重ね合わせで作る。
            // 1本の太いストロークだと内側の端がそのまま段差(境界線)として
            // 見えてしまうため(ボタン側で同じ問題を踏んだ)。
            // 2026-09-11: にじみ幅を半分程度に絞り、そのぶんアルファを上げて
            // 「細く強い」ハイライトへ。本数も増やして段差が出ないようにする。
            //
            // ---- 縁の減衰の作り方（2026-09-14 全面的に作り直し）------------
            //
            // 縁から内側への減衰は「太さを変えたストロークの重ね合わせ」で
            // 作っている。1本の太いストロークだと内側の端がそのまま段差として
            // 見えてしまうため（ボタン側で同じ問題を踏んだ）。
            //
            // 【何が重かったか】従来は光ごとに drawPath を12本、それが4種類、
            // さらに乗算の翳りで8本、合わせて56本のストロークを毎回描いていた。
            // 実測（[Island][build]）で、この加算ぶんだけで build 全体の87%
            // （6.68ms / 7.51ms）。
            //
            // 犯人は「塗る面積」ではなく「1ストロークあたりの固定費」だった。
            // 根拠は2つとも実測:
            //   - 帯の範囲へクリップして塗る面積を減らしても 6.68ms のまま
            //   - 本数を56→15本に減らすと 6.68ms → 1.14ms（本数比0.27に対し
            //     コスト比0.17）
            // drawPath はペンの太さぶんパスを太らせて多角形を作る
            // （QPainterPathStroker 相当）処理が本体で、これはクリップとは
            // 無関係にパス全体に対して毎回走る。
            //
            // 【どう直したか】その多角形を**1回だけ**作って使い回す。
            // 4つの光と乗算の翳りで span（減衰の届く距離）が
            // 0.045 / 0.035 / 0.050 / 0.040 / 0.050 とばらついていたのを
            // 0.045 に統一した。統一しないと太さの系列が光ごとに変わり、
            // 多角形を共有できないため。
            //   → 多角形の生成 48回 → 12回
            //   → あとはその12枚をグラデーションで塗り分けるだけ（塗りは安い）
            //
            // 見た目への影響: span は基本高さ42px基準なので実寸1.47〜2.10px、
            // ストローク幅にして2.9〜4.2px。統一で動くのは各光の届く距離が
            // 最大0.3px（半幅）ぶんだけで、ガラスの縁の質感としては
            // 見分けがつかない範囲。明るさ（＝重なる本数）は12本のままなので
            // 変わらない。
            //
            // ※ 本数を減らす方向は採らなかった。距離dを覆う本数は
            //   count*(1 - d/span) なので、本数を半分にすると加算で積み上がる
            //   光の量もそのまま半減する。島は高さ42→362へアニメーション
            //   するため、途中で本数が切り替わると縁の光が一段暗くなる。
            //   （CanvasVideo.cpp の paintGlassPlate() は innerH>72 で
            //     12→6本にしているので、ラベルのリストを開く途中で同じ現象が
            //     起きているはず。そちらは別途。）
            constexpr int kBandCount = 12;
            const qreal kBandSpan = m_baseH * 0.045;
            QPainterPath bandOutlines[kBandCount];
            {
                // QPen(QBrush, w) の既定と同じ端・継ぎ目にしておく
                // （閉じたパスなので実際に効くのは継ぎ目だけ）。
                QPainterPathStroker stroker;
                stroker.setCapStyle(Qt::SquareCap);
                stroker.setJoinStyle(Qt::BevelJoin);
                for (int i = 0; i < kBandCount; ++i) {
                    stroker.setWidth(kBandSpan * 2.0
                                     * (1.0 - static_cast<qreal>(i) / kBandCount));
                    bandOutlines[i] = stroker.createStroke(path);
                }
            }
#ifndef NDEBUG
            secMark(m_statBuildOutlineMs);
#endif
            int diagStrokes = 0;
            // bandRect を渡すと、その範囲へクリップしてから塗る。上辺／下辺の
            // 光のグラデーションは lightSpan の外側が完全に透明
            // （QLinearGradient は既定の PadSpread なので終端の先は最後の
            //   停止色＝α0）なので、1ピクセルも変わらない。
            //
            // 2026-09-14: QBrush をループの外で1回だけ作る。
            // fillPath(path, gradient) は QGradient から QBrush へ**毎回**
            // 変換され、Qtのラスタエンジンはブラシごとにグラデーションの
            // 色テーブル（1024段）を用意する。旧コードの
            // painter.setPen(QPen(QBrush(gradient), w)) も同じく毎回作り直して
            // いた。56本ぶんのブラシ構築が効いているのではないか、という仮説。
            //
            // 【これまでの実測】
            //   本数 56→15本                : 6.68ms → 1.14ms（大きく効く）
            //   帯へクリップして面積を減らす : 効かない
            //   多角形の生成 48→12回        : 効かない
            // この3つを同時に満たすのは「1回の描画呼び出しごとに、パスの長さに
            // 比例して発生する、生成でも塗りでもない固定費」。ブラシの構築が
            // その候補。outline= と plus= の内訳で切り分ける。
            const auto fillBands = [&](int count, const QGradient &gradient,
                                       const QRectF &bandRect = QRectF()) {
                count = qMin(count, kBandCount);
                const QBrush bandBrush(gradient);
                painter.save();
                if (!bandRect.isEmpty())
                    painter.setClipRect(bandRect, Qt::IntersectClip);
                for (int i = 0; i < count; ++i) {
                    painter.fillPath(bandOutlines[i], bandBrush);
                    ++diagStrokes;
                }
                painter.restore();
            };

            painter.save();
            painter.setClipPath(path);
            painter.setBrush(Qt::NoBrush);

            // 光を散らすフロスト
            painter.fillRect(rect(), QColor(255, 255, 255, 10));
#ifndef NDEBUG
            secMark(m_statBuildFrostMs);
#endif

            // 加算: 上辺の光
            painter.setCompositionMode(QPainter::CompositionMode_Plus);
            QLinearGradient topLight(QPointF(0, top), QPointF(0, top + lightSpan));
            topLight.setColorAt(0.00, QColor(255, 255, 255, 14));
            topLight.setColorAt(0.45, QColor(255, 255, 255, 4));
            topLight.setColorAt(1.00, QColor(255, 255, 255, 0));
            fillBands(12, topLight,
                      QRectF(left, top - 2.0, right - left, lightSpan + 4.0));

            // 加算: 下辺の光(上辺より控えめ)
            QLinearGradient bottomLight(QPointF(0, bottom - lightSpan), QPointF(0, bottom));
            bottomLight.setColorAt(0.00, QColor(255, 255, 255, 0));
            bottomLight.setColorAt(0.55, QColor(255, 255, 255, 3));
            bottomLight.setColorAt(1.00, QColor(255, 255, 255, 9));
            fillBands(12, bottomLight,
                      QRectF(left, bottom - lightSpan - 2.0,
                             right - left, lightSpan + 4.0));

            // 加算: 光源の方向づけ。上下のベース光の上へ「左上から差す主光」と
            // 「右下へ抜ける副光」を足して、島だけ斜めからの照明に見せる
            // (ボタンは小さいので従来どおり上下対称のまま)。
            // グラデーションの軸を対角に取るだけなので、+ボタンで縦に伸びても
            // 左上→右下の関係はそのまま保たれる。
            const qreal dx = (right - left) * 0.55;
            const qreal dy = innerH * 0.55;
            QLinearGradient topLeftLight(QPointF(left, top), QPointF(left + dx, top + dy));
            topLeftLight.setColorAt(0.00, QColor(255, 255, 255, 20));
            topLeftLight.setColorAt(0.55, QColor(255, 255, 255, 3));
            topLeftLight.setColorAt(1.00, QColor(255, 255, 255, 0));
            fillBands(12, topLeftLight);

            QLinearGradient bottomRightLight(QPointF(right - dx, bottom - dy),
                                             QPointF(right, bottom));
            bottomRightLight.setColorAt(0.00, QColor(255, 255, 255, 0));
            bottomRightLight.setColorAt(0.45, QColor(255, 255, 255, 2));
            bottomRightLight.setColorAt(1.00, QColor(255, 255, 255, 11));
            fillBands(12, bottomRightLight);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
#ifndef NDEBUG
            secMark(m_statBuildPlusMs);   // 加算 4本ぶん = 48 drawPath
#endif

            // 乗算: 上下の光が届かない左右の縁を、アクセント色のまま沈める
            painter.setCompositionMode(QPainter::CompositionMode_Multiply);
            QLinearGradient sideShade(QPointF(0, top), QPointF(0, bottom));
            sideShade.setColorAt(0.00, withAlpha(shadeColor, 0));
            sideShade.setColorAt(s1,   withAlpha(shadeColor, 30));
            sideShade.setColorAt(s2,   withAlpha(shadeColor, 30));
            sideShade.setColorAt(1.00, withAlpha(shadeColor, 0));
            fillBands(8, sideShade);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            painter.restore();
#ifndef NDEBUG
            secMark(m_statBuildShadeMs);  // 乗算(側面) 8 drawPath
#endif

            // 乗算: 輪郭線(ガラス板の切り口)
            painter.setCompositionMode(QPainter::CompositionMode_Multiply);
            QLinearGradient contour(QPointF(0, top), QPointF(0, bottom));
            contour.setColorAt(0.00, withAlpha(shadeColor, 120));
            contour.setColorAt(s1,   withAlpha(shadeColor, 230));
            contour.setColorAt(s2,   withAlpha(shadeColor, 230));
            contour.setColorAt(1.00, withAlpha(shadeColor, 190));
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QBrush(contour), 1.0));
            painter.drawPath(path);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
#ifndef NDEBUG
            secMark(m_statBuildContourMs);
            m_statBuildStrokes += diagStrokes;
#endif
        }
        painter.end();
#ifndef NDEBUG
        secMark(m_statBuildEndMs);        // QPainter::end() = ラスタの後始末
#endif
#ifndef NDEBUG
        ++m_statBuilds;
        m_statBuildMsSum += buildTimer.nsecsElapsed() / 1e6;
#endif
    }

    QPainter widgetPainter(this);
    widgetPainter.drawPixmap(0, 0, m_bodyCache);

    // 2026-09-11: かつてここにカーソル追従の縁ハイライト（カーソル位置を
    // 中心にした半径120pxの放射グラデーションで輪郭をなぞるストローク）が
    // あったが、上のガラス表現と光の向きが競合するため削除した。
    //
    // 2026-09-14: つまり現在の paintEvent() はカーソル位置に一切依存して
    // いない（この関数の全体を見てもカーソルを読む箇所は無い）。
    // DynamicIslandController::tick() にはそれを前提とした「カーソルが
    // 動いたら update()」という分岐が残っていたが、実測で
    // 「畳んでいる間は glowReq=561 に対して paints=3（＝完全な空振り）」
    // 「出ている間は paints のうち3分の1がこれ由来で、絵は前回と同一」
    // と確認できたため削除した（そちらのコメント参照）。
#ifndef NDEBUG
    widgetPainter.end();
    const double statMs = statTimer.nsecsElapsed() / 1e6;
    ++m_statPaints;
    if (statSameAsLast)
        ++m_statPaintsSame;
    if (m_opacity && m_opacity->opacity() <= 0.01)
        ++m_statPaintsHidden;
    m_statPaintMsSum += statMs;
    m_statPaintMsMax = qMax(m_statPaintMsMax, statMs);
#endif
}

void DynamicIsland::mousePressEvent(QMouseEvent *event)
{
    // 島はどんな形態（畳んだピル／設定パネル展開／確認プロンプト展開）でも
    // 掴めばウィンドウを動かせる、というのが一貫した挙動。確認プロンプト中も
    // 例外にしない。
    // プロンプトのパネルとラベルは Qt::WA_TransparentForMouseEvents を
    // 付けてここへイベントを素通しさせている（IslandPromptPanelのコンストラクタ
    // 参照）。YES/NOボタンだけがイベントを受け取る。
    // なおドラッグで全画面が解除されるとウィンドウ高さが変わるが、
    // プロンプトの高さ上限は notifyHostWindowResized() が追従させる。
    // 2026-09-16: 設定を開いている間も、島のどこを掴んでもウィンドウを動かせる
    // （このアプリは島以外でウィンドウを動かせないため）。一時的に入れていた
    // 「設定中は上端の帯だけ掴める」制限は外した。
    // ボタンやプルダウンなど、押すと反応する部品は自分でイベントを受け取るので
    // ここまで届かない。行の余白・項目名・パネルの空き部分は受け取らずに
    // 島まで伝わってくる。
    // 2026-09-17: 設定「右ドラッグでウィンドウを移動」がONなら、右ボタンでも
    // 島を掴んで動かせる（キャンバス上の右ドラッグと揃える）。
    const bool rightDragMove = event->button() == Qt::RightButton
        && AppSettings::instance().rightDragWindowMoveEnabled();
    if (event->button() == Qt::LeftButton || rightDragMove) {
        m_dragging     = true;
        // 島の中で実際にクリックした位置（島ローカル座標）を保持する。
        // ウィンドウ全体の左上からのオフセットではなく「島の中のどこを
        // 掴んだか」を基準にすることで、最大化⇔通常サイズの切り替えで
        // 島自身の位置（センタリング）が変わってもカーソルとのズレが
        // 生じないようにする。
        m_dragStartPos = event->position().toPoint();
        event->accept();
    }
}
void DynamicIsland::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        // 全画面状態の基準は「全画面ボタンを押してから何もしていない間」と
        // する。島をつかんでウィンドウ位置を少しでも動かした時点で全画面は
        // 解除されたものとして扱い、通常サイズへ戻す。
        auto *mw = qobject_cast<MainWindow*>(window());
        if (mw && mw->isWindowFullscreen()) {
            mw->setWindowFullscreen(false);
            // 2026-07-08 バグ修正: ウィンドウ全画面＋バブル全画面（フォーカス）中に
            // 島をドラッグして全画面解除すると、がくがく点滅するように見える不具合。
            // 原因: setWindowFullscreen(false)は内部でnotifyWindowMaximizedChanged()
            // を同期的に呼び、バブルがフォーカス中であれば
            // CanvasWidget::notifyWindowMaximizedChanged()がmw->animateToAspect()
            // で450msの自動ウィンドウリサイズアニメーション(mw->resizeAnim())を
            // 開始してしまう。その直後、このmouseMoveEvent自身がカーソル位置に
            // 追従させるためwindow()->move()で毎フレームウィンドウ位置を書き換える
            // ため、「自動アニメーションによる移動」と「ドラッグによる手動移動」が
            // 同じウィンドウジオメトリを奪い合い、全画面解除前の位置と現在の
            // ドラッグ位置が交互に描画されるちらつきになっていた。
            // ドラッグ中はユーザーの手動操作を優先し、自動アニメーションは
            // 即座に打ち切る。
            // 2026-07-09 バグ修正: 従来は打ち切ったまま何もしていなかったため、
            // バブルがフォーカス中（バブル全画面）だと、ウィンドウが全画面
            // 解除直後のサイズ（＝ウィンドウサイズロックのサイズ）のまま
            // 動画アスペクト比に合わず、余白（黒帯）付きのバブル全画面に
            // なってしまっていた。ドラッグ終了時に再フィットをやり直すよう
            // フラグを立てておく（mouseReleaseEvent参照）。
            if (mw->resizeAnim())
                mw->resizeAnim()->stop();
            m_pendingFocusAspectRefit = true;
        }

        // 「カーソルが掴んでいる島の中の点」がその場に留まるように、
        // 島の望ましいグローバル位置から逆算してウィンドウを配置する。
        // setWindowFullscreen(false)でウィンドウサイズが変わっても、
        // resizeEvent経由で島は既に新しい幅に合わせて再センタリング済み
        // （pos()が最新値）なので、「元のサイズにカーソル基準でその他の辺が
        // 集まってくる」形になる。
        //
        // 2026-10-11: DPIの違うモニター間でも追従がずれないよう、物理ピクセルで
        // 移動する（MainWindow::dragWindowNative参照）。掴んだ点のウィンドウ左上
        // からのオフセットは、島内の位置(m_dragStartPos)＋島自身の位置(pos())。
        if (!(mw && mw->dragWindowNative(QPointF(m_dragStartPos + pos()), false))) {
            const QPoint globalPos = event->globalPosition().toPoint();
            const QPoint desiredIslandTopLeftGlobal = globalPos - m_dragStartPos;
            window()->move(desiredIslandTopLeftGlobal - pos());
        }
    }
    event->accept();
}
void DynamicIsland::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    // 2026-07-09 バグ修正: ドラッグによるウィンドウ全画面解除で打ち切った
    // 「フォーカス中バブルへのアスペクト再フィット」をドラッグ終了後に
    // やり直す。バブルがフォーカス中でなければ何もしない（MainWindow::
    // refitFocusedBubbleAspect()内で判定される）。ウィンドウサイズロックの
    // 復元先はセッション中の再フィットでは上書きされないため、この呼び出しで
    // ロック済みサイズが失われることはない。
    if (m_pendingFocusAspectRefit) {
        m_pendingFocusAspectRefit = false;
        if (auto *mw = qobject_cast<MainWindow*>(window()))
            mw->refitFocusedBubbleAspect();
    }
    event->accept();
}

// ---- DynamicIslandController ----
DynamicIslandController::DynamicIslandController(DynamicIsland *island, QWidget *window, CanvasWidget *canvas)
    : QObject(window), m_island(island), m_window(window), m_canvas(canvas)
{
    m_islandAnim = new QPropertyAnimation(island, "islandWidth", this);
    m_islandAnim->setDuration(500);
    m_islandAnim->setEasingCurve(QEasingCurve::OutBack);

    auto *eff = qobject_cast<QGraphicsOpacityEffect*>(island->graphicsEffect());
    m_fadeAnim = new QPropertyAnimation(eff, "opacity", this);
    m_fadeAnim->setDuration(120); // 早めに消える

    m_lockTimer = new QTimer(this);
    m_lockTimer->setSingleShot(true);
    m_lockTimer->setInterval(300);
    connect(m_lockTimer, &QTimer::timeout, this, [this]{ m_animLock = false; });

    // 2026-07-12: 設定パネルを開いたままキャンバス操作した後、島をフェード
    // 復帰させるまでの猶予（操作そのものが途切れてから0.5秒。慣性の終了は
    // 待たない）。
    m_manipulationFadeInTimer = new QTimer(this);
    m_manipulationFadeInTimer->setSingleShot(true);
    m_manipulationFadeInTimer->setInterval(500);
    connect(m_manipulationFadeInTimer, &QTimer::timeout, this, [this]{
        m_manipulationFadeActive = false;
        auto *eff = qobject_cast<QGraphicsOpacityEffect*>(m_island->graphicsEffect());
        m_fadeAnim->stop();
        m_fadeAnim->setStartValue(eff->opacity());
        m_fadeAnim->setEndValue(1.0);
        m_fadeAnim->start();
    });

    // 2026-09-15: 設定を開いた瞬間、出現途中の幅アニメ（→240, OutBack）が
    // 残っていると設定の展開アニメと幅を奪い合う。開くときは必ず止める。
    connect(island, &DynamicIsland::settingsOpenChanged, this, [this](bool open){
        if (!open)
            return;
        m_islandAnim->stop();
    });

    QTimer::singleShot(0, island, &DynamicIsland::prepareForFirstAnimation);
}

void DynamicIslandController::tick()
{
    // 2026-07-11: 右島(LayerBar)と同じ流儀。カメラのホイール押し込みパン・
    // バブル/フレームのつかみ移動中は、カーソルがトリガー領域や島自体の
    // 上にあっても問答無用で隠す（邪魔・チラつき防止）。animLock中の
    // 通常アニメを妨げないよう、既に表示中の場合のみ強制的に閉じる。
    // 2026-09-13: 確認プロンプト表示中は、カーソル位置にもキャンバス操作にも
    // 一切反応しない。ユーザーが答えるまで島は出したままにする
    // （設定パネルのときのような一時フェードもしない。答えを待っている
    //   表示が消えると、何に答えているのか分からなくなるため）。
    if (m_island->isPromptOpen()) {
        m_manipulationFadeInTimer->stop();
        m_manipulationFadeActive = false;
        return;
    }

    // 2026-09-15: 設定の開閉アニメ（島がウィンドウ全体⇔通常幅）の間は、幅を
    // そちらが動かしているので手を出さない。閉じ終わったら通常の制御へ戻り、
    // カーソルが離れていれば次のtickで畳まれる。
    if (m_island->isSettingsResizing())
        return;

    // 2026-09-17: 判定を CanvasWidget::shouldHideIslandForCanvas() に集約。
    // パン・つかみ移動（バブル全画面中のパンを含む）に加え、長押し2倍速の間も隠す。
    if (m_canvas && m_canvas->shouldHideIslandForCanvas()) {
        // 2026-07-12 バグ修正: 設定パネルを開いたままキャンバス操作を
        // すると、hideIsland()が島の幅を畳むアニメーションを始めてしまい、
        // 島の子ウィジェットである設定パネル（QComboBox等を含む）がその
        // 幅変化に巻き込まれて崩れた見た目になっていた。
        // 「設定パネル展開中は消さない」というtick()通常時のルール
        // （下のtriggerArea分岐内）が、この強制非表示の分岐には
        // 及んでいなかったのが原因。設定パネル展開中はhideIsland()を
        // 呼ばず、代わりにオパシティだけを0へフェードして一時的に隠す
        // （m_showing/m_settingsOpen/島の幅は一切変更しない）。
        if (m_island->isSettingsOpen()) {
            // 2026-07-12: 操作が再開されたら、前回の一時停止で仕込んでいた
            // 「0.5秒後にフェード復帰」タイマーは仕切り直す（これが無いと、
            // 一度パンを止めてまたすぐパンし直した時、古いタイマーが
            // ドラッグ中にそのまま発火して島が見えてしまう）。
            m_manipulationFadeInTimer->stop();
            if (!m_manipulationFadeActive) {
                m_manipulationFadeActive = true;
                auto *eff = qobject_cast<QGraphicsOpacityEffect*>(m_island->graphicsEffect());
                m_fadeAnim->stop();
                m_fadeAnim->setStartValue(eff->opacity());
                m_fadeAnim->setEndValue(0.0);
                m_fadeAnim->start();
            }
            return;
        }
        if (m_showing && !m_animLock)
            hideIsland();
        return;
    }
    if (m_manipulationFadeActive && !m_manipulationFadeInTimer->isActive()) {
        // 2026-07-12: 操作（isCanvasBeingManipulated()＝ドラッグそのもの）
        // が途切れた瞬間。ここで即座にフェード復帰させるのではなく、
        // 0.5秒間の猶予タイマーを仕込む（実際の復帰はタイマー発火時の
        // ラムダで行う。m_manipulationFadeActiveのfalse化もそちらで行う）。
        // 慣性でカメラがまだ動いていても、このisCanvasBeingManipulated()
        // 自体はドラッグ終了時点で既にfalseになっている（m_activeModeを
        // 見ているだけで慣性の有無は見ていない）ため、意図通り「慣性の
        // 終了を待たず、操作が途切れてから0.5秒」になる。
        m_manipulationFadeInTimer->start();
    }

    QPoint local = m_window->mapFromGlobal(QCursor::pos());
    int cx = m_window->width() / 2;
    QRect triggerArea(cx - 150, 0, 300, 80);
    bool inTrigger = triggerArea.contains(local);

    // 2026-09-12: 左上のラベル／リストが出ている間は、その矩形を島の
    // トリガーから除く。ウィンドウが狭いと島の判定（中央上部300x80）が
    // リストへ食い込み、リストを触ろうとしただけで島が出てリストが畳まれて
    // いた。リストの中は素直にリストとして扱い、島を出したいときは
    // リストの外の判定へカーソルを持っていく、という住み分けにする。
    // ラベルが出ていないときは空の矩形が返るので従来どおり。
    if (inTrigger && m_canvas) {
        const QRect labelLocal = m_canvas->labelUiRect();
        if (!labelLocal.isEmpty()) {
            const QRect labelInWindow(
                m_window->mapFromGlobal(m_canvas->mapToGlobal(labelLocal.topLeft())),
                labelLocal.size());
            if (labelInWindow.contains(local))
                inTrigger = false;
        }
    }

    if (inTrigger && !m_showing && !m_animLock) {
        showIsland();
    } else if (!inTrigger && m_showing && !m_animLock) {
        // 設定パネル展開中は消さない。
        // 2026-07-13 バグ修正: 以前はここで早期returnしていたが、トリガー
        // 領域（島上部300x80px）より下＝設定パネル内のボタン等にカーソルが
        // ある間は毎tickここで抜けてしまい、この下のグロー更新に一切到達
        // せず縁のカーソル追従ハイライトが凍結していた。「パネルを閉じ
        // ない」ためのスキップ対象はhideIsland()の判定だけなので、returnを
        // やめて条件を反転し、グロー更新へは常に流れ落ちるようにする。
        if (!m_island->isSettingsOpen()) {
            // アイランド自体にカーソルがある場合は閉じない
            QRect islandGlobal(m_island->mapToGlobal(QPoint(0,0)), m_island->size());
            islandGlobal.adjust(-30,-30,30,30);
            if (!islandGlobal.contains(QCursor::pos()))
                hideIsland();
        }
    }

    // 2026-09-14: ここには「カーソル追従グローの位置が前回tickから変わって
    // いたら m_island->update()」という分岐があったが、削除した。
    //
    // 理由:
    //   1. 追従グローは 2026-09-11 に廃止され、ガラスの縁の固定表現へ
    //      置き換わっている（DynamicIsland::paintEvent() 末尾のコメント）。
    //      現在の paintEvent() はカーソル位置を一切読まないので、この
    //      update() が作る絵は必ず前フレームと同一だった。
    //   2. 判定に使っていた m_island->mapFromGlobal(QCursor::pos()) は
    //      グローバル座標を写しているだけなので、島の上どころか画面の
    //      どこでカーソルを動かしても毎回通っていた。
    //   3. 実測（[Island][stat]）: 島を畳んでいる間は glowReq=561 に対し
    //      paints=3。不透明度0の間は Qt 側が描画を打ち切るため完全な空振り。
    //      島が出ている間は paints=100 のうち afterGlow=38 がこれ由来で、
    //      いずれも前回と同じ絵を描き直していた。
    //
    // 幅・高さ・アクセントカラー・マスターボリューム表示は、いずれも
    // それぞれの setter / アニメーションが個別に update() を出しているので、
    // ここから駆動する必要は無い。
#ifndef NDEBUG
    // 計測の2秒窓を実時間で閉じるためだけの呼び出し（描画は一切しない）。
    m_island->noteIslandTick();
#endif
}

void DynamicIslandController::showIsland()
{
    m_showing  = true;
    m_island->setRetracting(false);
    m_animLock = true;
    m_lockTimer->start();

    auto *eff = qobject_cast<QGraphicsOpacityEffect*>(m_island->graphicsEffect());
    m_fadeAnim->stop();
    eff->setOpacity(1.0);

    m_islandAnim->stop();
    m_islandAnim->setStartValue(m_island->islandWidth());
    m_islandAnim->setEndValue(240);
    m_islandAnim->start();

    m_island->raise();

    // ボタンを順番に出現（Python版と同じ遅延）
    m_island->btnSettings->startAppear();
    m_island->btnClose->startAppear();
    QTimer::singleShot(70,  m_island->btnMax, &AnimatedButton::startAppear);
    QTimer::singleShot(140, m_island->btnMin, &AnimatedButton::startAppear);
}

void DynamicIslandController::openPromptView()
{
    // 一時フェード中（設定パネル＋キャンバス操作）だった場合の後始末。
    m_manipulationFadeInTimer->stop();
    m_manipulationFadeActive = false;

    const bool wasShowing = m_showing;

    m_showing  = true;
    m_island->setRetracting(false);
    m_animLock = true;
    m_lockTimer->start();

    auto *eff = qobject_cast<QGraphicsOpacityEffect*>(m_island->graphicsEffect());
    m_fadeAnim->stop();
    eff->setOpacity(1.0);

    // 通常の展開幅(240)ではなくプロンプト用の幅まで広げる。
    m_islandAnim->stop();
    m_islandAnim->setStartValue(m_island->islandWidth());
    m_islandAnim->setEndValue(DynamicIsland::promptIslandWidth());
    m_islandAnim->start();

    m_island->raise();

    // 既に出ている島に対して出現アニメをやり直すと、ボタンが一度消えて
    // から生え直すように見えてうるさい。畳まれた状態から出すときだけ。
    if (!wasShowing) {
        m_island->btnSettings->startAppear();
        m_island->btnClose->startAppear();
        QTimer::singleShot(70,  m_island->btnMax, &AnimatedButton::startAppear);
        QTimer::singleShot(140, m_island->btnMin, &AnimatedButton::startAppear);
    }
}

void DynamicIslandController::closePromptView()
{
    // 閉じたつもりが既に次のプロンプトが開いている場合（前のプロンプトを
    // 打ち切って差し替えたときなど）、ここで通常幅へ戻すと、開いたばかりの
    // プロンプトが240px幅の島に押し込められて中身が切れる。
    if (m_island->isPromptOpen())
        return;

    // 幅だけ通常の展開幅へ戻し、あとは tick() の通常制御に返す。
    // カーソルが島から離れていれば、次以降のtickでhideIsland()され畳まれる。
    // m_lockTimer(300ms)の間はtickが手を出さないので、その間にプロンプトの
    // 閉じアニメ(250ms)が終わり、hideIsland()は正しい高さから畳める。
    m_animLock = true;
    m_lockTimer->start();

    m_islandAnim->stop();
    m_islandAnim->setStartValue(m_island->islandWidth());
    m_islandAnim->setEndValue(240);
    m_islandAnim->start();
}

void DynamicIslandController::hideIsland()
{
    m_showing  = false;
    // 畳み始めた瞬間に立てる。左上のリストはこれを見て、島が消え切るのを
    // 待たずに開き直す（DynamicIsland::isRetracting()のコメント参照）。
    m_island->setRetracting(true);
    m_animLock = true;
    m_lockTimer->start();

    // ボタンをリセット
    for (auto *btn : {m_island->btnSettings, m_island->btnClose,
                      m_island->btnMax, m_island->btnMin})
        btn->resetState();

    m_islandAnim->stop();
    // 畳み切った姿は「直径 = 基本の高さ」の丸。height() は設定パネルや
    // 確認プロンプトの閉じアニメーションが終わるまで大きいままなので、
    // そちらを使うと畳み始めのタイミング次第で大きな四角になる。
    m_islandAnim->setStartValue(m_island->islandWidth());
    m_islandAnim->setEndValue(m_island->baseHeight());
    m_islandAnim->start();

    QTimer::singleShot(50, this, [this]{
        startFadeOut();
    });
}

void DynamicIslandController::startFadeOut()
{
    auto *eff = qobject_cast<QGraphicsOpacityEffect*>(m_island->graphicsEffect());
    m_fadeAnim->stop();
    m_fadeAnim->setStartValue(eff->opacity());
    m_fadeAnim->setEndValue(0.0);
    m_fadeAnim->start();
}
