#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/McpService.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PlateOps.hpp"
#include "slic3r/GUI/Operations/ProjectOps.hpp"
#include "slic3r/GUI/Operations/RunStatus.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"

#include <boost/filesystem/path.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::GUI::Ops;
using namespace std::chrono_literals;

namespace {

// A run that started for request `started_for` (5, the test ticket's own, unless given).
SliceRunRecord run(uint64_t seq, uint64_t revision, uint64_t request, bool cancelled = false, bool finished = true,
                   uint64_t started_for = 5, int plate = 0)
{
    SliceRunRecord r;
    r.seq                   = seq;
    r.finished              = finished;
    r.cancelled             = cancelled;
    r.ok                    = finished && !cancelled;
    r.revision_at_finish    = revision;
    r.request_seq_at_finish = request;
    r.request_seq_at_start  = started_for;
    r.plate                 = plate;
    return r;
}

SliceRunRecord plate_run(uint64_t seq, int plate, std::string error = {}, bool cancelled = false)
{
    SliceRunRecord r = run(seq, 1, 5, cancelled, true, 5, plate);
    r.ok             = r.ok && error.empty();
    r.error          = std::move(error);
    return r;
}

std::vector<PlateOutcome> outcomes(const std::vector<PlateResult>& results)
{
    std::vector<PlateOutcome> out;
    for (const PlateResult& r : results)
        out.push_back(r.outcome);
    return out;
}

SliceTicketState ticket(uint64_t request_mine, uint64_t run_before, uint64_t revision)
{
    SliceTicketState t;
    t.request_seq_mine  = request_mine;
    t.run_seq_before    = run_before;
    t.revision_at_start = revision;
    t.seen_running      = true;
    t.started           = std::chrono::steady_clock::now() - 10s;
    return t;
}

SliceObservation observe(uint64_t request, uint64_t revision, std::vector<SliceRunRecord> runs, bool slicing = false)
{
    SliceObservation o;
    o.slicing_any      = slicing;
    o.request_seq      = request;
    o.current_revision = revision;
    o.runs_since_start = std::move(runs);
    return o;
}

} // namespace

TEST_CASE("classify_plates reports ok, error, empty, skipped and cancelled", "[Operations]")
{
    const std::vector<PlateObservation> plates{{0, true, false}, {1, false, false}, {2, false, true}, {3, false, false}};
    const auto results = classify_plates({0, 1, 2, 3, 7}, plates, {plate_run(11, 0), plate_run(12, 1, "boom")});
    CHECK(outcomes(results) == std::vector<PlateOutcome>{PlateOutcome::Ok, PlateOutcome::Error, PlateOutcome::Empty,
                                                         PlateOutcome::Skipped, PlateOutcome::Skipped});
    CHECK(results[1].error == "boom");
    CHECK(outcomes(classify_plates({3}, plates, {plate_run(11, 0, {}, true)})) == std::vector<PlateOutcome>{PlateOutcome::Cancelled});
    // Finished without a result and without saying why.
    SliceRunRecord no_result = plate_run(11, 1);
    no_result.ok             = false;
    const auto failed        = classify_plates({1}, plates, {no_result});
    CHECK(failed[0].outcome == PlateOutcome::Error);
    CHECK(failed[0].error == "the slice finished without a result");
}

TEST_CASE("a plate keeps the outcome of its own run whatever happened to it afterwards", "[Operations]")
{
    // An edit replaced the result.
    CHECK(outcomes(classify_plates({0}, {{0, false, false}}, {plate_run(11, 0)})) == std::vector<PlateOutcome>{PlateOutcome::Ok});
    // Someone deleted the objects.
    CHECK(outcomes(classify_plates({0}, {{0, false, true}}, {plate_run(11, 0)})) == std::vector<PlateOutcome>{PlateOutcome::Ok});
    // Someone else's slice succeeded after ours failed.
    const auto results = classify_plates({0}, {{0, true, false}}, {plate_run(11, 0, "Floating regions")});
    CHECK(results[0].outcome == PlateOutcome::Error);
    CHECK(results[0].error == "Floating regions");
    // The plate was deleted.
    CHECK(outcomes(classify_plates({0}, {}, {plate_run(11, 0)})) == std::vector<PlateOutcome>{PlateOutcome::Ok});
}

TEST_CASE("Slice All stopping at a failing plate leaves the later plates skipped", "[Operations]")
{
    const std::vector<PlateObservation> plates{{0, true, false}, {1, false, false}, {2, false, false}};
    const auto results = classify_plates({0, 1, 2}, plates, {plate_run(11, 0), plate_run(12, 1, "Floating regions")});
    CHECK(outcomes(results) == std::vector<PlateOutcome>{PlateOutcome::Ok, PlateOutcome::Error, PlateOutcome::Skipped});
    CHECK(results[2].error.empty());
}

TEST_CASE("a plate the validation refused reports that error", "[Operations]")
{
    // Refused at the request: Plater records a finished run carrying the validation message.
    auto results = classify_plates({0}, {{0, false, false}}, {plate_run(11, 0, "Nozzle too small")});
    CHECK(results[0].outcome == PlateOutcome::Error);
    CHECK(results[0].error == "Nozzle too small");
    // Refused before the request (the setting change validated it), so no run at all.
    results = classify_plates({0}, {{0, false, false, "Layer height cannot exceed nozzle diameter"}}, {});
    CHECK(results[0].outcome == PlateOutcome::Error);
    CHECK(results[0].error == "Layer height cannot exceed nozzle diameter");
}

TEST_CASE("a slice stays pending until it was seen running or the grace period passed", "[Operations]")
{
    SliceTicketState t = ticket(5, 10, 1);
    t.seen_running     = false;
    t.started          = std::chrono::steady_clock::now();
    CHECK(slice_status(t, observe(5, 1, {}), t.started + 100ms) == TicketStatus::Pending);
    CHECK(slice_status(t, observe(5, 1, {}), t.started + kSliceStartGrace + 1ms) == TicketStatus::Done);
    CHECK(slice_status(ticket(5, 10, 1), observe(5, 1, {}, true), std::chrono::steady_clock::now()) == TicketStatus::Pending);
}

TEST_CASE("a slice is superseded by another request or an edit while it ran", "[Operations]")
{
    const auto now = std::chrono::steady_clock::now();
    const SliceTicketState t = ticket(5, 10, 1);
    CHECK(slice_status(t, observe(6, 1, {run(11, 1, 6)}), now) == TicketStatus::Superseded); // a GUI slice request during it
    CHECK(slice_status(t, observe(5, 2, {run(11, 2, 5)}), now) == TicketStatus::Superseded); // an edit before its run finished
    CHECK(slice_status(t, observe(5, 1, {run(11, 1, 5)}), now) == TicketStatus::Done);
}

TEST_CASE("actions after a slice finished do not supersede it", "[Operations]")
{
    const auto now = std::chrono::steady_clock::now();
    const SliceTicketState t = ticket(5, 10, 1);
    // Another request and an edit, both after the run recorded its finish.
    CHECK(slice_status(t, observe(6, 2, {run(11, 1, 5)}), now) == TicketStatus::Done);
}

TEST_CASE("a slice with no run of its own never reads an earlier run's completion", "[Operations]")
{
    const auto now = std::chrono::steady_clock::now();
    // Empty slice after a project reset: the last earlier run (seq 10) finished at another revision.
    SliceTicketState empty = ticket(5, 10, 7);
    empty.seen_running     = false;
    CHECK(slice_status(empty, observe(5, 7, {run(10, 3, 4)}), now) == TicketStatus::Done);
    // Cached slice whose start revision differs from the last earlier run's.
    CHECK(slice_status(empty, observe(5, 7, {}), now) == TicketStatus::Done);
    // No run, but the project changed before the terminal poll.
    CHECK(slice_status(empty, observe(5, 8, {}), now) == TicketStatus::Superseded);
}

TEST_CASE("cancellation is taken only from the operation's own runs", "[Operations]")
{
    const SliceTicketState t = ticket(5, 10, 1);
    CHECK_FALSE(slice_cancelled(t, observe(5, 1, {run(10, 1, 5, true), run(11, 1, 5)})));
    CHECK(slice_cancelled(t, observe(5, 1, {run(11, 1, 5, true)})));
    // A person's slice started during the operation (a later request) and cancelled is not ours.
    CHECK_FALSE(slice_cancelled(t, observe(6, 1, {run(11, 1, 5), run(12, 1, 6, true, true, 6)})));
}

TEST_CASE("a later request supersedes at once only when it came while the slice ran", "[Operations]")
{
    const auto             now = std::chrono::steady_clock::now();
    const SliceTicketState t   = ticket(5, 10, 1);
    // A person's slice for request 6, still running, restarted ours.
    SliceObservation during = observe(6, 1, {run(11, 1, 6, true), run(12, 0, 0, false, false, 6)}, true);
    during.interrupted      = true;
    CHECK(slice_status(t, during, now) == TicketStatus::Superseded);
    // Slice All asked again between two of its plates: the first plate's run finished cleanly.
    SliceObservation between = observe(6, 1, {run(11, 1, 5), run(12, 0, 0, false, false, 6)}, true);
    between.interrupted      = true;
    CHECK(slice_status(t, between, now) == TicketStatus::Superseded);
    // Requested after ours had finished: our result stands, and their slice is not waited for.
    CHECK(slice_status(t, observe(6, 1, {run(11, 1, 5), run(12, 0, 0, false, false, 6)}, true), now) == TicketStatus::Done);
    // Our own run still running, nobody else's: pending.
    CHECK(slice_status(t, observe(5, 1, {run(11, 0, 0, false, false)}, true), now) == TicketStatus::Pending);
}

TEST_CASE("a plate's result is the operation's until someone else slices it", "[Operations]")
{
    const SliceTicketState t = ticket(5, 10, 1);
    // Already sliced: no run touched plate 0.
    CHECK(plate_result_is_own(0, t, observe(5, 1, {})));
    // Our run was the plate's last.
    CHECK(plate_result_is_own(0, t, observe(5, 1, {plate_run(11, 0)})));
    // A person sliced plate 0 again after ours finished (request 6).
    CHECK_FALSE(plate_result_is_own(0, t, observe(6, 1, {plate_run(11, 0), run(12, 1, 6, false, true, 6, 0)})));
    // Their slice of another plate leaves ours alone.
    CHECK(plate_result_is_own(0, t, observe(6, 1, {plate_run(11, 0), run(12, 1, 6, false, true, 6, 1)})));
}

TEST_CASE("a reslice or export under the request's number that the request did not start is not the operation's", "[Operations]")
{
    const SliceTicketState t = ticket(5, 10, 1);
    // Our run, then a GUI export of the same plate that failed (no new slice request).
    SliceRunRecord export_run = run(12, 1, 5, false, true, 5, 0);
    export_run.for_request    = false;
    export_run.ok             = false;
    export_run.error          = "Disk full";
    const SliceObservation obs = observe(5, 1, {plate_run(11, 0), export_run});
    REQUIRE(own_runs(t, obs).size() == 1);
    CHECK(outcomes(classify_plates({0}, {{0, true, false}}, own_runs(t, obs))) == std::vector<PlateOutcome>{PlateOutcome::Ok});
    // Its result is no longer known to be ours, so no statistics.
    CHECK_FALSE(plate_result_is_own(0, t, obs));
}

TEST_CASE("a slice request refused before validating reports the refusal over a cached result", "[Operations]")
{
    // Plater records the refusal (a broken mixed filament here) as a rejected run of the request.
    const auto results = classify_plates({0}, {{0, true, false}}, {plate_run(11, 0, "A mixed filament has a deleted or mismatched component")});
    CHECK(results[0].outcome == PlateOutcome::Error);
    CHECK(results[0].error == "A mixed filament has a deleted or mismatched component");
}

TEST_CASE("a run already slicing that the request left running is the operation's", "[Operations]")
{
    const auto       now = std::chrono::steady_clock::now();
    SliceTicketState t   = ticket(5, 10, 1);
    t.adopted_run        = 10;
    // Started for an earlier request, finished under ours with an error.
    SliceRunRecord adopted = run(10, 1, 5, false, true, 4);
    adopted.ok             = false;
    adopted.error          = "Floating regions";
    const SliceObservation obs = observe(5, 1, {adopted});
    REQUIRE(own_runs(t, obs).size() == 1);
    CHECK(slice_status(t, obs, now) == TicketStatus::Done);
    const auto results = classify_plates({0}, {{0, false, false}}, own_runs(t, obs));
    CHECK(results[0].outcome == PlateOutcome::Error);
    CHECK(results[0].error == "Floating regions");
    // Not adopted, the same run is someone else's.
    CHECK(own_runs(ticket(5, 10, 1), obs).empty());
}

TEST_CASE("early runs of a long Slice All are still seen", "[Operations]")
{
    const SliceTicketState      t = ticket(5, 100, 1);
    std::vector<SliceRunRecord> runs;
    for (uint64_t i = 1; i <= 20; ++i)
        runs.push_back(run(100 + i, 1, 5, i == 2));
    CHECK(slice_cancelled(t, observe(5, 1, runs)));
    runs[1].cancelled          = false;
    runs[2].revision_at_finish = 9;
    CHECK(slice_status(t, observe(5, 1, runs), std::chrono::steady_clock::now()) == TicketStatus::Superseded);
}

TEST_CASE("export status follows its own sequence number", "[Operations]")
{
    ExportRecord rec;
    rec.seq = 4;
    CHECK(export_status(3, 3, std::nullopt, 1) == ExportStatus::Rejected);
    CHECK(export_status(3, 4, rec, 1) == ExportStatus::Pending);
    rec.finished           = true;
    rec.ok                 = true;
    rec.revision_at_finish = 1;
    CHECK(export_status(3, 4, rec, 1) == ExportStatus::Ok);
    rec.revision_at_finish = 2;
    CHECK(export_status(3, 4, rec, 1) == ExportStatus::Superseded);
    rec.ok = false;
    CHECK(export_status(3, 4, rec, 2) == ExportStatus::Failed);
    rec.cancelled = true;
    CHECK(export_status(3, 4, rec, 2) == ExportStatus::Cancelled);
}

TEST_CASE("an outcome holds a value or a refusal", "[Operations]")
{
    const Outcome<int> value(7);
    REQUIRE(value);
    CHECK(value.value() == 7);

    const Outcome<int> refused(Refusal{RefusalKind::Busy, "slicing_in_progress", "Slicing is in progress."});
    REQUIRE_FALSE(refused);
    CHECK(refused.refusal().kind == RefusalKind::Busy);
    CHECK(std::string(refused.refusal().code) == "slicing_in_progress");
}

TEST_CASE("an outcome of a string is not taken for a refusal", "[Operations]")
{
    const Outcome<std::string> value(std::string("Generic PLA"));
    REQUIRE(value);
    CHECK(value.value() == "Generic PLA");
}

TEST_CASE("a refusal's English text fills its placeholders in order", "[Operations]")
{
    const Refusal refusal{RefusalKind::Precondition, "preset_not_offered", "%1% does not offer \"%2%\".", {"Filament slot 2", "Generic PETG"}};
    CHECK(refusal.english() == "Filament slot 2 does not offer \"Generic PETG\".");
}

TEST_CASE("a refusal with no placeholders keeps a percent sign", "[Operations]")
{
    const Refusal refusal{RefusalKind::Failed, "fill_failed", "The bed is 100% full."};
    CHECK(refusal.english() == "The bed is 100% full.");
}

TEST_CASE("a refusal whose values do not match its placeholders still gives text", "[Operations]")
{
    const Refusal too_few{RefusalKind::Precondition, "x", "%1% and %2%", {"one"}};
    CHECK(too_few.english() == "one and ");
    const Refusal too_many{RefusalKind::Precondition, "x", "%1% only", {"one", "two"}};
    CHECK(too_many.english() == "one only");
}

TEST_CASE("a refusal without a translation reads in the UI's language as it does in English", "[Operations]")
{
    const Refusal refusal{RefusalKind::Precondition, "no_such_thing", "Thing %1% of the test does not exist", {"3"}};
    CHECK(std::string(refusal.translated().ToUTF8().data()) == refusal.english());
}

TEST_CASE("no_such_plate names the plates the project has", "[Operations]")
{
    const Refusal one = no_such_plate(4, 1);
    CHECK(std::string(one.code) == "no_such_plate");
    CHECK(one.kind == RefusalKind::Precondition);
    CHECK(one.english() == "Plate 4 does not exist (the project has plate 0 only)");
    CHECK(no_such_plate(-1, 3).english() == "Plate -1 does not exist (the project has plates 0..2)");
}

TEST_CASE("a project is not saved under a sliced file's name", "[Operations]")
{
    // Refused on the name alone, before anything is asked of the app.
    const Outcome<Done> saved = save_project_as({boost::filesystem::path("C:/out/part.gcode.3mf")});
    REQUIRE_FALSE(saved);
    CHECK(std::string(saved.refusal().code) == "sliced_file_name");
    CHECK(saved.refusal().kind == RefusalKind::Precondition);

    const Outcome<Done> upper = save_project_as({boost::filesystem::path("C:/out/PART.GCODE.3MF")});
    REQUIRE_FALSE(upper);
    CHECK(std::string(upper.refusal().code) == "sliced_file_name");
}

// ---- The catalogs name their operations ---------------------------------------------------------------
//
// A tool that changes something, and a command that takes a param, either names the operations
// (GUI/Operations) it reaches or is listed here with the reason it reaches none. The lists are what a
// new tool or command has to be added to, or kept off, in plain sight. They say how an entry is
// classified; they do not check what its handler calls, which scripts/check_operation_calls.py does
// for the owner calls an operation stands for.

namespace {

// Every function of GUI/Operations a catalog entry may name.
const std::set<std::string> kOperations = {
    "start_gcode_export", "export_sliced_3mf", "export_model_3mf",       "export_stl",      "send_gcode_to_host",
    "import_models",      "open_project",      "new_project",            "save_project_as", "set_instance_count",
    "select_plate",       "set_plate_locked",  "start_arrange",          "start_slice",     "show_layer_range",
    "resolve_preset",     "select_preset",     "assign_filament_preset", "import_presets",
};

// What an exception is, so that the reason reads the same for every entry of a kind.
constexpr const char* kRunsWhatItIsGiven = "runs what its arguments name, which is classified where it is defined";
constexpr const char* kDrivesTheGui      = "drives the GUI as a person does: the widget it acts on decides what happens";
constexpr const char* kSessionState      = "changes the automation session (jobs, the journal, recordings), not the project";
constexpr const char* kOneEntryPoint     = "its function has one automation entry point, so no second caller can drift from it";
constexpr const char* kAsks              = "the asking path: it calls the owner's method, which asks the person";

const std::map<std::string, const char*> kToolExceptions = {
    {"run_replay", kRunsWhatItIsGiven},
    {"run_script", kRunsWhatItIsGiven},
    {"run_action", kRunsWhatItIsGiven},
    {"call_tool", kRunsWhatItIsGiven},
    {"call_cad_tool", kRunsWhatItIsGiven},
    {"run_javascript", kRunsWhatItIsGiven},
    {"click", kDrivesTheGui},
    {"set_widget_value", kDrivesTheGui},
    {"press_keys", kDrivesTheGui},
    {"mouse_gesture", kDrivesTheGui},
    {"pick_gizmo_file", kDrivesTheGui},
    {"answer_dialog", kDrivesTheGui},
    {"auto_answer_dialogs", kDrivesTheGui},
    {"set_window", kDrivesTheGui},
    {"cancel_job", kSessionState},
    {"export_replay", kSessionState},
    {"start_recording", kSessionState},
    {"stop_recording", kSessionState},
    {"encode_recording", kSessionState},
    {"generate_calibration_test", kOneEntryPoint},
    {"save_preset", kOneEntryPoint},
    {"delete_preset", kOneEntryPoint},
    {"transform", kOneEntryPoint},
    {"add_part", kOneEntryPoint},
    {"select_objects", kOneEntryPoint},
    {"set_height_ranges", kOneEntryPoint},
    {"set_variable_layer_height", kOneEntryPoint},
    {"set_custom_gcode_at_layer", kOneEntryPoint},
};

const std::map<std::string, const char*> kCommandExceptions = {
    {"load_project", kAsks},
    {"go_to_tab", kOneEntryPoint},
    {"open_preferences", kOneEntryPoint},
    {"start_recording", kSessionState},
    {"export_stl_multi", kOneEntryPoint},
    {"export_config", kOneEntryPoint},
    {"obj_rename", kOneEntryPoint},
    {"obj_order_by_name", kOneEntryPoint},
    {"obj_order_by_filament", kOneEntryPoint},
    {"obj_order_by_height", kOneEntryPoint},
    {"obj_order_by_size", kOneEntryPoint},
    {"add_primitive_svg", kOneEntryPoint},
    {"add_primitive_cube", kOneEntryPoint},
    {"add_primitive_cylinder", kOneEntryPoint},
    {"add_primitive_sphere", kOneEntryPoint},
    {"add_primitive_cone", kOneEntryPoint},
    {"add_primitive_disc", kOneEntryPoint},
    {"add_primitive_torus", kOneEntryPoint},
    {"install_local_plugin", kOneEntryPoint},
    {"filament_delete", kOneEntryPoint},
};

} // namespace

TEST_CASE("a range of layers is brought into the plate's layers, and one given backwards shows its first layer", "[Operations]")
{
    // 100 layers are the slider's ticks 0..99.
    LayerTicks ticks = layer_ticks(20, 60, 100);
    CHECK(ticks.low == std::optional<int>(19));
    CHECK(ticks.high == 59);

    ticks = layer_ticks(60, 20, 100);
    CHECK(ticks.low == std::optional<int>(59));
    CHECK(ticks.high == 59);

    ticks = layer_ticks(-5, 500, 100);
    CHECK(ticks.low == std::optional<int>(0));
    CHECK(ticks.high == 99);

    ticks = layer_ticks(500, 600, 100);
    CHECK(ticks.low == std::optional<int>(99));
    CHECK(ticks.high == 99);

    ticks = layer_ticks(1, 0, 100);
    CHECK(ticks.low == std::optional<int>(0));
    CHECK(ticks.high == 0);

    // Without a first layer the lower thumb is not moved.
    ticks = layer_ticks(std::nullopt, 10, 100);
    CHECK_FALSE(ticks.low.has_value());
    CHECK(ticks.high == 9);
    CHECK(layer_ticks(std::nullopt, 500, 100).high == 99);
    CHECK(layer_ticks(std::nullopt, -3, 100).high == 0);
}

TEST_CASE("a filament pick finds the selector's row that stores the preset, else an unnamed row labelled with it", "[Operations]")
{
    using Slic3r::GUI::PresetRow;
    using Slic3r::GUI::row_for_preset;
    // A filament selector as the sidebar lays it out: the AMS filaments under their header, which store
    // no preset name, then the presets under theirs. A row's index is its item's, headers included.
    const std::vector<PresetRow> rows = {{"", "AMS filaments", true},
                                         {"", "Bambu ABS"},
                                         {"", "Generic PETG"},
                                         {"", "System presets", true},
                                         {"Bambu PLA Basic @BBL X1C", "Bambu PLA Basic"},
                                         {"Generic PETG", "Generic PETG"},
                                         {"", "Bambu PLA Basic", true}};
    CHECK(row_for_preset(rows, "Bambu PLA Basic @BBL X1C") == 4);
    // An AMS filament's row stores no name, so its label is the name a pick of it carries.
    CHECK(row_for_preset(rows, "Bambu ABS") == 1);
    // The row that stores the name is taken, wherever an unnamed one labelled with it is.
    CHECK(row_for_preset(rows, "Generic PETG") == 5);
    // A row that stores a name is picked by that name and not by its label, and a header never is.
    CHECK(row_for_preset(rows, "Bambu PLA Basic") == -1);
    CHECK(row_for_preset(rows, "AMS filaments") == -1);
    CHECK(row_for_preset(rows, "Bambu PETG HF") == -1);
    CHECK(row_for_preset({}, "Bambu ABS") == -1);
}

TEST_CASE("a tool that changes something names its operations or is a listed exception", "[Operations]")
{
    for (const Slic3r::GUI::Mcp::ToolDef& tool : Slic3r::GUI::Mcp::McpService::offline_catalog().tools()) {
        if (tool.read_only)
            continue;
        INFO(tool.name);
        const bool excepted = kToolExceptions.count(tool.name) != 0;
        // One or the other: an exception that names operations has stopped being one.
        CHECK(tool.operations.empty() == excepted);
    }
}

TEST_CASE("a command with a param names its operations or is a listed exception", "[Operations]")
{
    for (const Slic3r::GUI::NativeCommand& command : Slic3r::GUI::NativeCommands::catalog()) {
        if (command.param.type == Slic3r::GUI::CommandParamType::None)
            continue;
        INFO(command.key);
        const bool excepted = kCommandExceptions.count(command.key) != 0;
        CHECK(command.operations.empty() == excepted);
    }
}

TEST_CASE("a catalog entry names only operations the layer has", "[Operations]")
{
    for (const Slic3r::GUI::Mcp::ToolDef& tool : Slic3r::GUI::Mcp::McpService::offline_catalog().tools())
        for (const std::string& operation : tool.operations) {
            INFO(tool.name << " names " << operation);
            CHECK(kOperations.count(operation) == 1);
        }
    for (const Slic3r::GUI::NativeCommand& command : Slic3r::GUI::NativeCommands::catalog())
        for (const std::string& operation : command.operations) {
            INFO(command.key << " names " << operation);
            CHECK(kOperations.count(operation) == 1);
        }
}

TEST_CASE("the exception lists name only entries the catalogs have", "[Operations]")
{
    const Slic3r::GUI::Mcp::ToolRegistry& tools = Slic3r::GUI::Mcp::McpService::offline_catalog();
    for (const auto& [name, reason] : kToolExceptions) {
        INFO(name << ": " << reason);
        const Slic3r::GUI::Mcp::ToolDef* tool = tools.find(name);
        REQUIRE(tool != nullptr);
        // A read-only tool needs no exception.
        CHECK_FALSE(tool->read_only);
    }
    for (const auto& [key, reason] : kCommandExceptions) {
        INFO(key << ": " << reason);
        const Slic3r::GUI::NativeCommand* command = Slic3r::GUI::NativeCommands::find(key);
        REQUIRE(command != nullptr);
        CHECK(command->param.type != Slic3r::GUI::CommandParamType::None);
    }
}

TEST_CASE("the field that names a tool's operations is in no listing", "[Operations]")
{
    // tools/list and search_actions stay as they are: the field is for the catalog test alone.
    const Slic3r::GUI::Mcp::ToolDef* tool = Slic3r::GUI::Mcp::McpService::offline_catalog().find("export");
    REQUIRE(tool != nullptr);
    REQUIRE_FALSE(tool->operations.empty());
    CHECK(tool->input_schema.dump().find("start_gcode_export") == std::string::npos);
    CHECK(tool->description.find("start_gcode_export") == std::string::npos);
}
