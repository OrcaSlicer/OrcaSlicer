#pragma once

#include <functional>
#include <string>

#include <wx/event.h>
#include <wx/timer.h>

#include "PrebuildQueue.hpp"

namespace Slic3r { namespace GUI {

// Runs the queue's units while the user is idle. Once the user has been idle long enough,
// slices run one per timer message until the work is done or input arrives, so the event
// loop handles what it has between slices. Main thread only.
class IdleScheduler
{
public:
    // input_idle_ms reports how long ago the user last touched the mouse or keyboard.
    explicit IdleScheduler(std::function<int()> input_idle_ms);

    void add(LazyBase& task) { m_queue.add(task); }
    void clear() { m_queue.clear(); }
    // The task names in queue order, for a log line.
    std::string names() const { return m_queue.names(); }

    // Arms the timer; it stops itself once no task is pending, so call again when a task
    // appears.
    void start();
    void stop();

private:
    void tick();

    PrebuildQueue        m_queue;
    wxTimer              m_timer;
    std::function<int()> m_input_idle_ms;
    bool                 m_in_slice{ false };
};

}} // namespace Slic3r::GUI
