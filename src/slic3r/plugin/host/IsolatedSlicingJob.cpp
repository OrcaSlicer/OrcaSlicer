#include "IsolatedSlicingJob.hpp"

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SlicingAdmission.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <wx/app.h>
#include <wx/thread.h>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Slic3r {
namespace {

namespace fs = boost::filesystem;

const std::unordered_set<std::string>& rejected_candidate_keys()
{
    // These options either enter an explicitly forbidden extension seam, change
    // the process-global coordinate scale through Plater::set_bed_shape(), or
    // switch away from the only technology supported by the v1 executor.
    static const std::unordered_set<std::string> keys {
        "post_process",
        "slicing_pipeline_plugin",
        "plugins",
        "printer_technology",
        "printable_area",
        "bed_exclude_area",
        "wrapping_exclude_area",
        "printable_height",
        "extruder_printable_area",
        "extruder_printable_height",
        "bed_custom_texture",
        "bed_custom_model",
    };
    return keys;
}

bool has_nonempty_strings(const DynamicPrintConfig& config, const char* key)
{
    const auto* values = config.option<ConfigOptionStrings>(key);
    return values != nullptr && !values->values.empty();
}

std::string validation_error_text(const std::map<std::string, std::string>& errors)
{
    if (errors.empty())
        return {};

    std::string out = "Invalid complete FFF configuration";
    for (const auto& [key, value] : errors) {
        out += "; ";
        out += key;
        out += ": ";
        out += value;
    }
    return out;
}

void validate_baseline(IsolatedSlicingBaseline& baseline)
{
    if (baseline.config.option("printer_technology") == nullptr ||
        baseline.config.opt_enum<PrinterTechnology>("printer_technology") != ptFFF)
        throw std::runtime_error("Isolated slicing v1 supports FFF technology only");
    if (has_nonempty_strings(baseline.config, "slicing_pipeline_plugin"))
        throw std::runtime_error("Isolated slicing v1 rejects slicing pipeline plugins");
    if (has_nonempty_strings(baseline.config, "post_process"))
        throw std::runtime_error("Isolated slicing v1 rejects post_process commands");

    const std::string error = validation_error_text(baseline.config.validate());
    if (!error.empty())
        throw std::runtime_error(error);
}

fs::path normalized_storage_root(const std::string& storage_root)
{
    if (storage_root.empty())
        throw std::runtime_error("Plugin storage root is empty");

    boost::system::error_code ec;
    fs::path root = fs::absolute(fs::path(storage_root), ec).lexically_normal();
    if (ec)
        throw std::runtime_error("Unable to resolve plugin storage root: " + ec.message());
    fs::create_directories(root, ec);
    if (ec)
        throw std::runtime_error("Unable to create plugin storage root: " + ec.message());
    root = fs::weakly_canonical(root, ec);
    if (ec)
        throw std::runtime_error("Unable to canonicalize plugin storage root: " + ec.message());
    return root;
}

fs::path create_owned_temp_dir(const fs::path& storage_root)
{
    boost::system::error_code ec;
    const fs::path parent = storage_root / "isolated_slicing";
    fs::create_directories(parent, ec);
    if (ec)
        throw std::runtime_error("Unable to create isolated slicing storage: " + ec.message());

    for (unsigned int attempt = 0; attempt < 32; ++attempt) {
        const fs::path candidate = parent / fs::unique_path("job-%%%%-%%%%-%%%%", ec);
        if (ec)
            throw std::runtime_error("Unable to generate isolated slicing directory: " + ec.message());
        if (!is_inside_allowed_root(candidate, storage_root))
            throw std::runtime_error("Generated isolated slicing directory escaped plugin storage");
        if (fs::create_directory(candidate, ec))
            return candidate;
        if (ec)
            throw std::runtime_error("Unable to create isolated slicing directory: " + ec.message());
    }
    throw std::runtime_error("Unable to allocate a unique isolated slicing directory");
}

void append_print_warnings(const Print& print, std::vector<std::string>& out)
{
    for (int step = 0; step < static_cast<int>(psCount); ++step) {
        const auto state = print.step_state_with_warnings(static_cast<PrintStep>(step));
        for (const PrintStateBase::Warning& warning : state.warnings)
            if (warning.current && !warning.message.empty())
                out.emplace_back(warning.message);
    }
    for (const PrintObject* object : print.objects()) {
        for (int step = 0; step < static_cast<int>(posCount); ++step) {
            const auto state = object->step_state_with_warnings(static_cast<PrintObjectStep>(step));
            for (const PrintStateBase::Warning& warning : state.warnings)
                if (warning.current && !warning.message.empty())
                    out.emplace_back(warning.message);
        }
    }
}

IsolatedPrintStatistics copy_print_statistics(const PrintStatistics& source)
{
    IsolatedPrintStatistics out;
    out.estimated_normal_print_time = source.estimated_normal_print_time;
    out.estimated_silent_print_time = source.estimated_silent_print_time;
    out.total_used_filament         = source.total_used_filament;
    out.total_extruded_volume       = source.total_extruded_volume;
    out.total_cost                  = source.total_cost;
    out.total_toolchanges           = source.total_toolchanges;
    out.total_weight                = source.total_weight;
    out.total_wipe_tower_cost       = source.total_wipe_tower_cost;
    out.total_wipe_tower_filament   = source.total_wipe_tower_filament;
    out.initial_tool                = source.initial_tool;
    out.filament_stats              = source.filament_stats;
    return out;
}

IsolatedGCodeStatistics copy_gcode_statistics(const GCodeProcessorResult& source)
{
    IsolatedGCodeStatistics out;
    out.result_id             = source.id;
    out.move_count            = source.moves.size();
    out.line_count            = source.lines_ends.size();
    out.filament_count        = source.filaments_count;
    out.normal_time_seconds   = source.print_statistics.modes[
        static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
    out.silent_time_seconds   = source.print_statistics.modes[
        static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time;
    out.total_filament_changes = source.print_statistics.total_filament_changes;
    out.total_extruder_changes = source.print_statistics.total_extruder_changes;
    out.total_travel_distance  = source.print_statistics.total_travel_distance;
    out.total_travel_moves     = source.print_statistics.total_travel_moves;

    unsigned int highest_layer = 0;
    bool have_layer = false;
    for (const GCodeProcessorResult::MoveVertex& move : source.moves) {
        highest_layer = std::max(highest_layer, move.layer_id);
        have_layer = true;
    }
    out.layer_count = have_layer ? static_cast<size_t>(highest_layer) + 1 : 0;
    return out;
}

} // namespace

struct IsolatedSlicingJob::Baseline {
    Baseline(IsolatedSlicingBaseline snapshot_, std::string storage_root_)
        : snapshot(std::move(snapshot_)), storage_root(std::move(storage_root_)) {}

    IsolatedSlicingBaseline snapshot;
    std::string             storage_root;
};

const char* isolated_slicing_state_name(IsolatedSlicingState state) noexcept
{
    switch (state) {
    case IsolatedSlicingState::Ready:     return "ready";
    case IsolatedSlicingState::Running:   return "running";
    case IsolatedSlicingState::Succeeded: return "succeeded";
    case IsolatedSlicingState::Cancelled: return "cancelled";
    case IsolatedSlicingState::Failed:    return "failed";
    case IsolatedSlicingState::Closed:    return "closed";
    }
    return "unknown";
}

std::shared_ptr<IsolatedSlicingJob> IsolatedSlicingJob::capture_live()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    if (!wxIsMainThread())
        throw std::runtime_error("Isolated slicing baseline capture must run on the UI thread");

    GUI::Plater* plater = GUI::wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("Plater is not available");
    PresetBundle* bundle = GUI::wxGetApp().preset_bundle;
    if (bundle == nullptr)
        throw std::runtime_error("Preset bundle is not available");
    if (plater->printer_technology() != ptFFF)
        throw std::runtime_error("Isolated slicing v1 supports FFF technology only");

    GUI::PartPlate* plate = plater->get_partplate_list().get_curr_plate();
    if (plate == nullptr)
        throw std::runtime_error("Current plate is not available");

    const std::string plugin_key = PluginAuditManager::instance().current_plugin();
    if (plugin_key.empty())
        throw std::runtime_error("Isolated slicing jobs must be created from a plugin callback");
    const std::string storage_root = PluginManager::instance().get_storage_dir(plugin_key);

    IsolatedSlicingBaseline baseline;
    baseline.model                = plater->model();
    baseline.plate_config         = *plate->config();
    baseline.plate_index          = plate->get_index();
    baseline.plate_origin         = plate->get_origin();
    baseline.filament_maps        = plate->get_real_filament_maps(bundle->project_config);
    baseline.filament_volume_maps = plate->get_filament_volume_maps();
    if (baseline.filament_volume_maps.empty())
        baseline.filament_volume_maps =
            bundle->get_default_nozzle_volume_types_for_filaments(baseline.filament_maps);

    baseline.config = bundle->get_printer_extruder_count() > 1
        ? bundle->full_config(false, baseline.filament_maps, baseline.filament_volume_maps)
        : bundle->full_config(false);
    baseline.config.apply(baseline.plate_config, true);
    baseline.bbl_vendor = bundle->is_bbl_vendor();
    baseline.check_mixed_filament_compatibility =
        GUI::wxGetApp().app_config == nullptr ||
        GUI::wxGetApp().app_config->get("enable_high_low_temp_mixed_printing") == "false";
    baseline.extruder_filament_info = plater->extruder_filament_info_for_slicing();

    // No GUI object, preset, config option or Python wrapper is retained past this point.
    return create_owned(std::move(baseline), storage_root, plugin_key);
}

std::shared_ptr<IsolatedSlicingJob> IsolatedSlicingJob::create_owned(
    IsolatedSlicingBaseline baseline, std::string plugin_storage_root,
    std::string plugin_key)
{
    validate_baseline(baseline);
    const fs::path root = normalized_storage_root(plugin_storage_root);
    std::shared_ptr<IsolatedSlicingJob> job(
        new IsolatedSlicingJob(std::move(baseline), root.string()));
    if (!plugin_key.empty() &&
        !PluginManager::instance().register_isolated_slicing_job(plugin_key, job))
        throw std::runtime_error("Plugin is unloading; new isolated slicing jobs are rejected");
    return job;
}

IsolatedSlicingJob::IsolatedSlicingJob(
    IsolatedSlicingBaseline baseline, std::string plugin_storage_root)
    : m_baseline(std::make_shared<const Baseline>(
          std::move(baseline), std::move(plugin_storage_root)))
    , m_candidate(m_baseline->snapshot.config)
{}

IsolatedSlicingJob::~IsolatedSlicingJob()
{
    close();
}

std::map<std::string, std::string> IsolatedSlicingJob::apply_candidate(
    const std::map<std::string, std::string>& patch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != IsolatedSlicingState::Ready)
        throw std::runtime_error("Candidate settings may only be changed before the job runs");

    DynamicPrintConfig staged(m_baseline->snapshot.config);
    std::map<std::string, std::string> canonical;
    for (const auto& [key, value] : patch) {
        if (rejected_candidate_keys().count(key) != 0)
            throw std::runtime_error("Candidate setting is not supported by isolated slicing v1: " + key);
        if (staged.option(key, false) == nullptr)
            throw std::runtime_error("Unknown candidate setting: " + key);
        staged.set_deserialize_strict(key, value);
        canonical.emplace(key, staged.opt_serialize(key));
    }

    if (staged.option("printer_technology") == nullptr ||
        staged.opt_enum<PrinterTechnology>("printer_technology") != ptFFF)
        throw std::runtime_error("Isolated slicing v1 supports FFF technology only");
    if (has_nonempty_strings(staged, "slicing_pipeline_plugin"))
        throw std::runtime_error("Isolated slicing v1 rejects slicing pipeline plugins");
    if (has_nonempty_strings(staged, "post_process"))
        throw std::runtime_error("Isolated slicing v1 rejects post_process commands");

    const std::string error = validation_error_text(staged.validate());
    if (!error.empty())
        throw std::runtime_error(error);

    // Commit only after every option and the complete staged config validated.
    m_candidate           = std::move(staged);
    m_canonical_candidate = canonical;
    return canonical;
}

std::map<std::string, std::string> IsolatedSlicingJob::candidate() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_canonical_candidate;
}

void IsolatedSlicingJob::run()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != IsolatedSlicingState::Ready)
        throw std::runtime_error("Isolated slicing jobs are single-use");

    SlicingAdmissionToken admission =
        try_acquire_slicing_admission(SlicingAdmissionMode::IsolatedSlicing);
    if (!admission)
        throw std::runtime_error("Slicing is busy; isolated slicing cannot overlap another job");

    const fs::path temp_dir = create_owned_temp_dir(fs::path(m_baseline->storage_root));
    m_owned_temp_dir = temp_dir.string();
    m_cancel_requested.store(false, std::memory_order_release);
    m_state        = IsolatedSlicingState::Running;
    m_result       = IsolatedSlicingResult{};
    m_result.state = IsolatedSlicingState::Running;

    DynamicPrintConfig candidate(m_candidate);
    auto canonical = m_canonical_candidate;
    try {
        m_worker = std::thread(
            [this, admission = std::move(admission), candidate = std::move(candidate),
             canonical = std::move(canonical)]() mutable {
                (void) admission; // RAII: held through terminal worker state.
                worker_main(std::move(candidate), std::move(canonical));
            });
    } catch (...) {
        m_state        = IsolatedSlicingState::Ready;
        m_result.state = IsolatedSlicingState::Ready;
        cleanup_owned_output();
        throw;
    }
}

void IsolatedSlicingJob::worker_main(
    DynamicPrintConfig candidate, std::map<std::string, std::string> canonical_candidate)
{
    IsolatedSlicingResult result;
    result.state     = IsolatedSlicingState::Running;
    result.candidate = std::move(canonical_candidate);

    try {
        auto print = std::make_shared<Print>();
        print->set_status_silent();
        print->set_plate_index(m_baseline->snapshot.plate_index);
        print->set_plate_origin(m_baseline->snapshot.plate_origin);
        print->is_BBL_printer() = m_baseline->snapshot.bbl_vendor;
        print->set_check_multi_filaments_compatibility(
            m_baseline->snapshot.check_mixed_filament_compatibility);
        print->set_extruder_filament_info(m_baseline->snapshot.extruder_filament_info);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_active_print = print;
        }
        if (m_cancel_requested.load(std::memory_order_acquire))
            throw CanceledException();

        // This native worker never enters Python, so the GIL is not held during
        // apply, process or direct export.
        print->apply(m_baseline->snapshot.model, std::move(candidate), false);

        std::vector<StringObjectException> validation_warnings;
        const StringObjectException validation_error = print->validate(&validation_warnings);
        for (const StringObjectException& warning : validation_warnings)
            if (!warning.string.empty())
                result.warnings.emplace_back(warning.string);
        if (!validation_error.string.empty())
            throw std::runtime_error(validation_error.string);

        if (m_cancel_requested.load(std::memory_order_acquire))
            throw CanceledException();
        print->process();
        if (m_cancel_requested.load(std::memory_order_acquire))
            throw CanceledException();

        const fs::path output_path = fs::path(m_owned_temp_dir) / "output.gcode";
        if (!is_inside_allowed_root(output_path, fs::path(m_baseline->storage_root)))
            throw std::runtime_error("Isolated G-code output escaped plugin storage");

        GCodeProcessorResult gcode_result;
        const std::string actual_output =
            print->export_gcode(output_path.string(), &gcode_result, nullptr);
        if (!is_inside_allowed_root(fs::path(actual_output), fs::path(m_owned_temp_dir)))
            throw std::runtime_error("Isolated G-code export escaped its owned temporary directory");
        if (m_cancel_requested.load(std::memory_order_acquire))
            throw CanceledException();

        boost::system::error_code ec;
        const std::uintmax_t output_size = fs::file_size(actual_output, ec);
        if (ec)
            throw std::runtime_error("Unable to inspect isolated G-code output: " + ec.message());

        result.print_statistics = copy_print_statistics(print->print_statistics());
        result.gcode_statistics = copy_gcode_statistics(gcode_result);
        for (const GCodeProcessorResult::SliceWarning& warning : gcode_result.warnings)
            if (!warning.msg.empty())
                result.warnings.emplace_back(warning.msg);
        append_print_warnings(*print, result.warnings);
        result.gcode_path = actual_output;
        result.gcode_size = output_size;
        result.state      = IsolatedSlicingState::Succeeded;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_active_print.reset();
        m_result = std::move(result);
        m_state  = IsolatedSlicingState::Succeeded;
        return;
    } catch (const CanceledException&) {
        result.state = IsolatedSlicingState::Cancelled;
        result.error = "Isolated slicing was cancelled";
    } catch (const std::exception& error) {
        if (m_cancel_requested.load(std::memory_order_acquire)) {
            result.state = IsolatedSlicingState::Cancelled;
            result.error = "Isolated slicing was cancelled";
        } else {
            result.state = IsolatedSlicingState::Failed;
            result.error = error.what();
        }
    } catch (...) {
        result.state = IsolatedSlicingState::Failed;
        result.error = "Unknown isolated slicing failure";
    }

    cleanup_owned_output();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_active_print.reset();
    m_state  = result.state;
    m_result = std::move(result);
}

void IsolatedSlicingJob::cancel() noexcept
{
    m_cancel_requested.store(true, std::memory_order_release);
    std::shared_ptr<Print> print;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        print = m_active_print;
    }
    if (print)
        print->cancel();
}

void IsolatedSlicingJob::join_worker() noexcept
{
    std::lock_guard<std::mutex> join_lock(m_join_mutex);
    if (m_worker.joinable())
        m_worker.join();
}

IsolatedSlicingState IsolatedSlicingJob::wait()
{
    join_worker();
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

IsolatedSlicingState IsolatedSlicingJob::state() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

IsolatedSlicingResult IsolatedSlicingJob::result() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state == IsolatedSlicingState::Ready || m_state == IsolatedSlicingState::Running)
        throw std::runtime_error("Isolated slicing result is not terminal");
    return m_result;
}

IsolatedSlicingSnapshotInfo IsolatedSlicingJob::snapshot() const
{
    IsolatedSlicingSnapshotInfo out;
    out.object_count = m_baseline->snapshot.model.objects.size();
    out.plate_index  = m_baseline->snapshot.plate_index;
    out.filament_maps = m_baseline->snapshot.filament_maps;
    out.filament_volume_maps = m_baseline->snapshot.filament_volume_maps;
    out.bbl_vendor = m_baseline->snapshot.bbl_vendor;
    out.check_mixed_filament_compatibility =
        m_baseline->snapshot.check_mixed_filament_compatibility;
    if (const auto* nozzles =
            m_baseline->snapshot.config.option<ConfigOptionFloats>("nozzle_diameter"))
        out.extruder_count = nozzles->values.size();
    for (const auto& configs : m_baseline->snapshot.extruder_filament_info)
        out.extruder_filament_config_count += configs.size();
    return out;
}

void IsolatedSlicingJob::cleanup_owned_output() noexcept
{
    if (m_owned_temp_dir.empty())
        return;
    try {
        const fs::path root(m_baseline->storage_root);
        const fs::path owned(m_owned_temp_dir);
        if (owned != root && is_inside_allowed_root(owned, root)) {
            boost::system::error_code ec;
            fs::remove_all(owned, ec);
        }
    } catch (...) {
        // Destruction and cancellation must remain noexcept. A cleanup failure
        // leaves only the job's uniquely named directory under plugin storage.
    }
}

void IsolatedSlicingJob::close() noexcept
{
    std::lock_guard<std::mutex> close_lock(m_close_mutex);
    cancel();
    join_worker();
    cleanup_owned_output();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_active_print.reset();
    if (m_state == IsolatedSlicingState::Ready) {
        m_result       = IsolatedSlicingResult{};
        m_result.state = IsolatedSlicingState::Closed;
    }
    m_state = IsolatedSlicingState::Closed;
}

} // namespace Slic3r
