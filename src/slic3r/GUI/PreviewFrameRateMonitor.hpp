#pragma once

#include <optional>
#include <string>

namespace Slic3r::GUI {

// Tells whether the sliced preview keeps drawing slower than min_fps while the user moves it.
// It is fed the render time of each frame drawn during an interaction, not the time between frames:
// the preview draws only on demand, so the spacing of frames follows the mouse and the FPS cap rather
// than the machine. Render time is summed into one-second buckets, and slow_seconds_needed slow buckets
// in a row report a slow preview.
class PreviewFrameRateMonitor
{
public:
    static constexpr double min_fps             = 15.;
    static constexpr int    slow_seconds_needed = 5;

    // Adds one frame's render time. Returns the average frame rate over the streak when the frame closes
    // the last slow bucket of a streak, and starts over; otherwise std::nullopt.
    std::optional<double> add_frame(double frame_ms)
    {
        m_bucket_ms += frame_ms;
        ++m_bucket_frames;
        if (m_bucket_ms < bucket_ms)
            return std::nullopt;

        // A closed bucket starts the next one from zero, so a long frame never spills into the next second.
        if (m_bucket_frames * 1000. < min_fps * m_bucket_ms) {
            ++m_slow_buckets;
            m_streak_ms += m_bucket_ms;
            m_streak_frames += m_bucket_frames;
        } else {
            m_slow_buckets  = 0;
            m_streak_ms     = 0.;
            m_streak_frames = 0;
        }
        m_bucket_ms     = 0.;
        m_bucket_frames = 0;

        if (m_slow_buckets < slow_seconds_needed)
            return std::nullopt;
        const double fps = m_streak_frames * 1000. / m_streak_ms;
        reset();
        return fps;
    }

    void reset() { *this = PreviewFrameRateMonitor(); }

private:
    static constexpr double bucket_ms = 1000.;

    double m_bucket_ms{0.};
    int    m_bucket_frames{0};
    int    m_slow_buckets{0};
    double m_streak_ms{0.};
    int    m_streak_frames{0};
};

// The drag mode to offer a preview that stays slow in `mode`: Shell only first, then Solid model, which
// draws the least. A mode that would draw the same toolpaths for this print is skipped.
inline std::optional<std::string> suggested_preview_drag_mode(const std::string& mode, bool shell_hides_infill, bool solid_usable)
{
    if (mode == "off" && shell_hides_infill)
        return std::string("shell");
    if ((mode == "off" || mode == "shell") && solid_usable)
        return std::string("solid");
    return std::nullopt;
}

} // namespace Slic3r::GUI
