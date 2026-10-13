#pragma once
#include <QObject>
#include <QString>
#include <QSize>
#include <QStringList>

// GLuint の定義（OpenGL ヘッダへの依存を避けるため直接宣言）
// IPbPlayer は OpenGL を直接使わない。テクスチャ ID を不透明な整数として扱うだけ。
#ifndef GL_TYPES_DEFINED
#define GL_TYPES_DEFINED
typedef unsigned int GLuint;
#endif

// ============================================================
//  IPbPlayer.h
//
//  再生エンジンとの橋渡しとなる純粋仮想インターフェース。
//
//  【責務】
//    - open / play / pause / stop / seek の薄いラッパー
//    - 映像フレームを OpenGL テクスチャ (GLuint) として供給
//    - 内部状態変化をシグナルで通知するのみ
//
//  【非責務 — このクラスには書かない】
//    - BubbleState の管理       → BubbleController
//    - Sleep / wakeUp の判断    → BubbleController
//    - FlowMode との連携        → BubbleController
//    - OpenAL Soft の呼び出し   → IPbAudioSink 経由で将来実装
//
//  【実装クラス】
//    engine/MpvPlayer   — libmpv 実装（Phase 2）
//    engine/StubPlayer  — テスト用ダミー（任意）
//
//  【依存】
//    Qt: QObject / QString / QSize のみ
//    libmpv: 一切 include しない（MpvPlayer.h のみが include する）
// ============================================================

// ---- エンジン内部の低レベル状態 ----
// BubbleState と 1:1 ではない。
// BubbleController がこれを受け取り BubbleState へ変換する。
enum class PbInternalState
{
    Idle,       // open() 未呼び出し
    Opening,    // open() 呼び出し後・最初のフレーム前
    Decoding,   // フレームが届いている（≒ Playing）
    Paused,     // エンジン内部で一時停止中
    Buffering,  // ネットワーク待ち
    Ended,      // EOF
    Error,      // デコードエラー
};

// ---- フレーム情報 ----
// 将来 HDR メタデータ等を追加しやすいように構造体化。
struct PbVideoFrame
{
    GLuint textureId {0};   // 有効な GL テクスチャ ID（0 = フレームなし）
    int    width     {0};
    int    height    {0};
    qint64 pts       {0};   // プレゼンテーションタイムスタンプ（ms）

    bool isValid() const { return textureId != 0; }
};

// ============================================================
class IPbPlayer : public QObject
{
    Q_OBJECT

public:
    explicit IPbPlayer(QObject *parent = nullptr) : QObject(parent) {}
    ~IPbPlayer() override = default;

    // ---- 再生制御 API ----

    // URL を開いてデコード準備を開始する。
    // 準備完了時に firstFrameReady() を emit する。
    // startPositionMs > 0 の場合、対応する実装ではファイルを開くのと同時に
    // その位置から開始する。open() の後に別途 seek() を送る方式だと、
    // まだファイルが実際に開かれる前に seek がエンジンへ届いて無視される
    // ことがある（セッション復元直後の再生位置が反映されない不具合の原因
    // だった）ため、確実性のため open() 自体に開始位置を持たせられるように
    // している。
    // audioUrl: 空でなければ、streamUrl（映像トラック）とは別のURLから
    // 音声トラックを結合して再生する（DASH分離ストリーム対応）。
    // 対応しない実装では無視してよい（デフォルト引数のため既存実装は
    // そのままコンパイルできる）。
    // disableVideoTrack: true の場合、映像トラックを一切デコードしない
    // （mpv実装ではvid=noで開く）。2026-07-08: ローカル音声ファイルに
    // 埋め込まれたジャケット画像（ID3 APIC等）を、mpvが通常の動画
    // トラックとして扱いhwdec経由でデコードしようとして稀にクラッシュする
    // 不具合の対策。音声バブル（LocalAudio）は画面にはジャケット画像を
    // Windows Shell経由の別サムネイルで表示するため、mpv側での映像
    // デコードは元々不要。対応しない実装では無視してよい。
    virtual void open(const QString &streamUrl, qint64 startPositionMs = 0,
                      const QString &audioUrl = QString(),
                      bool disableVideoTrack = false) = 0;
    virtual void setHttpHeaders(const QStringList &headers) { Q_UNUSED(headers); }

    // 再生開始（open() 完了後に呼ぶ）
    virtual void play() = 0;

    // 一時停止（音声・映像ともに停止）
    virtual void pause() = 0;

    // 完全停止・リソース解放（URL は保持しない）
    // Sleep 遷移時に BubbleController が呼ぶ
    virtual void stop() = 0;

    // シーク（ミリ秒）。精密シーク（該当フレームまで正確に合わせる）を
    // 要求するため、実装次第では対象キーフレームより後ろまでデコードが
    // 必要になり、多数回連続で呼ぶと重くなることがある
    // （例: シークバーをドラッグ中に毎フレーム呼ぶ用途には不向き）。
    virtual void seek(qint64 posMs) = 0;

    // 高速シーク（ミリ秒）。近傍キーフレームへ飛ぶだけの粗いシークで、
    // 精度より速度を優先する。シークバーのドラッグ中など、短時間に
    // 連続して呼ばれる用途向け。ドラッグ終了時には改めて通常の seek() を
    // 1回呼び、正確な位置へ合わせ直すこと。
    virtual void seekFast(qint64 posMs) = 0;

    // 2026-07-03: 詰まったら自動リトライするシーク。DASH（映像/音声別URL）
    // ストリームに対して、ロード直後・特定タイミングで精密シークを送ると
    // mpv側のデマルチプレクサキャッシュスレッドが詰まり、二度とフレームが
    // 進まなくなる既知の不具合（一部の配信CDNのRangeリクエスト特性が原因、
    // MpvPlayer.cppのコメント参照）への対策。「seek()単体では危険」な
    // 場面（ロード直後の再開・FocusSleep復帰など）では、実装がこちらを
    // 持っていればこちらを使うこと。デフォルト実装は単純にseek()へ
    // フォールバックする（MpvPlayer以外はwatchdog機構を持たないため）。
    virtual void seekWithStallWatchdog(qint64 posMs) { seek(posMs); }

    // ----------------------------------------------------------------
    //  AudioOnly モード用 API
    //
    //  pauseVideo()  : 映像トラックのみ停止（vid=no）。
    //                  音声・再生位置は継続する。
    //                  BubbleController が AudioOnly 遷移時に呼ぶ。
    //
    //  resumeVideo() : 映像トラックを再開（vid=auto）。
    //                  現在の再生位置のまま映像が復帰する。
    //                  BubbleController が AudioOnly → Playing 遷移時に呼ぶ。
    //
    //  デフォルト実装は何もしない（stop() を持つ実装でも上書き不要）。
    //  MpvPlayer のみ有効な実装を持つ。
    // ----------------------------------------------------------------
    virtual void pauseVideo()  { /* デフォルト: no-op */ }
    virtual void resumeVideo() { /* デフォルト: no-op */ }
    virtual bool isVideoSuppressed() const { return false; }
    virtual void setLowCacheMode(bool enabled) { Q_UNUSED(enabled) }
    virtual void releaseVideoResources() {}

    // 再生速度（1.0 = 等倍）。動画全画面中の「長押しで2倍速」機能用。
    // デフォルト実装は何もしない（速度制御を持たない実装向け）。
    virtual void setSpeed(double speed) { Q_UNUSED(speed) }

    // ---- 情報取得 ----
    virtual PbInternalState internalState() const = 0;
    virtual qint64          position()      const = 0;  // ms
    virtual qint64          duration()      const = 0;  // ms
    virtual QSize           videoSize()     const = 0;
    virtual float           volume()        const = 0;  // 0.0–1.0
    virtual void            setVolume(float v)    = 0;

    // ---- フレーム取得 ----
    // 最新フレームのテクスチャ情報を返す。
    // テクスチャは IPbPlayer 実装クラスが管理する（呼び出し元は deleteTexture しない）。
    // paintGL() 内から呼ぶこと（OpenGL コンテキスト必須）。
    virtual PbVideoFrame currentFrame() const = 0;

    // ---- ループ制御 ----
    virtual void setLooping(bool loop) { Q_UNUSED(loop) }

    // ---- 将来: 音声シンク（Phase 3） ----
    // virtual void setAudioSink(IPbAudioSink *sink) = 0;

signals:
    // 最初のフレームが届いた → BubbleController が Loading→Ready へ遷移
    void firstFrameReady();

    // 新フレームが更新された（毎フレーム emit）→ CanvasWidget::update() のトリガー
    void frameUpdated();

    // 内部状態が変化した → BubbleController がマッピング
    void internalStateChanged(PbInternalState state);

    // 再生位置が変化した（ms）→ 再生バー更新
    void positionChanged(qint64 posMs);

    // デコードエラー → BubbleController が Error 遷移
    void errorOccurred(const QString &message);
};
