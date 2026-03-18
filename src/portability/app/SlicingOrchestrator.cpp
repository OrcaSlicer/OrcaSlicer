#include "SlicingOrchestrator.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

namespace Slic3r::portability::app {

namespace {
std::string normalize_model_name(const std::string& model_name)
{
    if (model_name.empty())
        return "Current project";
    return model_name;
}
} // namespace

SlicingOrchestrator::~SlicingOrchestrator()
{
    cancel();
    if (m_worker.joinable())
        m_worker.join();
}

bool SlicingOrchestrator::start(const SliceRequest& request, ProgressCallback progress_callback, CompletionCallback completion_callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running.load())
        return false;

    if (m_worker.joinable())
        m_worker.join();

    m_cancel_requested = false;
    m_running          = true;

    m_worker = std::thread([this, request, progress_callback = std::move(progress_callback), completion_callback = std::move(completion_callback)]() mutable {
        const std::string model_name = normalize_model_name(request.model_name);

        if (request.quality_preset.empty()) {
            auto failure            = std::make_unique<SliceFailure>();
            failure->user_message   = "Unable to slice because no quality preset is selected.";
            failure->diagnostic_log = "slice.validation_error: missing quality preset";
            m_running               = false;
            completion_callback(nullptr, std::move(failure), false);
            return;
        }

        const int total_steps = 8;
        for (int step = 1; step <= total_steps; ++step) {
            if (m_cancel_requested.load()) {
                m_running = false;
                completion_callback(nullptr, nullptr, true);
                return;
            }

            SliceProgress progress;
            progress.progress_percent = (100.0 * step) / total_steps;
            switch (step) {
            case 1: progress.message = "Loading model geometry"; break;
            case 2: progress.message = "Preparing print regions"; break;
            case 3: progress.message = "Generating perimeters"; break;
            case 4: progress.message = "Computing infill"; break;
            case 5: progress.message = request.supports_enabled ? "Generating supports" : "Skipping supports"; break;
            case 6: progress.message = "Optimizing travel moves"; break;
            case 7: progress.message = "Estimating print duration"; break;
            default: progress.message = "Finalizing slice output"; break;
            }

            progress_callback(progress);
            std::this_thread::sleep_for(std::chrono::milliseconds(240));
        }

        auto success                      = std::make_unique<SliceSuccess>();
        success->layer_count              = std::max(30, 90 + request.infill_percent * 2);
        success->toolpath_count           = std::max(120, 1100 + request.infill_percent * 18 + (request.supports_enabled ? 220 : 0));
        success->estimated_print_time_seconds = std::max(9000, 9800 + request.infill_percent * 60 + (request.supports_enabled ? 1200 : 0));
        success->status_text              = "Slice complete";
        success->detail_text              = request.quality_preset + " • " + std::to_string(request.infill_percent) + "% infill • " + std::to_string(success->layer_count) + " layers";

        std::ostringstream log_stream;
        log_stream << "slice.start model='" << model_name << "' preset='" << request.quality_preset << "' infill=" << request.infill_percent
                   << " supports=" << (request.supports_enabled ? "on" : "off") << "\n";
        log_stream << "slice.output layers=" << success->layer_count << " toolpaths=" << success->toolpath_count
                   << " print_time_seconds=" << success->estimated_print_time_seconds;
        success->diagnostic_log = log_stream.str();

        m_running = false;
        completion_callback(std::move(success), nullptr, false);
    });

    return true;
}

void SlicingOrchestrator::cancel() { m_cancel_requested = true; }

bool SlicingOrchestrator::is_running() const { return m_running.load(); }

} // namespace Slic3r::portability::app
