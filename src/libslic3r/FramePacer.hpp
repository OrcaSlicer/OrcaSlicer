#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace Slic3r {

struct FrameSchedule
{
    // Discard every queued frame but the newest and show that one instead.
    bool                                  skip_to_newest = false;
    std::chrono::steady_clock::time_point show_at;
};

// Decides when a live stream's display shows each frame it takes from a DropOldestQueue of
// `queue_capacity` frames. Frames are paced one period apart at the stream's frame rate, so the
// picture runs smoothly even when frames arrive in bursts, while the delay stays bounded: a late
// frame is shown at once and restarts the schedule, a frame with more than half the queue waiting
// behind it is shown at once until the backlog is down to half, and after a stall longer than the
// queue's worth the display skips to the newest frame. A queue of one or two frames (a low frame
// rate) is too short to pace from without holding the picture back a whole period, so those frames
// are shown as they arrive.
class FramePacer
{
public:
    using Clock = std::chrono::steady_clock;

    FramePacer(int frame_rate, size_t queue_capacity)
        : m_period(std::chrono::microseconds(1000000 / std::max(frame_rate, 1)))
        , m_max_slack(m_period * static_cast<long long>(std::max<size_t>(queue_capacity, 1)))
        , m_backlog(queue_capacity / 2)
        , m_paced(queue_capacity > 2)
    {}

    // The stream paused: the next frame starts a fresh schedule.
    void reset() { m_scheduled = false; }

    // Schedules a frame just taken from the queue, with `waiting` frames still queued behind it.
    FrameSchedule schedule(Clock::time_point now, size_t waiting)
    {
        FrameSchedule result;
        if (!m_scheduled || now - m_next > m_max_slack) {
            result.skip_to_newest = true;
            m_next                = now;
            m_scheduled           = true;
        } else if (!m_paced || now > m_next || waiting > m_backlog) {
            m_next = now;
        }
        result.show_at = m_next;
        m_next += m_period;
        return result;
    }

private:
    Clock::duration   m_period;
    Clock::duration   m_max_slack;
    size_t            m_backlog;
    bool              m_paced;
    Clock::time_point m_next;
    bool              m_scheduled = false;
};

} // namespace Slic3r
