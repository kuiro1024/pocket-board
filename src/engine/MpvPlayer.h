#pragma once
#include <QHash>
#include <QByteArray>
#include "IPbPlayer.h"
#include <QSize>
#include <QStringList>
#include <QElapsedTimer>
#include <functional>
#include <memory>

struct MpvAsyncRenderSlot;
class VideoRenderThread;

// ============================================================
//  engine/MpvPlayer.h
//
//  IPbPlayer の libmpv 実装クラス。
//  mpv のヘッダは一切 include しない。
//  mpv_handle / mpv_render_context は void* として保持し、
//  .cpp 内でキャストする。
// ============================================================

class MpvPlayer : public IPbPlayer
{
    Q_OBJECT

public:
    explicit MpvPlayer(QObject *parent = nullptr);
    ~MpvPlayer() override;

    void open(const QString &streamUrl, qint64 startPositionMs = 0,
             const QString &audioUrl = QString(),
             bool disableVideoTrack = false) override;
    void setHttpHeaders(const QStringList &headers) override;
    void play()                            override;
    void pause()                           override;
    void stop()                            override;
    void seek(qint64 posMs)               override;
    void seekFast(qint64 posMs)           override;

    // 2026-07-03: DASH（video/audio別URL）の外部音声ストリームが、ロード
    // 直後のシーク要求で無応答のまま固まる既知の相性問題が実機ログで
    // 確認された（精密シーク・キーフレームシークの両方で再現、配信側の
    // Rangeリクエスト処理の癖が原因と見られる。MpvPlayer.cppのFILE_LOADED
    // 処理のコメント参照）。興味深いことに、固まった状態でも「もう一度
    // シークし直す」と復帰することがログで確認できたため、seekFast()を
    // 投げたあと一定時間ポジションが目標に近づかなければ自動的に
    // 再シークするウォッチドッグ付きシーク。DASHロード直後の同期シーク
    // 箇所ではこちらを使うこと。
    void seekWithStallWatchdog(qint64 posMs) override;
    void setLooping(bool loop)             override;
    void setSpeed(double speed)            override;

    // ---- AudioOnly モード ----
    // pauseVideo()  : vid=no — 映像トラックのみ切り離す。音声・位置は継続。
    // resumeVideo() : vid=auto — 映像トラックを現在位置のまま再開。
    void pauseVideo()  override;
    void resumeVideo() override;
    void setLowCacheMode(bool enabled) override;
    void releaseVideoResources() override;

    PbInternalState internalState()  const override;
    qint64          position()       const override;
    qint64          duration()       const override;
    QSize           videoSize()      const override;
    float           volume()         const override;
    void            setVolume(float v)     override;

    PbVideoFrame currentFrame()                          const override;
    void         logDiagnostics(const char *tag = "Memory") const;

    bool hasMpvHandle() const { return m_mpv != nullptr; }
    bool hasRenderContext() const { return m_async != nullptr; }

    bool hasFbo() const { return m_fbo != 0 && m_fboColorTex != 0; }
    bool hasValidFboTexture() const { return m_fboColorTex != 0 && m_fboW > 0 && m_fboH > 0; }
    bool isVideoSuppressed() const override { return m_videoSuppressed; }
    GLuint fboColorTextureId() const { return m_fboColorTex; }
    GLuint fboTextureId() const { return m_fboColorTex; }
    int fboWidth() const { return m_fboW; }
    int fboHeight() const { return m_fboH; }
    quint64 estimatedFboBytes() const { return static_cast<quint64>(m_fboW) * static_cast<quint64>(m_fboH) * 4ULL; }
    QString debugPropertyString(const char *name) const { return mpvPropertyString(name); }
    QString debugPropertyFlagString(const char *name) const { return mpvPropertyFlagString(name); }

    // ---- 2026-09-15 非同期化 P2（md/async_video_render_design_2026-09-15.md）----
    // 2026-09-16 P7: 唯一の描画経路。呼ぶと、mpv_render_context の生成・描画・解放を
    // すべて VideoRenderThread（共有GLコンテキスト）上で行う「非同期プレイヤー」
    // になる。一度決めたら破棄まで経路は変わらない。GL current は不要。
    // 生成の成否はワーカーで確定するので、失敗は asyncGlInitFailed() で届く。
    bool initializeGlAsync(VideoRenderThread *thread);
    bool isAsyncRender() const { return m_async != nullptr; }
    // paintGL() から毎フレーム呼ぶ。w/h = 描画FBOの目標サイズ、
    // renderAllowed = 従来の suppressRenderToFbo の否定（判定式は呼び出し側のまま）。
    void updateAsyncRenderState(int w, int h, bool renderAllowed);
    // 2026-09-15 P3: 状態遷移時に呼ぶ（paintGL が来ない画面外バブルでも即反映）。
    void setAsyncRenderAllowed(bool renderAllowed);
    // paintGL() の描画前に呼ぶ。ワーカーが新しいフレームを出していれば受け取り、
    // 以降このフレームの currentFrame()/fboTextureId() はその1枚を指す。
    // 戻り値: 今回新しい1枚を受け取ったら true（FlowPreload の準備完了判定用）
    bool latchAsyncFrame();
    // 非同期破棄（free → terminate_destroy）がワーカーで完了した後に
    // GUIスレッドで呼ばれるフック（OLEドロップターゲット修復用。MpvTeardown が設定）。
    static void setAfterAsyncTeardownHook(std::function<void()> hook);
    // 以下はワーカー/コールバックからGUIへ戻ってきた時の処理（内部用）
    void handleAsyncUpdateOnGui(quint64 frameSeq);
    // 2026-09-23: ワーカーでの mpv_render_context_create() が終わった通知。
    // これより前に open() が来ていた場合は、そこで預かったファイルをここで開く
    // （下の m_pendingOpen* と open() のコメント参照）。
    void handleAsyncRenderAttachedOnGui();
    static void runAfterAsyncTeardownHookOnGui();

signals:
    void _wakeup();
    // 2026-09-15 P2: ワーカーでの mpv_render_context_create() が失敗した
    void asyncGlInitFailed();

private slots:
    void processMpvEvents();

private:
    void handleMpvEvent(void *event);
    // 2026-09-16: GUIスレッドから mpv への同期呼び出しを無くす（書き込み）。
    // 非同期で積むだけ。失敗は SET_PROPERTY_REPLY でログに出す。
    void setPropertyStringAsync(const char *name, const QByteArray &value);
    void applyAsyncRenderGate();
    void handlePropertyChange(void *prop);
    QString mpvPropertyString(const char *name) const;
    QString mpvPropertyFlagString(const char *name) const;
    void maybeHandleHwdecDecodeError(const char *prefix, const char *text);
    void scheduleDashSeekWatchdog(qint64 targetMs, int token, int retriesLeft);
    void checkDashSeekWatchdog(qint64 targetMs, int token, int retriesLeft);
    // 2026-07-12: seek()/seekFast()/DASHシーク詰まり監視の再試行、全ての
    // 実際のmpv seekコマンド発行を担う下請け関数。m_dashSeekWatchdogToken
    // には一切触れない（詳細はMpvPlayer.cppのseek()コメント参照）。
    void seekCommand(qint64 posMs, const char *flags);

    // mpv_handle* / mpv_render_context* を void* で保持
    // .cpp 内でのみキャストする
    void *m_mpv       = nullptr; // レンダーコンテキストは m_async 側（ワーカー所有）

    PbInternalState m_internalState {PbInternalState::Idle};
    qint64  m_position  {0};
    qint64  m_duration  {0};
    QSize   m_videoSize;
    float   m_volume    {1.0f};

    GLuint  m_fbo         {0};
    GLuint  m_fboColorTex {0};
    int     m_fboW        {0};
    int     m_fboH        {0};

    bool    m_glInitialized    {false};
    bool    m_firstFrameEmitted{false};
    bool    m_lowCacheMode     {false};
    QStringList m_httpHeaders;

    // 2026-07-08: 直近のopen()がdisableVideoTrack=trueで開かれたかどうか。
    // vid=noの場合、映像フレームが一切デコードされないため、
    // firstFrameReady()の本来の発火元（initializeGl()内のrender
    // update callback、実際に映像フレームが描画された時のみ呼ばれる）が
    // 永久に発火しない。これによりBubbleControllerがLoading状態のまま
    // 固まる不具合（一時停止アイドル解放→wakeUp()での再オープン時に
    // 実際に発生）の対策として、FILE_LOADED時点で代わりに
    // firstFrameReady()を合成発火させるために使う。
    bool    m_videoTrackDisabled {false};

    // AudioOnly モード中かどうか（vid=no を送った後 true になる）
    bool    m_videoSuppressed  {false};

    // h264「Missing reference picture」検出によるソフトウェアデコード
    // 自動フォールバックを、1ファイルにつき一度だけ行うためのフラグ。
    // open() のたびにリセットする。
    bool    m_hwdecFallbackTriggered {false};

    // 2026-07-03: DASH（audio-file=あり）かつ再生位置指定(start=)ありで
    // open()した場合、loadfileに両方を同時に渡すとmpv/ffmpegのデマルチ
    // プレクサがフリーズし一度もフレームを描画できなくなる不具合が
    // 実機ログで確認された。この場合はstart=をloadfileから外し、
    // FILE_LOADED後に改めてseek()する（-1 = 保留中のseekなし）。
    qint64  m_pendingSeekAfterLoadMs {-1};

    // 2026-07-04: 直近のopen()がDASH（audio-file=あり=映像/音声別URL）で
    // 開かれたかどうか。seekWithStallWatchdog()が、DASH特有のデマルチ
    // プレクサ詰まり対策（seekFast()+自動リトライ監視）を適用すべきか、
    // それとも通常の精密seek()で十分か（＝5秒スキップ等の体感精度を
    // 犠牲にしない）を判断するために使う。open()のたびに更新する。
    bool    m_isDash {false};

    // 2026-07-03: seekWithStallWatchdog()用の状態。m_dashSeekTargetMsは
    // 「現在監視中のシーク目標位置」(-1=監視なし/解決済み)。
    // m_dashSeekWatchdogTokenはopen()やシークのやり直しのたびに
    // インクリメントし、古いQTimer::singleShotコールバックが別ファイル/
    // 別シークに対して誤発火しないようにするための世代カウンタ。
    qint64  m_dashSeekTargetMs        {-1};
    int     m_dashSeekWatchdogToken   {0};

    // 2026-07-13追記: checkDashSeekWatchdog()の誤判定対策用。詳細はcppの
    // コメント参照。「前回チェック時点でのm_position」を覚えておき、
    // 今回までの間に位置が実際に進んでいるかどうかで「本当に詰まって
    // いるか」を判定する。-1 = まだ一度もチェックしていない（今回が
    // 監視開始後の初回チェック）。
    qint64  m_dashSeekLastObservedPos {-1};

    // 2026-09-12: mpvの常時垂れ流し警告の間引き。
    // 画面外／非表示のバブルを描画していない間、mpvは
    // "mpv_render_context_render() not being called or stuck." を延々と出す。
    // 本体の設計どおりの状態（描いていないだけ）なので実害はないが、
    // ログが埋まって他の調査ができなくなるため、初回だけ出して以後は
    // 一定間隔で件数だけを報告する。
    QElapsedTimer m_noisyLogClock;

    // 2026-09-15 P2: 非同期プレイヤーの共有状態（ワーカーのジョブも保持する）。
    // nullptr ならGL未初期化（P7で同期経路は撤去）。
    std::shared_ptr<MpvAsyncRenderSlot> m_async;
    quint64 m_asyncFrontSerial {0};
    bool    m_asyncGateAllowed {false}; // 呼び出し側の判定（m_videoSuppressed とは別に保持）
    quint64 m_asyncOpenSeq {0};     // open() 時点のフレーム通し番号（firstFrameReady 判定用）
    // ---- 2026-09-23: レンダーコンテキストが出来るまで open() を預かる ----
    // mpv はファイルを開いた瞬間に映像出力（vo=libmpv）を初期化する。その時点で
    // レンダーコンテキストがまだ無いと "No render context set" → そのファイルの
    // 映像トラックが落とされ、音声だけが再生される（実機で発生。後から
    // コンテキストが出来ても、そのファイルのままでは映像は戻らない）。
    // コンテキスト生成はワーカースレッドへ投げていて数ms〜数十msかかるので、
    // 出来るまでの open() はここへ預け、通知が来てから実行する。
    bool    m_pendingOpenValid {false};
    QString m_pendingOpenUrl;
    QString m_pendingOpenAudioUrl;
    qint64  m_pendingOpenStartMs {0};
    bool    m_pendingOpenDisableVideo {false};
    void    flushPendingOpen();
    int           m_noisyLogSuppressed {0};

    // 2026-09-16: GUIスレッドから mpv への同期呼び出しを無くす（読み取り）。
    // 診断ログ用の値は mpv_get_property_async で取り寄せて手元に置き、
    // mpvPropertyString() はその控えを返す（取り寄せた時点からの経過msを併記）。
    struct PropMirror { QString value; qint64 atMs {-1}; bool pending {false}; };
    mutable QHash<QByteArray, PropMirror> m_propMirror;
    mutable QElapsedTimer m_propMirrorClock;
    QHash<quint64, QByteArray> m_setReplyNames;
    quint64 m_setReplySeq {0};
    // hwdec 自動フォールバック判定用（observe で更新。同期読み取りの代わり）
    QString m_hwdecCurrent;
    QString mirroredProperty(const char *name, int format) const;
    void handlePropertyReply(void *event);
};
