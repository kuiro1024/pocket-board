#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>

// ---- GpuProfiler（2026-10-12）--------------------------------------------
// キャンバスの1フレームを「区間」に分けて、GPU時間と描画回数を測る計測用クラス。
// Shift+P で CanvasWidget の左上に表示する（md/ のレイヤー合成の計画の土台）。
//
// 【測り方】区間の切り替わりごとに GL_TIMESTAMP のクエリを1つ打つだけ
// （enter(区間)）。隣り合うタイムスタンプの差を、前の区間の時間として足す。
// バブルごとに「影→ガラス→中身→ガラス」と区間が行き来しても、それぞれの
// 区間へ正しく積み上がる。結果は数フレーム後に「届いていれば」読むだけで、
// GPUを待たない（計測そのものでフレームを止めない）。
//
// 描画回数は Renderer / GlassHighlightEffect / BackdropBlur の glDraw* の直前で
// countDraw() を呼んで数え、区間の切り替わりで前の区間へ振り分ける。
//
// 表示していない間も計測は止めない（1フレームに数十個のクエリだけなので軽い）。
// 止めたい場合は setEnabled(false)。
class GpuProfiler : protected QOpenGLFunctions_3_3_Core
{
public:
    enum class Section {
        Background = 0, // 背景・グリッド・フロー背景など、バブルより前
        Item,           // バブル本体（枠・中身・名前・リサイズの印。動画の描画も含む）
        AmbientGlow,    // 動画の色の光（裾・辺・十字の筋）
        Shadow,         // アンビエントシャドウ・接触影
        Refraction,     // 背景キャプチャ・屈折プリパス（グミは縮小）
        Glass,          // ガラスのライティング（グミの中身の縮小を含む）
        Overlay,        // バブルより後（選択枠・強グローなど）
        Backdrop,       // UIの背後ぼかし（島など）
        Count
    };

    static const char *sectionName(Section s);

    bool initialize();
    void release();
    void setEnabled(bool on) { m_enabled = on; }
    bool isEnabled() const { return m_enabled; }

    void beginFrame();
    void enter(Section s);
    void endFrame();
    void countBubble() { ++m_frameBubbles; }

    // 描画1回ごとに呼ぶ（どこからでも呼べるよう static）。
    static void countDraw() { ++s_drawCalls; }

    // 直近の集計（約0.5秒ごとに更新）。表示用の文字列。
    QStringList summaryLines() const { return m_summary; }

private:
    void collect(int slot);
    void publish();

    static constexpr int kSlots = 4;          // 何フレーム遅れで結果を読むか
    static constexpr qint64 kWindowMs = 500;  // 集計の窓
    struct Slot {
        QVector<GLuint> queries;
        QVector<int> sections;
        int count = 0;
        bool pending = false;
    };
    std::array<Slot, kSlots> m_slots;
    int m_frame = 0;
    int m_current = -1;
    Section m_section = Section::Background;
    bool m_inFrame = false;
    bool m_initialized = false;
    bool m_enabled = true;

    static inline quint64 s_drawCalls = 0;
    quint64 m_drawMark = 0;
    std::array<quint64, int(Section::Count)> m_frameDraws {};
    int m_frameBubbles = 0;
    QElapsedTimer m_cpuTimer;

    // 窓の中の合計
    std::array<double, int(Section::Count)> m_sumGpuMs {};
    std::array<quint64, int(Section::Count)> m_sumDraws {};
    int m_gpuFrames = 0;      // GPU結果を読めたフレーム数
    int m_cpuFrames = 0;      // 描いたフレーム数
    double m_sumCpuMs = 0.0;
    quint64 m_sumBubbles = 0;
    QElapsedTimer m_window;
    QStringList m_summary;
};
