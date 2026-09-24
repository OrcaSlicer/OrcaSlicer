#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <vector>

namespace Slic3r {
namespace GUI {

// CPU and GPU time spent in the named sections of a frame, shown under the FPS overlay. A section
// runs from the previous mark to the one naming it. GPU times are read back a few frames late, so
// profiling never waits on the GPU.
class FrameProfiler
{
public:
    struct Section
    {
        const char* name{ nullptr };
        double cpu_ms{ 0.0 };
        double gpu_ms{ 0.0 };
    };

    void begin_frame();
    void mark(const char* name);
    void end_frame();
    // The sections of the last frame read back, smoothed over the previous ones.
    const std::vector<Section>& sections() const { return m_sections; }
    // Frees the queries, with the context current.
    void reset();

private:
    static constexpr size_t FRAMES_IN_FLIGHT = 4;
    static constexpr size_t MAX_SECTIONS = 32;

    struct Frame
    {
        std::array<unsigned int, MAX_SECTIONS + 1> queries{};
        std::array<const char*, MAX_SECTIONS> names{};
        std::array<double, MAX_SECTIONS> cpu_ms{};
        size_t count{ 0 };
        bool pending{ false };
    };

    void collect();

    std::array<Frame, FRAMES_IN_FLIGHT> m_frames;
    Frame* m_recording{ nullptr };
    size_t m_next{ 0 };
    std::chrono::steady_clock::time_point m_last_mark;
    std::vector<Section> m_sections;
};

} // namespace GUI
} // namespace Slic3r
