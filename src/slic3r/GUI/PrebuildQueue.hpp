#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "Lazy.hpp"

namespace Slic3r { namespace GUI {

// Holders to build at idle, ordered by prebuild_order(), run one slice at a time. The
// holders outlive the queue. Main thread only.
class PrebuildQueue
{
public:
    struct Slice
    {
        std::string name;                // the task that ran; empty when nothing was pending
        int         units{ 0 };          // units run
        long long   ms{ 0 };             // time spent
        bool        completed{ false };  // the task has no more units
        bool        remaining{ false };  // some task is still pending
    };

    // Lower order runs first, equal order in the order added.
    void add(LazyBase& task)
    {
        auto after = std::find_if(m_tasks.begin(), m_tasks.end(), [&](const LazyBase* t) { return t->prebuild_order() > task.prebuild_order(); });
        m_tasks.insert(after, &task);
    }

    void clear() { m_tasks.clear(); }

    bool pending() const
    {
        return std::any_of(m_tasks.begin(), m_tasks.end(), [](const LazyBase* t) { return t->pending(); });
    }

    // The task names in queue order, comma separated.
    std::string names() const
    {
        std::string out;
        for (const LazyBase* t : m_tasks)
            out += (out.empty() ? "" : ", ") + t->name();
        return out;
    }

    // Runs units of the first pending task until it completes, budget_ms of now_ms() have
    // passed, or interrupt() is true after a unit; on_unit gets the task name and each
    // unit's duration. A task with no work is passed over and stays in the queue, so one
    // whose work returns is pending again.
    Slice run_slice(int                                                       budget_ms,
                    const std::function<long long()>&                         now_ms,
                    const std::function<bool()>&                              interrupt,
                    const std::function<void(const std::string&, long long)>& on_unit = {})
    {
        Slice slice;
        auto  next = std::find_if(m_tasks.begin(), m_tasks.end(), [](const LazyBase* t) { return t->pending(); });
        if (next == m_tasks.end())
            return slice;
        // The pointer is copied out, since a unit may add tasks and reallocate m_tasks.
        LazyBase* const task    = *next;
        slice.name              = task->name();
        const long long started = now_ms();
        for (;;) {
            const long long unit_started = now_ms();
            const bool      more         = task->build_step();
            const long long now          = now_ms();
            ++slice.units;
            if (on_unit)
                on_unit(slice.name, now - unit_started);
            slice.ms = now - started;
            if (!more) {
                slice.completed = true;
                break;
            }
            if (slice.ms >= budget_ms || interrupt())
                break;
        }
        slice.remaining = pending();
        return slice;
    }

private:
    std::vector<LazyBase*> m_tasks;
};

}} // namespace Slic3r::GUI
