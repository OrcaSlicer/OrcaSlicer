#include "libslic3r/libslic3r.h"
#include "FrameProfiler.hpp"

#include "3DScene.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <cstring>

namespace Slic3r {
namespace GUI {

void FrameProfiler::begin_frame()
{
    m_recording = nullptr;
    // GL 3.3 or ARB_timer_query
    if (glQueryCounter == nullptr)
        return;

    collect(false);
    Frame& frame = m_frames[m_next];
    // Every frame in flight still waits for the GPU: skip this one rather than stall.
    if (frame.pending)
        return;

    if (frame.queries[0] == 0)
        glsafe(::glGenQueries(GLsizei(frame.queries.size()), frame.queries.data()));
    glsafe(::glQueryCounter(frame.queries[0], GL_TIMESTAMP));
    frame.count = 0;
    frame.averaged = m_averaging;
    m_recording = &frame;
    m_last_mark = std::chrono::steady_clock::now();
}

void FrameProfiler::mark(const char* name)
{
    if (m_recording == nullptr || m_recording->count == MAX_SECTIONS)
        return;

    Frame& frame = *m_recording;
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    glsafe(::glQueryCounter(frame.queries[frame.count + 1], GL_TIMESTAMP));
    // Else the GPU time would include the CPU time spent until the driver flushes on its own.
    glsafe(::glFlush());
    frame.names[frame.count] = name;
    frame.cpu_ms[frame.count] = std::chrono::duration<double, std::milli>(now - m_last_mark).count();
    ++frame.count;
    m_last_mark = now;
}

void FrameProfiler::end_frame()
{
    if (m_recording == nullptr)
        return;

    m_recording->pending = m_recording->count > 0;
    m_recording = nullptr;
    m_next = (m_next + 1) % FRAMES_IN_FLIGHT;
}

void FrameProfiler::collect(bool wait)
{
    constexpr double SMOOTHING = 0.1;

    // Oldest first: a frame whose results are not in yet holds back the newer ones too.
    for (size_t i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        Frame& frame = m_frames[(m_next + i) % FRAMES_IN_FLIGHT];
        if (!frame.pending)
            continue;

        if (!wait) {
            GLint available = 0;
            glsafe(::glGetQueryObjectiv(frame.queries[frame.count], GL_QUERY_RESULT_AVAILABLE, &available));
            if (available == 0)
                break;
        }

        std::array<GLuint64, MAX_SECTIONS + 1> stamps{};
        for (size_t j = 0; j <= frame.count; ++j)
            glsafe(::glGetQueryObjectui64v(frame.queries[j], GL_QUERY_RESULT, &stamps[j]));
        frame.pending = false;

        const bool averaged = frame.averaged && m_averaging;
        if (averaged)
            ++m_averaged_frames;
        std::vector<Section> sections;
        sections.reserve(frame.count);
        for (size_t j = 0; j < frame.count; ++j) {
            Section section{ frame.names[j], frame.cpu_ms[j], stamps[j + 1] > stamps[j] ? double(stamps[j + 1] - stamps[j]) * 1e-6 : 0.0 };
            if (averaged) {
                auto sum = std::find_if(m_sums.begin(), m_sums.end(), [&section](const Section& s) { return std::strcmp(s.name, section.name) == 0; });
                if (sum == m_sums.end())
                    m_sums.push_back(section);
                else {
                    sum->cpu_ms += section.cpu_ms;
                    sum->gpu_ms += section.gpu_ms;
                }
            }
            for (const Section& prev : m_sections) {
                if (std::strcmp(prev.name, section.name) == 0) {
                    section.cpu_ms = prev.cpu_ms + SMOOTHING * (section.cpu_ms - prev.cpu_ms);
                    section.gpu_ms = prev.gpu_ms + SMOOTHING * (section.gpu_ms - prev.gpu_ms);
                    break;
                }
            }
            sections.push_back(section);
        }
        m_sections = std::move(sections);
    }
}

void FrameProfiler::start_averaging()
{
    m_averaging = true;
    m_sums.clear();
    m_averaged_frames = 0;
}

std::vector<FrameProfiler::Section> FrameProfiler::finish_averaging(bool wait)
{
    if (wait && glQueryCounter != nullptr)
        collect(true);
    std::vector<Section> averages = std::move(m_sums);
    for (Section& section : averages) {
        section.cpu_ms /= double(m_averaged_frames);
        section.gpu_ms /= double(m_averaged_frames);
    }
    m_averaging = false;
    m_sums.clear();
    m_averaged_frames = 0;
    return averages;
}

void FrameProfiler::reset()
{
    for (Frame& frame : m_frames) {
        if (frame.queries[0] != 0)
            glsafe(::glDeleteQueries(GLsizei(frame.queries.size()), frame.queries.data()));
        frame = Frame();
    }
    m_recording = nullptr;
    m_sections.clear();
    m_averaging = false;
    m_sums.clear();
    m_averaged_frames = 0;
}

} // namespace GUI
} // namespace Slic3r
