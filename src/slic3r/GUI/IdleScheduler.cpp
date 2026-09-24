#include "IdleScheduler.hpp"

#include <chrono>

#include <boost/log/trivial.hpp>

#include "libslic3r/Utils.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Slic3r { namespace GUI {

namespace {

// The timer's period while waiting for the user to go quiet.
constexpr int tick_ms  = 250;
// Input-free time before a slice may start, about a double-click interval, so a user
// mid-gesture is left alone.
constexpr int quiet_ms = 500;
// Budget of one slice. The next slice is a timer message, so paint, timers and input
// queued meanwhile are handled first; a click waits at most a slice plus the unit that
// overran it.
constexpr int slice_ms = 40;
// On GTK a due timer runs ahead of repaints and posted events, so the next slice waits a few ms.
#ifdef __WXGTK__
constexpr int next_slice_ms = 5;
#else
constexpr int next_slice_ms = 0;
#endif

// True when unhandled keyboard, button, touch or pen input is queued; only Windows can ask.
bool input_pending()
{
#ifdef _WIN32
    // Windows synthesizes a mouse move whenever a window appears under the cursor, so those
    // do not count.
    return (GetQueueStatus(QS_INPUT & ~QS_MOUSEMOVE) >> 16) != 0;
#else
    return false;
#endif
}

} // namespace

IdleScheduler::IdleScheduler(std::function<int()> input_idle_ms) : m_input_idle_ms(std::move(input_idle_ms))
{
    m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
}

void IdleScheduler::start()
{
    if (!m_timer.IsRunning())
        m_timer.Start(tick_ms);
}

void IdleScheduler::stop()
{
    m_timer.Stop();
}

void IdleScheduler::tick()
{
    // A unit that pumps the event loop lets the timer fire inside its own slice.
    if (m_in_slice)
        return;
    if (!m_queue.pending()) {
        stop();
        return;
    }
    if (m_input_idle_ms() < quiet_ms || input_pending()) {
        start();
        return;
    }
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const char* const fn = __FUNCTION__;
    m_in_slice           = true;
    PrebuildQueue::Slice slice;
    {
        ScopeGuard in_slice([this] { m_in_slice = false; });
        slice = m_queue.run_slice(slice_ms, now_ms, input_pending, [fn](const std::string& name, long long ms) {
            BOOST_LOG_TRIVIAL(debug) << fn << ": " << name << ": unit took " << ms << " ms";
        });
    }
    if (slice.completed)
        BOOST_LOG_TRIVIAL(info) << fn << ": task complete: " << slice.name << ", " << slice.units << " unit(s) in " << slice.ms << " ms this slice";
    else
        BOOST_LOG_TRIVIAL(debug) << fn << ": " << slice.name << ": " << slice.units << " unit(s) in " << slice.ms << " ms, yielding";
    if (!slice.remaining)
        stop();
    else if (input_pending())
        m_timer.Start(tick_ms);
    else
        m_timer.StartOnce(next_slice_ms);
}

}} // namespace Slic3r::GUI
