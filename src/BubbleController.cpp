#include "BubbleController.h"
#include <QDebug>
#include <QElapsedTimer>
#include <algorithm>

// ============================================================
//  BubbleController.cpp
// ============================================================

namespace {
// 2026-07-16: AudioOnly復帰の負荷分散（同時復帰スタガー）。
//
// 【背景】カメラを大きく動かす等で複数のバブルが同時にAudioOnlyから
// 画面内へ戻ると、それぞれのresumeVideo()（vid=auto送信）がmpv内部で
// デコーダ・hwdec・フィルタチェーンの再構築＋refresh seekを引き起こす。
// これが同一フレーム付近で何本も重なると、CPU/GPU/音声デバイスへの
// 瞬間的な負荷が集中し、実機ログで複数本同時にAudio device underrun/
// Playing→Bufferingが発生することを確認した（詳細は
// #AudioOnly-resume-stutter調査ログ）。個々のresumeVideo()自体の重さは
// mpv側の制約でこれ以上削れないため、プロセス全体で見て復帰呼び出しの
// タイミングを少しずつずらし、同時に重なる本数を減らす。
//
// 実装は極めてシンプルな「次に空いているスロット」方式。全BubbleController
// インスタンスで共有する静的状態を持ち、resumeVideo()を呼びたい側が
// scheduleStaggeredResumeDelay()を呼ぶと、次に空いているスロットまでの
// 遅延msを返す（今すぐ実行してよければ0）。一定時間（kIdleResetMs）
// 復帰が無ければ基準点をリセットするため、無関係な単発復帰まで
// 毎回遅延させることはない。
constexpr int kResumeStaggerIntervalMs = 60;
constexpr qint64 kResumeStaggerIdleResetMs = 500;

QElapsedTimer &resumeStaggerClock()
{
    static QElapsedTimer clock;
    static bool started = false;
    if (!started) {
        clock.start();
        started = true;
    }
    return clock;
}

int scheduleStaggeredResumeDelay()
{
    static qint64 nextSlotMs = 0;
    QElapsedTimer &clock = resumeStaggerClock();
    const qint64 nowMs = clock.elapsed();
    if (nowMs - nextSlotMs > kResumeStaggerIdleResetMs)
        nextSlotMs = nowMs;
    const qint64 delay = std::max<qint64>(0, nextSlotMs - nowMs);
    nextSlotMs = std::max(nowMs, nextSlotMs) + kResumeStaggerIntervalMs;
    return static_cast<int>(delay);
}
} // namespace

BubbleController::BubbleController(QObject *parent)
    : QObject(parent)
{
    // AudioOnly 中の position 進行確認タイマー
    m_audioOnlyLogTimer.setInterval(AUDIO_ONLY_LOG_INTERVAL_MS);
    m_audioOnlyLogTimer.setSingleShot(false);
    connect(&m_audioOnlyLogTimer, &QTimer::timeout,
            this, &BubbleController::onAudioOnlyPositionLog);
}

BubbleController::~BubbleController() = default;

// ================================================================
//  IPbPlayer 注入
// ================================================================
void BubbleController::setPlayer(IPbPlayer *player)
{
    if (m_player)
        m_player->disconnect(this);

    m_player = player;
    if (!m_player) return;

    connect(m_player, &IPbPlayer::firstFrameReady,
            this,     &BubbleController::onPlayerFirstFrameReady);
    connect(m_player, &IPbPlayer::frameUpdated,
            this,     &BubbleController::onPlayerFrameUpdated);
    connect(m_player, &IPbPlayer::internalStateChanged,
            this,     &BubbleController::onPlayerInternalStateChanged);
    connect(m_player, &IPbPlayer::errorOccurred,
            this,     &BubbleController::onPlayerError);
}

// ================================================================
//  状態遷移
// ================================================================
void BubbleController::transitionTo(BubbleState newState)
{
    if (newState == m_state) return;

    if (!isBubbleStateTransitionValid(m_state, newState)) {
        qWarning("[BC] 不正な遷移を拒否: %s → %s",
                 qPrintable(bubbleStateToString(m_state)),
                 qPrintable(bubbleStateToString(newState)));
        return;
    }

    BubbleState oldState = m_state;
    m_state = newState;

    qDebug("[BC %p] %s → %s",
           static_cast<void*>(this),
           qPrintable(bubbleStateToString(oldState)),
           qPrintable(bubbleStateToString(newState)));

    emit stateChanged(newState, oldState);
}

void BubbleController::transitionToHidden(BubbleState hiddenState)
{
    if (isHiddenOptimizationSuppressed()) {
        qDebug("[BC %p] [FocusSleep][hiddenSuppress] block transitionToHidden target=%s state=%s player=%p remainingMs=%lld",
               static_cast<void*>(this),
               qPrintable(bubbleStateToString(hiddenState)),
               qPrintable(bubbleStateToString(m_state)),
               static_cast<void*>(m_player),
               static_cast<long long>(m_hiddenOptimizationSuppressMs - m_hiddenOptimizationSuppressTimer.elapsed()));
        return;
    }

    m_stateBeforeHidden = m_state;
    m_sleepTimerActive = false;
    m_sleepGraceRemainMs = 0;
    m_audioOnlyLogLastPos = m_player ? m_player->position() : 0;
    if (m_player) m_player->setLowCacheMode(true);
    m_audioOnlyLogTimer.start();
    qDebug("[BC %p] [%s] enter hidden  pos=%.2fs",
           static_cast<void*>(this),
           qPrintable(bubbleStateToString(hiddenState)),
           m_audioOnlyLogLastPos / 1000.0);
    transitionTo(hiddenState);
}

void BubbleController::wakeUpHidden()
{
    if (m_state == BubbleState::PausedHidden) {
        const qint64 pos = m_player ? m_player->position() : 0;
        qDebug("[BC %p] [PausedHidden] back_in_view  pos=%.2fs  next=Paused",
               static_cast<void*>(this), pos / 1000.0);
        if (m_player) m_player->setLowCacheMode(false);
        m_audioOnlyLogTimer.stop();
        m_sleepTimerActive = false;
        m_sleepGraceRemainMs = 0;
        m_stateBeforeHidden = BubbleState::Idle;
        transitionTo(BubbleState::Paused);
        return;
    }

    if (m_state == BubbleState::ReadyHidden) {
        const qint64 pos = m_player ? m_player->position() : 0;
        qDebug("[BC %p] [ReadyHidden] back_in_view  pos=%.2fs  next=Ready",
               static_cast<void*>(this), pos / 1000.0);
        if (m_player) m_player->setLowCacheMode(false);
        m_audioOnlyLogTimer.stop();
        m_sleepTimerActive = false;
        m_sleepGraceRemainMs = 0;
        m_stateBeforeHidden = BubbleState::Idle;
        transitionTo(BubbleState::Ready);
    }
}

bool BubbleController::canReceivePlayCmd() const
{
    return m_state == BubbleState::Ready
        || m_state == BubbleState::Paused
        || m_state == BubbleState::ReadyHidden
        || m_state == BubbleState::PausedHidden
        || m_state == BubbleState::Buffering
        || m_state == BubbleState::Ended;
}

// ================================================================
//  Phase 1 ブリッジ
// ================================================================
void BubbleController::notifyQMediaPlayerReady()
{
    if (m_state == BubbleState::Loading)
        transitionTo(BubbleState::Ready);
}
void BubbleController::notifyQMediaPlayerPlaying()
{
    if (m_state == BubbleState::Ready
     || m_state == BubbleState::Paused
     || m_state == BubbleState::Buffering)
        transitionTo(BubbleState::Playing);
}
void BubbleController::notifyQMediaPlayerPaused()
{
    if (m_state == BubbleState::Playing)
        transitionTo(BubbleState::Paused);
}
void BubbleController::notifyQMediaPlayerEnded()
{
    if (m_state == BubbleState::Playing
     || m_state == BubbleState::Buffering) {
        transitionTo(BubbleState::Ended);
        emit playbackEnded();
    }
}

// ================================================================
//  再生操作 API
// ================================================================
void BubbleController::loadUrl(const QString &sourceUrl,
                                const QString &streamUrl,
                                int quality,
                                const QStringList &httpHeaders,
                                qint64 startPositionMs,
                                const QString &audioUrl,
                                bool disableVideoTrack)
{
    m_sourceUrl           = sourceUrl;
    m_streamUrl           = streamUrl;
    m_audioStreamUrl      = audioUrl;
    m_httpHeaders         = httpHeaders;
    m_currentQuality      = quality;
    m_targetQuality       = quality;
    m_sleepTimerActive    = false;
    m_qualityChangePending= false;
    m_stateBeforeHidden   = BubbleState::Idle;
    m_sleepReason = SleepReason::None;
    m_videoTrackDisabled  = disableVideoTrack;
    // 別動画への差し替え等で新規ロードする場合、前の動画のdeep focus sleep
    // 再開位置が誤って引き継がれないようにリセットしておく。
    m_deepFocusSleepResumePositionMs = -1;

    // 【2026-09-12】いまの状態から直接 Loading へ行けないことがある。
    // 状態遷移表（BubbleState.h）で Playing / Buffering / AudioOnly →
    // Loading は不正なので、そのまま transitionTo(Loading) すると弾かれ、
    // mpvだけが新しいURLを開いて状態機械は元のまま＝再生が始まらない
    // （実機ログ: バブルが画面外＝AudioOnly の間にリストから別の動画を
    // 選ぶと "不正な遷移を拒否: AudioOnly → Loading" のあと固まる）。
    //
    // 呼び出し側で pause() してから来る経路もあるが、pause() はmpvへ
    // コマンドを送るだけで状態が変わるのは非同期なので間に合わない。
    // ここで同期的に一段挟んでから Loading へ進む
    // （Buffering は Paused へ直接行けないので Playing 経由）。
    // AudioOnly / *Hidden から来た場合、そちらで動かしていた見張りを
    // 止めておく（止めないと、新しい動画に対して古い再生位置と比べる
    // "pos advancing ... (WARNING - stopped)" が出続ける）。
    m_audioOnlyLogTimer.stop();
    m_sleepGraceRemainMs = 0;

    if (!isBubbleStateTransitionValid(m_state, BubbleState::Loading)) {
        if (m_state == BubbleState::Buffering)
            transitionTo(BubbleState::Playing);
        if (m_state == BubbleState::Playing
         || m_state == BubbleState::AudioOnly)
            transitionTo(BubbleState::Paused);
    }

    transitionTo(BubbleState::Loading);
    if (m_player) {
        m_player->setLowCacheMode(false);
        m_player->setHttpHeaders(m_httpHeaders);
        // 2026-07-17: play()のEndedブランチ（このファイル内、コメント参照）
        // と同じ理由の対策。mpvの pause プロパティは「値が変化した瞬間」
        // だけ変更イベントを出す。直前の動画は pause=0（再生中）のまま
        // EOFに達している可能性があり（連続再生でplaybackEnded直後に
        // 同じMpvPlayerインスタンスへ新しいURLをloadUrl()する経路等）、
        // その状態でopen()するだけだと pause は 0→0 で変化せず、
        // Playingへの遷移トリガーとなる internalStateChanged(Decoding)
        // が発火しないまま state が Ready に固まってしまう
        // （mpv自体は実際に再生を続けるため、シークは効くが再生/一時停止
        // ボタンやUIの状態表示だけがズレる不具合として現れる）。
        // 明示的に一度 pause=1 にしてから open() し、後続の再生開始で
        // 必ず 1→0 の本物のedgeを発生させる。
        m_player->pause();
        m_player->open(streamUrl, startPositionMs, audioUrl, m_videoTrackDisabled);
    }
}

void BubbleController::play()
{
    if (!m_player) return;

    if (m_state == BubbleState::AudioOnly) {
        wakeUpVideo();
        return;
    }
    if (m_state == BubbleState::Sleep) {
        wakeUp();
        return;
    }
    if (m_state == BubbleState::PausedHidden
     || m_state == BubbleState::ReadyHidden) {
        wakeUpHidden();
        qDebug("[BC %p] [%s] play after hidden  pos=%.2fs",
               static_cast<void*>(this),
               qPrintable(bubbleStateToString(m_state)),
               m_player ? m_player->position() / 1000.0 : 0.0);
    }
    if (m_state == BubbleState::Playing) {
        qDebug("[BC %p] すでに Playing — play() スキップ", static_cast<void*>(this));
        return;
    }
    if (m_state == BubbleState::Ended) {
        if (!m_streamUrl.isEmpty()) {
            transitionTo(BubbleState::Loading);
            m_player->setHttpHeaders(m_httpHeaders);
            // mpvの pause プロパティは「値が変化した瞬間」だけ変更イベントを出す。
            // 再生終了直前は pause=0（再生中）のままEOFに達しているため、ここで
            // open()するだけだと pause は 0→0 で一切変化せず、Playingへの復帰
            // トリガーとなる internalStateChanged(Decoding) が二度と発火しない
            // （＝mpv自体は鳴っているのにアプリの状態がPlayingへ上がらず、
            //   UIが再生中と認識できないまま固まる不具合の直接原因）。
            // 明示的に一度 pause=1 にしてから open() し、この後の play() で
            // 必ず 1→0 の本物のedgeを発生させて確実にDecodingを発火させる。
            m_player->pause();
            m_player->open(m_streamUrl, 0, m_audioStreamUrl, m_videoTrackDisabled);
        }
        return;
    }

    if (canReceivePlayCmd())
        m_player->play();
}

void BubbleController::pause()
{
    if (!m_player) return;

    if (m_state == BubbleState::AudioOnly) {
        m_audioOnlyLogTimer.stop();
        m_player->pause();
        m_player->resumeVideo();
        m_stateBeforeHidden = BubbleState::Paused;
        transitionTo(BubbleState::PausedHidden);
        // 2026-07-08: AudioOnly（画面外再生中）からの直接pause()は、この場で
        // PausedHiddenへ遷移するためonPlayerInternalStateChanged()の
        // Pausedケースを経由しない。ここで明示的にアイドルタイマーを開始する。
        m_pausedIdleTimer.restart();
        return;
    }

    if (m_state == BubbleState::Playing
     || m_state == BubbleState::Buffering)
        m_player->pause();
}

void BubbleController::togglePlayPause()
{
    // 2026-09-12: AudioOnly（画面外＝映像だけ停止・音は継続）と Buffering も
    // 「再生中」として扱う。Playing だけを見ていると、画面外のバブルで
    // 一時停止を押しても play() 側へ落ちて止まらず、UIの ▶ / || と実際の
    // 挙動が食い違う（リストのヘッダにボタンを付けて表面化した）。
    if (m_state == BubbleState::Playing
     || m_state == BubbleState::AudioOnly
     || m_state == BubbleState::Buffering)
        pause();
    else
        play();
}

void BubbleController::seek(qint64 ms)
{
    if (!m_player) return;
    if (m_state == BubbleState::Ended)
        transitionTo(BubbleState::Ready);
    // 2026-07-08: 一時停止中のシークバー操作も「動画視聴の一環」として
    // アイドルタイマーをリセットする対象に含める（設計方針として確認済み）。
    if (m_state == BubbleState::Paused || m_state == BubbleState::PausedHidden)
        m_pausedIdleTimer.restart();
    // 2026-07-04: シークバー操作・矢印キーシーク（±5秒）等、通常の精密
    // シーク経路すべてがここを通る。実機ログで、DASH（映像/音声別URL）
    // ストリームに対してこの経路から精密seek()を送ると、既知の
    // デマルチプレクサ詰まりバグ（MpvPlayer.cppのコメント参照）が
    // 再現し、詰まった結果メインスレッドを巻き込んでUI全体が無反応に
    // なる（全画面解除不可・Flowの次動画へ進めない等）ことを確認した。
    // そのため一時期はFocusSleep復帰時と同様、自動リトライ付きの
    // seekWithStallWatchdog()に統一していた。
    //
    // 2026-07-12追記: 詰まりの根本原因（mpv/ffmpegが配信CDNへ
    // 単一の巨大なオープンエンドRangeリクエストを投げ、応答が無いまま
    // 無限に待ち続けること）自体を動画サイト用プラグイン側のストリーム中継（数MB
    // 単位に分割したRangeリクエストへ変換して中継するローカルサーバー）
    // で解消したため、この経路でseekWithStallWatchdog()のキーフレーム
    // シーク（seekFast、"absolute+keyframes"）を使い続ける必要がなくなった。
    // 実機検証で、キーフレーム間隔が広い動画だと矢印キー±5秒シークが
    // 常に同じキーフレームに丸められて「シークしても同じ場所に戻る
    // ように見える」不具合が発覚したため、通常のシーク経路は精密seek()
    // に戻す。FocusSleep復帰時・FILE_LOADED直後の位置復元（実際に
    // ネットワークが冷えた状態からの大きなジャンプになりやすい経路）は
    // 引き続きseekWithStallWatchdog()を使う（BubbleController::seek()
    // 経由ではなく、それぞれ個別に直接呼んでいる箇所を参照）。
    m_player->seek(ms);
}

void BubbleController::seekFast(qint64 ms)
{
    if (!m_player) return;
    if (m_state == BubbleState::Ended)
        transitionTo(BubbleState::Ready);
    if (m_state == BubbleState::Paused || m_state == BubbleState::PausedHidden)
        m_pausedIdleTimer.restart();
    m_player->seekFast(ms);
}

void BubbleController::setSpeed(double speed)
{
    if (m_player)
        m_player->setSpeed(speed);
}

void BubbleController::retry(qint64 resumePositionMs)
{
    if (m_state != BubbleState::Error) return;
    if (m_streamUrl.isEmpty()) return;
    transitionTo(BubbleState::Loading);
    if (m_player) {
        m_player->setHttpHeaders(m_httpHeaders);
        m_player->open(m_streamUrl, qMax<qint64>(0, resumePositionMs), m_audioStreamUrl, m_videoTrackDisabled);
    }
}

// ================================================================
//  画面外最適化制御
// ================================================================
void BubbleController::notifyScreenRect(const QRectF &screenRect,
                                         const QRectF &viewportRect)
{
    if (isHiddenOptimizationSuppressed()) {
        if (isOutsideViewport(screenRect, viewportRect)) {
            qDebug("[BC %p] [FocusSleep][hiddenSuppress] skip notify out_of_view state=%s player=%p remainingMs=%lld rect=(%.1f,%.1f %.1fx%.1f)",
                   static_cast<void*>(this),
                   qPrintable(bubbleStateToString(m_state)),
                   static_cast<void*>(m_player),
                   static_cast<long long>(m_hiddenOptimizationSuppressMs - m_hiddenOptimizationSuppressTimer.elapsed()),
                   screenRect.x(), screenRect.y(), screenRect.width(), screenRect.height());
        }
        m_sleepTimerActive = false;
        m_sleepGraceRemainMs = 0;
        return;
    }

    // 2026-07-08: 一時停止アイドル解放。Paused/PausedHiddenのどちらでも
    // （画面内外を問わず）、シーク操作を含む再生関連の活動が
    // PAUSED_IDLE_RELEASE_MS続けて無ければデコード資源を解放する。
    // notifyScreenRect()はPausedHidden中も毎フレーム呼ばれ続けるため、
    // FocusSleepの深休止判定と同様ここで経過時間を見るだけで済む
    // （専用タイマー不要）。
    if ((m_state == BubbleState::Paused || m_state == BubbleState::PausedHidden)
     && m_pausedIdleTimer.isValid()
     && m_pausedIdleTimer.elapsed() >= PAUSED_IDLE_RELEASE_MS) {
        requestPausedIdleRelease();
        return;
    }

    if (m_state == BubbleState::AudioOnly) {
        if (!isOutsideViewport(screenRect, viewportRect))
            wakeUpVideo();
        return;
    }

    if (m_state == BubbleState::Sleep) {
        // 2026-07-03: 画面内復帰による自動wakeUp()は、本来「画面外に長時間
        // 放置された単独バブルが画面内に戻ったら復帰する」という別目的の
        // ロジック。flowStandby中のバブルは「画面内にいるかどうか」とは
        // 無関係に、Flow側が明示的にsetFlowStandby(false)+play()するまで
        // 眠ったままであるべき（そもそもFlow編集中は絶対に再生されない
        // 前提）。ここでm_flowStandbyを見ずに無条件でwakeUp()すると、
        // 毎フレーム呼ばれるnotifyScreenRect()がflowStandby中のバブルを
        // 画面内というだけで叩き起こし、他方でリンク再同期処理が即座に
        // 寝かせ直すため、再生⇄停止を高速に繰り返す不具合になる
        // （実機で確認済み）。
        //
        // 2026-07-07: 同じ理由で m_sleepReason == DeepFocusSleep も除外する。
        // このSleepは「他バブルが現在フォーカス中で、自分は長時間の
        // FocusSleep後にさらに深く休止させられた」ことを意味し、画面内に
        // あるかどうかとは無関係にフォーカスが解除されるまで眠ったままで
        // あるべき。ここでcanAutoWakeFromSleep()の一般条件だけを見て
        // 無条件にwakeUp()すると、フォーカス中の動画とは別のバブル
        // （多くの場合キャンバス上に常時見えているローカル動画）が、
        // 単に画面内にあるというだけで毎フレームのnotifyScreenRect()経由で
        // 勝手に再生・発音してしまう（フォーカス中の動画に他の音声が混ざる
        // 不具合の実際の原因）。このreasonでのSleepからの復帰は、
        // CanvasWidget::endFocusPlaybackPolicy()の遅延ウェイク
        // （canWakeFromFocusPolicy()経由、フォーカス解除時にのみ実行される）
        // だけが担当する。canAutoWakeFromSleep()自体は変更しない
        // （そちらはcanWakeFromFocusPolicy()としてフォーカス解除時の
        // 正規の復帰にも使われているため、ここだけで追加除外する）。
        // 2026-07-08: PausedIdle（一時停止アイドル解放）も同じ理由で除外する。
        // ユーザーが明示的に一時停止したまま解放されたバブルが、単に画面内へ
        // 戻ってきたというだけで勝手に再生を再開してはならない
        // （キャンバスを見回しているだけで意図せず再生が始まる不具合になる）。
        // 明示的な再生操作（bcPlay()/togglePlayPause()）経由のwakeUp(true)
        // だけが、このSleepからの復帰を担当する。
        if (m_sleepReason != SleepReason::DeepFocusSleep
         && m_sleepReason != SleepReason::PausedIdle
         && canAutoWakeFromSleep() && !isOutsideViewport(screenRect, viewportRect))
            wakeUp();
        return;
    }

    if (m_state == BubbleState::PausedHidden
     || m_state == BubbleState::ReadyHidden) {
        if (!isOutsideViewport(screenRect, viewportRect))
            wakeUpHidden();
        return;
    }

    if (m_state == BubbleState::FocusSleep) {
        // 2026-07-04: 他の動画が全画面中で自分は一時停止のみ（デコード資源は
        // 保持）のFocusSleep状態が長時間（FOCUS_SLEEP_DEEP_RELEASE_MS）続く
        // 場合、さらに踏み込んでmpv/FBOまで解放する（深い休止）。
        // notifyScreenRect()はFocusSleep中も毎フレーム呼ばれ続けるため、
        // ここで経過時間を見るだけで済む（専用タイマー不要）。
        if (m_focusSleepElapsedTimer.isValid()
         && m_focusSleepElapsedTimer.elapsed() >= FOCUS_SLEEP_DEEP_RELEASE_MS) {
            requestDeepFocusSleep();
        }
        return;
    }

    const bool active = m_state == BubbleState::Playing
                     || m_state == BubbleState::Paused
                     || m_state == BubbleState::Ready
                     || m_state == BubbleState::Buffering;
    if (!active) return;

    if (!m_offscreenAudioOnlyEnabled) {
        m_sleepTimerActive = false;
        m_sleepGraceRemainMs = 0;
        return;
    }

    if (isOutsideViewport(screenRect, viewportRect)) {
        if (!m_sleepTimerActive) {
            m_sleepTimerActive    = true;
            m_sleepGraceRemainMs  = SLEEP_GRACE_MS;
            // 2026-09-14 Phase4: 経過時間ベースへ移行（BubbleController.h の
            // SYNC_INTERVAL_MS のコメント参照）。
            m_sleepGraceClock.start();
            m_sleepGraceLastLogMs = 0;
            // 2026-07-04: Flowチェーンの全画面切り替え直後に、なぜかカメラが
            // 対象バブルへ移動していないように見える不具合の調査用。
            // screenRect/viewportRectの実値がないと「本当にカメラが
            // 動いていないのか」「動いてはいるが何かの理由でこの判定だけ
            // 外れているのか」を切り分けられないため、両方をログに出す。
            qDebug("[BC %p] [AudioOnly] 画面外を検知 — 猶予タイマー開始 (%dms) pos=%.2fs "
                   "screenRect=(%.1f,%.1f %.1fx%.1f) viewport=(%.1f,%.1f %.1fx%.1f)",
                   static_cast<void*>(this),
                   SLEEP_GRACE_MS,
                   m_player ? m_player->position() / 1000.0 : 0.0,
                   screenRect.x(), screenRect.y(), screenRect.width(), screenRect.height(),
                   viewportRect.x(), viewportRect.y(), viewportRect.width(), viewportRect.height());
        } else {
            // 2026-09-14 Phase4: 「呼ばれた回数 × 8ms」から実時間ベースへ。
            // notifyScreenRect() の呼び出し周期が変わっても猶予は3秒のまま。
            const qint64 graceElapsed =
                m_sleepGraceClock.isValid() ? m_sleepGraceClock.elapsed() : 0;
            m_sleepGraceRemainMs =
                static_cast<int>(SLEEP_GRACE_MS - graceElapsed);
            // 2026-07-04調査用: 猶予期間中、画面内に戻ったかどうかが
            // 本当に一度も成立していないのかを約400msおきに可視化する
            // （毎フレームだと3秒で300行以上出て読めなくなるため間引く）。
            if (graceElapsed - m_sleepGraceLastLogMs >= 400) {
                m_sleepGraceLastLogMs = graceElapsed;
                qDebug("[BC %p] [AudioOnly] 猶予継続中 remainMs=%lld "
                       "screenRect=(%.1f,%.1f %.1fx%.1f) viewport=(%.1f,%.1f %.1fx%.1f)",
                       static_cast<void*>(this),
                       static_cast<long long>(m_sleepGraceRemainMs),
                       screenRect.x(), screenRect.y(), screenRect.width(), screenRect.height(),
                       viewportRect.x(), viewportRect.y(), viewportRect.width(), viewportRect.height());
            }
            if (m_sleepGraceRemainMs <= 0) {
                if (m_state == BubbleState::Playing
                 || m_state == BubbleState::Buffering) {
                    requestAudioOnly();
                } else if (m_state == BubbleState::Paused) {
                    transitionToHidden(BubbleState::PausedHidden);
                } else if (m_state == BubbleState::Ready) {
                    transitionToHidden(BubbleState::ReadyHidden);
                }
            }
        }
    } else {
        if (m_sleepTimerActive) {
            qDebug("[BC %p] [AudioOnly] 猶予タイマーリセット（画面内に戻った）",
                   static_cast<void*>(this));
        }
        m_sleepTimerActive    = false;
        m_sleepGraceRemainMs  = 0;
    }
}

void BubbleController::setOffscreenAudioOnlyEnabled(bool enabled)
{
    if (m_offscreenAudioOnlyEnabled == enabled)
        return;

    m_offscreenAudioOnlyEnabled = enabled;
    qDebug("[BC %p] [AudioOnly] offscreen resource saving=%d",
           static_cast<void*>(this), enabled);

    // 設定をOFFにした瞬間、画面外にいるライブ配信も映像を戻す。
    if (!enabled && m_state == BubbleState::AudioOnly)
        wakeUpVideo();
}

// ----------------------------------------------------------------
//  requestAudioOnly(): 画面外 → 映像デコードのみ停止・音声継続
// ----------------------------------------------------------------
void BubbleController::requestAudioOnly()
{
    if (!m_offscreenAudioOnlyEnabled)
        return;

    if (m_state != BubbleState::Playing
     && m_state != BubbleState::Buffering)
        return;

    if (isHiddenOptimizationSuppressed()) {
        qDebug("[BC %p] [FocusSleep][hiddenSuppress] block requestAudioOnly state=%s player=%p remainingMs=%lld",
               static_cast<void*>(this),
               qPrintable(bubbleStateToString(m_state)),
               static_cast<void*>(m_player),
               static_cast<long long>(m_hiddenOptimizationSuppressMs - m_hiddenOptimizationSuppressTimer.elapsed()));
        return;
    }

    const double posS = m_player ? m_player->position() / 1000.0 : 0.0;
    m_stateBeforeHidden = m_state;

    qDebug("[BC %p] [AudioOnly] Playing -> AudioOnly  reason=out_of_view  pos=%.2fs",
           static_cast<void*>(this), posS);
    qDebug("[BC %p] [AudioOnly]   → pauseVideo() を呼びます（vid=no）",
           static_cast<void*>(this));

    if (m_player)
        m_player->pauseVideo();
    if (m_player)
        m_player->setLowCacheMode(true);

    m_sleepTimerActive    = false;
    m_sleepGraceRemainMs  = 0;

    // AudioOnly 中の position 進行確認タイマーを開始
    m_audioOnlyLogLastPos = m_player ? m_player->position() : 0;
    m_audioOnlyLogTimer.start();

    // 2026-07-16: AudioOnlyに新規突入するたびに復帰予約フラグをリセット
    // しておく（念のための保険。通常はperformResumeFromAudioOnly()側で
    // 既にfalseに戻っているはず）。
    m_resumeFromAudioOnlyScheduled = false;

    transitionTo(BubbleState::AudioOnly);
}

// ----------------------------------------------------------------
//  wakeUpVideo(): AudioOnly → Playing（seek 不要・映像デコード再開）
// ----------------------------------------------------------------
void BubbleController::wakeUpVideo()
{
    if (m_state != BubbleState::AudioOnly) return;

    // 2026-07-16: notifyScreenRect()はAudioOnly中も毎フレーム呼ばれ続け、
    // 画面内にあれば毎回wakeUpVideo()に入ってくる。スタガー遅延中は
    // performResumeFromAudioOnly()が実行される（＝m_stateがAudioOnly
    // から抜ける）までstateが変わらないため、ガードなしだと毎フレーム
    // ここへ再突入してscheduleStaggeredResumeDelay()を呼んでしまい、
    // 共有スロットを無駄に消費して実際の復帰が数秒単位で遅延する
    // 不具合があった（5本同時復帰の実機ログで確認）。
    // 既に予約済みなら何もしない。
    if (m_resumeFromAudioOnlyScheduled) return;
    m_resumeFromAudioOnlyScheduled = true;

    // タイマー停止
    m_audioOnlyLogTimer.stop();

    // 2026-07-16: 複数バブルが同時にAudioOnlyから復帰する場合、実処理
    // （resumeVideo()呼び出し）をプロセス全体で少しずつずらして発行する
    // （負荷分散、詳細はファイル先頭のscheduleStaggeredResumeDelay()の
    // コメント参照）。すぐ実行してよい場合は遅延0で即座に呼ぶため、
    // 1本だけの復帰では従来と体感差が出ない。
    const int staggerDelayMs = scheduleStaggeredResumeDelay();
    if (staggerDelayMs <= 0) {
        performResumeFromAudioOnly();
        return;
    }

    qDebug("[BC %p] [AudioOnly] 同時復帰の負荷分散のため resumeVideo() を%dms遅延実行します",
           static_cast<void*>(this), staggerDelayMs);
    QTimer::singleShot(staggerDelayMs, this, [this]() {
        performResumeFromAudioOnly();
    });
}

// ----------------------------------------------------------------
//  performResumeFromAudioOnly(): wakeUpVideo() の実処理本体。
//  スタガー遅延の有無にかかわらずここに集約する。遅延実行された場合、
//  待っている間に状態が変わっている可能性があるため、先頭で
//  AudioOnly状態のままかどうかを再確認する。
// ----------------------------------------------------------------
void BubbleController::performResumeFromAudioOnly()
{
    // 2026-07-16: 呼ばれた時点（早期returnも含む）で予約フラグを解除する。
    // ここで解除しておかないと、m_stateが既に変わっていた場合に
    // フラグがtrueのまま残り、次にAudioOnlyへ再突入したときに
    // wakeUpVideo()が二度と反応しなくなる。
    m_resumeFromAudioOnlyScheduled = false;

    if (m_state != BubbleState::AudioOnly) return;

    const double posS = m_player ? m_player->position() / 1000.0 : 0.0;

    qDebug("[BC %p] [AudioOnly] AudioOnly -> Playing  reason=back_in_view  pos=%.2fs",
           static_cast<void*>(this), posS);
    qDebug("[BC %p] [AudioOnly]   → resumeVideo() を呼びます（vid=auto）",
           static_cast<void*>(this));

    // 2026-07-16: 順序を入れ替え。以前は resumeVideo()（vid=auto送信）を
    // 先に呼んでいたため、demuxerがまだ低キャッシュモード
    // （demuxer-max-back-bytes=1MiB等）のままの状態でキーフレーム
    // シークバック先を決めてしまい、実測で数秒〜6秒以上も過去の
    // キーフレームまで戻ることがあった（video-resume-probeログで確認）。
    // このシークバック距離が大きいと再バッファリング
    // （Playing→Buffering、音声underrun、約0.3秒の実際の停止）を
    // 誘発しやすく、複数バブル同時復帰時のカクつきの一因になっていた。
    // 先に setLowCacheMode(false) でキャッシュ上限を通常に戻してから
    // resumeVideo() を呼ぶことで、シークバック判定時により広い
    // demuxerキャッシュ範囲を使えるようにし、戻り距離を縮める。
    if (m_player)
        m_player->setLowCacheMode(false);
    if (m_player)
        m_player->resumeVideo();

    const BubbleState targetState =
        (m_stateBeforeHidden == BubbleState::Buffering)
            ? BubbleState::Buffering
            : BubbleState::Playing;
    m_stateBeforeHidden = BubbleState::Idle;
    transitionTo(targetState);

}

// ----------------------------------------------------------------
//  AudioOnly 中の position 進行確認（タイマーコールバック）
// ----------------------------------------------------------------
void BubbleController::onAudioOnlyPositionLog()
{
    if (m_state != BubbleState::AudioOnly
     && m_state != BubbleState::PausedHidden
     && m_state != BubbleState::ReadyHidden) {
        m_audioOnlyLogTimer.stop();
        return;
    }

    const qint64 nowPos  = m_player ? m_player->position() : 0;
    const double nowS    = nowPos  / 1000.0;
    const double lastS   = m_audioOnlyLogLastPos / 1000.0;
    const bool   moving  = nowPos > m_audioOnlyLogLastPos;

    if (m_state == BubbleState::AudioOnly) {
        qDebug("[BC %p] [AudioOnly] pos advancing: %.2f -> %.2f  (%s)",
               static_cast<void*>(this),
               lastS, nowS,
               moving ? "OK - moving" : "WARNING - stopped");
    } else {
        qDebug("[BC %p] [%s] pos stable: %.2f -> %.2f  (%s)",
               static_cast<void*>(this),
               qPrintable(bubbleStateToString(m_state)),
               lastS, nowS,
               moving ? "WARNING - moving" : "OK - stopped");
    }

    m_audioOnlyLogLastPos = nowPos;
}

// ----------------------------------------------------------------
//  requestSleep(): 完全停止（音声も停止）
//  通常の画面外最適化には requestAudioOnly() を使うこと。
// ----------------------------------------------------------------
void BubbleController::requestSleep(SleepReason reason)
{
    if (m_state == BubbleState::Sleep) {
        if (reason == SleepReason::PlaybackEnded)
            m_sleepReason = reason;
        return;
    }
    if (m_state == BubbleState::Idle
     || m_state == BubbleState::Error
     || m_state == BubbleState::Loading)
        return;

    m_sleepReason = reason;
    qDebug("[BC %p] Sleep 移行 — 完全停止（音声も停止） reason=%d",
           static_cast<void*>(this),
           static_cast<int>(reason));

    if (m_player)
        m_player->stop();
    if (m_player)
        m_player->setLowCacheMode(false);

    m_sleepTimerActive    = false;
    m_sleepGraceRemainMs  = 0;
    m_audioOnlyLogTimer.stop();
    m_stateBeforeHidden   = BubbleState::Idle;
    transitionTo(BubbleState::Sleep);
}

// ----------------------------------------------------------------
//  【メモ】FocusSleepとメモリ節約についての注意（2026-07-03検討）
// ----------------------------------------------------------------
// FocusSleep突入時にFBOやmpvインスタンス自体を解放してメモリを浮かせる案は
// 過去に一度試されており、md/bug.md に「FocusSleep解除後に映像がサムネイル
// 固定になり戻らない」という未解決バグとして記録されている。現在の実装が
// pause()のみで済ませている（FBO・mpvインスタンスを一切壊さない）のは、
// 復帰の確実さを優先してこの問題を避けるためと考えられる。
// そのため、同じ「壊して作り直す」系の節約策はFocusSleepには適用しない。
//
// より踏み込んだ節約（stop()だけでなくmpvインスタンス自体を破棄し、
// wakeUp()時に新規生成し直す）は、"完全休止・復帰は最初から再ロード"という
// 前提を既に持つ requestSleep()/wakeUp()（このすぐ上）でなら検討の余地がある。
// ただし試算では浮くメモリは休止バブル1個あたり数MB〜10MB程度と見積もりが甘く、
// 「エンジンを壊して作り直す」こと自体がFocusSleepと同種の不具合リスクを
// 持ち込みうるため、費用対効果を見極めてから着手すること。
void BubbleController::requestFocusSleep()
{
    if (!m_player) return;
    if (m_state == BubbleState::FocusSleep
     || m_state == BubbleState::Idle
     || m_state == BubbleState::Error
     || m_state == BubbleState::Ended
     || m_state == BubbleState::Sleep)
        return;

    // Loading 中は position がまだないので 0 のまま筊え、
    // mpv open() の完了待ち中なので pause() は呼ばない（安全ガード）。
    // 復帾後は wakeUpFromFocusSleep() 内で再度 play() されるので、
    // onPlayerFirstFrameReady で m_flowStandby チェックが有効になる。
    const bool isLoading = (m_state == BubbleState::Loading);

    m_focusSleepPosition = m_player->position();
    qDebug("[BC %p] [FocusSleep] enter from=%s pos=%.2fs isLoading=%d",
           static_cast<void*>(this),
           qPrintable(bubbleStateToString(m_state)),
           m_focusSleepPosition / 1000.0,
           isLoading);

    m_audioOnlyLogTimer.stop();
    m_sleepTimerActive = false;
    m_sleepGraceRemainMs = 0;
    if (!isLoading)
        m_player->pause();  // Loading 中は mpv がまだ open() 中なので pause() をスキップ
    // 2026-07-04: 長時間フォーカスが続いた場合にrequestDeepFocusSleep()へ
    // 進めるかどうかの判定用に、FocusSleep突入時刻を記録しておく。
    m_focusSleepElapsedTimer.restart();
    transitionTo(BubbleState::FocusSleep);
}

void BubbleController::requestDeepFocusSleep()
{
    if (m_state != BubbleState::FocusSleep) return;
    if (!m_player) return;

    m_deepFocusSleepResumePositionMs = m_player->position();
    qDebug("[BC %p] [FocusSleep] %dms経過 — 完全休止へ降格（デコード資源を解放） pos=%.2fs",
           static_cast<void*>(this),
           FOCUS_SLEEP_DEEP_RELEASE_MS,
           m_deepFocusSleepResumePositionMs / 1000.0);

    m_player->stop();
    m_player->setLowCacheMode(false);
    m_sleepReason = SleepReason::DeepFocusSleep;
    m_sleepTimerActive   = false;
    m_sleepGraceRemainMs = 0;
    m_stateBeforeHidden  = BubbleState::Idle;
    // Sleep突入は既存のstateChangedフック（CanvasVideo側）でmpv/FBOが
    // 丸ごと解放される。以後フォーカスが解除された際は、この状態(Sleep)を
    // 見てbcPlay()（wakeUp()経由、保存した位置から再オープン）で復帰する
    // （CanvasWidget::endFocusPlaybackPolicy()の遅延ウェイク参照）。
    transitionTo(BubbleState::Sleep);
}

// ----------------------------------------------------------------
//  requestPausedIdleRelease(): 一時停止アイドル解放
//  Paused/PausedHiddenのままPAUSED_IDLE_RELEASE_MSを過ぎても活動が無い
//  場合に呼ぶ。requestDeepFocusSleep()と同様、再開位置を
//  m_deepFocusSleepResumePositionMsへ保存した上でSleepへ降格し、
//  mpv/FBOを解放する（Sleep突入は既存のstateChangedフック(CanvasVideo側)
//  でmpv/FBOが丸ごと解放される、Phase8での既存の仕組みにそのまま相乗り）。
//  DeepFocusSleepと違い「他バブルが全画面中」という前提が無いため、
//  画面内外どちらの状態からでも起こりうる。
// ----------------------------------------------------------------
void BubbleController::requestPausedIdleRelease()
{
    if (m_state != BubbleState::Paused && m_state != BubbleState::PausedHidden)
        return;
    if (!m_player) return;

    m_deepFocusSleepResumePositionMs = m_player->position();
    qDebug("[BC %p] [PausedIdle] %dms間操作なし — 完全休止へ降格（デコード資源を解放） pos=%.2fs from=%s",
           static_cast<void*>(this),
           PAUSED_IDLE_RELEASE_MS,
           m_deepFocusSleepResumePositionMs / 1000.0,
           qPrintable(bubbleStateToString(m_state)));

    m_player->stop();
    m_player->setLowCacheMode(false);
    m_sleepReason = SleepReason::PausedIdle;
    m_sleepTimerActive   = false;
    m_sleepGraceRemainMs = 0;
    m_audioOnlyLogTimer.stop();
    m_stateBeforeHidden  = BubbleState::Idle;
    transitionTo(BubbleState::Sleep);
}

void BubbleController::wakeUpFromFocusSleep(bool resumePlayback)
{
    forceRecoverFromFocusSleep(resumePlayback);
}

void BubbleController::forceRecoverFromFocusSleep(bool resumePlayback)
{
    if (!m_player) return;

    const bool wasFocusSleep = (m_state == BubbleState::FocusSleep);
    const bool suppressed = m_player->isVideoSuppressed();
    if (!wasFocusSleep && !suppressed) {
        qDebug("[BC %p] [FocusRecover] skip state=%s suppressed=%d player=%p",
               static_cast<void*>(this),
               qPrintable(bubbleStateToString(m_state)),
               suppressed,
               static_cast<void*>(m_player));
        return;
    }

    const qint64 restorePos = m_focusSleepPosition;
    qDebug("[BC %p] [FocusRecover] start resume=%d stateBefore=%s suppressed=%d pos=%.2fs player=%p",
           static_cast<void*>(this),
           resumePlayback,
           qPrintable(bubbleStateToString(m_state)),
           suppressed,
           restorePos / 1000.0,
           static_cast<void*>(m_player));

    if (suppressed) {
        m_player->resumeVideo();
        m_player->setLowCacheMode(false);
        qDebug("[BC %p] [FocusRecover] resumeVideo forced", static_cast<void*>(this));
    }
    // 2026-07-03: 精密seek()単体だとDASHストリームでデマルチプレクサ
    // キャッシュスレッドが詰まり、二度とフレームが進まなくなる既知の
    // 不具合を踏むことがある（IPbPlayer::seekWithStallWatchdog()の
    // コメント・MpvPlayer.cppのDASHシーク詰まり調査参照）。FocusSleep
    // 復帰は「ロード直後ではないが再開位置への一発シーク」という
    // まさに危険な条件に当てはまるため、詰まったら自動リトライする
    // seekWithStallWatchdog()を使う。
    if (wasFocusSleep && restorePos > 0)
        m_player->seekWithStallWatchdog(restorePos);

    const BubbleState targetState = resumePlayback ? BubbleState::Playing : BubbleState::Paused;
    if (m_state != targetState)
        transitionTo(targetState);
    if (resumePlayback)
        m_player->play();
    else
        m_player->pause();
    qDebug("[BC %p] [FocusRecover] %s forced stateAfter=%s suppressed=%d player=%p",
           static_cast<void*>(this),
           resumePlayback ? "play" : "pause",
           qPrintable(bubbleStateToString(m_state)),
           m_player->isVideoSuppressed(),
           static_cast<void*>(m_player));

    suppressHiddenOptimizationFor(2000);
}

void BubbleController::suppressHiddenOptimizationFor(int ms)
{
    m_hiddenOptimizationSuppressMs = std::max(0, ms);
    if (m_hiddenOptimizationSuppressMs <= 0)
        return;

    m_hiddenOptimizationSuppressTimer.restart();
    m_sleepTimerActive = false;
    m_sleepGraceRemainMs = 0;
    qDebug("[BC %p] [FocusSleep][hiddenSuppress] start ms=%d state=%s player=%p",
           static_cast<void*>(this),
           m_hiddenOptimizationSuppressMs,
           qPrintable(bubbleStateToString(m_state)),
           static_cast<void*>(m_player));
}

bool BubbleController::isHiddenOptimizationSuppressed() const
{
    return m_hiddenOptimizationSuppressTimer.isValid()
        && m_hiddenOptimizationSuppressTimer.elapsed() < m_hiddenOptimizationSuppressMs;
}

// ----------------------------------------------------------------
//  wakeUp(): Sleep → Loading（再オープン・最初から）
// ----------------------------------------------------------------
void BubbleController::wakeUp(bool resumePlayback)
{
    if (m_state != BubbleState::Sleep) return;
    m_sleepReason = SleepReason::None;
    m_sleepWakeResumePlayback = resumePlayback;
    if (m_streamUrl.isEmpty()) {
        qDebug("[BC %p] wakeUp スキップ — streamUrl が空", static_cast<void*>(this));
        return;
    }
    // 2026-07-03: メモリ節約のためSleep中はm_player自体（MpvPlayer）が
    // 丸ごと解放されていることがある。ここではプレイヤーの生成はできない
    // （OpenGLコンテキストが必要でBubbleControllerの責務外）ため、状態は
    // Sleepのまま据え置き、CanvasVideo側にplayerNeeded()で再生成を依頼する。
    // CanvasVideoはGLコンテキストcurrent保証があるpaintGL()内でプレイヤーを
    // 作り直し、setPlayer()した上で改めてwakeUp()を呼び直す。
    if (!m_player) {
        qDebug("[BC %p] wakeUp — player未生成のためplayerNeeded()を発火", static_cast<void*>(this));
        emit playerNeeded();
        return;
    }
    // 2026-07-04: requestDeepFocusSleep()経由でSleepへ落ちた場合は、
    // 保存しておいた再開位置がここにある。通常のSleep（Flowスタンバイや
    // 長時間画面外放置等）は0開始のままでよいため、値がある場合のみ使い、
    // 使用後は必ず-1へ戻す（次回以降の通常wakeUp()に影響させない）。
    const qint64 resumeMs = m_deepFocusSleepResumePositionMs >= 0
                                 ? m_deepFocusSleepResumePositionMs : 0;
    m_deepFocusSleepResumePositionMs = -1;
    qDebug("[BC %p] wakeUp — 再ロード開始 resumeMs=%lld resumePlayback=%d", static_cast<void*>(this),
           static_cast<long long>(resumeMs),
           resumePlayback);
    transitionTo(BubbleState::Loading);
    m_player->setHttpHeaders(m_httpHeaders);
    m_player->open(m_streamUrl, resumeMs, m_audioStreamUrl, m_videoTrackDisabled);
}

bool BubbleController::canAutoWakeFromSleep() const
{
    return m_state == BubbleState::Sleep
        && m_sleepReason != SleepReason::PlaybackEnded;
}

bool BubbleController::isOutsideViewport(const QRectF &sr,
                                          const QRectF &vp) const
{
    return sr.right()  < vp.left()   - SLEEP_MARGIN_PX
        || sr.left()   > vp.right()  + SLEEP_MARGIN_PX
        || sr.bottom() < vp.top()    - SLEEP_MARGIN_PX
        || sr.top()    > vp.bottom() + SLEEP_MARGIN_PX;
}

// ================================================================
//  品質管理
// ================================================================
void BubbleController::notifyScreenPixels(int screenW, int screenH)
{
    int maxPx     = std::max(screenW, screenH);
    int newTarget = computeTargetQuality(maxPx);
    if (newTarget == m_targetQuality) return;

    m_targetQuality = newTarget;
    if (m_targetQuality != m_currentQuality && !m_qualityChangePending) {
        m_qualityChangePending = true;
        qDebug("[BC %p] 品質切り替えリクエスト: %dp → %dp",
               static_cast<void*>(this),
               m_currentQuality, m_targetQuality);
        emit qualityChangeRequested(m_targetQuality);
    }
}

int BubbleController::computeTargetQuality(int screenPx) const
{
    static constexpr int levels[] = {144, 360, 480, 720, 1080};
    for (int q : levels)
        if (screenPx <= static_cast<int>(q * 1.3)) return q;
    return QUALITY_HD;
}

// ================================================================
//  画面位置の記録（旧SurroundMode連携のスタブ。サラウンドは2026-09-14に
//  廃止したが、setScreenPosition()自体は他から呼ばれるため残してある）
// ================================================================
void BubbleController::setScreenPosition(const QPointF &screenCenter,
                                          const QSizeF  &viewportSize)
{
    m_screenCenter = screenCenter;
    Q_UNUSED(viewportSize)
}

// ================================================================
//  情報取得
// ================================================================
PbVideoFrame BubbleController::currentFrame() const
{
    if (!m_player
     || m_state == BubbleState::Sleep
     || m_state == BubbleState::Idle)
        return {};
    return m_player->currentFrame();
}

qint64 BubbleController::position() const
{
    // requestDeepFocusSleep()/requestPausedIdleRelease()でSleepへ降格した
    // バブルはm_playerごと解放済み（省メモリのためmpvインスタンスを丸ごと
    // 破棄する）。その際の再開位置はm_playerではなく
    // m_deepFocusSleepResumePositionMsにのみ残っているため、position()は
    // まずそちらを優先する必要がある。
    //
    // これを見ずにm_player有無だけで判定すると、一時停止アイドル解放後の
    // バブルはposition()が常に0を返してしまい、セッション保存経路
    // （CanvasSessionSerializer::sessionToJson()のvideo->position()呼び出し。
    // レイヤー切り替え時のSessionManager::save()もこれを使う）が
    // 「一時停止して少し放置していただけの動画」の再生位置を0として
    // 保存してしまい、次回復元時に先頭から再生される不具合の原因になっていた。
    //
    // 通常のrequestSleep()（FlowMode待機・長時間画面外放置等、意図的に
    // 0開始でよいケース）ではm_deepFocusSleepResumePositionMsは-1のままなので、
    // その場合は従来通りm_player(nullptr)→0にフォールバックする。
    if (m_deepFocusSleepResumePositionMs >= 0)
        return m_deepFocusSleepResumePositionMs;
    return m_player ? m_player->position() : 0;
}

qint64 BubbleController::duration() const
{
    return m_player ? m_player->duration() : 0;
}

float BubbleController::volume() const
{
    return m_player ? m_player->volume() : 1.0f;
}

void BubbleController::setVolume(float v)
{
    if (m_player) m_player->setVolume(v);
}

// ================================================================
//  IPbPlayer シグナル受信
// ================================================================
void BubbleController::onPlayerFirstFrameReady()
{
    if (m_state == BubbleState::Loading) {
        transitionTo(BubbleState::Ready);
        emit firstFrameReady();
        if (!m_sleepWakeResumePlayback) {
            m_sleepWakeResumePlayback = true;
            if (m_player)
                m_player->pause();
        } else {
            m_sleepWakeResumePlayback = true;
            play();
        }
    } else if (m_state == BubbleState::FocusSleep) {
        // Loading 中に FocusSleep に入った後に mpv がロード完了した場合。
        // そのまま FocusSleep を維持し、再生しない。
        // 復帾時に wakeUpFromFocusSleep() が play() する。
        qDebug("[BC %p] [FocusSleep] firstFrameReady during FocusSleep — keeping sleep, skip play",
               static_cast<void*>(this));
        m_player->pause();  // 確实に一時停止する
        emit firstFrameReady();
    }
}

void BubbleController::onPlayerFrameUpdated()
{
    emit frameUpdated();
}

void BubbleController::onPlayerInternalStateChanged(PbInternalState internal)
{
    switch (internal) {
    case PbInternalState::Decoding:
        if (m_state == BubbleState::FocusSleep) {
            // Loading 中に FocusSleep に入った後、mpv が Decoding 状態に移行しても
            // Playing へ澛行しない。FocusSleep を維持。
            if (m_player) m_player->pause();
            qDebug("[BC %p] [FocusSleep] Decoding during FocusSleep — pause", static_cast<void*>(this));
            break;
        }
        if (m_state == BubbleState::Loading
         || m_state == BubbleState::Ready
         || m_state == BubbleState::Paused
         || m_state == BubbleState::Buffering
         || m_state == BubbleState::AudioOnly)
            transitionTo(BubbleState::Playing);
        break;

    case PbInternalState::Paused:
        if (m_state == BubbleState::Playing
         || m_state == BubbleState::AudioOnly) {
            transitionTo(BubbleState::Paused);
            // 2026-07-08: 一時停止アイドル解放タイマーを開始。
            m_pausedIdleTimer.restart();
        }
        break;

    case PbInternalState::Buffering:
        if (m_state == BubbleState::Playing)
            transitionTo(BubbleState::Buffering);
        break;

    case PbInternalState::Ended:
        if (m_state == BubbleState::Playing
         || m_state == BubbleState::Buffering
         || m_state == BubbleState::AudioOnly) {
            m_audioOnlyLogTimer.stop();
            transitionTo(BubbleState::Ended);
            emit playbackEnded();
        }
        break;

    default:
        break;
    }
}

void BubbleController::onPlayerError(const QString &message)
{
    qWarning("[BC %p] プレイヤーエラー: %s",
             static_cast<void*>(this), qPrintable(message));
    reportError(message);
}

void BubbleController::reportError(const QString &message)
{
    m_audioOnlyLogTimer.stop();
    // Idle→Errorは直接遷移できない（BubbleState.h参照）。IPbPlayerが
    // 一度も繋がっていない段階（mpvのGL初期化失敗等）で呼ばれた場合は
    // Idleのままなので、Loadingを経由してからErrorへ落とす。
    //
    // 【不具合調査メモ】もしtransitionTo()が"不正な遷移を拒否"という
    // qWarningを出してError化に失敗している場合（ログ検索: "不正な遷移を拒否"）、
    // ここに来た時点でm_stateがIdle/Loading以外（想定外の状態からの呼び出し）
    // になっている可能性が高い。呼び出し元（CanvasVideo::initializeMpv()の
    // GL失敗時など）でどの状態から呼ばれたか確認すること。
    if (m_state == BubbleState::Idle)
        transitionTo(BubbleState::Loading);
    transitionTo(BubbleState::Error);
    emit errorOccurred(message);
}
