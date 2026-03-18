#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r::portability::app {

struct SliceRequest {
    std::string model_name;
    std::string quality_preset;
    int         infill_percent {15};
    bool        supports_enabled {false};
};

struct SliceProgress {
    double      progress_percent {0.0};
    std::string message;
};

struct SliceSuccess {
    int         layer_count {0};
    int         toolpath_count {0};
    int         estimated_print_time_seconds {0};
    std::string status_text;
    std::string detail_text;
    std::string diagnostic_log;
};

struct SliceFailure {
    std::string user_message;
    std::string diagnostic_log;
};

class SlicingOrchestrator
{
public:
    using ProgressCallback   = std::function<void(const SliceProgress&)>;
    using CompletionCallback = std::function<void(std::unique_ptr<SliceSuccess>, std::unique_ptr<SliceFailure>, bool cancelled)>;

    ~SlicingOrchestrator();

    bool start(const SliceRequest& request, ProgressCallback progress_callback, CompletionCallback completion_callback);
    void cancel();
    bool is_running() const;

private:
    mutable std::mutex m_mutex;
    std::thread        m_worker;
    std::atomic_bool   m_cancel_requested {false};
    std::atomic_bool   m_running {false};
};

} // namespace Slic3r::portability::app
