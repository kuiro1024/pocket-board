#include "GpuProfiler.h"

const char *GpuProfiler::sectionName(Section s)
{
    switch (s) {
    case Section::Background:  return "背景";
    case Section::Item:        return "バブル本体";
    case Section::AmbientGlow: return "動画の色の光";
    case Section::Shadow:      return "影";
    case Section::Refraction:  return "屈折・キャプチャ";
    case Section::Glass:       return "ガラス";
    case Section::Overlay:     return "選択枠など";
    case Section::Backdrop:    return "UIのぼかし";
    case Section::Count:       break;
    }
    return "?";
}

bool GpuProfiler::initialize()
{
    if (m_initialized)
        return true;
    if (!initializeOpenGLFunctions())
        return false;
    m_initialized = true;
    m_window.start();
    return true;
}

void GpuProfiler::release()
{
    if (!m_initialized)
        return;
    for (Slot &slot : m_slots) {
        if (!slot.queries.isEmpty())
            glDeleteQueries(slot.queries.size(), slot.queries.data());
        slot = Slot();
    }
    m_initialized = false;
    m_inFrame = false;
}

void GpuProfiler::beginFrame()
{
    if (!m_initialized || !m_enabled)
        return;
    m_current = m_frame % kSlots;
    ++m_frame;
    Slot &slot = m_slots[m_current];
    // この枠に前回のフレームの結果が残っていれば読む（届いていなければ捨てる）。
    if (slot.pending)
        collect(m_current);
    slot.count = 0;
    slot.pending = false;
    m_frameDraws.fill(0);
    m_frameBubbles = 0;
    m_drawMark = s_drawCalls;
    m_section = Section::Background;
    m_inFrame = true;
    m_cpuTimer.start();
    // 他の枠も、届いていれば先に読んでおく（遅れを小さくする）。
    for (int i = 0; i < kSlots; ++i)
        if (i != m_current && m_slots[i].pending)
            collect(i);
    enter(Section::Background);
}

void GpuProfiler::enter(Section s)
{
    if (!m_inFrame)
        return;
    // 描画回数は前の区間へ
    if (m_section != Section::Count)
        m_frameDraws[int(m_section)] += s_drawCalls - m_drawMark;
    m_drawMark = s_drawCalls;
    m_section = s;

    Slot &slot = m_slots[m_current];
    if (slot.count >= slot.queries.size()) {
        const int grow = qMax(32, slot.queries.size());
        const int old = slot.queries.size();
        slot.queries.resize(old + grow);
        slot.sections.resize(old + grow);
        glGenQueries(grow, slot.queries.data() + old);
    }
    glQueryCounter(slot.queries[slot.count], GL_TIMESTAMP);
    slot.sections[slot.count] = int(s);
    ++slot.count;
}

void GpuProfiler::endFrame()
{
    if (!m_inFrame)
        return;
    enter(Section::Count);   // 最後のタイムスタンプ（区間の終わり）
    m_slots[m_current].pending = true;
    m_inFrame = false;

    m_sumCpuMs += m_cpuTimer.nsecsElapsed() / 1.0e6;
    ++m_cpuFrames;
    for (int i = 0; i < int(Section::Count); ++i)
        m_sumDraws[i] += m_frameDraws[i];
    m_sumBubbles += m_frameBubbles;

    if (m_window.elapsed() >= kWindowMs)
        publish();
}

void GpuProfiler::collect(int index)
{
    Slot &slot = m_slots[index];
    slot.pending = false;
    if (slot.count < 2)
        return;
    GLint available = 0;
    glGetQueryObjectiv(slot.queries[slot.count - 1], GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available)
        return;   // まだ届いていない。待たずに捨てる
    GLuint64 prev = 0;
    glGetQueryObjectui64v(slot.queries[0], GL_QUERY_RESULT, &prev);
    for (int i = 1; i < slot.count; ++i) {
        GLuint64 t = 0;
        glGetQueryObjectui64v(slot.queries[i], GL_QUERY_RESULT, &t);
        const int sec = slot.sections[i - 1];
        if (sec >= 0 && sec < int(Section::Count) && t >= prev)
            m_sumGpuMs[sec] += double(t - prev) / 1.0e6;
        prev = t;
    }
    ++m_gpuFrames;
}

void GpuProfiler::publish()
{
    const double seconds = m_window.elapsed() / 1000.0;
    QStringList lines;
    double totalGpu = 0.0;
    quint64 totalDraws = 0;
    for (int i = 0; i < int(Section::Count); ++i) {
        totalGpu += m_sumGpuMs[i];
        totalDraws += m_sumDraws[i];
    }
    const double gf = qMax(1, m_gpuFrames);
    const double cf = qMax(1, m_cpuFrames);
    lines << QStringLiteral("GPU計測（Shift+Pで閉じる）  描画 %1回/秒")
                 .arg(m_cpuFrames / qMax(0.001, seconds), 0, 'f', 1);
    if (m_cpuFrames == 0) {
        lines << QStringLiteral("この0.5秒は描き直しなし");
    } else {
        lines << QStringLiteral("1フレーム  GPU %1ms  CPU %2ms  描画 %3回  バブル %4個")
                     .arg(totalGpu / gf, 0, 'f', 2)
                     .arg(m_sumCpuMs / cf, 0, 'f', 2)
                     .arg(double(totalDraws) / cf, 0, 'f', 0)
                     .arg(double(m_sumBubbles) / cf, 0, 'f', 0);
        for (int i = 0; i < int(Section::Count); ++i) {
            const double ms = m_sumGpuMs[i] / gf;
            const double draws = double(m_sumDraws[i]) / cf;
            if (ms < 0.005 && draws < 0.5)
                continue;
            lines << QStringLiteral("  %1  %2ms  %3回")
                         .arg(QString::fromUtf8(sectionName(Section(i))))
                         .arg(ms, 0, 'f', 2)
                         .arg(draws, 0, 'f', 0);
        }
    }
    m_summary = lines;

    m_sumGpuMs.fill(0.0);
    m_sumDraws.fill(0);
    m_gpuFrames = 0;
    m_cpuFrames = 0;
    m_sumCpuMs = 0.0;
    m_sumBubbles = 0;
    m_window.restart();
}
