#pragma once
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QTimer>
#include <QElapsedTimer>
#include <QStringList>
#include "BubbleState.h"
#include "IPbPlayer.h"

// ============================================================
//  BubbleController.h
// ============================================================

class BubbleController : public QObject
{
    Q_OBJECT

public:
    enum class SleepReason {
        None,
        ResourceRelease,
        DeepFocusSleep,
        PlaybackEnded,
        // 2026-07-08: Paused/PausedHiddenのまま長時間（PAUSED_IDLE_RELEASE_MS）
        // 操作が無かったことによる自動解放。DeepFocusSleepと同様、画面内
        // 復帰だけで勝手に再生を再開してはいけない特殊なSleep理由として
        // notifyScreenRect()のSleep分岐で除外扱いする。
        PausedIdle,
    };

    static constexpr int QUALITY_LOW    = 360;
    static constexpr int QUALITY_NORMAL = 480;
    static constexpr int QUALITY_FOCUS  = 720;
    static constexpr int QUALITY_HD     = 1080;

    static constexpr int SLEEP_MARGIN_PX  = 200;
    static constexpr int SLEEP_GRACE_MS   = 3000;
    // 2026-09-14 Phase4: SYNC_INTERVAL_MS(=8) を廃止した。
    // 画面外猶予の残り時間を「notifyScreenRect()が呼ばれた回数 × 8ms」で
    // 数えていたため、呼び出し元（MainWindowのsyncLogicタイマー）の周期を
    // 変えると猶予時間がそのまま伸び縮みするという隠れた結合になっていた。
    // 実際 Phase4 で 120Hz → 60Hz にした時点で猶予は3秒→6秒に化けていた。
    // いまは m_sleepGraceClock による経過時間ベースなので、呼び出し周期が
    // 何Hzでも猶予は SLEEP_GRACE_MS のまま変わらない。
    // 2026-07-04: 他の動画が全画面中、自分がFocusSleep（一時停止のみ・
    // デコード資源は保持）のまま何秒続いたら、さらに踏み込んでmpv/FBOまで
    // 解放する（深い休止）かの猶予。すぐに解放すると「ちょっと確認して
    // すぐ戻る」操作のたびに毎回再ロードが必要になり使い勝手が悪いため、
    // 猶予を設ける。
    // 2026-07-04追記: 実機確認の結果、復帰（保存位置からの再オープン）が
    // 想定より速く体感を損なわないため、30秒→15秒→5秒に短縮。
    static constexpr int FOCUS_SLEEP_DEEP_RELEASE_MS = 5000;

    // 2026-07-08: Paused/PausedHiddenのまま（画面内外を問わず）シーク操作を
    // 含む再生関連の活動が無いまま経過してよい猶予。これを過ぎると
    // requestPausedIdleRelease()でSleepへ降格し、mpv/FBOを解放する
    // （ローカル動画はネイティブ解像度のままデコードするため、一時停止した
    // まま複数バブルを同時に「温存」し続けるとメモリが増え続ける対策）。
    static constexpr int PAUSED_IDLE_RELEASE_MS = 30000;

    // AudioOnly 中の position ログ間隔（ms）
    static constexpr int AUDIO_ONLY_LOG_INTERVAL_MS = 3000;

    explicit BubbleController(QObject *parent = nullptr);
    ~BubbleController() override;

    void       setPlayer(IPbPlayer *player);
    IPbPlayer *player() const { return m_player; }

    BubbleState state()         const { return m_state; }
    bool isSleeping()           const { return m_state == BubbleState::Sleep; }
    bool isAudioOnly()          const { return m_state == BubbleState::AudioOnly; }
    bool isPlaying()            const { return m_state == BubbleState::Playing; }
    bool canReceivePlayCmd()    const;
    // 現在のFocusSleepセッション開始からの経過時間。DeepFocusSleepで
    // Sleepへ降格した後も、フォーカス解除時の自動再開期限判定に使うため
    // 最後にrequestFocusSleep()された時刻を維持する。未開始なら-1。
    qint64 focusSleepElapsedMs() const {
        return m_focusSleepElapsedTimer.isValid() ? m_focusSleepElapsedTimer.elapsed() : -1;
    }
    QString sourceUrl()         const { return m_sourceUrl; }
    // 2026-09-23: 今流しているものはそのままに、その「名前」だけを付け替える。
    // ラベルの長押しで Solo → Mix にしたときのように、再生しているストリームは
    // 同じ動画のままで、URL（＝並びの中での意味づけ）だけが変わる場合に使う。
    // ここを更新しないと、本体が「mpvは前の動画を抱えたまま」と誤解し、
    // 再生/停止を受け付けなくなる（CanvasVideo::controllerHoldsStaleSource）。
    void renameSourceUrl(const QString &newSourceUrl) { m_sourceUrl = newSourceUrl; }
    int     currentQuality()    const { return m_currentQuality; }
    int     targetQuality()     const { return m_targetQuality; }

    // audioUrl: DASH分離ストリーム対応。空でなければ streamUrl（映像）とは
    // 別のURLから音声を結合する（IPbPlayer::open()参照）。
    // disableVideoTrack: 2026-07-08。trueの場合、以後このバブルが
    // Ended再生・retry()・wakeUp()等で何度再オープンされても、常に
    // 映像トラックを無効化して開く（IPbPlayer::open()参照）。ローカル
    // 音声ファイル（埋め込みジャケットのデコードクラッシュ対策）向け。
    void loadUrl(const QString &sourceUrl,
                 const QString &streamUrl,
                 int quality = QUALITY_NORMAL,
                 const QStringList &httpHeaders = {},
                 qint64 startPositionMs = 0,
                 const QString &audioUrl = QString(),
                 bool disableVideoTrack = false);

    void play();
    void pause();
    void togglePlayPause();
    void seek(qint64 ms);
    // 高速（キーフレーム基準）シーク。シークバーのドラッグ中など、短時間に
    // 連続して呼ばれる用途向け。ドラッグ終了時には改めてseek()を1回呼び、
    // 正確な位置へ合わせ直すこと（詳細はIPbPlayer::seekFast()参照）。
    void seekFast(qint64 ms);
    // Error状態から同じストリームURLで再オープンを試みる（ローカルファイル用。
    // 署名付きURLが失効しうるプラグイン解決動画にはCanvasVideo側で
    // resolveViaPlugin()による再解決を使うこと）。resumePositionMsを渡すと
    // 0の代わりにその位置から再開する（省略時は先頭から）。
    void retry(qint64 resumePositionMs = 0);
    // 再生速度（1.0 = 等倍）。動画全画面中の「長押しで2倍速」用。
    void setSpeed(double speed);

    // 2026-07-15: IPbPlayerがまだ一度もsetPlayer()されていない段階
    // （mpvのGL初期化失敗等）で発生したエラーをError状態として報告する。
    // onPlayerError()（IPbPlayer::errorOccurred経由）と違い、この時点の
    // 状態はIdleであることが多く、Idle→Errorは直接遷移できない
    // （BubbleState.h参照）ため、必要ならLoadingを経由してからErrorへ
    // 遷移する。
    void reportError(const QString &message);

    void notifyQMediaPlayerReady();
    void notifyQMediaPlayerPlaying();
    void notifyQMediaPlayerPaused();
    void notifyQMediaPlayerEnded();

    void notifyScreenRect(const QRectF &screenRect, const QRectF &viewportRect);
    // 通常動画は常に true。ライブ配信だけは島設定で切り替える。
    void setOffscreenAudioOnlyEnabled(bool enabled);
    void requestAudioOnly();
    void wakeUpVideo();
    void requestSleep(SleepReason reason = SleepReason::ResourceRelease);
    void requestSleepAfterPlaybackEnd() { requestSleep(SleepReason::PlaybackEnded); }
    void wakeUp(bool resumePlayback = true);
    void requestFocusSleep();
    // 2026-07-04: FocusSleep中に猶予（FOCUS_SLEEP_DEEP_RELEASE_MS）を過ぎても
    // フォーカスが解除されない場合に呼ぶ。再生位置を保存した上でSleepへ
    // 降格し、mpv/FBOを丸ごと解放する（Sleep突入時の既存フックに相乗り）。
    // 呼び出し元はnotifyScreenRect()（FocusSleep中は毎フレーム呼ばれる）を
    // 想定。
    void requestDeepFocusSleep();
    // 2026-07-08: Paused/PausedHiddenのままPAUSED_IDLE_RELEASE_MSを過ぎても
    // 活動（再生・シーク）が無い場合に呼ぶ。呼び出し元はnotifyScreenRect()
    // （Paused/PausedHidden中も画面内外に関わらず毎フレーム呼ばれる）を想定。
    void requestPausedIdleRelease();
    void wakeUpFromFocusSleep(bool resumePlayback);
    void forceRecoverFromFocusSleep(bool resumePlayback);
    void suppressHiddenOptimizationFor(int ms);

    void notifyScreenPixels(int screenW, int screenH);

    bool wasReleasedAfterPlaybackEnd() const { return m_sleepReason == SleepReason::PlaybackEnded; }
    bool canAutoWakeFromSleep() const;

    void setScreenPosition(const QPointF &screenCenter, const QSizeF &viewportSize);
    // 2026-09-14: サラウンドモード廃止に伴い
    // setSurroundExcluded() / isSurroundExcluded() を削除。

    PbVideoFrame currentFrame() const;
    qint64       position()     const;
    qint64       duration()     const;
    float        volume()       const;
    void         setVolume(float v);

signals:
    void stateChanged(BubbleState newState, BubbleState oldState);
    void firstFrameReady();
    void frameUpdated();
    void playbackEnded();
    void qualityChangeRequested(int targetQuality);
    void errorOccurred(const QString &message);
    // 2026-07-03: Sleep状態でメモリ節約のためm_player自体が解放されている
    // 場合に wakeUp() が呼ばれた際に発火する。IPbPlayer/MpvPlayerの生成は
    // OpenGLコンテキストが必要でBubbleControllerの責務外のため、実際の
    // プレイヤー再生成はCanvasVideo側（GLコンテキストcurrent保証がある
    // paintGL()内）に委ね、生成完了後に改めてwakeUp()をリトライしてもらう。
    void playerNeeded();

private slots:
    void onPlayerFirstFrameReady();
    void onPlayerFrameUpdated();
    void onPlayerInternalStateChanged(PbInternalState internal);
    void onPlayerError(const QString &message);

    // AudioOnly 中に再生位置が進んでいることを確認するログ
    void onAudioOnlyPositionLog();

private:
    void transitionTo(BubbleState newState);
    void transitionToHidden(BubbleState hiddenState);
    void wakeUpHidden();
    // 2026-07-16: 複数バブル同時のAudioOnly復帰負荷分散用。wakeUpVideo()の
    // 実処理本体（resumeVideo()呼び出し＋状態遷移）をここに切り出し、
    // 同時復帰が重なる場合はQTimer::singleShotで少しずつずらして呼ぶ。
    // 詳細はBubbleController.cppのscheduleStaggeredResumeDelay()参照。
    void performResumeFromAudioOnly();
    bool isOutsideViewport(const QRectF &screenRect, const QRectF &viewportRect) const;
    bool isHiddenOptimizationSuppressed() const;
    int  computeTargetQuality(int screenPx) const;

    IPbPlayer  *m_player = nullptr;
    BubbleState m_state  {BubbleState::Idle};
    BubbleState m_stateBeforeHidden {BubbleState::Idle};
    // 2026-07-16: notifyScreenRect()はAudioOnly中も毎フレーム呼ばれ続けるため、
    // wakeUpVideo()もスタガー遅延中に何度も再突入し得る。実処理は
    // performResumeFromAudioOnly()実行（＝状態がAudioOnlyから抜ける）まで
    // 一度も終わらないため、そのままだと毎フレーム
    // scheduleStaggeredResumeDelay()が呼ばれて共有スロットを無駄に消費し、
    // 実際の復帰が数秒単位で遅延する不具合があった（5本同時復帰の実機ログで確認）。
    // このフラグで「既に復帰予約済みか」を管理し、二重予約を防ぐ。
    bool        m_resumeFromAudioOnlyScheduled {false};
    qint64      m_focusSleepPosition {0};
    // FocusSleepに入っていた経過時間の計測用（FOCUS_SLEEP_DEEP_RELEASE_MSでrequestDeepFocusSleep()
    // へ進めるかどうかの判定に使う）。FocusSleep以外の状態では未使用。
    QElapsedTimer m_focusSleepElapsedTimer;
    // requestDeepFocusSleep()で保存した再開位置（ms）。-1 = 該当なし。
    // wakeUp()がSleepから再オープンする際、ここに値があれば0ではなく
    // この位置から再開し、使用後は-1にリセットする。
    qint64      m_deepFocusSleepResumePositionMs {-1};
    bool        m_sleepWakeResumePlayback {true};
    SleepReason m_sleepReason {SleepReason::None};

    // 2026-07-08: Paused/PausedHiddenへ入った時刻からの経過時間計測用。
    // requestPausedIdleRelease()へ進めるかどうかの判定に使う。
    // m_focusSleepElapsedTimerと同様、Paused/PausedHidden以外の状態では
    // 未使用（値が古くても状態ガードで参照されないため無害）。
    // 画面内外の切り替え（Paused⇄PausedHidden）では意図的にリセットせず、
    // 再生・シーク等の実際の活動があった時のみrestart()する。
    QElapsedTimer m_pausedIdleTimer;

    QString m_sourceUrl;
    QString m_streamUrl;
    // DASH分離ストリーム対応。空文字列 = 通常経路（streamUrlに音声も含む）。
    // loadUrl() で設定され、Ended後の再生・retry()・wakeUp() 等で
    // 同じ音声トラックを使って再オープンするために保持する。
    QString m_audioStreamUrl;
    QStringList m_httpHeaders;
    // 2026-07-08: loadUrl()で指定された値を保持し、Ended再生・retry()・
    // wakeUp()等、同じバブルが何度再オープンされても一貫して
    // IPbPlayer::open()へ渡す（loadUrl()自体は最初の1回しか呼ばれない
    // ため、ここに保存しておかないと2回目以降のopen()で消えてしまう）。
    bool m_videoTrackDisabled  {false};

    bool m_sleepTimerActive    {false};
    int  m_sleepGraceRemainMs  {0};
    // 2026-09-14 Phase4: 画面外猶予の実測クロック（SYNC_INTERVAL_MS の
    // コメント参照）。m_sleepTimerActive を立てるときに start() する。
    QElapsedTimer m_sleepGraceClock;
    qint64 m_sleepGraceLastLogMs {0};
    QElapsedTimer m_hiddenOptimizationSuppressTimer;
    int  m_hiddenOptimizationSuppressMs {0};


    QPointF m_screenCenter;

    int  m_currentQuality      {QUALITY_NORMAL};
    int  m_targetQuality       {QUALITY_NORMAL};
    bool m_qualityChangePending{false};

    // AudioOnly 中の position 進行確認タイマー
    QTimer  m_audioOnlyLogTimer;
    qint64  m_audioOnlyLogLastPos {0};

    bool    m_offscreenAudioOnlyEnabled {true};
};
