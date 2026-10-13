#include "CanvasOverlayWidget.h"

#include "CanvasWidget.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QPaintEvent>
#include <QPainter>
#include <QRegion>

CanvasOverlayWidget::CanvasOverlayWidget(CanvasWidget *canvas, QWidget *parent)
    : QWidget(parent), m_canvas(canvas)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
    m_blurRegions.attach(this);

    m_uiAlphaAnim = new QVariantAnimation(this);
    m_uiAlphaAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_uiAlphaAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_uiAlpha = v.toDouble();
        invalidateSuppressibleUi();
    });

    // 2026-09-17: 長押し2倍速の非表示。2倍速バッジ（refreshFocusUi の範囲）は
    // 隠さないので、フェード中に無効化するのは再生UIとツールチップの矩形だけ。
    m_speedHoldAlphaAnim = new QVariantAnimation(this);
    m_speedHoldAlphaAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_speedHoldAlphaAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_speedHoldAlpha = v.toDouble();
        refreshPlaybackUi();
        refreshLauncherTooltipUi();
    });
}

// ===== 長押し2倍速中の非表示（2026-09-17）==================================
//
// 【なぜ】一般的な動画プレイヤーと同じく、2倍速で見ている間は動画の上に何も被せない。
//
// 【既存の表示ロジックとの関係】
//   再生バーやラベルの「カーソルが動いたら出る／止まったら消える」判定には
//   一切手を入れない。それらは裏でいつも通り動き続け、ここではその上から
//   不透明度を掛けるだけ。なので2倍速を終えた瞬間、その時点で本来出ている
//   べきUIがそのままフェードで戻る（状態を二重に持たないので食い違わない）。
//
// 【フレーム更新への配慮】setUiSuppressed() と同じ方針。
//   - フェード中の無効化は refreshPlaybackUi / refreshLauncherTooltipUi の
//     細い矩形だけ。2倍速バッジは自前のアニメで refreshFocusUi を出している。
//   - 隠れ切った後は refreshPlaybackUi / refreshPlaybackEqualizer を空振り
//     させる（再生中のシークバー・イコライザの update() を出さない）。
//   - 戻し始めに一度だけ全面を無効化（隠れていた間 paint を飛ばしたので、
//     ラベル等の「直近に描いた矩形」が古いため）。
void CanvasOverlayWidget::setSpeedHoldSuppressed(bool suppressed)
{
    if (m_speedHoldSuppressed == suppressed)
        return;
    m_speedHoldSuppressed = suppressed;

    // 消えるのは素早く、戻るのは少しゆっくり。
    m_speedHoldAlphaAnim->stop();
    m_speedHoldAlphaAnim->setDuration(suppressed ? 140 : 260);
    m_speedHoldAlphaAnim->setStartValue(m_speedHoldAlpha);
    m_speedHoldAlphaAnim->setEndValue(suppressed ? 0.0 : 1.0);
    m_speedHoldAlphaAnim->start();

    if (!suppressed)
        update();
}

bool CanvasOverlayWidget::playbackUiFullyHidden() const
{
    if (uiFullyHidden())
        return true;
    return m_speedHoldAlpha <= 0.0
        && m_speedHoldAlphaAnim->state() != QAbstractAnimation::Running;
}

// ===== 設定パネル展開中の非表示（2026-09-15）================================
//
// 【なぜ】島の背後ブラーはキャンバスのFBOだけをぼかす（Blur.h）。このウィジェットは
// FBOの外（兄弟のQWidget）なので、島を大きく広げるとラベルや再生バーが島越しに
// くっきり見えてしまう。設定を開いている間はこちらを隠す。
//
// 【フレーム更新への配慮】
//   - QGraphicsOpacityEffect は使わない。効果を掛けるとこのウィジェットの描画が
//     毎回オフスクリーン経由になり、再生中ずっと重くなる。代わりに paintEvent()
//     冒頭の painter.setOpacity() で済ませる。
//   - フェード中の無効化は refresh*() と同じ細い矩形のみ（全面 update() しない）。
//   - 完全に隠れている間は refresh*() を空振りさせ、再生中のイコライザや
//     シークバーのための update() を出さない。
//   - 戻すときは各 refresh の矩形が「現在の」UI位置から計算されるので、
//     隠れていた間にUIが動いていても描き漏れない（隠れていた間は何も描いて
//     いないので、消し残しも無い）。
void CanvasOverlayWidget::setUiSuppressed(bool suppressed)
{
    if (m_uiSuppressed == suppressed)
        return;
    m_uiSuppressed = suppressed;

    // 消すときは島の展開（420ms）より先に消え切るよう短め、戻すときは
    // 島が畳まれるのに合わせる（2026-09-15: 閉じるアニメを開くときと同じ
    // 420ms に揃えたので、こちらも合わせた）。
    m_uiAlphaAnim->stop();
    m_uiAlphaAnim->setDuration(suppressed ? 160 : 420);
    m_uiAlphaAnim->setStartValue(m_uiAlpha);
    m_uiAlphaAnim->setEndValue(suppressed ? 0.0 : 1.0);
    m_uiAlphaAnim->start();

    // 戻し始めだけは一度全面を無効化する。一部のUI（ラベル等）は無効化範囲を
    // 「直近のpaintで実際に描いた矩形」から求めるが、隠れていた間はpaintを
    // 飛ばしているのでその値が古い。1回きりなのでコストは問題にならない。
    if (!suppressed)
        update();
}

bool CanvasOverlayWidget::uiFullyHidden() const
{
    return m_uiAlpha <= 0.0
        && m_uiAlphaAnim->state() != QAbstractAnimation::Running;
}

void CanvasOverlayWidget::invalidateSuppressibleUi()
{
    refreshPlaybackUi();
    refreshFocusUi();
    refreshLauncherTooltipUi();
}

void CanvasOverlayWidget::refreshPlaybackUi()
{
    if (playbackUiFullyHidden())
        return;
    // The label occupies the top strip and the controls/volume popup the
    // bottom strip. Keeping the damage region narrow avoids waking unrelated
    // parts of the window backing store.
    // 2026-09-12: 上側の帯はラベル（左上のロゴ＋テキスト、高さ約48px）用。
    // ラベルのクリックで開くコントローラのパネルはその下へ長く伸びるが、
    // 帯を一律に320pxへ広げると「ウィンドウ幅×320px」を毎フレーム描き
    // 直して転送することになり、大きいウィンドウほど重くなっていた
    // （パネルの幅は244px程度しかないのに、右端まで巻き込んでいた）。
    // 帯はラベルぶんに戻し、パネルは実際に占めている矩形だけを足す。
    // 2026-09-14 D-3: 帯を「ウィンドウ幅いっぱい」で頼むのをやめる。
    //
    // 【なぜ】[CanvasOverlay][stat] の実測で、全画面のとき
    //     area/frame=2.00〜2.38Mpx  rects/frame=1.0〜1.2  （1821x1317=2.40Mpx）
    // つまりウィンドウ全面が1枚の矩形として返ってきていた。上の帯（全幅×96）
    // と下の帯（全幅×144）を1つのリージョンで渡すと、その2つを含む最小の
    // 四角＝ウィンドウ全体になる。Qtのバッキングストアはリージョンが複雑だと
    // バウンディング矩形へ丸めるので、0.4Mpxの要求が2.4Mpxになって返る。
    // 窓が小さいときは上下の帯で面積の大半なので損しないが、大きいほど損が
    // 増える（＝「全画面にすると何もかも少し重い」の一因）。
    //
    // 上はラベル（＋開いていればリスト）の実寸、下は再生バーが実際に出て
    // いるときだけ。どちらも無ければ足さない。
    QRegion damage;

    // ---- 上：ラベルとリスト ----
    // パネルは伸び縮みするので、前回の矩形も一緒に無効化する
    // （畳んで消えたフレームで描画済みの部分を消すため）。
    QRect panelRect;
    QRect labelRect;
    if (m_canvas) {
        if (auto *av = m_canvas->activeVideo()) {
            const QRectF r = av->labelPanelScreenRect();
            if (!r.isEmpty())
                panelRect = r.toAlignedRect().adjusted(-8, -8, 8, 8);
        }
        // 2026-09-16 F-1: labelUiRect() ではなく labelUiDamageRect()。
        // 非アクティブ化してフェードアウト中の動画はもう labelOwnerVideo()
        // ではないが、描画側はフェードが終わるまで描き続けるため、
        // labelUiRect() だけだとリストの画素が消えずに残る。
        labelRect = m_canvas->labelUiDamageRect().adjusted(-12, -12, 12, 12);
    }
    const QRect panelDamage = panelRect.united(m_lastLabelPanelRect);
    if (!panelDamage.isEmpty())
        damage += panelDamage.intersected(rect());
    m_lastLabelPanelRect = panelRect;
    if (!labelRect.isEmpty())
        damage += labelRect.intersected(rect());
    if (damage.isEmpty()) {
        // まだ一度も描いていない等で実寸が分からないときだけ、従来の帯。
        constexpr int kTopUiHeight = 96;
        damage += QRect(0, 0, width(), qMin(kTopUiHeight, height()));
    }

    // ---- 下：再生バー ----
    // 出ていない（フェードも終わっている）フレームでは足さない。ここを
    // 無条件に足していたせいで、リストだけが動くフレームでも上下がつながり、
    // バウンディング矩形がウィンドウ全体になっていた。
    //
    // 2026-09-15 D-7: 帯の高さを実寸に合わせる。
    //
    // 【なぜ】2026-09-15に入れた要求面積の計測（D-6、確認後に撤去）で、
    // この帯（全幅×144）が再生UIの要求面積のほぼ全部を占めていた
    // （1920幅で 1回あたり0.42Mpx、うち0.28Mpxがこの帯）。
    // しかし paintPlaybackOverlay() が実際に描いているのは
    //   ・丸ボタン2個   … 中心 y = H-28、半径16 → 上端 H-44
    //   ・シークバー    … y = H-30、高さ4（ハンドルを入れても H-37 程度）
    //   ・時刻の台      … 高さ32、中心 H-28 → 上端 H-44
    // で、音量スライダーが閉じていれば H-44 より上には何も無い。
    // 144はその3倍以上で、残りは毎回ただの空振りだった。
    // 横はシークバーが本当に端まで伸びるので全幅のままでよい。
    if (!m_canvas || m_canvas->playbackBarNeedsDamage()) {
        // 44（実際の上端）＋ ガラスの影とブラーのにじみぶんの余白。
        constexpr int kBarCoreHeight = 72;
        int top = qMax(0, height() - kBarCoreHeight);
        // 音量スライダーが開いている間だけ上へ伸ばす（閉じている間は
        // 触らない）。矩形は描画側と同じ volSliderScreenRect() を使う。
        if (m_canvas) {
            if (auto *av = m_canvas->activeVideo()) {
                if (av->isVolSliderVisible()) {
                    const QRectF vs = av->volSliderScreenRect(height());
                    if (!vs.isEmpty()) {
                        // 2026-09-16 F-3: 余白を8→24。volSliderScreenRect() は
                        // 当たり判定にも使われるので広げられないが、実際の溝は
                        // 高さ 80*m_volSliderAnim で、この anim は OutBack ＝
                        // 開き切る手前で1.0を超えて行き過ぎる（最大約1.1＝+8px）。
                        // さらに白いハンドルが半径5ぶん上へ出る。8では足りず、
                        // スライダーの一番上が無効化範囲から外れて切れていた。
                        constexpr int kVolSliderOvershootPad = 24;
                        top = qMin(top,
                                   qMax(0, vs.toAlignedRect().top()
                                              - kVolSliderOvershootPad));
                    }
                }
            }
        }
        damage += QRect(0, top, width(), height() - top);
    }
    if (damage.isEmpty())
        return;
    update(damage);
}

// 2026-09-12: イコライザだけの部分更新。
// paintEvent()はイベントの領域でクリップして「いつもどおり全部」を描くので、
// 板のキャッシュもこの矩形に重なる部分だけが転送される。描画構造は
// 変えていない（変えたのは「どの範囲の再描画を要求するか」だけ）。
// 矩形の中身は CanvasVideo::equalizerDamageRect() が決める。
void CanvasOverlayWidget::refreshPlaybackEqualizer(const QRect &damage)
{
    if (playbackUiFullyHidden())
        return;
    const QRect r = damage.intersected(rect());
    if (r.isEmpty())
        return;
    update(r);
}

void CanvasOverlayWidget::refreshFocusUi()
{
    if (uiFullyHidden())
        return;
    // The x2 badge is centered near the top. Seek feedback can appear on
    // either side of the window center, so invalidate both old/new positions
    // to clear correctly when its direction changes.
    QRegion damage(QRect(0, 0, width(), qMin(96, height())));
    constexpr int kIndicatorDamageSize = 88;
    const int indicatorY = height() / 2 - kIndicatorDamageSize / 2;
    for (const double xFactor : {0.28, 0.72}) {
        const int indicatorX = qRound(width() * xFactor) - kIndicatorDamageSize / 2;
        damage += QRect(indicatorX, indicatorY,
                        kIndicatorDamageSize, kIndicatorDamageSize);
    }
    update(damage);
}

void CanvasOverlayWidget::refreshBorderUi()
{
    // The cursor-follow line lives entirely at the window edge. The strong
    // reset glow remains in GL because it must invert the canvas pixels.
    constexpr int kBorderDamageWidth = 28;
    const int horizontal = qMin(kBorderDamageWidth, height());
    const int vertical = qMin(kBorderDamageWidth, width());
    QRegion damage(QRect(0, 0, width(), horizontal));
    damage += QRect(0, qMax(0, height() - horizontal), width(), horizontal);
    damage += QRect(0, 0, vertical, height());
    damage += QRect(qMax(0, width() - vertical), 0, vertical, height());
    update(damage);
}

void CanvasOverlayWidget::refreshLauncherTooltipUi()
{
    if (playbackUiFullyHidden())
        return;
    const QRect current = m_canvas
        ? m_canvas->launcherTooltipOverlayRect().adjusted(-2, -2, 2, 2)
        : QRect();
    const QRect damageRect = m_lastLauncherTooltipRect.united(current);
    m_lastLauncherTooltipRect = current;
    if (!damageRect.isEmpty()) {
        update(damageRect);
    }
}

// ===== 全画面が重い原因の切り分け（2026-09-14 D-1）=======================
//
// 【なぜ】[CanvasOverlay][stat] を窓サイズで比べると、UI本体（playback）は
// 707x476 でも 1821x1317 でも 1.6〜1.8ms で変わらないのに、border だけが
// 0.40ms → 1.21ms と3倍になる。paintBorderOverlay() はウィンドウの外周を
// 毎フレーム、全長にわたってアンチエイリアス付きでストロークしているので、
// 周長（707+476 → 1821+1317 ≒ 2.7倍）にそのまま比例する。しかも
// paintEvent は4つの描画を無条件に全部呼ぶため、ラベルのスクロールだけが
// 理由のフレームでも外周線を引き直している。
//
// もう一人の容疑者は area/frame=2.40Mpx（＝ウィンドウ全面）。半透明である
// このウィジェットのバッキングストアは窓サイズのARGB32なので、全画面では
// 9.6MB を再ラスタライズ＋再合成することになる（この合成コストは下の
// statTimer には入らない＝いま数字に出ていない）。
//
// どちらが主犯かで対策がまったく別物になるので、ここで切り替えて切り分ける。
//   0 … 通常
//   1 … 何も描かない（UIは消える。半透明レイヤーの合成コストだけが残る）
//   2 … border だけ描かない（UIは見えたまま）
// 2026-09-14 D-2: border は「濃さ固定＋外周の帯に掛かるフレームだけ描く」
// へ直したので、通常運転(0)へ戻す。1 はまだ残してあるので、全画面がなお
// 重いときに「UIを一切描かなくても重いか」を見るのに使える。
constexpr int kOverlayPaintDiagMode = 0;

void CanvasOverlayWidget::paintEvent(QPaintEvent *event)
{
    if (!m_canvas)
        return;
    if (kOverlayPaintDiagMode == 1)
        return;

    // 2026-09-14: 計測（メンバのコメント参照）。[RenderSchedule]のavgPaintは
    // GLパスだけの時間なので、GLの外で消えている時間がここなのかを見る。
    QElapsedTimer statTimer;
    statTimer.start();

    QPainter painter(this);
    painter.setClipRegion(event->region());
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // UI部品の背後ぼかし。描画コードが各要素の矩形をadd()していき、今回
    // 描かれなかった要素はend()で自動的に無効化される(Blur.h参照)。
    // ぼかし自体はCanvasWidget::paintGL()側のGLパスで行われるので、ここでは
    // 「どこをぼかすか」を伝えるだけ。
    // 2026-09-14: 内訳の計測。実測でこのpaintEvent全体が6〜14ms/フレーム
    // かかっており、しかもウィンドウの大きさをほぼ問わない（839x516でも
    // 1821x1317でも同じ）＝面積ではなく「何を描いているか」が原因。
    // 4つの呼び出しのどれなのかをここで切り分ける。
    QElapsedTimer sectionTimer;
    sectionTimer.start();
    // 2026-09-15: 設定パネル展開中の非表示。完全に隠れている間は描画自体を
    // 飛ばすが、begin()/end() は必ず通す（end()が前回分のブラー領域を
    // 無効化するので、UIが消えたのにブラーだけ残ることがない）。
    // 2026-09-17: 再生UIには「設定パネル」×「長押し2倍速」の両方の係数を、
    // フォーカス表示（2倍速バッジ・シーク表示）には設定パネルの係数だけを掛ける。
    // Regions::setOpacityScale() は add() の時点で掛かるので、描画ごとに
    // 切り替えてよい（Blur.cpp の Regions::add 参照）。
    const double playbackAlpha = m_uiAlpha * m_speedHoldAlpha;
    const bool drawPlayback = playbackAlpha > 0.0;
    const bool drawFocus = m_uiAlpha > 0.0;
    m_blurRegions.begin();
    if (drawPlayback) {
        painter.save();
        painter.setOpacity(playbackAlpha);
        m_blurRegions.setOpacityScale(playbackAlpha);
        m_canvas->paintPlaybackOverlay(painter, &m_blurRegions);
        painter.restore();
    }
    const double msPlayback = sectionTimer.nsecsElapsed() / 1e6;
    if (drawFocus) {
        painter.save();
        painter.setOpacity(m_uiAlpha);
        m_blurRegions.setOpacityScale(m_uiAlpha);
        m_canvas->paintFocusOverlay(painter, &m_blurRegions);
        painter.restore();
    }
    m_blurRegions.end();
    const double msFocus = sectionTimer.nsecsElapsed() / 1e6 - msPlayback;
    // 外周の境界線はウィンドウ枠の表現なので隠さない（setUiSuppressed()参照）。
    if (kOverlayPaintDiagMode != 2)
        m_canvas->paintBorderOverlay(painter, event->region());
    const double msBorder =
        sectionTimer.nsecsElapsed() / 1e6 - msPlayback - msFocus;
    if (drawPlayback) {
        painter.save();
        painter.setOpacity(playbackAlpha);
        m_canvas->paintLauncherOverlay(painter);
        painter.restore();
    }
    const double msLauncher =
        sectionTimer.nsecsElapsed() / 1e6 - msPlayback - msFocus - msBorder;
    m_statPlaybackMsSum += msPlayback;
    m_statFocusMsSum    += msFocus;
    m_statBorderMsSum   += msBorder;
    m_statLauncherMsSum += msLauncher;

    // A resize can move the tooltip without going through its show/hide
    // signal. Keep the cached damage rectangle aligned with what was
    // actually painted so a later hide invalidates the correct pixels.
    const QRect currentTooltipRect =
        m_canvas->launcherTooltipOverlayRect().adjusted(-2, -2, 2, 2);
    if (!currentTooltipRect.isEmpty())
        m_lastLauncherTooltipRect = currentTooltipRect;

    // ---- 計測の集計（2秒ごと）----
    const double ms = statTimer.nsecsElapsed() / 1e6;
    ++m_statFrames;
    m_statMsSum += ms;
    m_statMsMax = qMax(m_statMsMax, ms);
    for (const QRect &r : event->region())
        m_statAreaSum += static_cast<qint64>(r.width()) * r.height();
    // 2026-09-14: 「こちらが要求した細い帯」なのか「Qtがウィンドウ全体へ
    // 広げた1枚」なのかを区別する。rects=1 かつ bbox≒ウィンドウなら後者で、
    // 原因はこちらのinvalidateではなく、下のQOpenGLWidgetが毎フレーム
    // present するたびに半透明の子ウィジェットごと再合成されていること。
    m_statRectsSum += event->region().rectCount();
    const QRect bbox = event->region().boundingRect();
    m_statBboxAreaSum += static_cast<qint64>(bbox.width()) * bbox.height();
    if (bbox.width() * bbox.height() >= width() * height())
        ++m_statFullFrames;
    if (!m_statWindow.isValid())
        m_statWindow.start();
    if (m_statWindow.elapsed() >= 2000 && m_statFrames > 0) {
        qDebug("[CanvasOverlay][stat] v=f1 diag=%d frames=%d paint=%.2fms(max %.2f) "
               "playback=%.2f focus=%.2f border=%.2f launcher=%.2f "
               "area/frame=%.2fMpx rects/frame=%.1f fullFrames=%d widget=%dx%d",
               kOverlayPaintDiagMode,
               m_statFrames, m_statMsSum / m_statFrames, m_statMsMax,
               m_statPlaybackMsSum / m_statFrames,
               m_statFocusMsSum / m_statFrames,
               m_statBorderMsSum / m_statFrames,
               m_statLauncherMsSum / m_statFrames,
               double(m_statAreaSum) / m_statFrames / 1.0e6,
               double(m_statRectsSum) / m_statFrames,
               m_statFullFrames, width(), height());
        m_statWindow.restart();
        m_statFrames = 0;
        m_statMsSum = m_statMsMax = 0.0;
        m_statAreaSum = 0;
        m_statRectsSum = 0;
        m_statBboxAreaSum = 0;
        m_statFullFrames = 0;
        m_statPlaybackMsSum = m_statFocusMsSum = 0.0;
        m_statBorderMsSum = m_statLauncherMsSum = 0.0;
    }
}
