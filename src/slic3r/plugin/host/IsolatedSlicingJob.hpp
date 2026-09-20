#ifndef slic3r_IsolatedSlicingJob_hpp_
#define slic3r_IsolatedSlicingJob_hpp_

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {

class Print;

enum class IsolatedSlicingState {
    Ready,
    Running,
    Succeeded,
    Cancelled,
    Failed,
    Closed,
};

struct IsolatedSlicingSnapshotInfo {
    size_t           object_count { 0 };
    int              plate_index { 0 };
    std::vector<int> filament_maps;
    std::vector<int> filament_volume_maps;
    bool             bbl_vendor { false };
    bool             check_mixed_filament_compatibility { true };
    size_t           extruder_count { 0 };
    size_t           extruder_filament_config_count { 0 };
};

struct IsolatedPrintStatistics {
    std::string              estimated_normal_print_time;
    std::string              estimated_silent_print_time;
    double                   total_used_filament { 0.0 };
    double                   total_extruded_volume { 0.0 };
    double                   total_cost { 0.0 };
    int                      total_toolchanges { 0 };
    double                   total_weight { 0.0 };
    double                   total_wipe_tower_cost { 0.0 };
    double                   total_wipe_tower_filament { 0.0 };
    unsigned int             initial_tool { 0 };
    std::map<size_t, double> filament_stats;
};

struct IsolatedGCodeStatistics {
    unsigned int result_id { 0 };
    size_t       move_count { 0 };
    size_t       line_count { 0 };
    size_t       layer_count { 0 };
    size_t       filament_count { 0 };
    float        normal_time_seconds { 0.0f };
    float        silent_time_seconds { 0.0f };
    unsigned int total_filament_changes { 0 };
    unsigned int total_extruder_changes { 0 };
    float        total_travel_distance { 0.0f };
    unsigned int total_travel_moves { 0 };
};

struct IsolatedSlicingResult {
    IsolatedSlicingState              state { IsolatedSlicingState::Ready };
    std::string                       error;
    std::map<std::string, std::string> candidate;
    IsolatedPrintStatistics           print_statistics;
    IsolatedGCodeStatistics           gcode_statistics;
    std::vector<std::string>          warnings;
    std::string                       gcode_path;
    std::uintmax_t                    gcode_size { 0 };
};

// Internal owned snapshot. It is never bound to Python; the host factory fills it
// synchronously on the UI thread, then the job stores it as const data.
struct IsolatedSlicingBaseline {
    Model                                        model;
    DynamicPrintConfig                           config;
    DynamicPrintConfig                           plate_config;
    int                                          plate_index { 0 };
    Vec3d                                        plate_origin { Vec3d::Zero() };
    std::vector<int>                             filament_maps;
    std::vector<int>                             filament_volume_maps;
    bool                                         bbl_vendor { false };
    bool                                         check_mixed_filament_compatibility { true };
    std::vector<std::vector<DynamicPrintConfig>> extruder_filament_info;
};

class IsolatedSlicingJob
{
public:
    // Plugin-facing entry point. Capture is synchronous and rejects calls that
    // are not made by a registered plugin on Orca's UI thread.
    static std::shared_ptr<IsolatedSlicingJob> capture_live();

    // Internal construction seam used by focused host tests. The resulting job
    // has exactly the same ownership, validation and worker behavior as a live capture.
    static std::shared_ptr<IsolatedSlicingJob> create_owned(
        IsolatedSlicingBaseline baseline, std::string plugin_storage_root,
        std::string plugin_key = {});

    ~IsolatedSlicingJob();

    IsolatedSlicingJob(const IsolatedSlicingJob&)            = delete;
    IsolatedSlicingJob& operator=(const IsolatedSlicingJob&) = delete;

    std::map<std::string, std::string> apply_candidate(
        const std::map<std::string, std::string>& patch);
    std::map<std::string, std::string> candidate() const;
    // Creates a new single-use job over this job's immutable owned baseline.
    // The audited owner plugin is resolved natively; Python supplies no identity.
    std::shared_ptr<IsolatedSlicingJob> fork_from_baseline() const;
    void                 run();
    void                 cancel() noexcept;
    IsolatedSlicingState wait();
    IsolatedSlicingState state() const;
    IsolatedSlicingResult result() const;
    IsolatedSlicingSnapshotInfo snapshot() const;
    void close() noexcept;

private:
    struct Baseline;

    explicit IsolatedSlicingJob(std::shared_ptr<const Baseline> baseline);

    static std::shared_ptr<IsolatedSlicingJob> create_from_shared_baseline(
        std::shared_ptr<const Baseline> baseline, const std::string& plugin_key);

    void worker_main(DynamicPrintConfig candidate,
                     std::map<std::string, std::string> canonical_candidate);
    void join_worker() noexcept;
    void cleanup_owned_output() noexcept;

    const std::shared_ptr<const Baseline> m_baseline;

    mutable std::mutex m_mutex;
    std::mutex         m_join_mutex;
    std::mutex         m_close_mutex;
    std::thread        m_worker;
    std::shared_ptr<Print> m_active_print;
    std::atomic<bool>  m_cancel_requested { false };

    DynamicPrintConfig                    m_candidate;
    std::map<std::string, std::string>    m_canonical_candidate;
    IsolatedSlicingState                  m_state { IsolatedSlicingState::Ready };
    IsolatedSlicingResult                 m_result;
    std::string                           m_owned_temp_dir;
};

const char* isolated_slicing_state_name(IsolatedSlicingState state) noexcept;

} // namespace Slic3r

#endif // slic3r_IsolatedSlicingJob_hpp_
