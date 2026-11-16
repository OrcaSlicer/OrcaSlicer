#pragma once
#include <mutex>
#include <utility>
namespace Slic3r::Gpu {
// Initialization, qualification, dispatch and staging cleanup share one lock.
class ContextLifecycle {
public:
    template<class Initialize> void initialize_once(Initialize&& initialize) {
        // Always enter call_once before the resource mutex. Dispatch and cleanup
        // acquire only the mutex; neither can destroy qualification buffers.
        std::call_once(m_once, [&] {
            auto lock = acquire();
            std::forward<Initialize>(initialize)();
        });
    }
    std::unique_lock<std::mutex> acquire() const {
        return std::unique_lock<std::mutex>(m_mutex);
    }
private:
    std::once_flag m_once;
    mutable std::mutex m_mutex;
};
}
