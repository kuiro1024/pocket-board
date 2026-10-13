#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QHash>
#include <QRectF>
#include <QSizeF>
#include <functional>

class Renderer;

// ---- BubbleLayerCache（2026-10-12）----------------------------------------
// バブルごとの「動かない下地」（アンビエントシャドウ・接触影・枠・リサイズの印）を
// 1枚のテクスチャへ焼き付けておき、毎フレームはそれを1回貼るだけにする。
// レイヤー合成の第1段階（GPU計測で、描画回数の大半がバブル本体の枠まわりだった）。
//
// 【描き直す条件】
//   ・位置が変わるだけ（パン・移動）… 描き直さない。貼る場所を変えるだけ
//   ・大きさが変わる（ズーム・リサイズ）… 変わっている間はその場で描き（拡大縮小して
//     貼ると粗が目立った）、止まってから kSettleMs 経ったら新しい大きさで焼き直す
//   ・見た目が変わる（スタイル・品質・グミ等）… 呼び出し側が渡す contentKey が変わる
// 1フレームに焼き直すのは kMaxBakesPerFrame 個まで（ズーム直後に一斉に焼かない）。
//
// 【使わない場面】は呼び出し側（CanvasWidget）が判定して draw() を呼ばない:
// 選択中（アクティブの枠は点滅する）・屈折中・融合中・フェード中・全画面中など。
class BubbleLayerCache : protected QOpenGLFunctions_3_3_Core
{
public:
    enum class Result {
        Drawn,      // 焼いた絵をそのまま貼った（必要なら今焼いた）
        Stretched,  // （未使用。2026-10-12 以降、大きさが変わっている間はその場で描く）
        NotCached   // 使えなかった。呼び出し側がいつも通り描く
    };

    bool initialize();
    void release();
    void setEnabled(bool on) { m_enabled = on; }
    bool isEnabled() const { return m_enabled; }

    void beginFrame();
    void endFrame();
    void forget(const void *key);

    // layerRect: 焼き付ける範囲（画面の論理座標。影の広がりまで含めた矩形）
    // contentKey: 見た目を決める値のハッシュ（大きさ・位置は含めない）
    // bake: 画面座標のまま下地を描く関数（Renderer は焼き付け先へ向けてある）
    Result draw(Renderer &r, const void *key, const QRectF &layerRect,
                quint64 contentKey, qreal dpr, const std::function<void()> &bake);

    // このフレームで拡大縮小のまま貼ったものがあるか（止まった後に焼き直すため、
    // 呼び出し側は少し後に描き直しを予約する）
    bool needsSharpenFrame() const { return m_stretchedThisFrame > 0; }

    int entryCount() const { return m_entries.size(); }
    qint64 bytes() const { return m_bytes; }
    int bakesThisFrame() const { return m_bakesThisFrame; }
    int hitsThisFrame() const { return m_hitsThisFrame; }
    int stretchedThisFrame() const { return m_stretchedThisFrame; }

private:
    struct Entry {
        GLuint tex = 0, fbo = 0;
        int texW = 0, texH = 0;
        QSizeF bakedSize;      // 焼いたときの layerRect の大きさ（論理px）
        QSizeF lastSeenSize;   // 直近のフレームで見た大きさ
        quint64 contentKey = 0;
        qint64 sizeChangedMs = 0;
        quint64 lastFrame = 0;
        bool valid = false;
    };
    bool bake(Renderer &r, Entry &e, const QRectF &layerRect, int texW, int texH,
              quint64 contentKey, const std::function<void()> &bakeFn);
    void freeEntry(Entry &e);

    static constexpr qint64 kSettleMs = 150;          // 大きさが止まってから焼き直すまで
    static constexpr int kMaxBakesPerFrame = 6;
    static constexpr qint64 kMaxTexturePixels = 2500000; // これより大きい下地は焼かない
    static constexpr qint64 kMaxBytes = 160ll * 1024 * 1024;
    static constexpr quint64 kEvictAfterFrames = 300; // 見えなくなって約5秒で捨てる

    QHash<const void *, Entry> m_entries;
    qint64 m_bytes = 0;
    quint64 m_frame = 0;
    int m_bakesThisFrame = 0;
    int m_hitsThisFrame = 0;
    int m_stretchedThisFrame = 0;
    bool m_initialized = false;
    bool m_enabled = true;
    qint64 nowMs() const;
};
