#ifndef SLIC3R_BACKGROUND_TASK_HPP
#define SLIC3R_BACKGROUND_TASK_HPP

#include <future>
#include <functional>
#include <string>

namespace Slic3r {

// Portable contracts for background task orchestration. These interfaces avoid
// assumptions about a specific GUI framework or event-loop implementation.
class TaskProgressSink {
public:
    virtual ~TaskProgressSink() = default;

    virtual void clear_progress() = 0;
    virtual void set_progress(int value) = 0;
    virtual void set_status_text(const char *message_utf8) = 0;
    virtual void show_error_info(const std::string &message,
                                 int                code,
                                 const std::string &description,
                                 const std::string &extra) = 0;
};

class TaskCancellation {
public:
    virtual ~TaskCancellation() = default;
    virtual bool was_canceled() const = 0;
};

class MainThreadExecutor {
public:
    virtual ~MainThreadExecutor() = default;

    // Execute a callback on whichever thread/context owns foreground state.
    // Implementations may dispatch to a GUI loop, a CLI pump, or test harness.
    virtual std::future<void> call_on_main_thread(std::function<void()> fn) = 0;
};

} // namespace Slic3r

#endif // SLIC3R_BACKGROUND_TASK_HPP
