#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Mcp/PythonScriptsDialog.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/ExportOps.hpp"
#include "slic3r/GUI/Operations/GizmoOps.hpp"
#include "slic3r/GUI/Operations/InstallOps.hpp"
#include "slic3r/GUI/Operations/ObjectOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PlateOps.hpp"
#include "slic3r/GUI/Operations/PluginOps.hpp"
#include "slic3r/GUI/Operations/PreferenceOps.hpp"
#include "slic3r/GUI/Operations/PresetOps.hpp"
#include "slic3r/GUI/Operations/PrinterOps.hpp"
#include "slic3r/GUI/Operations/ProjectOps.hpp"
#include "slic3r/GUI/Operations/RunStatus.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/Utils/FixModelByCgal.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BrimEarsPoint.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TextConfiguration.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <admesh/stl.h>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
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

// ---- The object commands' checks, on a model built in the case ------------------------------------------------------

namespace {

// An object named `name` of `parts` cubes, solid parts all, with `instances` instances.
Slic3r::ModelObject* add_cubes(Slic3r::Model& model, const std::string& name, int instances, int parts)
{
    Slic3r::ModelObject* object = model.add_object();
    object->name                = name;
    for (int p = 0; p < parts; ++p)
        object->add_volume(Slic3r::make_cube(10., 10., 10.), Slic3r::ModelVolumeType::MODEL_PART, false);
    for (int i = 0; i < instances; ++i)
        object->add_instance();
    return object;
}

std::string code_of(const std::optional<Refusal>& refusal) { return refusal ? refusal->code : ""; }

} // namespace

TEST_CASE("the refusals of a command's targets have their codes and say what to do", "[Operations]")
{
    for (const auto& [refusal, code] : std::vector<std::pair<Refusal, std::string>>{{nothing_selected(), "nothing_selected"},
                                                                                    {wrong_selection(), "wrong_selection"},
                                                                                    {one_object_only(), "one_object_only"},
                                                                                    {gizmo_keeps_selection(), "gizmo_open"}}) {
        INFO(code);
        CHECK(refusal.code == code);
        CHECK(refusal.kind == RefusalKind::Precondition);
        CHECK_FALSE(refusal.english().empty());
        // No placeholder is left in the text.
        CHECK(refusal.english().find('%') == std::string::npos);
    }
    CHECK(nothing_selected().english() == "Nothing is selected: name the objects, or select them first.");
}

TEST_CASE("deleting part of a cut object is refused with the question the object list asks", "[Operations]")
{
    Slic3r::Model model;
    add_cubes(model, "half", 1, 2)->cut_id.init();
    add_cubes(model, "twice", 2, 1);

    Targets part;
    part.parts = {{0, 1}};
    const std::optional<Refusal> refused = delete_refusal(model, part);
    REQUIRE(refused);
    CHECK(code_of(refused) == "cut_object");
    CHECK(refused->english() == "This action will break a cut correspondence.\nAfter that, model consistency can't be guaranteed.\n\nTo "
                                "manipulate with solid parts or negative volumes you have to invalidate cut information first.");
    // A whole cut object is asked about too: a call without a person to answer is refused.
    Targets half;
    half.objects = {0};
    CHECK(code_of(delete_refusal(model, half)) == "cut_object");

    // Neither a whole object nor one instance of an object with two.
    Targets whole;
    whole.objects = {1};
    CHECK_FALSE(delete_refusal(model, whole));
    Targets instance;
    instance.instances = {{1, 1}};
    CHECK_FALSE(delete_refusal(model, instance));

    // An object keeps a solid part: its last one is not deleted alone, while with every part it is the whole object.
    Slic3r::Model        parts;
    Slic3r::ModelObject* object = add_cubes(parts, "with modifier", 1, 1);
    object->add_volume(Slic3r::make_cube(5., 5., 5.), Slic3r::ModelVolumeType::PARAMETER_MODIFIER, false);
    Targets last;
    last.parts = {{0, 0}};
    CHECK(code_of(delete_refusal(parts, last)) == "last_solid_part");
    last.parts = {{0, 0}, {0, 1}};
    CHECK_FALSE(delete_refusal(parts, last));
    CHECK(deletion(parts, last).objects == std::vector<int>{0});
}

TEST_CASE("a delete names what went and what stayed of what it was asked for, by identity", "[Operations]")
{
    using Pairs = std::vector<std::pair<int, int>>;
    // An object of two solid parts and a modifier, both solid parts asked for: the object list deletes the second and
    // refuses the first, the object's last solid part.
    Slic3r::Model        model;
    Slic3r::ModelObject* object = add_cubes(model, "two parts", 1, 2);
    object->add_volume(Slic3r::make_cube(5., 5., 5.), Slic3r::ModelVolumeType::PARAMETER_MODIFIER, false);
    Targets doomed;
    doomed.parts               = {{0, 0}, {0, 1}};
    const TargetIds two_parts  = target_ids(model, doomed);
    object->delete_volume(1);
    const DeletedTargets refused_one = deleted_targets(doomed, two_parts, model);
    CHECK(refused_one.deleted.parts == Pairs{{0, 1}});
    CHECK(refused_one.kept.parts == Pairs{{0, 0}});

    // What stayed is found where it is now: a part after a deleted one has moved down.
    Slic3r::Model        three;
    Slic3r::ModelObject* parts = add_cubes(three, "three parts", 1, 3);
    doomed.parts               = {{0, 0}, {0, 1}};
    const TargetIds first_two  = target_ids(three, doomed);
    parts->delete_volume(0);
    const DeletedTargets moved = deleted_targets(doomed, first_two, three);
    CHECK(moved.deleted.parts == Pairs{{0, 0}});
    CHECK(moved.kept.parts == Pairs{{0, 0}});

    // Objects and instances alike: of two objects and an instance asked for, one object went, and the other object and
    // the instance, of an object after it, stayed.
    Slic3r::Model objects;
    add_cubes(objects, "a", 1, 1);
    add_cubes(objects, "b", 1, 1);
    add_cubes(objects, "c", 2, 1);
    Targets named;
    named.objects             = {0, 1};
    named.instances           = {{2, 1}};
    const TargetIds named_ids = target_ids(objects, named);
    objects.delete_object(std::size_t(1));
    const DeletedTargets one_object = deleted_targets(named, named_ids, objects);
    CHECK(one_object.deleted.objects == std::vector<int>{1});
    CHECK(one_object.deleted.instances.empty());
    CHECK(one_object.kept.objects == std::vector<int>{0});
    CHECK(one_object.kept.instances == Pairs{{1, 1}});

    // Everything asked for went: nothing kept.
    const DeletedTargets all_gone = deleted_targets(named, named_ids, Slic3r::Model());
    CHECK(all_gone.deleted.objects == named.objects);
    CHECK(all_gone.deleted.instances == named.instances);
    CHECK(all_gone.kept.objects.empty());
    CHECK(all_gone.kept.instances.empty());
}

TEST_CASE("a rename is refused an empty name and a name a file name cannot hold", "[Operations]")
{
    CHECK(code_of(rename_refusal("")) == "empty_name");
    for (const char* name : {"a/b", "a<b", "a>b", "a:b", "a\\b", "a|b", "a?b", "a*b", "a\"b"}) {
        INFO(name);
        CHECK(code_of(rename_refusal(name)) == "invalid_name");
    }
    CHECK_FALSE(rename_refusal("infill zone"));
}

TEST_CASE("an object's number of instances is from 1 to 1000, and a cut object keeps its own", "[Operations]")
{
    Slic3r::Model model;
    add_cubes(model, "plain", 1, 1);
    add_cubes(model, "half", 1, 1)->cut_id.init();
    add_cubes(model, "hidden", 2, 1)->instances[1]->printable = false;
    add_cubes(model, "three", 3, 1);

    CHECK_FALSE(instance_count_refusal(model, 0, 1));
    CHECK_FALSE(instance_count_refusal(model, 0, 1000));
    for (int count : {-2, 1001}) {
        INFO(count);
        CHECK(code_of(instance_count_refusal(model, 3, count)) == "bad_instance_count");
    }
    CHECK(code_of(instance_count_refusal(model, 1, 2)) == "cut_object");
    // An object keeps one instance at least, and the refusal says that is why.
    const std::optional<Refusal> last = instance_count_refusal(model, 0, 0);
    CHECK(code_of(last) == "last_instance");
    REQUIRE(last);
    CHECK_THAT(last->english(), Catch::Matchers::ContainsSubstring("\"plain\" has one instance"));
}

TEST_CASE("an instance that is not printable keeps instances from being added, not from being removed", "[Operations]")
{
    // As Increase Instances and Decrease Instances are enabled (Plater::priv::can_increase_instances(),
    // can_decrease_instances()): only the first has the printable rule.
    Slic3r::Model model;
    add_cubes(model, "hidden", 2, 1)->instances[1]->printable = false;
    CHECK_FALSE(instance_count_refusal(model, 0, 1));
    CHECK_FALSE(instance_count_refusal(model, 0, 2));
    CHECK(code_of(instance_count_refusal(model, 0, 3)) == "unprintable_instance");
    // A cut object neither gains nor loses one.
    Slic3r::Model cut;
    add_cubes(cut, "half", 2, 1)->cut_id.init();
    CHECK(code_of(instance_count_refusal(cut, 0, 1)) == "cut_object");
    // Set Number of Instances, whose dialog may ask for more, is disabled as Increase Instances is.
    CHECK(code_of(add_instances_refusal(model, 0)) == "unprintable_instance");
    CHECK(code_of(add_instances_refusal(cut, 0)) == "cut_object");
    Slic3r::Model plain;
    add_cubes(plain, "plain", 1, 1);
    CHECK_FALSE(add_instances_refusal(plain, 0));
}

TEST_CASE("separating instances is refused for an object of one instance and for an instance it lacks", "[Operations]")
{
    Slic3r::Model model;
    add_cubes(model, "single", 1, 1);
    add_cubes(model, "twice", 2, 1);

    Targets single;
    single.objects = {0};
    const std::optional<Refusal> one = separate_refusal(model, single);
    CHECK(code_of(one) == "one_instance");
    REQUIRE(one);
    CHECK_THAT(one->english(), Catch::Matchers::ContainsSubstring("\"single\" has one instance"));

    Targets lacks;
    lacks.instances = {{1, 5}};
    const std::optional<Refusal> missing = separate_refusal(model, lacks);
    CHECK(code_of(missing) == "no_such_instance");
    REQUIRE(missing);
    CHECK_THAT(missing->english(), Catch::Matchers::ContainsSubstring("5"));

    Targets whole;
    whole.objects = {1};
    CHECK_FALSE(separate_refusal(model, whole));
    Targets some;
    some.instances = {{1, 0}};
    CHECK_FALSE(separate_refusal(model, some));
    // Instances of two objects are two separations.
    Targets both;
    both.objects = {0, 1};
    CHECK(code_of(separate_refusal(model, both)) == "one_object_only");
}

TEST_CASE("a split is refused a mesh of one shell, saying so, and not a mesh of two", "[Operations]")
{
    Slic3r::Model model;
    add_cubes(model, "solid", 1, 1);
    Slic3r::TriangleMesh two = Slic3r::make_cube(10., 10., 10.);
    Slic3r::TriangleMesh apart = Slic3r::make_cube(10., 10., 10.);
    apart.translate(30.f, 0.f, 0.f);
    two.merge(apart);
    Slic3r::ModelObject* pair = model.add_object();
    pair->name                = "pair";
    pair->add_volume(std::move(two), Slic3r::ModelVolumeType::MODEL_PART, false);
    pair->add_instance();
    add_cubes(model, "assembly", 1, 2);

    Targets solid;
    solid.objects = {0};
    const std::optional<Refusal> one = split_refusal(model, solid);
    CHECK(code_of(one) == "one_shell");
    REQUIRE(one);
    CHECK_THAT(one->english(), Catch::Matchers::ContainsSubstring("\"solid\""));
    CHECK_THAT(one->english(), Catch::Matchers::ContainsSubstring("one piece"));

    Targets shells;
    shells.objects = {1};
    CHECK_FALSE(split_refusal(model, shells));
    // A part is split by its own shells: one cube of an object of two is one piece.
    Targets part;
    part.parts = {{2, 1}};
    CHECK(code_of(split_refusal(model, part)) == "one_shell");
    // An object of several parts splits into objects by its parts.
    Targets parts;
    parts.objects = {2};
    CHECK_FALSE(split_refusal(model, parts));
}

TEST_CASE("an assembly needs two objects or instances, none of them cut", "[Operations]")
{
    Slic3r::Model model;
    add_cubes(model, "one", 1, 1);
    add_cubes(model, "two", 2, 1);
    add_cubes(model, "half", 1, 1)->cut_id.init();

    Targets single;
    single.objects = {0};
    const std::optional<Refusal> alone = assemble_refusal(model, single);
    CHECK(code_of(alone) == "too_few_objects");
    REQUIRE(alone);
    CHECK(alone->english() == "Select two or more objects first.");
    CHECK(code_of(assemble_refusal(model, Targets{})) == "too_few_objects");

    Targets pair;
    pair.objects = {0, 1};
    CHECK_FALSE(assemble_refusal(model, pair));
    // Two instances of one object are two, as the object list counts its rows.
    Targets instances;
    instances.instances = {{1, 0}, {1, 1}};
    CHECK_FALSE(assemble_refusal(model, instances));

    Targets cut;
    cut.objects = {0, 2};
    const std::optional<Refusal> refused = assemble_refusal(model, cut);
    CHECK(code_of(refused) == "cut_object");
    REQUIRE(refused);
    CHECK(refused->english() == "Cut objects can't be assembled.");
}

TEST_CASE("a part's type change keeps an object's last solid part, and a subtype is a precise seam's", "[Operations]")
{
    using Slic3r::ModelVolumeType;
    Slic3r::Model        model;
    Slic3r::ModelObject* object = add_cubes(model, "body", 1, 2);
    object->add_volume(Slic3r::make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER, false);

    // One solid part of two may become anything; both may not, unless they stay solid.
    Targets one;
    one.parts = {{0, 0}};
    CHECK_FALSE(part_type_refusal(model, one, ModelVolumeType::NEGATIVE_VOLUME, std::nullopt));
    Targets both;
    both.parts = {{0, 0}, {0, 1}};
    const std::optional<Refusal> last = part_type_refusal(model, both, ModelVolumeType::PARAMETER_MODIFIER, std::nullopt);
    CHECK(code_of(last) == "last_solid_part");
    REQUIRE(last);
    CHECK(last->english() == "The type of the last solid object part cannot be changed.");
    CHECK_FALSE(part_type_refusal(model, both, ModelVolumeType::MODEL_PART, std::nullopt));
    // The modifier with a solid part: what it becomes leaves the solid parts as they are.
    Targets mixed;
    mixed.parts = {{0, 2}, {0, 0}};
    CHECK_FALSE(part_type_refusal(model, mixed, ModelVolumeType::SUPPORT_BLOCKER, std::nullopt));

    // A subtype goes with a precise seam.
    Targets modifier;
    modifier.parts = {{0, 2}};
    CHECK(code_of(part_type_refusal(model, modifier, ModelVolumeType::PARAMETER_MODIFIER, ModelVolumeType::PRECISE_SEAM_RIGHT)) ==
          "subtype_without_seam");
    CHECK_FALSE(part_type_refusal(model, modifier, ModelVolumeType::PRECISE_SEAM_CENTER, ModelVolumeType::PRECISE_SEAM_RIGHT));
}

TEST_CASE("a text part becomes a part, a negative part or a modifier, and nothing else", "[Operations]")
{
    using Slic3r::ModelVolumeType;
    Slic3r::Model        model;
    Slic3r::ModelObject* object = add_cubes(model, "body", 1, 1);
    Slic3r::ModelVolume* text   = object->add_volume(Slic3r::make_cube(5., 5., 1.), ModelVolumeType::PARAMETER_MODIFIER, false);
    text->text_configuration    = Slic3r::TextConfiguration{};
    REQUIRE(text->is_text());

    Targets lettering;
    lettering.parts = {{0, 1}};
    for (const ModelVolumeType kept : {ModelVolumeType::MODEL_PART, ModelVolumeType::NEGATIVE_VOLUME, ModelVolumeType::PARAMETER_MODIFIER})
        CHECK_FALSE(part_type_refusal(model, lettering, kept, std::nullopt));
    for (const ModelVolumeType refused : {ModelVolumeType::SUPPORT_BLOCKER, ModelVolumeType::SUPPORT_ENFORCER, ModelVolumeType::PRECISE_SEAM_CENTER}) {
        const std::optional<Refusal> why = part_type_refusal(model, lettering, refused, std::nullopt);
        CHECK(code_of(why) == "text_part");
        REQUIRE(why);
        CHECK(why->english() == "A text or SVG part can only be a part, a negative part or a modifier.");
    }
}

TEST_CASE("a cut object is not mirrored, saying so", "[Operations]")
{
    Slic3r::Model model;
    add_cubes(model, "plain", 1, 1);
    add_cubes(model, "half", 1, 1)->cut_id.init();

    Targets plain;
    plain.objects = {0};
    CHECK_FALSE(mirror_refusal(model, plain));
    Targets cut;
    cut.parts = {{1, 0}};
    const std::optional<Refusal> refused = mirror_refusal(model, cut);
    CHECK(code_of(refused) == "cut_object");
    REQUIRE(refused);
    CHECK_THAT(refused->english(), Catch::Matchers::ContainsSubstring("Cut objects"));
    CHECK_THAT(refused->english(), Catch::Matchers::ContainsSubstring("mirrored"));
}

TEST_CASE("objects are centred by the plate each stands on, one on no plate with the current plate's", "[Operations]")
{
    // Objects 0 and 1 stand on plate 1, object 2 on plate 2, object 3 on no plate; plate 1 is current.
    const std::map<int, int> stands{{0, 1}, {1, 1}, {2, 2}, {3, -1}};
    const auto plate_of = [&stands](int object) { return stands.at(object); };
    const std::map<int, std::vector<int>> groups = objects_by_plate({0, 1, 2}, plate_of, 1);
    CHECK(groups == std::map<int, std::vector<int>>{{1, {0, 1}}, {2, {2}}});
    CHECK(objects_by_plate({2, 3}, plate_of, 1) == std::map<int, std::vector<int>>{{1, {3}}, {2, {2}}});
    // A plate named holds them all.
    const auto on_plate_0 = [](int) { return 0; };
    CHECK(objects_by_plate({0, 1, 2, 3}, on_plate_0, 1) == std::map<int, std::vector<int>>{{0, {0, 1, 2, 3}}});
}

TEST_CASE("what does not rest on the bed is dropped, and what does is left as it is", "[Operations]")
{
    Slic3r::Model model;
    add_cubes(model, "resting", 1, 1);
    add_cubes(model, "lifted", 2, 1)->instances[1]->set_offset(Slic3r::Vec3d(0., 0., 15.));
    Slic3r::ModelObject* parts = add_cubes(model, "parts", 1, 2);
    parts->volumes[1]->set_offset(Slic3r::Vec3d(0., 0., 30.));

    Targets resting;
    resting.objects = {0};
    resting.parts   = {{2, 0}};
    const Targets none = objects_above_bed(model, resting);
    CHECK(none.objects.empty());
    CHECK(none.instances.empty());
    CHECK(none.parts.empty());

    Targets named;
    named.objects   = {0, 1};
    named.instances = {{1, 0}, {1, 1}};
    named.parts     = {{2, 0}, {2, 1}};
    const Targets above = objects_above_bed(model, named);
    // An object one of whose instances is above the bed, that instance, and the part above it.
    CHECK(above.objects == std::vector<int>{1});
    CHECK(above.instances == std::vector<std::pair<int, int>>{{1, 1}});
    CHECK(above.parts == std::vector<std::pair<int, int>>{{2, 1}});
}

TEST_CASE("Auto-Orient turns the instances of printable objects that stand on no locked plate", "[Operations]")
{
    Slic3r::Model model;
    add_cubes(model, "free", 2, 1);
    add_cubes(model, "not printable", 1, 1)->printable = false;
    add_cubes(model, "locked", 1, 1);
    // Instance 1 of "free" and the instance of "locked" stand on a locked plate.
    const auto locked = [](int object, int instance) { return (object == 0 && instance == 1) || object == 2; };

    CHECK(orientable(model, {0, 1, 2}, locked) == std::vector<std::pair<int, int>>{{0, 0}});
    // Nothing of these can be turned, which the job would take for nothing selected.
    CHECK(orientable(model, {1}, locked).empty());
    CHECK(orientable(model, {1, 2}, locked).empty());
    CHECK(orientable(model, {}, locked).empty());
}

namespace {

// Whether each instance of `model` is printable, object by object.
std::vector<bool> instances_printable(const Slic3r::Model& model)
{
    std::vector<bool> out;
    for (const Slic3r::ModelObject* object : model.objects)
        for (const Slic3r::ModelInstance* instance : object->instances)
            out.push_back(instance->printable);
    return out;
}

// A 20 mm cube, closed, or with one triangle taken out, which leaves a hole of three open edges.
Slic3r::ModelObject* add_cube_mesh(Slic3r::Model& model, const std::string& name, bool open)
{
    indexed_triangle_set its = Slic3r::make_cube(20., 20., 20.).its;
    if (open)
        its.indices.pop_back();
    Slic3r::ModelObject* object = model.add_object();
    object->name                = name;
    object->add_volume(Slic3r::TriangleMesh(std::move(its)));
    object->add_instance();
    return object;
}

} // namespace

TEST_CASE("an instance is made not printable alone, and an object with every instance of it", "[Operations]")
{
    Slic3r::Model model;
    add_cubes(model, "three", 3, 1);
    add_cubes(model, "other", 1, 1);

    Targets instance;
    instance.instances = {{0, 1}};
    CHECK(set_printable(model, instance, false) == std::vector<std::pair<int, int>>{{0, 1}});
    // Its two siblings and the other object are as they were.
    CHECK(instances_printable(model) == std::vector<bool>{true, false, true, true});

    Targets object;
    object.objects = {0};
    CHECK(set_printable(model, object, false) == std::vector<std::pair<int, int>>{{0, 0}, {0, 1}, {0, 2}});
    CHECK(instances_printable(model) == std::vector<bool>{false, false, false, true});
    CHECK(set_printable(model, object, true).size() == 3);
    CHECK(instances_printable(model) == std::vector<bool>{true, true, true, true});
}

TEST_CASE("Auto Drop is set on the objects named alone", "[Operations]")
{
    Slic3r::Model model;
    add_cubes(model, "first", 2, 1);
    add_cubes(model, "second", 1, 1);
    add_cubes(model, "third", 1, 1);

    Targets named;
    named.objects = {0, 2};
    CHECK(set_auto_drop(model, named, false) == std::vector<int>{0, 2});
    CHECK_FALSE(model.objects[0]->auto_drop);
    CHECK(model.objects[1]->auto_drop);
    CHECK_FALSE(model.objects[2]->auto_drop);
    named.objects = {2};
    CHECK(set_auto_drop(model, named, true) == std::vector<int>{2});
    CHECK_FALSE(model.objects[0]->auto_drop);
    CHECK(model.objects[2]->auto_drop);
}

TEST_CASE("an object's mesh errors count the open edges of its meshes", "[Operations]")
{
    Slic3r::Model model;
    const MeshErrors closed = mesh_errors(*add_cube_mesh(model, "closed", false));
    CHECK(closed.open_edges == 0);
    CHECK(closed.manifold);
    const MeshErrors open = mesh_errors(*add_cube_mesh(model, "open", true));
    CHECK(open.open_edges == 3);
    CHECK_FALSE(open.manifold);
}

TEST_CASE("a repair closes a mesh's hole, and a progress that says stop cancels it", "[Operations]")
{
    Slic3r::Model        model;
    Slic3r::ModelObject* repaired = add_cube_mesh(model, "repaired", true);
    std::string          result;
    bool                 modified = false;
    CHECK(Slic3r::fix_model_with_cgal(*repaired, -1, [](int, const std::string&) { return true; }, result, false, &modified));
    CHECK(modified);
    CHECK(result.empty());
    CHECK(mesh_errors(*repaired).open_edges == 0);

    Slic3r::ModelObject* cancelled = add_cube_mesh(model, "cancelled", true);
    CHECK_FALSE(Slic3r::fix_model_with_cgal(*cancelled, -1, [](int, const std::string&) { return false; }, result, false));
    CHECK(mesh_errors(*cancelled).open_edges == 3);
}

TEST_CASE("a repair cancelled after an object's first part was repaired lists that object as changed and cancelled", "[Operations]")
{
    Slic3r::Model        model;
    Slic3r::ModelObject* object = add_cube_mesh(model, "two parts", true);
    add_cube_mesh(model, "untouched", true);
    object->volumes[0]->name = "first";
    // A second part, a sphere of 43,680 triangles less the one taken out to open it, whose repair outlasts the cancel
    // given when it starts.
    indexed_triangle_set sphere = Slic3r::its_make_sphere(10., 0.03);
    sphere.indices.pop_back();
    object->add_volume(Slic3r::TriangleMesh(std::move(sphere)))->name = "second";
    const int open_before = mesh_errors(*object).open_edges;
    REQUIRE(open_before > 3);

    // The progress names the part being repaired; anything after the first part cancels.
    const auto first_only = [](std::size_t, int, const std::string& what) { return what.empty() || what == "first"; };
    std::vector<std::pair<int, int>> updated;
    const FixedModels fixed = repair_models(model, {{0, -1}, {1, -1}}, first_only, false,
                                            [&updated](int object, int part) { updated.emplace_back(object, part); });
    CHECK(fixed.cancelled);
    REQUIRE(fixed.models.size() == 1);
    CHECK(fixed.models[0].object == 0);
    CHECK(fixed.models[0].changed);
    CHECK(fixed.models[0].cancelled);
    CHECK(fixed.models[0].before.open_edges == open_before);
    // The first part's hole is closed; the object it stopped in is given to the scene, and the next object is as it was.
    CHECK(fixed.models[0].after.open_edges < open_before);
    CHECK(updated == std::vector<std::pair<int, int>>{{0, -1}});
    CHECK(mesh_errors(*model.objects[1]).open_edges == 3);
}

namespace {

// `paths` as generic paths, which compare alike whatever separator a platform joins them with.
std::set<std::string> generic(const std::vector<std::string>& paths)
{
    std::set<std::string> out;
    for (const std::string& path : paths)
        out.insert(boost::filesystem::path(path).generic_string());
    return out;
}

} // namespace

TEST_CASE("a reload finds each part's file, beside its object's when it moved, and lists those it cannot find", "[Operations]")
{
    Slic3r::Model        model;
    Slic3r::ModelObject* files = add_cubes(model, "files", 1, 5);
    files->input_file          = "/project/model.3mf";
    files->volumes[0]->source.input_file = "/models/there.stl";
    files->volumes[1]->source.input_file = "/models/gone.stl";
    // A primitive was loaded from no file, and a built-in model is reloaded from none.
    files->volumes[3]->source.input_file              = "/resources/handy_models/cube.stl";
    files->volumes[3]->source.is_from_builtin_objects = true;
    // Its file moved from where it was loaded, and one of its name is beside the project.
    files->volumes[4]->source.input_file = "/old/beside.stl";
    const std::set<std::string> present{"/models/there.stl", "/project/beside.stl", "/resources/handy_models/cube.stl"};
    const auto exists = [&present](const std::string& path) { return present.count(boost::filesystem::path(path).generic_string()) != 0; };

    Targets object;
    object.objects = {0};
    const ReloadSources all = reload_sources(model, object, exists);
    CHECK(all.parts == std::vector<std::pair<int, int>>{{0, 0}, {0, 4}});
    CHECK(generic(all.paths) == std::set<std::string>{"/models/there.stl", "/project/beside.stl"});
    CHECK(all.missing == std::vector<std::pair<int, int>>{{0, 1}});

    // Parts named are the only ones looked at.
    Targets gone;
    gone.parts = {{0, 1}, {0, 2}};
    const ReloadSources missing = reload_sources(model, gone, exists);
    CHECK(missing.parts.empty());
    CHECK(missing.paths.empty());
    CHECK(missing.missing == std::vector<std::pair<int, int>>{{0, 1}});
    Targets primitive;
    primitive.parts = {{0, 2}, {0, 3}};
    const ReloadSources none = reload_sources(model, primitive, exists);
    CHECK(none.parts.empty());
    CHECK(none.missing.empty());
}

TEST_CASE("a part is replaced with a file of one mesh, and a file of two objects or two parts is refused", "[Operations]")
{
    Slic3r::Model one;
    add_cubes(one, "one", 1, 1);
    CHECK_FALSE(replacement_refusal(one));

    Slic3r::Model two_objects;
    add_cubes(two_objects, "first", 1, 1);
    add_cubes(two_objects, "second", 1, 1);
    const std::optional<Refusal> objects = replacement_refusal(two_objects);
    CHECK(code_of(objects) == "several_parts");
    REQUIRE(objects);
    CHECK(objects->english() == "Unable to replace with more than one volume");

    Slic3r::Model two_parts;
    add_cubes(two_parts, "parts", 1, 2);
    CHECK(code_of(replacement_refusal(two_parts)) == "several_parts");

    // The part replaced: the one named, or the one part of an object named; an object of two parts names none.
    Slic3r::Model model;
    add_cubes(model, "single", 1, 1);
    add_cubes(model, "double", 1, 2);
    Targets part;
    part.parts = {{1, 1}};
    REQUIRE(replaced_part(model, part));
    CHECK(replaced_part(model, part).value() == std::make_pair(1, 1));
    Targets single;
    single.objects = {0};
    REQUIRE(replaced_part(model, single));
    CHECK(replaced_part(model, single).value() == std::make_pair(0, 0));
    Targets two;
    two.objects = {1};
    REQUIRE_FALSE(replaced_part(model, two));
    CHECK(std::string(replaced_part(model, two).refusal().code) == "several_parts");
}

// ---- The plate commands' checks, on a project's plate count ------------------------------------------------------

TEST_CASE("a plate operation refuses an index the project lacks as no_such_plate", "[Operations]")
{
    // A project of three plates, 0 to 2. Renaming, locking and selecting a plate check its index alone; deleting and
    // duplicating it check the index first.
    for (int plate : {-1, 3, 9}) {
        INFO(plate);
        CHECK(code_of(plate_index_refusal(plate, 3)) == "no_such_plate");
        CHECK(code_of(delete_plate_refusal(plate, 3)) == "no_such_plate");
        CHECK(code_of(duplicate_plate_refusal(plate, 3, false)) == "no_such_plate");
    }
    const std::optional<Refusal> missing = plate_index_refusal(9, 3);
    REQUIRE(missing);
    CHECK(missing->kind == RefusalKind::Precondition);
    CHECK(missing->english() == "Plate 9 does not exist (the project has plates 0..2)");
    for (int plate : {0, 2}) {
        INFO(plate);
        CHECK_FALSE(plate_index_refusal(plate, 3));
        CHECK_FALSE(delete_plate_refusal(plate, 3));
        CHECK_FALSE(duplicate_plate_refusal(plate, 3, false));
    }
}

TEST_CASE("the only plate is not deleted, and a plate is not duplicated past the most a project holds or while slicing", "[Operations]")
{
    const std::optional<Refusal> only = delete_plate_refusal(0, 1);
    CHECK(code_of(only) == "last_plate");
    REQUIRE(only);
    CHECK(only->english() == "Cannot delete the only plate.");
    CHECK_FALSE(delete_plate_refusal(0, 2));

    // A thousand plates are more than a project holds.
    const std::optional<Refusal> full = duplicate_plate_refusal(0, 1000, false);
    CHECK(code_of(full) == "too_many_plates");
    REQUIRE(full);
    CHECK(full->english() == "Cannot duplicate a plate (maximum reached).");
    // No plate is added while a slice runs; it says so, and to wait.
    const std::optional<Refusal> slicing = duplicate_plate_refusal(0, 2, true);
    CHECK(code_of(slicing) == "slicing_in_progress");
    REQUIRE(slicing);
    CHECK(slicing->kind == RefusalKind::Busy);
}

// ---- Filament slots, exports and imports: their checks, on the counts and names a project has ----------------------

TEST_CASE("a mixed filament mixes two or more of the project's filaments, each once and of one type, with one ratio each that "
          "add up to 100", "[Operations]")
{
    // A project of three PLA filaments that mix, 1 to 3.
    const std::vector<std::string> pla{"PLA", "PLA", "PLA"};
    CHECK_FALSE(mixed_filament_refusal(pla, {1, 2}, {50, 50}));
    CHECK_FALSE(mixed_filament_refusal(pla, {3, 1, 2}, {20, 30, 50}));
    CHECK_FALSE(mixed_filament_refusal(pla, {1, 3}, {12.5, 87.5}));

    const std::optional<Refusal> one = mixed_filament_refusal(pla, {2}, {100});
    CHECK(code_of(one) == "too_few_components");
    REQUIRE(one);
    CHECK(one->english() == "A mixed filament mixes two filaments or more.");
    CHECK(code_of(mixed_filament_refusal(pla, {}, {})) == "too_few_components");
    // A filament named twice, which the Add Mixed Filament dialog's selectors never offer, with two others or alone.
    for (const std::vector<int>& components : std::vector<std::vector<int>>{{2, 2}, {1, 2, 2}, {3, 1, 3}}) {
        INFO(components.size());
        CHECK(code_of(mixed_filament_refusal(pla, components, std::vector<double>(components.size(), 100. / 3))) ==
              "duplicate_component");
    }
    const std::optional<Refusal> twice = mixed_filament_refusal(pla, {1, 2, 2}, {40, 30, 30});
    REQUIRE(twice);
    CHECK(twice->english() == "Filament 2 is named twice: a mixed filament mixes each filament once.");

    for (const std::vector<int>& components : std::vector<std::vector<int>>{{1, 4}, {0, 1}, {2, -1}}) {
        INFO(components[0] << ", " << components[1]);
        CHECK(code_of(mixed_filament_refusal(pla, components, {50, 50})) == "no_such_slot");
    }
    const std::optional<Refusal> missing = mixed_filament_refusal(pla, {1, 4}, {50, 50});
    REQUIRE(missing);
    CHECK(missing->english() == "Filament 4 does not exist: the filaments that mix are 1 to 3.");

    // Filaments of one type: the dialog's OK takes no other mix, and the sidebar marks one broken.
    const std::vector<std::string> pla_petg{"PLA", "PETG", "PLA"};
    CHECK_FALSE(mixed_filament_refusal(pla_petg, {1, 3}, {50, 50}));
    const std::optional<Refusal> types = mixed_filament_refusal(pla_petg, {1, 2}, {50, 50});
    CHECK(code_of(types) == "mixed_types");
    REQUIRE(types);
    CHECK(types->english() == "Cannot mix different filament types");
    CHECK(types->names == std::vector<std::string>{"1: PLA", "2: PETG"});

    // A ratio per filament, as percentages that add up to 100.
    for (const std::vector<double>& ratios : std::vector<std::vector<double>>{{100}, {40, 50, 10}, {40, 50}, {60, 50}, {}, {0, 100}}) {
        INFO(ratios.size());
        CHECK(code_of(mixed_filament_refusal(pla, {1, 2}, ratios)) == "bad_ratios");
    }
    const std::optional<Refusal> ratios = mixed_filament_refusal(pla, {1, 2}, {40, 50});
    REQUIRE(ratios);
    CHECK(ratios->english() == "Give one ratio per filament, as percentages that add up to 100.");
}

TEST_CASE("a filament slot is deleted, or merged into another, unless it is the only one or names none", "[Operations]")
{
    // A project of three filament slots, 1 to 3.
    CHECK_FALSE(filament_delete_refusal(3, 3, std::nullopt));
    CHECK_FALSE(filament_delete_refusal(3, 1, 3));
    CHECK_FALSE(filament_delete_refusal(3, 2, 1));

    const std::optional<Refusal> only = filament_delete_refusal(1, 1, std::nullopt);
    CHECK(code_of(only) == "last_filament_slot");
    REQUIRE(only);
    CHECK(only->english() == "The only filament slot cannot be deleted.");

    for (int slot : {0, 4, -1}) {
        INFO(slot);
        CHECK(code_of(filament_delete_refusal(3, slot, std::nullopt)) == "no_such_slot");
    }
    const std::optional<Refusal> missing = filament_delete_refusal(3, 4, std::nullopt);
    REQUIRE(missing);
    CHECK(missing->english() == "Filament slot must be 1 to 3.");

    for (int into : {0, 4}) {
        INFO(into);
        CHECK(code_of(filament_delete_refusal(3, 2, into)) == "no_such_slot");
    }
    const std::optional<Refusal> nowhere = filament_delete_refusal(3, 2, 4);
    REQUIRE(nowhere);
    CHECK(nowhere->english() == "The slot to merge into must be 1 to 3.");

    const std::optional<Refusal> itself = filament_delete_refusal(3, 2, 2);
    CHECK(code_of(itself) == "merge_into_itself");
    REQUIRE(itself);
    CHECK(itself->english() == "A filament slot cannot be merged into itself.");
}

TEST_CASE("an export of some objects names objects the project has, in a format that writes objects", "[Operations]")
{
    Slic3r::Model empty;
    Slic3r::Model model;
    add_cubes(model, "first", 1, 1);
    add_cubes(model, "second", 2, 1);

    // A file per object needs objects to write.
    for (const char* format : {"stl", "drc"}) {
        INFO(format);
        CHECK(code_of(export_objects_refusal(format, {}, true, empty)) == "no_objects");
        CHECK_FALSE(export_objects_refusal(format, {}, true, model));
        CHECK_FALSE(export_objects_refusal(format, {1}, true, model));
        CHECK_FALSE(export_objects_refusal(format, {0, 1}, false, model));
    }
    const std::optional<Refusal> nothing = export_objects_refusal("stl", {}, true, empty);
    REQUIRE(nothing);
    CHECK(nothing->english() == "The project has no objects.");

    // The other formats write the whole model, a plate's slice or what the Preview shows.
    for (const char* format : {"gcode", "sliced_3mf", "3mf", "toolpaths_obj"}) {
        INFO(format);
        CHECK(code_of(export_objects_refusal(format, {0}, false, model)) == "objects_not_taken");
        CHECK_FALSE(export_objects_refusal(format, {}, false, model));
    }
    const std::optional<Refusal> whole = export_objects_refusal("3mf", {0}, false, model);
    REQUIRE(whole);
    CHECK(whole->english() == "Only an STL or a DRC export takes objects, not 3mf.");

    for (int object : {2, -1}) {
        INFO(object);
        CHECK(code_of(export_objects_refusal("stl", {0, object}, false, model)) == "no_such_object");
    }
    const std::optional<Refusal> missing = export_objects_refusal("drc", {5}, true, model);
    REQUIRE(missing);
    CHECK(missing->english() == "Object 5 does not exist (the project has objects 0..1)");
    CHECK(code_of(export_objects_refusal("stl", {0}, false, empty)) == "no_objects");
}

TEST_CASE("the files taken from a ZIP archive are model files it holds", "[Operations]")
{
    const std::vector<std::string> in_archive{"cube.stl", "parts/cylinder.obj", "parts/bracket.step"};
    CHECK_FALSE(zip_entries_refusal(in_archive, {}));
    CHECK_FALSE(zip_entries_refusal(in_archive, {"parts/cylinder.obj"}));
    CHECK_FALSE(zip_entries_refusal(in_archive, {"cube.stl", "parts/bracket.step"}));

    for (const std::vector<std::string>& asked :
         std::vector<std::vector<std::string>>{{"cylinder.obj"}, {"cube.stl", "readme.txt"}, {"CUBE.STL"}}) {
        INFO(asked.back());
        const std::optional<Refusal> refused = zip_entries_refusal(in_archive, asked);
        CHECK(code_of(refused) == "no_such_entry");
        REQUIRE(refused);
        CHECK(refused->english() == "The archive holds no model file " + asked.back() + ".");
        // With the names it has.
        CHECK(refused->names == in_archive);
    }
}

// ---- Printer models and system filaments ----------------------------------------------------------------

namespace {

// The models section of an installed set: vendor -> model -> nozzles.
using Printers = std::map<std::string, std::map<std::string, std::set<std::string>>>;

// An installed set of these printers and one filament.
InstallSet install_set(Printers printers)
{
    InstallSet set;
    set.printers  = std::move(printers);
    set.filaments = {{"Bambu PLA Basic @BBL X1C", "true"}};
    return set;
}

const std::string kX1C = "Bambu Lab X1 Carbon";

} // namespace

TEST_CASE("installing a printer model changes the named models and keeps the rest", "[Operations]")
{
    const InstallSet installed = install_set({{"BBL", {{kX1C, {"0.4"}}}}, {"Prusa", {{"Original Prusa MK4S", {"0.4", "0.6"}}}}});

    // A model of another vendor, with one nozzle, joins the two.
    InstallSet changed = changed_install_set(installed, {{"Goofoo", "Goofoo Velo+", {"0.4"}}}, {});
    CHECK(changed.printers == Printers{{"BBL", {{kX1C, {"0.4"}}}},
                                       {"Goofoo", {{"Goofoo Velo+", {"0.4"}}}},
                                       {"Prusa", {{"Original Prusa MK4S", {"0.4", "0.6"}}}}});
    // The filaments are not the models' to change.
    CHECK(changed.filaments == installed.filaments);

    // A second nozzle of an installed model keeps its first.
    changed = changed_install_set(installed, {{"BBL", kX1C, {"0.6"}}}, {});
    CHECK(changed.printers == Printers{{"BBL", {{kX1C, {"0.4", "0.6"}}}}, {"Prusa", {{"Original Prusa MK4S", {"0.4", "0.6"}}}}});

    // Removing one nozzle keeps the model's other.
    changed = changed_install_set(installed, {}, {{"Prusa", "Original Prusa MK4S", {"0.6"}}});
    CHECK(changed.printers == Printers{{"BBL", {{kX1C, {"0.4"}}}}, {"Prusa", {{"Original Prusa MK4S", {"0.4"}}}}});

    // Removing a model's last nozzle removes the model, and a vendor left with none goes with it.
    changed = changed_install_set(installed, {}, {{"BBL", kX1C, {"0.4"}}});
    CHECK(changed.printers == Printers{{"Prusa", {{"Original Prusa MK4S", {"0.4", "0.6"}}}}});

    // Removing a model that is not installed, or a nozzle of an installed one that is not, changes nothing.
    CHECK(changed_install_set(installed, {}, {{"Goofoo", "Goofoo Velo+", {"0.4"}}}).printers == installed.printers);
    CHECK(changed_install_set(installed, {}, {{"BBL", kX1C, {"0.8"}}}).printers == installed.printers);
    // Nor does the model of that name under another vendor.
    CHECK(changed_install_set(installed, {}, {{"Clone", kX1C, {"0.4"}}}).printers == installed.printers);
}

TEST_CASE("the printer models are every vendor's, with their nozzles and those installed", "[Operations]")
{
    // The Printer Selection dialog's data (printer_profile_data()): the models of two vendors, and a third vendor
    // with a model of the first one's name.
    const nlohmann::json profile = {
        {"model", nlohmann::json::array({{{"model", kX1C}, {"name", kX1C}, {"vendor", "BBL"}, {"nozzle_diameter", "0.4;0.6"}},
                                         {{"model", "Bambu Lab A1 mini"},
                                          {"name", "Bambu Lab A1 mini"},
                                          {"vendor", "BBL"},
                                          {"nozzle_diameter", "0.2;0.4"}},
                                         {{"model", "Goofoo Velo+"},
                                          {"name", "Goofoo Velo+"},
                                          {"vendor", "Goofoo"},
                                          {"nozzle_diameter", "0.2;0.4;0.6"}},
                                         {{"model", kX1C}, {"name", kX1C}, {"vendor", "Clone"}, {"nozzle_diameter", "0.4;0.6"}}})},
        {"machine", nlohmann::json::object()}};
    const InstallSet installed = install_set({{"BBL", {{kX1C, {"0.4"}}}}});

    const std::vector<PrinterModel> all = printer_models_from(profile, installed, "", "");
    REQUIRE(all.size() == 4);
    CHECK(all[0].vendor == "BBL");
    CHECK(all[0].model == kX1C);
    CHECK(all[0].nozzles == std::vector<std::string>{"0.4", "0.6"});
    CHECK(all[0].installed_nozzles == std::vector<std::string>{"0.4"});
    CHECK(all[1].model == "Bambu Lab A1 mini");
    CHECK(all[1].installed_nozzles.empty());
    CHECK(all[2].nozzles == std::vector<std::string>{"0.2", "0.4", "0.6"});
    // The model of that name under another vendor is not the one installed.
    CHECK(all[3].vendor == "Clone");
    CHECK(all[3].installed_nozzles.empty());

    // vendor keeps one vendor's models, in either case.
    const std::vector<PrinterModel> goofoo = printer_models_from(profile, installed, "goofoo", "");
    REQUIRE(goofoo.size() == 1);
    CHECK(goofoo[0].model == "Goofoo Velo+");
    CHECK(printer_models_from(profile, installed, "BBL", "").size() == 2);

    // query matches a model's name whatever its case.
    const std::vector<PrinterModel> carbon = printer_models_from(profile, installed, "bbl", "x1 CARBON");
    REQUIRE(carbon.size() == 1);
    CHECK(carbon[0].vendor == "BBL");
    CHECK(printer_models_from(profile, installed, "", "MINI").at(0).model == "Bambu Lab A1 mini");
    CHECK(printer_models_from(profile, installed, "", "carbon").size() == 2);
    CHECK(printer_models_from(profile, installed, "Goofoo", "carbon").empty());
}

TEST_CASE("uninstalling is refused the last printer's last nozzle and not one of two", "[Operations]")
{
    const InstallSet one = install_set({{"BBL", {{kX1C, {"0.4"}}}}});
    const std::optional<Refusal> last = uninstall_refusal(one, {{"BBL", kX1C, {"0.4"}}});
    CHECK(code_of(last) == "last_printer");
    REQUIRE(last);
    CHECK(last->english() == "At least one printer model must stay installed.");
    // Both nozzles of the only model.
    CHECK(code_of(uninstall_refusal(install_set({{"BBL", {{kX1C, {"0.4", "0.6"}}}}}), {{"BBL", kX1C, {"0.4", "0.6"}}})) ==
          "last_printer");

    // One of two nozzles, one of two models, and a model that is not installed.
    CHECK_FALSE(uninstall_refusal(install_set({{"BBL", {{kX1C, {"0.4", "0.6"}}}}}), {{"BBL", kX1C, {"0.4"}}}));
    CHECK_FALSE(uninstall_refusal(install_set({{"BBL", {{kX1C, {"0.4"}}}}, {"Goofoo", {{"Goofoo Velo+", {"0.4"}}}}}),
                                  {{"BBL", kX1C, {"0.4"}}}));
    CHECK_FALSE(uninstall_refusal(one, {{"Goofoo", "Goofoo Velo+", {"0.4"}}}));
}

TEST_CASE("the installed set of a configuration without a filaments section keeps every filament the selectors offer", "[Operations]")
{
    const std::vector<std::string> offered = {"Bambu PLA Basic @BBL X1C", "Generic PLA @System"};
    Slic3r::AppConfig              config;
    config.set_vendors(Printers{{"BBL", {{kX1C, {"0.4"}}}}});
    // No section installs every filament: the set a printer change writes back names those the selectors offer, so
    // that writing it hides none of them.
    REQUIRE_FALSE(config.has_section(Slic3r::AppConfig::SECTION_FILAMENTS));
    const InstallSet without = installed_set_from(config, offered);
    CHECK(without.printers == Printers{{"BBL", {{kX1C, {"0.4"}}}}});
    CHECK(without.filaments == std::map<std::string, std::string>{{"Bambu PLA Basic @BBL X1C", "true"}, {"Generic PLA @System", "true"}});
    // A printer installed into it keeps them.
    CHECK(changed_install_set(without, {{"Goofoo", "Goofoo Velo+", {"0.4"}}}, {}).filaments == without.filaments);

    // A section, even an empty one, is the set as it stands.
    config.set_section(Slic3r::AppConfig::SECTION_FILAMENTS, {{"Generic PLA @System", "true"}});
    CHECK(installed_set_from(config, offered).filaments == std::map<std::string, std::string>{{"Generic PLA @System", "true"}});
    config.set_section(Slic3r::AppConfig::SECTION_FILAMENTS, {});
    REQUIRE(config.has_section(Slic3r::AppConfig::SECTION_FILAMENTS));
    CHECK(installed_set_from(config, offered).filaments.empty());
}

TEST_CASE("a call names a printer model by its name, its vendor where two have it, and its nozzles", "[Operations]")
{
    const std::vector<PrinterModel> known = {{"BBL", kX1C, {"0.4", "0.6"}, {}},
                                             {"Clone", kX1C, {"0.4"}, {}},
                                             {"Goofoo", "Goofoo Velo+", {"0.2", "0.4", "0.6"}, {}}};

    // Without nozzles, all of the model's; with them, those; the vendor in either case.
    const Outcome<std::vector<ModelNozzles>> every = resolve_models(known, {{"", "Goofoo Velo+", {}}});
    REQUIRE(every);
    REQUIRE(every.value().size() == 1);
    CHECK(every.value()[0].vendor == "Goofoo");
    CHECK(every.value()[0].nozzles == std::vector<std::string>{"0.2", "0.4", "0.6"});
    const Outcome<std::vector<ModelNozzles>> named = resolve_models(known, {{"bbl", kX1C, {"0.6"}}});
    REQUIRE(named);
    CHECK(named.value()[0].vendor == "BBL");
    CHECK(named.value()[0].nozzles == std::vector<std::string>{"0.6"});

    // A name two vendors have needs its vendor, and the refusal names them.
    const Outcome<std::vector<ModelNozzles>> two = resolve_models(known, {{"", kX1C, {"0.4"}}});
    REQUIRE_FALSE(two);
    CHECK(two.refusal().code == std::string("ambiguous_model"));
    CHECK(two.refusal().names == std::vector<std::string>{"BBL", "Clone"});

    // An unknown model, vendor or nozzle, with the closest names; every model is checked, not only the first.
    const Outcome<std::vector<ModelNozzles>> typo = resolve_models(known, {{"", "Goofoo Velo+", {}}, {"", "Goofoo Velo", {}}});
    REQUIRE_FALSE(typo);
    CHECK(typo.refusal().code == std::string("no_such_model"));
    CHECK(typo.refusal().english() == "No printer model is called \"Goofoo Velo\".");
    REQUIRE_FALSE(typo.refusal().names.empty());
    CHECK(typo.refusal().names.front() == "Goofoo Velo+");
    const Outcome<std::vector<ModelNozzles>> elsewhere = resolve_models(known, {{"Goofoo", kX1C, {}}});
    REQUIRE_FALSE(elsewhere);
    CHECK(elsewhere.refusal().code == std::string("no_such_model"));
    CHECK(elsewhere.refusal().english() == "Goofoo has no printer model called \"Bambu Lab X1 Carbon\".");
    CHECK(elsewhere.refusal().names == std::vector<std::string>{"Goofoo Velo+"});
    const Outcome<std::vector<ModelNozzles>> vendor = resolve_models(known, {{"Bambu", kX1C, {}}});
    REQUIRE_FALSE(vendor);
    CHECK(vendor.refusal().code == std::string("no_such_vendor"));
    CHECK(vendor.refusal().english() == "No printer vendor is called \"Bambu\".");
    REQUIRE_FALSE(vendor.refusal().names.empty());
    CHECK(vendor.refusal().names.front() == "BBL");
    const Outcome<std::vector<ModelNozzles>> nozzle = resolve_models(known, {{"BBL", kX1C, {"0.4", "0.8"}}});
    REQUIRE_FALSE(nozzle);
    CHECK(nozzle.refusal().code == std::string("no_such_nozzle"));
    CHECK(nozzle.refusal().english() == "Bambu Lab X1 Carbon has no nozzle \"0.8\".");
    CHECK(nozzle.refusal().names == std::vector<std::string>{"0.4", "0.6"});
}

namespace {

// System filaments as the System Filaments dialog lists them: the Bambu PLA Basic of three printers, two of them
// installed; a Generic PLA whose name is another preset's short name; and a filament no installed printer fits.
const std::vector<FilamentRow> kFilaments = {
    {"Bambu PLA Basic @BBL X1C", "Bambu PLA Basic", "Bambu Lab", "PLA", true},
    {"Bambu PLA Basic @BBL A1", "Bambu PLA Basic", "Bambu Lab", "PLA", true},
    {"Bambu PLA Basic @BBL P1P", "Bambu PLA Basic", "Bambu Lab", "PLA", false},
    {"Bambu PETG HF @BBL X1C", "Bambu PETG HF", "Bambu Lab", "PETG", true},
    {"Generic PLA @System", "Generic PLA", "Generic", "PLA", true},
    {"Generic PLA", "Generic PLA", "Generic", "PLA", true},
    {"Polymaker PLA @P1P", "Polymaker PLA", "Polymaker", "PLA", false},
};

InstallFilamentsArgs named(std::vector<std::string> names)
{
    InstallFilamentsArgs args;
    args.names = std::move(names);
    return args;
}

InstallFilamentsArgs of_type(std::string vendor, std::string type)
{
    InstallFilamentsArgs args;
    args.vendor        = std::move(vendor);
    args.filament_type = std::move(type);
    return args;
}

} // namespace

TEST_CASE("a filament's short name stands for its preset of each printer it fits, and a preset's name for itself", "[Operations]")
{
    using Names = std::vector<std::string>;
    // The short name the selector shows: the presets of the installed printers, not the P1P's.
    CHECK(filament_presets_named(kFilaments, "Bambu PLA Basic") == Names{"Bambu PLA Basic @BBL X1C", "Bambu PLA Basic @BBL A1"});
    // A preset's own name: that preset, whether an installed printer fits it or not.
    CHECK(filament_presets_named(kFilaments, "Bambu PLA Basic @BBL P1P") == Names{"Bambu PLA Basic @BBL P1P"});
    CHECK(filament_presets_named(kFilaments, "Generic PLA") == Names{"Generic PLA"});
    CHECK(filament_presets_named(kFilaments, "Generic PLA @System") == Names{"Generic PLA @System"});
    // A short name of a filament no installed printer fits, and a name in another case, stand for nothing.
    CHECK(filament_presets_named(kFilaments, "Polymaker PLA").empty());
    CHECK(filament_presets_named(kFilaments, "bambu pla basic").empty());

    // A call's names, each preset once.
    const Outcome<Names> both = filaments_named(kFilaments, named({"Bambu PLA Basic", "Bambu PETG HF", "Bambu PLA Basic @BBL A1"}));
    REQUIRE(both);
    CHECK(both.value() == Names{"Bambu PLA Basic @BBL X1C", "Bambu PLA Basic @BBL A1", "Bambu PETG HF @BBL X1C"});
    // A vendor and a type, in any case: every preset of them that fits an installed printer.
    const Outcome<Names> bambu_pla = filaments_named(kFilaments, of_type("bambu lab", "pla"));
    REQUIRE(bambu_pla);
    CHECK(bambu_pla.value() == Names{"Bambu PLA Basic @BBL X1C", "Bambu PLA Basic @BBL A1"});
}

TEST_CASE("a filament name that matches nothing is refused with the closest names", "[Operations]")
{
    const Outcome<std::vector<std::string>> typo = filaments_named(kFilaments, named({"Bambu PETG HF", "Bambu PLA Basik"}));
    REQUIRE_FALSE(typo);
    CHECK(typo.refusal().code == std::string("no_such_filament"));
    CHECK(typo.refusal().english() == "No system filament is called \"Bambu PLA Basik\".");
    REQUIRE_FALSE(typo.refusal().names.empty());
    CHECK(typo.refusal().names.front() == "Bambu PLA Basic");

    // A short name no installed printer fits, with its presets.
    const Outcome<std::vector<std::string>> unfit = filaments_named(kFilaments, named({"Polymaker PLA"}));
    REQUIRE_FALSE(unfit);
    CHECK(unfit.refusal().code == std::string("fits_no_printer"));
    CHECK(unfit.refusal().names == std::vector<std::string>{"Polymaker PLA @P1P"});

    // A vendor without the type, with the types it has; a vendor no filament has, with the closest vendors.
    const Outcome<std::vector<std::string>> tpu = filaments_named(kFilaments, of_type("Bambu Lab", "TPU"));
    REQUIRE_FALSE(tpu);
    CHECK(tpu.refusal().code == std::string("no_matching_filament"));
    CHECK(tpu.refusal().english() == "No system filament of Bambu Lab of type TPU fits an installed printer.");
    CHECK(tpu.refusal().names == std::vector<std::string>{"PETG", "PLA"});
    const Outcome<std::vector<std::string>> vendor = filaments_named(kFilaments, of_type("Bambu", "PLA"));
    REQUIRE_FALSE(vendor);
    CHECK(vendor.refusal().code == std::string("no_filament_vendor"));
    REQUIRE_FALSE(vendor.refusal().names.empty());
    CHECK(vendor.refusal().names.front() == "Bambu Lab");
}

TEST_CASE("uninstalling a filament is refused the preset a slot uses, naming the slot", "[Operations]")
{
    const std::vector<std::string> slots = {"Bambu PLA Basic @BBL X1C", "Generic PLA @System"};
    const std::optional<Refusal> first = filament_uninstall_refusal({"Bambu PETG HF @BBL X1C", "Bambu PLA Basic @BBL X1C"}, slots);
    CHECK(code_of(first) == "filament_in_use");
    REQUIRE(first);
    CHECK(first->english() == "Filament slot 1 uses Bambu PLA Basic @BBL X1C.");
    const std::optional<Refusal> second = filament_uninstall_refusal({"Generic PLA @System"}, slots);
    REQUIRE(second);
    CHECK(second->english() == "Filament slot 2 uses Generic PLA @System.");
    // One no slot uses.
    CHECK_FALSE(filament_uninstall_refusal({"Bambu PETG HF @BBL X1C"}, slots));
}

// ---- Preferences ----------------------------------------------------------------------------------------

TEST_CASE("the preferences table names each key once, with a kind and a tab", "[Operations]")
{
    const std::set<std::string> tabs{"general", "files", "behaviour", "control", "graphics", "online", "developer"};
    // The setting that lets Python scripts run unrestricted, by the key python_scripts_setting() reads.
    Slic3r::AppConfig python;
    python.set_bool("python_scripts_unrestricted", true);
    REQUIRE(Slic3r::GUI::Mcp::python_scripts_setting(python));

    std::set<std::string> keys;
    for (const Preference& p : preferences()) {
        INFO(p.key);
        CHECK(keys.insert(p.key).second);
        CHECK(tabs.count(p.tab) == 1);
        CHECK_FALSE(p.section.empty());
        CHECK_FALSE(p.label.empty());
        if (p.kind == PreferenceKind::Choice) {
            std::set<std::string> values;
            for (const auto& [value, label] : p.choices) {
                CHECK(values.insert(value).second);
                CHECK_FALSE(label.empty());
            }
            // Only the two whose entries the running app lists have none, and a call may write neither.
            if (p.choices.empty()) {
                CHECK(std::set<std::string>{"language", "network_plugin_version"}.count(p.key) == 1);
                CHECK(p.refused != nullptr);
            } else
                CHECK(p.choices.size() >= 2);
            // A row the dialog builds from the table shows its first choice where nothing is stored.
            if (!p.own_row)
                CHECK(values.count(p.fallback) == 1);
        }
        if (p.kind == PreferenceKind::Number) {
            CHECK(p.min.has_value());
            if (p.min && p.max)
                CHECK(*p.min < *p.max);
            // A row the dialog builds from the table is a spin control or a decimal field, with both bounds.
            if (!p.own_row)
                CHECK(p.max.has_value());
        }
        // Its key is not the Python scripts' setting.
        Slic3r::AppConfig config;
        config.set_bool(p.key, true);
        CHECK_FALSE(Slic3r::GUI::Mcp::python_scripts_setting(config));
    }
    const Preference* language = find_preference(preferences(), "language");
    REQUIRE(language != nullptr);
    CHECK(language->tab == "general");
    REQUIRE(language->refused != nullptr);
    // The checking function refuses the Python scripts' setting as a key the table lacks.
    const std::optional<Refusal> python_refused = check_preferences(preferences(), {{"python_scripts_unrestricted", true}});
    CHECK(code_of(python_refused) == "not_a_preference");
    REQUIRE(python_refused);
    CHECK(python_refused->english() == "python_scripts_unrestricted is not a setting of the Preferences dialog.");
}

TEST_CASE("a preference whose control has a side effect is written, and one whose control asks first or moves this server is refused",
          "[Operations]")
{
    // Preset sync and stealth mode run their control's effect (apply_preference()).
    for (const char* key : {"sync_user_preset", "stealth_mode"}) {
        INFO(key);
        const Preference* p = find_preference(preferences(), key);
        REQUIRE(p != nullptr);
        CHECK(p->refused == nullptr);
    }
    CHECK_FALSE(check_preferences(preferences(), {{"stealth_mode", true}, {"sync_user_preset", false}}));
    // The mixed-temperature switch asks to confirm a warning; the MCP server's switch and port end or move the
    // connection of the call that asks.
    for (const char* key : {"enable_high_low_temp_mixed_printing", "mcp_enabled", "mcp_port"}) {
        INFO(key);
        const Preference* p = find_preference(preferences(), key);
        REQUIRE(p != nullptr);
        CHECK(p->refused != nullptr);
    }
    CHECK(code_of(check_preferences(preferences(), {{"stealth_mode", true}, {"mcp_enabled", false}})) == "preference_refused");
}

TEST_CASE("the rows the Preferences dialog marks as needing a restart are flagged as read at the next start", "[Operations]")
{
    std::set<std::string> flagged;
    for (const Preference& p : preferences())
        if (p.restart_needed)
            flagged.insert(p.key);
    std::set<std::string> expected = {"enable_multi_machine", SETTING_OPENGL_AA_SAMPLES, SETTING_USE_ENCRYPTED_TOKEN_FILE};
#ifdef SLIC3R_CAD
    expected.insert("enable_cad_feature");
#endif
#ifdef __linux__
    expected.insert({"window_buttons_on_left", "prefer_discrete_gpu"});
#endif
    CHECK(flagged == expected);
    // Each is a value a call may write.
    for (const std::string& key : flagged) {
        INFO(key);
        CHECK(find_preference(preferences(), key)->refused == nullptr);
    }
}

namespace {

// A table of five rows, as the Preferences dialog's table holds them.
std::vector<Preference> preference_rows()
{
    Preference arrange;
    arrange.key      = "auto_arrange";
    arrange.tab      = "behaviour";
    arrange.section  = "Prepare";
    arrange.label    = "Auto arrange plate after cloning";
    arrange.kind     = PreferenceKind::Checkbox;
    arrange.fallback = "false";
    Preference flush = arrange;
    flush.key        = "auto_calculate_flush";
    flush.label      = "Auto flush after changing...";
    flush.kind       = PreferenceKind::Choice;
    flush.choices    = {{"all", "All"}, {"color change", "Color"}, {"disabled", "None"}};
    flush.fallback   = "all";
    Preference fps   = arrange;
    fps.section      = "FPS";
    fps.tab          = "graphics";
    fps.key          = "opengl_fps_cap";
    fps.label        = "FPS cap";
    fps.kind         = PreferenceKind::Number;
    fps.min          = 0;
    fps.max          = 240;
    fps.fallback     = "";
    Preference language = flush;
    language.tab        = "general";
    language.key        = "language";
    language.label      = "Language";
    language.choices    = {};
    language.refused    = "%1%: changing the language restarts OrcaSlicer.";
    Preference downloads = arrange;
    downloads.tab        = "files";
    downloads.section    = "Files";
    downloads.key        = "download_path";
    downloads.label      = "Downloads folder";
    downloads.kind       = PreferenceKind::Text;
    downloads.folder     = true;
    downloads.fallback   = "";
    return {arrange, flush, fps, language, downloads};
}

} // namespace

TEST_CASE("a write of several preferences is checked whole before any is written", "[Operations]")
{
    const std::vector<Preference> table = preference_rows();
    // Good keys and values pass.
    CHECK_FALSE(check_preferences(table, {{"auto_arrange", false}, {"auto_calculate_flush", "color change"}, {"opengl_fps_cap", 60}}));

    // One refused key among good ones refuses the call, naming it, whether it comes before or after them.
    std::optional<Refusal> refused = check_preferences(table, {{"auto_arrange", true}, {"language", "de"}});
    CHECK(code_of(refused) == "preference_refused");
    REQUIRE(refused);
    CHECK(refused->english() == "language: changing the language restarts OrcaSlicer.");
    CHECK(refused->names == std::vector<std::string>{"language"});
    refused = check_preferences(table, {{"language", "de"}, {"opengl_fps_cap", 60}});
    CHECK(code_of(refused) == "preference_refused");

    // A value outside a choice's values names the values.
    refused = check_preferences(table, {{"auto_arrange", true}, {"auto_calculate_flush", "colour"}});
    CHECK(code_of(refused) == "no_such_choice");
    REQUIRE(refused);
    CHECK(refused->english() == "auto_calculate_flush has no choice \"colour\".");
    CHECK(refused->names == std::vector<std::string>{"all", "color change", "disabled"});

    // A number outside its range names the range, and a number field takes no fraction where it takes whole numbers.
    refused = check_preferences(table, {{"opengl_fps_cap", 300}});
    CHECK(code_of(refused) == "out_of_range");
    REQUIRE(refused);
    CHECK(refused->english() == "opengl_fps_cap takes a number from 0 to 240.");
    CHECK(refused->names == std::vector<std::string>{"0", "240"});
    CHECK(code_of(check_preferences(table, {{"opengl_fps_cap", 59.5}})) == "not_a_whole_number");
    CHECK(code_of(check_preferences(table, {{"opengl_fps_cap", "60"}})) == "not_a_number");

    // A checkbox takes true or false, not their text.
    CHECK(code_of(check_preferences(table, {{"auto_arrange", "true"}})) == "not_a_boolean");

    // A folder's text takes the absolute path of an existing folder; a number, a missing folder, a file or a relative
    // path is refused, naming the text it was given.
    const std::string folder = std::string(TEST_DATA_DIR);
    CHECK_FALSE(check_preferences(table, {{"download_path", folder}}));
    refused = check_preferences(table, {{"download_path", 5}});
    CHECK(code_of(refused) == "not_a_text");
    REQUIRE(refused);
    CHECK(refused->english() == "download_path takes a text.");
    const std::string missing = folder + "/no_such_folder";
    refused = check_preferences(table, {{"auto_arrange", true}, {"download_path", missing}});
    CHECK(code_of(refused) == "not_a_folder");
    REQUIRE(refused);
    CHECK(refused->english() == "download_path takes the absolute path of an existing folder, which \"" + missing + "\" is not.");
    CHECK(code_of(check_preferences(table, {{"download_path", folder + "/20mm_cube.obj"}})) == "not_a_folder");
    CHECK(code_of(check_preferences(table, {{"download_path", "data"}})) == "not_a_folder");
    CHECK(code_of(check_preferences(table, {{"download_path", ""}})) == "not_a_folder");

    // A key the table lacks is refused with the closest keys.
    refused = check_preferences(table, {{"auto_arange", true}});
    CHECK(code_of(refused) == "not_a_preference");
    REQUIRE(refused);
    REQUIRE_FALSE(refused->names.empty());
    CHECK(refused->names.front() == "auto_arrange");
}

TEST_CASE("a preference's value is stored as its control stores it and read back by its kind", "[Operations]")
{
    std::vector<Preference> table = preference_rows();
    const Preference& arrange = table[0];
    CHECK(preference_text(arrange, true) == "true");
    CHECK(preference_text(arrange, false) == "false");
    CHECK(preference_value(arrange, "true") == true);
    CHECK(preference_value(arrange, "1") == true);
    CHECK(preference_value(arrange, "") == false);
    // The dark mode's checkbox stores 1 or 0, which its fallback says.
    Preference dark_mode = arrange;
    dark_mode.fallback   = "0";
    CHECK(preference_text(dark_mode, true) == "1");
    CHECK(preference_text(dark_mode, false) == "0");

    const Preference& flush = table[1];
    CHECK(preference_text(flush, "disabled") == "disabled");
    CHECK(preference_value(flush, "disabled") == "disabled");

    // A whole number as the spin control writes it; a decimal field's with its decimals, as wxString::FromCDouble().
    const Preference& fps = table[2];
    CHECK(preference_text(fps, 60) == "60");
    CHECK(preference_value(fps, "60") == 60);
    CHECK(preference_value(fps, "60").is_number_integer());
    Preference orbit = fps;
    orbit.decimals   = 2;
    CHECK(preference_text(orbit, 0.5) == "0.50");
    CHECK(preference_text(orbit, 1) == "1.00");
    CHECK(preference_value(orbit, "1.0") == 1.0);
    // Text no number reads as itself.
    CHECK(preference_value(orbit, "fast") == "fast");

    // A text is stored and read as it is.
    const Preference& downloads = table[4];
    CHECK(preference_text(downloads, "C:/Downloads") == "C:/Downloads");
    CHECK(preference_value(downloads, "C:/Downloads") == "C:/Downloads");
}

// ---- Syncing the filaments from the AMS ---------------------------------------------------------------------

TEST_CASE("the filaments sync from the AMS of a connected printer of the selected model, and map only for a project with objects",
          "[Operations]")
{
    AmsSyncState ready;
    ready.printer             = true;
    ready.printer_name        = "Lab X1C";
    ready.selected_printer    = "Bambu Lab X1 Carbon";
    ready.slots_listed        = true;
    ready.printer_answered    = true;
    ready.filament_loaded     = true;
    ready.filament_installed  = true;
    ready.same_printer        = true;
    ready.project_has_objects = true;
    CHECK_FALSE(ams_sync_refusal(ready, AmsSyncMode::Map));
    CHECK_FALSE(ams_sync_refusal(ready, AmsSyncMode::Overwrite));
    CHECK_FALSE(ams_sync_refusal(ready, std::nullopt));

    const auto code = [](const AmsSyncState& state, std::optional<AmsSyncMode> mode) {
        const std::optional<Refusal> refusal = ams_sync_refusal(state, mode);
        return refusal ? std::string(refusal->code) : std::string("(none)");
    };
    AmsSyncState none;
    CHECK(code(none, AmsSyncMode::Overwrite) == "no_printer");
    CHECK(ams_sync_refusal(none, std::nullopt)->english() == "Connect a printer to synchronize the AMS filament list.");

    // A printer that answered with no slot has no filament system; one that did not answer is not connected.
    AmsSyncState no_slots = ready;
    no_slots.slots_listed = false;
    CHECK(code(no_slots, AmsSyncMode::Overwrite) == "no_filament_system");
    CHECK_THAT(ams_sync_refusal(no_slots, AmsSyncMode::Overwrite)->english(), Catch::Matchers::ContainsSubstring("Lab X1C is connected"));
    no_slots.printer_answered = false;
    CHECK(code(no_slots, AmsSyncMode::Overwrite) == "not_connected");
    CHECK_THAT(ams_sync_refusal(no_slots, AmsSyncMode::Overwrite)->english(), Catch::Matchers::ContainsSubstring("Bambu Lab X1 Carbon"));

    AmsSyncState empty = ready;
    empty.filament_loaded = false;
    CHECK(code(empty, AmsSyncMode::Map) == "unknown_filaments");
    empty.filament_installed = false;
    CHECK(code(empty, AmsSyncMode::Map) == "no_filament");

    AmsSyncState other = ready;
    other.same_printer = false;
    CHECK(code(other, AmsSyncMode::Overwrite) == "other_printer");

    // With no objects the sync dialog does not ask: the list is overwritten, which a call of map is not.
    AmsSyncState no_objects = ready;
    no_objects.project_has_objects = false;
    CHECK(code(no_objects, AmsSyncMode::Map) == "nothing_to_map");
    CHECK(code(no_objects, AmsSyncMode::Overwrite) == "(none)");
    CHECK(code(no_objects, std::nullopt) == "(none)");
}

// ---- Plugins ------------------------------------------------------------------------------------------------

namespace {

// The status of `ticket` once it is no longer pending, or Pending after ten seconds.
RefreshRun::Status settled(const RefreshRun& run, RefreshRun::Ticket ticket)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    RefreshRun::Status status = run.status(ticket);
    while (status.state == RefreshRun::State::Pending && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        status = run.status(ticket);
    }
    return status;
}

Slic3r::PluginDescriptor local_plugin(const std::string& key, const std::string& name, const std::string& version)
{
    Slic3r::PluginDescriptor plugin;
    plugin.plugin_key     = key;
    plugin.name           = name;
    plugin.version        = version;
    plugin.plugin_root    = "C:/plugins/" + key;
    plugin.entry_path     = plugin.plugin_root + "/" + key + ".py";
    plugin.metadata_valid = true;
    return plugin;
}

Slic3r::PluginDescriptor cloud_plugin(const std::string& key, const std::string& name, bool installed)
{
    Slic3r::PluginDescriptor plugin;
    plugin.plugin_key     = key;
    plugin.name           = name;
    plugin.version        = "2.0.0";
    plugin.latest_version = "2.1.0";
    plugin.metadata_valid = true;
    plugin.cloud          = Slic3r::CloudPluginState{};
    plugin.cloud->uuid    = key;
    if (installed) {
        plugin.cloud->installed  = true;
        plugin.installed_version = "1.0.0";
        plugin.plugin_root       = "C:/plugins/" + key;
    }
    return plugin;
}

} // namespace

TEST_CASE("a plugin refresh is pending until its work returns", "[Operations]")
{
    RefreshRun run;
    // The case holds the first work until it releases it. Each work notes when it begins and when it returns, so
    // the case reads the order on its own thread.
    std::promise<void>       release;
    std::shared_future<void> held = release.get_future().share();
    std::mutex               events_mutex;
    std::vector<std::string> events;
    const auto note = [&events_mutex, &events](const char* event) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.emplace_back(event);
    };
    const auto noted = [&events_mutex, &events] {
        std::lock_guard<std::mutex> lock(events_mutex);
        return events;
    };
    const RefreshRun::Ticket first = run.start([held, note] {
        note("first begins");
        held.wait();
        note("first returns");
    });
    CHECK(run.status(first).state == RefreshRun::State::Pending);
    // A second refresh while the first runs is a run of its own, which waits for the first.
    const RefreshRun::Ticket second = run.start([note] { note("second begins"); });
    CHECK(second != first);
    std::this_thread::sleep_for(50ms);
    CHECK(run.status(first).state == RefreshRun::State::Pending);
    CHECK(run.status(second).state == RefreshRun::State::Pending);
    CHECK(noted() == std::vector<std::string>{"first begins"});
    release.set_value();
    CHECK(settled(run, first).state == RefreshRun::State::Done);
    CHECK(settled(run, second).state == RefreshRun::State::Done);
    CHECK(noted() == std::vector<std::string>{"first begins", "first returns", "second begins"});

    // A work that throws leaves its run failed, with what it threw, and the work asked for after it still runs.
    std::promise<void>       release_failing;
    std::shared_future<void> failing_held = release_failing.get_future().share();
    const RefreshRun::Ticket third        = run.start([failing_held] {
        failing_held.wait();
        throw std::runtime_error("the plugin hub did not answer");
    });
    const RefreshRun::Ticket fourth       = run.start([note] { note("fourth begins"); });
    CHECK(third != second);
    CHECK(fourth != third);
    release_failing.set_value();
    const RefreshRun::Status failed = settled(run, third);
    CHECK(failed.state == RefreshRun::State::Failed);
    CHECK(failed.error == "the plugin hub did not answer");
    CHECK(settled(run, fourth).state == RefreshRun::State::Done);
    CHECK(noted().back() == "fourth begins");
    // The first run's status stays as it ended, and a ticket never given is no run.
    CHECK(run.status(first).state == RefreshRun::State::Done);
    CHECK(run.status(RefreshRun::Ticket{}).state == RefreshRun::State::Failed);

    // An object that goes away while one work runs and another waits leaves both to run, in turn.
    auto                     order       = std::make_shared<std::vector<int>>();
    auto                     order_mutex = std::make_shared<std::mutex>();
    std::promise<void>       release_last;
    std::shared_future<void> last_held = release_last.get_future().share();
    std::promise<void>       last_ran;
    std::future<void>        last_done = last_ran.get_future();
    {
        RefreshRun gone;
        gone.start([last_held, order, order_mutex] {
            last_held.wait();
            std::lock_guard<std::mutex> lock(*order_mutex);
            order->push_back(1);
        });
        gone.start([order, order_mutex, done = std::make_shared<std::promise<void>>(std::move(last_ran))] {
            {
                std::lock_guard<std::mutex> lock(*order_mutex);
                order->push_back(2);
            }
            done->set_value();
        });
    }
    release_last.set_value();
    REQUIRE(last_done.wait_for(10s) == std::future_status::ready);
    std::lock_guard<std::mutex> lock(*order_mutex);
    CHECK(*order == std::vector<int>{1, 2});
}

TEST_CASE("a refresh's ticket reads how its own run ended however many refreshes end after it", "[Operations]")
{
    RefreshRun               run;
    const RefreshRun::Ticket returned = run.start([] {});
    const RefreshRun::Ticket threw    = run.start([] { throw std::runtime_error("the plugin hub did not answer"); });
    RefreshRun::Ticket       last;
    for (int i = 0; i < 200; ++i)
        last = run.start([] {});
    REQUIRE(settled(run, last).state == RefreshRun::State::Done);
    CHECK(run.status(returned).state == RefreshRun::State::Done);
    const RefreshRun::Status failed = run.status(threw);
    CHECK(failed.state == RefreshRun::State::Failed);
    CHECK(failed.error == "the plugin hub did not answer");
}

TEST_CASE("a wait that last saw its refresh pending returns how it ended, however many refreshes end meanwhile",
          "[Operations]")
{
    RefreshRun               run;
    std::promise<void>       release_run;
    std::shared_future<void> run_held = release_run.get_future().share();
    const RefreshRun::Ticket ticket   = run.start([run_held] { run_held.wait(); });
    // The waiter is held in its `cancelled`, which it asks after it has seen the run pending, until the case lets it go.
    std::promise<void>       asked;
    std::future<void>        waiter_held = asked.get_future();
    std::promise<void>       release_waiter;
    std::shared_future<void> waiter_go = release_waiter.get_future().share();
    std::atomic<bool>        first{true};
    auto                     cancelled = [&asked, &first, waiter_go] {
        if (first.exchange(false)) {
            asked.set_value();
            waiter_go.wait();
        }
        return false;
    };
    auto waited = std::async(std::launch::async, [&] { return wait_for_refresh(run, ticket, cancelled, 1ms); });
    REQUIRE(waiter_held.wait_for(5s) == std::future_status::ready);
    // Its run and two hundred more end while it is held.
    release_run.set_value();
    RefreshRun::Ticket last;
    for (int i = 0; i < 200; ++i)
        last = run.start([] {});
    REQUIRE(settled(run, last).state == RefreshRun::State::Done);
    release_waiter.set_value();
    REQUIRE(waited.wait_for(5s) == std::future_status::ready);
    const std::optional<RefreshRun::Status> status = waited.get();
    REQUIRE(status.has_value());
    CHECK(status->state == RefreshRun::State::Done);
}

TEST_CASE("a wait for a plugin refresh stops when its call is cancelled, and the refresh goes on", "[Operations]")
{
    RefreshRun               run;
    std::promise<void>       release;
    std::shared_future<void> held   = release.get_future().share();
    const RefreshRun::Ticket ticket = run.start([held] { held.wait(); });
    std::atomic<bool>        cancel{false};
    auto waited = std::async(std::launch::async, [&] { return wait_for_refresh(run, ticket, [&cancel] { return cancel.load(); }, 10ms); });
    // While the refresh runs and nothing cancels, the wait goes on.
    CHECK(waited.wait_for(200ms) == std::future_status::timeout);
    cancel = true;
    REQUIRE(waited.wait_for(5s) == std::future_status::ready);
    // Stopped, while the refresh still runs.
    CHECK_FALSE(waited.get().has_value());
    CHECK(run.status(ticket).state == RefreshRun::State::Pending);
    release.set_value();
    CHECK(settled(run, ticket).state == RefreshRun::State::Done);

    // A wait on a refresh that has ended returns its status, Done or Failed with its text.
    const std::optional<RefreshRun::Status> done = wait_for_refresh(run, ticket, [] { return false; }, 10ms);
    REQUIRE(done.has_value());
    CHECK(done->state == RefreshRun::State::Done);
    const RefreshRun::Ticket                failing = run.start([] { throw std::runtime_error("the plugin hub did not answer"); });
    const std::optional<RefreshRun::Status> failed  = wait_for_refresh(run, failing, [] { return false; }, 10ms);
    REQUIRE(failed.has_value());
    CHECK(failed->state == RefreshRun::State::Failed);
    CHECK(failed->error == "the plugin hub did not answer");
}

TEST_CASE("the plugin list gives each plugin's key, name, version, state, capabilities and error", "[Operations]")
{
    Slic3r::PluginDescriptor loaded = local_plugin("loaded_key", "Loaded", "1.2.0");
    Slic3r::PluginDescriptor broken = local_plugin("broken_key", "Broken", "0.1.0");
    broken.error                    = "  the package has no entry point \n";
    Slic3r::PluginDescriptor loading = local_plugin("loading_key", "Loading", "3.0.0");
    Slic3r::PluginDescriptor offered = cloud_plugin("cloud_offered", "Offered", false);
    Slic3r::PluginDescriptor mine    = cloud_plugin("cloud_mine", "Mine", true);
    mine.cloud->is_mine              = true;

    std::map<std::string, PluginRuntime> runtime;
    runtime["loaded_key"].loaded       = true;
    runtime["loaded_key"].capabilities = {{"Run it", "script", true}, {"Read it", "importer", false}};
    runtime["loading_key"].loading     = true;

    const std::vector<PluginRow> rows = plugin_rows_from({loaded, broken, loading, offered, mine}, runtime);
    REQUIRE(rows.size() == 5);

    CHECK(rows[0].key == "loaded_key");
    CHECK(rows[0].name == "Loaded");
    CHECK(rows[0].version == "1.2.0");
    CHECK(rows[0].installed);
    CHECK(rows[0].enabled);
    CHECK(rows[0].status == "Activated");
    CHECK(rows[0].source == "local");
    CHECK(rows[0].error.empty());
    REQUIRE(rows[0].capabilities.size() == 2);
    CHECK(rows[0].capabilities[0].name == "Run it");
    CHECK(rows[0].capabilities[0].type == "script");
    CHECK(rows[0].capabilities[0].enabled);
    CHECK(rows[0].capabilities[1].name == "Read it");
    CHECK(rows[0].capabilities[1].type == "importer");
    CHECK_FALSE(rows[0].capabilities[1].enabled);

    // An error is the row's, trimmed, and a plugin not loaded has no capabilities.
    CHECK(rows[1].key == "broken_key");
    CHECK(rows[1].installed);
    CHECK_FALSE(rows[1].enabled);
    CHECK(rows[1].status == "Error");
    CHECK(rows[1].error == "the package has no entry point");
    CHECK(rows[1].capabilities.empty());

    CHECK(rows[2].status == "Loading");
    CHECK_FALSE(rows[2].enabled);

    // A cloud plugin not installed shows the latest version offered; one installed, the version on disk.
    CHECK(rows[3].key == "cloud_offered");
    CHECK_FALSE(rows[3].installed);
    CHECK(rows[3].version == "2.1.0");
    CHECK(rows[3].source == "subscribed");
    CHECK(rows[3].status == "Inactive");
    CHECK(rows[4].installed);
    CHECK(rows[4].version == "1.0.0");
    CHECK(rows[4].source == "mine");
}

// ---- The catalog names its operations ------------------------------------------------------------------
//
// A command that changes something either names the operations (GUI/Operations) it reaches or is listed
// here with the reason it reaches none. The list is what a new command has to be added to, or kept off, in
// plain sight. It says how an entry is classified; it does not check what its function calls, which
// scripts/check_operation_calls.py does for the owner calls an operation stands for.

namespace {

// Whether `command` is under the rule: it changes something, and it is not the Design tab's, whose commands
// act on the tab's document, which no operation stands for. Listed or not.
bool under_operations_rule(const Slic3r::GUI::NativeCommand& command)
{
    return !command.read_only && command.group != Slic3r::GUI::kDesignGroup;
}

// Every function of GUI/Operations a catalog entry may name.
const std::set<std::string> kOperations = {
    "start_gcode_export", "export_sliced_3mf", "export_model_3mf",       "export_stl",      "open_print_dialog",
    "import_models",      "open_project",      "new_project",            "save_project_as", "set_instance_count",
    "select_plate",       "set_plate_locked",  "start_arrange",          "start_slice",     "show_layer_range",
    "resolve_preset",     "select_preset",     "assign_filament_preset", "import_presets",  "delete_objects",
    "rename_object",      "separate_instances", "split_object",          "assemble_objects", "change_part_type",
    "order_objects",      "mirror_objects",     "center_objects",        "scale_to_fit",     "drop_objects",
    "orient_objects",     "add_primitive",      "delete_plate",          "duplicate_plate",  "rename_plate",
    "set_objects_printable", "set_objects_auto_drop", "fix_objects", "clone_objects", "reload_parts", "replace_part",
    "add_filament_slot",  "delete_filament_slot", "add_mixed_filament",   "edit_mixed_filament", "sync_presets",
    "export_toolpaths_obj",
    "printer_models",     "install_printers",     "uninstall_printers",   "install_filaments", "uninstall_filaments",
    "set_preferences",    "set_plugin_enabled",   "uninstall_plugin",     "reload_plugin",    "start_plugin_refresh",
    "reorder_objects",    "sync_filaments_from_ams", "open_gizmo",           "cut_object",
    "lay_on_face",        "set_brim_ears",        "test_print_host",      "send_job_command",
};

// What an exception is, so that the reason reads the same for every entry of a kind.
constexpr const char* kRunsWhatItIsGiven  = "runs what its arguments name, which is classified where it is defined";
constexpr const char* kDrivesTheGui       = "drives the GUI as a person does: the widget it acts on decides what happens";
constexpr const char* kSessionState       = "changes the automation session (jobs, the journal, recordings), not the project";
constexpr const char* kOneEntryPoint      = "its function has one automation entry point, so no second caller can drift from it";
constexpr const char* kHistory            = "it calls the undo and redo of the history it names, which no operation stands for";
constexpr const char* kMenuItemAdds       = "it runs its menu item's function, which adds an object, a part, a plate or a filament slot";
constexpr const char* kMenuItemOpens      = "it runs its menu item's function, which opens a window, a panel or a web page for a person";
constexpr const char* kMenuItemAppSetting = "it runs its menu item's function, which changes a setting of the app, not the project";

const std::map<std::string, const char*> kExceptions = {
    // The typed tools.
    {"replay_run", kRunsWhatItIsGiven},
    {"script_run", kRunsWhatItIsGiven},
    {"action_run", kRunsWhatItIsGiven},
    {"tool_call", kRunsWhatItIsGiven},
    {"webview_run_javascript", kRunsWhatItIsGiven},
    {"widget_click", kDrivesTheGui},
    {"widget_set_value", kDrivesTheGui},
    {"keyboard_press", kDrivesTheGui},
    {"mouse_gesture", kDrivesTheGui},
    {"gizmo_pick_file", kDrivesTheGui},
    {"dialog_answer", kDrivesTheGui},
    {"dialog_auto_answer", kDrivesTheGui},
    {"window_set", kDrivesTheGui},
    {"job_cancel", kSessionState},
    {"replay_export", kSessionState},
    {"recording_start", kSessionState},
    {"recording_stop", kSessionState},
    {"recording_encode", kSessionState},
    {"calibration_generate_test", kOneEntryPoint},
    {"preset_save", kOneEntryPoint},
    {"preset_delete", kOneEntryPoint},
    {"object_transform", kOneEntryPoint},
    {"part_add", kOneEntryPoint},
    {"object_select", kOneEntryPoint},
    {"object_set_height_ranges", kOneEntryPoint},
    {"object_set_variable_layer_height", kOneEntryPoint},
    {"plate_set_custom_gcode", kOneEntryPoint},
    {"settings_set", kOneEntryPoint},
    {"plate_set_settings", kOneEntryPoint},
    {"project_set_settings", kOneEntryPoint},
    {"filament_set_colour", kOneEntryPoint},
    {"filament_set_flushing", kOneEntryPoint},
    // The commands made from menu items, which run their items' functions.
    {"object_add_handy_model", kMenuItemAdds},
    {"object_add_text", kMenuItemAdds},
    {"object_add_svg", kMenuItemAdds},
    {"plate_add", kMenuItemAdds},
    {"window_open", kMenuItemOpens},
    {"app_open_web_page", kMenuItemOpens},
    {"app_show_config_folder", kMenuItemOpens},
    {"script_toggle_panel", "it runs its menu item's function, which shows or hides the Run Script panel for a person"},
    {"view_toggle_activity_panel", "it runs its menu item's function, which shows or hides the Agent Activity pane for a person"},
    {"settings_set_mode", kMenuItemAppSetting},
    {"settings_toggle_developer_mode", kMenuItemAppSetting},
    {"undo", kHistory},
    {"redo", kHistory},
    {"preset_export", kOneEntryPoint},
    {"plugin_install_local", kOneEntryPoint},
    {"preset_migrate_filaments", "it runs its menu item's function, which asks before it migrates the legacy user filament presets"},
    {"app_check_for_updates", "it runs its menu item's function, which asks the update server and shows its answer"},
    {"app_benchmark_3d_scene", "it runs its menu item's function, which times the 3D views on a test model in place of the project"},
    {"window_save_screenshot", "it runs its menu item's function, which saves a picture of the main window where recordings go"},
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

TEST_CASE("a command that changes something names its operations or is a listed exception", "[Operations]")
{
    for (const Slic3r::GUI::NativeCommand& command : Slic3r::GUI::NativeCommands::instance().commands()) {
        if (!under_operations_rule(command))
            continue;
        INFO(command.key);
        const bool excepted = kExceptions.count(command.key) != 0;
        // One or the other: an exception that names operations has stopped being one.
        CHECK(command.operations.empty() == excepted);
    }
}

TEST_CASE("a catalog entry names only operations the layer has", "[Operations]")
{
    for (const Slic3r::GUI::NativeCommand& command : Slic3r::GUI::NativeCommands::instance().commands())
        for (const std::string& operation : command.operations) {
            INFO(command.key << " names " << operation);
            CHECK(kOperations.count(operation) == 1);
        }
}

TEST_CASE("the exception list names only commands the catalog has under the rule", "[Operations]")
{
    for (const auto& [name, reason] : kExceptions) {
        INFO(name << ": " << reason);
        const Slic3r::GUI::NativeCommand* command = Slic3r::GUI::NativeCommands::find(name);
        REQUIRE(command != nullptr);
        // A read-only command needs no exception.
        CHECK(under_operations_rule(*command));
    }
}

// The Design tab's commands act on its document, which no operation stands for, as
// scripts/check_operation_calls.py leaves their files, Mcp/Tools/Design*.cpp, out.
TEST_CASE("the operations rule leaves the Design group and the read-only commands out", "[Operations]")
{
    Slic3r::GUI::NativeCommand shell;
    shell.key   = "shell";
    shell.group = "Design";
    CHECK_FALSE(under_operations_rule(shell));
    shell.group = "Objects";
    CHECK(under_operations_rule(shell));
    shell.read_only = true;
    CHECK_FALSE(under_operations_rule(shell));

    // With the CAD feature on, no Design command is under the rule, and none is on the exception list.
    Slic3r::GUI::CommandCatalog catalog;
    Slic3r::GUI::NativeCommands::register_all_commands(catalog, {true});
    for (const Slic3r::GUI::NativeCommand& command : catalog.commands()) {
        if (command.group != "Design")
            continue;
        INFO(command.key);
        CHECK_FALSE(under_operations_rule(command));
        CHECK(kExceptions.count(command.key) == 0);
    }
}

TEST_CASE("the field that names a tool's operations is in no listing", "[Operations]")
{
    // tools/list and action_list stay as they are: the field is for the catalog test alone.
    const Slic3r::GUI::NativeCommand* tool = Slic3r::GUI::NativeCommands::find("project_export");
    REQUIRE(tool != nullptr);
    REQUIRE_FALSE(tool->operations.empty());
    CHECK(tool->input_schema.dump().find("start_gcode_export") == std::string::npos);
    CHECK(tool->description.find("start_gcode_export") == std::string::npos);
}

// The ten gizmos a command opens, with the code of the selection's condition each gives when every fact is met.
const std::vector<std::pair<Slic3r::GUI::GLGizmosManager::EType, std::string>> kGizmoSelections{
    {Slic3r::GUI::GLGizmosManager::Cut, "needs_one_instance"},
    {Slic3r::GUI::GLGizmosManager::Flatten, "needs_one_instance"},
    {Slic3r::GUI::GLGizmosManager::BrimEars, "needs_one_instance"},
    {Slic3r::GUI::GLGizmosManager::FdmSupports, "needs_one_instance"},
    {Slic3r::GUI::GLGizmosManager::Seam, "needs_one_instance"},
    {Slic3r::GUI::GLGizmosManager::FuzzySkin, "needs_one_instance"},
    {Slic3r::GUI::GLGizmosManager::MmSegmentation, "needs_instance_or_part"},
    {Slic3r::GUI::GLGizmosManager::MeshBoolean, "needs_two_parts"},
    {Slic3r::GUI::GLGizmosManager::Simplify, "needs_one_part"},
    {Slic3r::GUI::GLGizmosManager::Measure, "needs_selection"},
};

// The code gizmo_refusal() gives, or "" for none.
std::string gizmo_refusal_code(Slic3r::GUI::GLGizmosManager::EType type, const Slic3r::GUI::Ops::GizmoFacts& facts, bool activable)
{
    const std::optional<Slic3r::GUI::Ops::Refusal> refusal = Slic3r::GUI::Ops::gizmo_refusal(type, facts, activable);
    return refusal ? std::string(refusal->code) : std::string();
}

TEST_CASE("every gizmo a command opens says why it stays closed", "[Operations][Gizmos]")
{
    using Slic3r::GUI::GLGizmosManager;
    using Slic3r::GUI::Ops::GizmoFacts;
    const GizmoFacts met{true, 2, false, false};
    for (const auto& [type, selection_code] : kGizmoSelections) {
        INFO(int(type));
        // Every fact met: the gizmo's own condition decides, and when it fails the selection is the reason.
        CHECK(gizmo_refusal_code(type, met, true).empty());
        CHECK(gizmo_refusal_code(type, met, false) == selection_code);
        // Each refusal is one whole sentence a person can read.
        const std::optional<Slic3r::GUI::Ops::Refusal> refusal = Slic3r::GUI::Ops::gizmo_refusal(type, met, false);
        REQUIRE(refusal.has_value());
        CHECK_FALSE(refusal->msgid.empty());
    }

    // A fact refuses whatever the gizmo's is_activable() says: Color Painting checks its printer and filaments in
    // on_is_selectable(), which opening a gizmo does not ask.
    const GizmoFacts not_fff{false, 2, false, false};
    for (const bool activable : {true, false}) {
        INFO(activable);
        for (const GLGizmosManager::EType painter :
             {GLGizmosManager::FdmSupports, GLGizmosManager::Seam, GLGizmosManager::FuzzySkin, GLGizmosManager::MmSegmentation})
            CHECK(gizmo_refusal_code(painter, not_fff, activable) == "needs_fff_printer");
        CHECK(gizmo_refusal_code(GLGizmosManager::MmSegmentation, GizmoFacts{true, 1, false, false}, activable) == "needs_two_filaments");
        // The printer is checked before the filaments, as Color Painting's on_is_selectable() checks them.
        CHECK(gizmo_refusal_code(GLGizmosManager::MmSegmentation, GizmoFacts{false, 1, false, false}, activable) == "needs_fff_printer");
        CHECK(gizmo_refusal_code(GLGizmosManager::Cut, GizmoFacts{true, 2, true, false}, activable) == "layers_editing_on");
        CHECK(gizmo_refusal_code(GLGizmosManager::Cut, GizmoFacts{true, 2, false, true}, activable) == "dowel_alone");
        // A cut object of one dowel is refused before the selection or Variable layer height is read, as
        // GLGizmoCut3D::on_is_activable() reads them.
        CHECK(gizmo_refusal_code(GLGizmosManager::Cut, GizmoFacts{true, 2, true, true}, activable) == "dowel_alone");
    }

    // A fact a gizmo does not read leaves it to its own condition.
    const GizmoFacts every_fact_fails{false, 1, true, true};
    for (const GLGizmosManager::EType reads_none :
         {GLGizmosManager::Flatten, GLGizmosManager::BrimEars, GLGizmosManager::MeshBoolean, GLGizmosManager::Simplify,
          GLGizmosManager::Measure})
        CHECK(gizmo_refusal_code(reads_none, every_fact_fails, true).empty());
    CHECK(gizmo_refusal_code(GLGizmosManager::Cut, GizmoFacts{false, 1, false, false}, true).empty());
}

namespace {

// A 20 mm cube centred on its own origin, its instance's centre at (100, 100, 10), as object_add_primitive places one
// on the bed.
Slic3r::ModelObject* placed_cube(Slic3r::Model& model)
{
    Slic3r::ModelObject* object = model.add_object();
    Slic3r::TriangleMesh mesh   = Slic3r::make_cube(20., 20., 20.);
    mesh.translate(-10.f, -10.f, -10.f);
    object->add_volume(std::move(mesh));
    object->add_instance()->set_offset(Slic3r::Vec3d(100., 100., 10.));
    return object;
}

// The code cut_refusal() gives, or "" for none.
std::string cut_refusal_code(const Slic3r::ModelObject& object, const Slic3r::Vec3d& point, const Slic3r::Vec3d& normal,
                             double sla_shift_z = 0.)
{
    const std::optional<Refusal> refusal = cut_refusal(object, 0, point, normal, sla_shift_z);
    return refusal ? std::string(refusal->code) : std::string();
}

} // namespace

TEST_CASE("a cut from arguments builds the attributes the Cut gizmo builds", "[Operations][Gizmos]")
{
    using Attribute = Slic3r::ModelObjectCutAttribute;
    // The gizmo's defaults on an object never cut: both pieces kept, the upper one laid on its cut face, and the cut
    // information left out, since the object has no cut id to carry on.
    const CutObjectArgs defaults;
    CHECK(cut_attributes(defaults, false, false) ==
          (Attribute::KeepUpper | Attribute::KeepLower | Attribute::PlaceOnCutUpper | Attribute::InvalidateCutInfo));
    // The keep_painting preference keeps the painting.
    CHECK(cut_attributes(defaults, true, false) ==
          (Attribute::KeepUpper | Attribute::KeepLower | Attribute::PlaceOnCutUpper | Attribute::InvalidateCutInfo | Attribute::KeepPaint));

    CutObjectArgs upper_only;
    upper_only.keep_lower = false;
    CHECK(cut_attributes(upper_only, false, false) == (Attribute::KeepUpper | Attribute::PlaceOnCutUpper | Attribute::InvalidateCutInfo));

    CutObjectArgs flipped;
    flipped.upper = CutPlacement::Flip;
    flipped.lower = CutPlacement::PlaceOnCut;
    CHECK(cut_attributes(flipped, false, false) ==
          (Attribute::KeepUpper | Attribute::KeepLower | Attribute::FlipUpper | Attribute::PlaceOnCutLower | Attribute::InvalidateCutInfo));

    // Cut to parts keeps both pieces and drops the placements, the upper piece's default among them.
    CutObjectArgs to_parts;
    to_parts.keep_lower = false;
    to_parts.to_parts   = true;
    CHECK(cut_attributes(to_parts, false, false) ==
          (Attribute::KeepUpper | Attribute::KeepLower | Attribute::KeepAsParts | Attribute::InvalidateCutInfo));

    // An object cut before keeps its cut id, so its pieces stay one cut with the earlier ones.
    CHECK(cut_attributes(defaults, false, true) == (Attribute::KeepUpper | Attribute::KeepLower | Attribute::PlaceOnCutUpper));
}

TEST_CASE("a cut is refused where the plane misses the object or the object has connectors", "[Operations][Gizmos]")
{
    Slic3r::Model              model;
    const Slic3r::ModelObject& cube = *placed_cube(model);
    const Slic3r::Vec3d        up   = Slic3r::Vec3d::UnitZ();
    // Through the cube, level or tilted.
    CHECK(cut_refusal_code(cube, {100., 100., 10.}, up).empty());
    CHECK(cut_refusal_code(cube, {100., 100., 10.}, {1., 0., 1.}).empty());
    // Above it, below it, and on its top and bottom faces, all of it lies on one side.
    CHECK(cut_refusal_code(cube, {100., 100., 50.}, up) == "plane_misses_object");
    CHECK(cut_refusal_code(cube, {100., 100., -5.}, up) == "plane_misses_object");
    CHECK(cut_refusal_code(cube, {100., 100., 20.}, up) == "plane_misses_object");
    CHECK(cut_refusal_code(cube, {100., 100., 0.}, up) == "plane_misses_object");
    // A plane beside it, upright.
    CHECK(cut_refusal_code(cube, {130., 100., 10.}, Slic3r::Vec3d::UnitX()) == "plane_misses_object");
    // A zero normal gives no plane.
    CHECK(cut_refusal_code(cube, {100., 100., 10.}, Slic3r::Vec3d::Zero()) == "plane_misses_object");
    // An SLA object lifted 5 mm stands from 5 to 25 mm in the 3D view, where the plane is: 22 mm crosses it, 2 mm does
    // not, the other way round from the cube on the bed.
    CHECK(cut_refusal_code(cube, {100., 100., 22.}, up, 5.).empty());
    CHECK(cut_refusal_code(cube, {100., 100., 2.}, up, 5.) == "plane_misses_object");
    CHECK(cut_refusal_code(cube, {100., 100., 22.}, up) == "plane_misses_object");
    CHECK(cut_refusal_code(cube, {100., 100., 2.}, up).empty());

    // Connectors are placed and cut in the Cut gizmo, whatever the plane.
    Slic3r::ModelObject& with_connectors = *placed_cube(model);
    with_connectors.cut_connectors.emplace_back();
    CHECK(cut_refusal_code(with_connectors, {100., 100., 10.}, up) == "has_connectors");

    // A cut object left as one dowel connector, on which the Cut gizmo does not open.
    Slic3r::ModelObject& dowel = *placed_cube(model);
    dowel.cut_id.init();
    dowel.volumes.front()->cut_info = Slic3r::ModelVolume::CutInfo(Slic3r::CutConnectorType::Dowel, 0.f, 0.f, true);
    CHECK(cut_refusal_code(dowel, {100., 100., 10.}, up) == "dowel_alone");

    // Each refusal is one whole sentence a person can read.
    const std::optional<Refusal> misses = cut_refusal(cube, 0, {100., 100., 50.}, up, 0.);
    REQUIRE(misses.has_value());
    CHECK_FALSE(misses->msgid.empty());
}

TEST_CASE("a cut's matrix places the plane as the Cut gizmo places it", "[Operations][Gizmos]")
{
    using Catch::Matchers::WithinAbs;
    Slic3r::Model              model;
    const Slic3r::ModelObject& cube = *placed_cube(model);
    const auto                 same = [](const Slic3r::Vec3d& got, const Slic3r::Vec3d& want) {
        INFO("got " << got.transpose() << ", expected " << want.transpose());
        CHECK((got - want).norm() < 1e-9);
    };

    // A level plane 12 mm above the bed: 2 mm above the instance's centre, and no turn.
    const Slic3r::Transform3d level = cut_matrix(cube, 0, {100., 100., 12.}, Slic3r::Vec3d::UnitZ(), 0.);
    same(level.translation(), {0., 0., 2.});
    CHECK(level.linear().isIdentity(1e-12));

    // A plane through the centre facing (1, 0, 1): no translation, and the plane's +Z turned to the normal, about Y.
    const Slic3r::Transform3d tilted = cut_matrix(cube, 0, {100., 100., 10.}, {1., 0., 1.}, 0.);
    same(tilted.translation(), Slic3r::Vec3d::Zero());
    same(tilted.linear() * Slic3r::Vec3d::UnitZ(), Slic3r::Vec3d(1., 0., 1.).normalized());
    same(tilted.linear() * Slic3r::Vec3d::UnitY(), Slic3r::Vec3d::UnitY());

    // An SLA object stands raised in the 3D view; its plane is lowered by the lift.
    const Slic3r::Transform3d lifted = cut_matrix(cube, 0, {100., 100., 12.}, Slic3r::Vec3d::UnitZ(), 5.);
    same(lifted.translation(), {0., 0., -3.});

    // A plane facing straight down is the gizmo's Flip, a half turn about X.
    const Slic3r::Transform3d down = cut_matrix(cube, 0, {100., 100., 10.}, -Slic3r::Vec3d::UnitZ(), 0.);
    same(down.linear() * Slic3r::Vec3d::UnitY(), -Slic3r::Vec3d::UnitY());
    same(down.linear() * Slic3r::Vec3d::UnitX(), Slic3r::Vec3d::UnitX());
    CHECK_THAT(down.linear().determinant(), WithinAbs(1., 1e-12));

    // A plane just off straight down keeps its tilt, 0.57° here: only straight down itself is the Flip.
    const Slic3r::Vec3d       near_down = Slic3r::Vec3d(0.01, 0., -1.).normalized();
    const Slic3r::Transform3d tilted_down = cut_matrix(cube, 0, {100., 100., 10.}, near_down, 0.);
    same(tilted_down.linear() * Slic3r::Vec3d::UnitZ(), near_down);
    // A turn this close to a half turn is computed to about 1e-11.
    CHECK_THAT(tilted_down.linear().determinant(), WithinAbs(1., 1e-9));
}


TEST_CASE("a direction in the 3D view becomes the normal flattening_rotate() takes", "[Operations][Gizmos]")
{
    // flattening_rotate() turns its normal into the 3D view by the inverse transpose of the instance's matrix without its
    // offset; the normal of a view direction must come back to that direction there.
    const auto in_view = [](const Slic3r::ModelInstance& instance, const Slic3r::Vec3d& normal) {
        return (instance.get_matrix_no_offset().linear().inverse().transpose() * normal).normalized().eval();
    };
    const auto same = [](const Slic3r::Vec3d& got, const Slic3r::Vec3d& want) {
        INFO("got " << got.transpose() << ", expected " << want.transpose());
        CHECK((got - want).norm() < 1e-9);
    };
    Slic3r::Model model;

    // Turned 30 degrees about X: the face that was its bottom points along (0, 0.5, -0.866) in the view.
    Slic3r::ModelInstance& turned = *placed_cube(model)->instances.front();
    turned.set_rotation(Slic3r::Vec3d(Slic3r::Geometry::deg2rad(30.), 0., 0.));
    const Slic3r::Vec3d bottom_in_view(0., 0.5, -std::sqrt(3.) / 2.);
    same(flatten_normal(turned, bottom_in_view, true), -Slic3r::Vec3d::UnitZ());
    same(in_view(turned, -Slic3r::Vec3d::UnitZ()), bottom_in_view);

    // In the object's own frame the normal is taken as given, of any length.
    same(flatten_normal(turned, Slic3r::Vec3d(0., 0., -2.), false), -Slic3r::Vec3d::UnitZ());

    // Scaled unevenly and turned, and mirrored: a view direction comes back to itself, though the object's normal is
    // not the view direction turned back.
    Slic3r::ModelInstance& scaled = *placed_cube(model)->instances.front();
    scaled.set_rotation(Slic3r::Vec3d(Slic3r::Geometry::deg2rad(30.), 0., Slic3r::Geometry::deg2rad(45.)));
    scaled.set_scaling_factor(Slic3r::Vec3d(2., 1., 0.5));
    Slic3r::ModelInstance& mirrored = *placed_cube(model)->instances.front();
    mirrored.set_rotation(Slic3r::Vec3d(0., Slic3r::Geometry::deg2rad(20.), 0.));
    mirrored.set_mirror(Slic3r::Vec3d(-1., 1., 1.));
    for (const Slic3r::ModelInstance* instance : {&scaled, &mirrored})
        for (const Slic3r::Vec3d& direction : std::vector<Slic3r::Vec3d>{Slic3r::Vec3d(1., 0., 0.), Slic3r::Vec3d(0., -1., -1.).normalized(),
                                               Slic3r::Vec3d(0.3, 0.4, -0.866).normalized()})
            same(in_view(*instance, flatten_normal(*instance, direction, true)), direction);
    const Slic3r::Vec3d along_x = flatten_normal(scaled, Slic3r::Vec3d::UnitX(), true);
    CHECK((along_x - scaled.get_matrix_no_offset().linear().inverse() * Slic3r::Vec3d::UnitX()).norm() > 1e-3);
}

TEST_CASE("brim ears asked for on the bed are stored in the object's coordinates", "[Operations][Gizmos]")
{
    using Catch::Matchers::WithinAbs;
    Slic3r::Model          model;
    Slic3r::ModelObject&   cube     = *placed_cube(model);
    Slic3r::ModelInstance& instance = *cube.instances.front();
    // Moved to (100, 100, 10) and turned 90 degrees about Z: a bed point 10 mm to the right of its centre is 10 mm along
    // the object's -Y, and one 10 mm in front of it 10 mm along its -X; both just under the bed, 10.0001 mm below its
    // centre.
    instance.set_rotation(Slic3r::Vec3d(0., 0., Slic3r::Geometry::deg2rad(90.)));
    std::vector<BrimEar> ears(2);
    ears[0].position = Slic3r::Vec2d(110., 100.);
    ears[0].radius   = 3.f;
    ears[1].position = Slic3r::Vec2d(100., 90.);
    const Outcome<std::vector<Slic3r::BrimPoint>> points = brim_points_from_bed(cube, 0, ears, 4.f);
    REQUIRE(points);
    REQUIRE(points.value().size() == 2);
    const std::vector<Slic3r::Vec3d> stored{{0., -10., -10.0001}, {-10., 0., -10.0001}};
    for (std::size_t i = 0; i < 2; ++i) {
        INFO(i);
        CHECK((points.value()[i].pos.cast<double>() - stored[i]).norm() < 1e-4);
    }
    // A radius given is kept, and one left out is the default.
    CHECK_THAT(points.value()[0].head_front_radius, WithinAbs(3., 1e-6));
    CHECK_THAT(points.value()[1].head_front_radius, WithinAbs(4., 1e-6));

    // The radius a gizmo's slider can set: from 0.1 to 100 mm.
    for (const float radius : {0.1f, 100.f}) {
        INFO(radius);
        CHECK(brim_points_from_bed(cube, 0, {BrimEar{Slic3r::Vec2d(100., 90.), radius}}, 4.f));
    }
    for (const float radius : {0.05f, 150.f}) {
        INFO(radius);
        const Outcome<std::vector<Slic3r::BrimPoint>> refused = brim_points_from_bed(cube, 0, {BrimEar{Slic3r::Vec2d(100., 90.), radius}}, 4.f);
        REQUIRE_FALSE(refused);
        CHECK(std::string(refused.refusal().code) == "radius_out_of_range");
        CHECK(refused.refusal().names == std::vector<std::string>{"0.1", "100"});
    }
    // No ears clear them.
    const Outcome<std::vector<Slic3r::BrimPoint>> none = brim_points_from_bed(cube, 0, {}, 4.f);
    REQUIRE(none);
    CHECK(none.value().empty());
}

namespace {

// What print_route() gives: the dialog and the action checked, as "print_dialog/ePrintPlate", or the refusal's code.
std::string route_of(bool send, bool all, const std::vector<Slic3r::GUI::MainFrame::PrintSelectType>& offered, bool has_print_host)
{
    using Slic3r::GUI::MainFrame;
    using Slic3r::GUI::Ops::PrintDialog;
    const Slic3r::GUI::Ops::Outcome<Slic3r::GUI::Ops::PrintRoute> route = Slic3r::GUI::Ops::print_route(send, all, offered, has_print_host);
    if (!route)
        return route.refusal().code;
    const PrintDialog dialog = route.value().dialog;
    std::string       text   = dialog == PrintDialog::Print ? "print" : dialog == PrintDialog::Storage ? "storage" : "host_upload";
    if (!route.value().action)
        return text + "/none";
    switch (*route.value().action) {
    case MainFrame::ePrintPlate: return text + "/ePrintPlate";
    case MainFrame::ePrintAll: return text + "/ePrintAll";
    case MainFrame::eSendGcode: return text + "/eSendGcode";
    case MainFrame::eSendToPrinter: return text + "/eSendToPrinter";
    case MainFrame::eSendToPrinterAll: return text + "/eSendToPrinterAll";
    default: return text + "/other";
    }
}

} // namespace

TEST_CASE("a print or send call takes the route of the Print button's action of that name", "[Operations][Printer]")
{
    using Slic3r::GUI::MainFrame;
    using Offer = std::vector<MainFrame::PrintSelectType>;
    // What MainFrame::available_print_actions() offers each kind of printer.
    const Offer vendor{MainFrame::eSendGcode, MainFrame::eExportGcode};
    const Offer bambu_host{MainFrame::ePrintPlate, MainFrame::eExportSlicedFile, MainFrame::eExportAllSlicedFile, MainFrame::eExportGcode};
    const Offer simplyprint{MainFrame::ePrintPlate, MainFrame::ePrintAll, MainFrame::eExportSlicedFile, MainFrame::eExportAllSlicedFile,
                            MainFrame::eExportGcode};
    const Offer network{MainFrame::ePrintPlate,       MainFrame::ePrintAll,         MainFrame::eSendToPrinter,
                        MainFrame::eSendToPrinterAll, MainFrame::eExportSlicedFile, MainFrame::eExportAllSlicedFile,
                        MainFrame::eExportGcode};
    // Print, Print all, Send and Send all, for each printer with a print host address.
    const auto [offer, name, print, print_all, send, send_all] = GENERATE_COPY(table<Offer, std::string, std::string, std::string, std::string, std::string>({
        {vendor, "another vendor's printer", "host_upload/eSendGcode", "all_plates_not_offered", "host_upload/none", "all_plates_not_offered"},
        {bambu_host, "a Bambu printer through a print host", "host_upload/ePrintPlate", "all_plates_not_offered", "host_upload/none",
         "all_plates_not_offered"},
        {simplyprint, "a Bambu printer through SimplyPrint", "host_upload/ePrintPlate", "host_upload/ePrintAll", "host_upload/none",
         "all_plates_not_offered"},
        {network, "a printer on the Bambu network", "print/ePrintPlate", "print/ePrintAll", "storage/eSendToPrinter",
         "storage/eSendToPrinterAll"},
    }));
    INFO(name);
    CHECK(route_of(false, false, offer, true) == print);
    CHECK(route_of(false, true, offer, true) == print_all);
    CHECK(route_of(true, false, offer, true) == send);
    CHECK(route_of(true, true, offer, true) == send_all);
}

TEST_CASE("a route through a print host needs its address, and the Bambu network's does not", "[Operations][Printer]")
{
    using Slic3r::GUI::MainFrame;
    const std::vector<MainFrame::PrintSelectType> vendor{MainFrame::eSendGcode, MainFrame::eExportGcode};
    const std::vector<MainFrame::PrintSelectType> network{MainFrame::ePrintPlate, MainFrame::ePrintAll, MainFrame::eSendToPrinter,
                                                          MainFrame::eSendToPrinterAll};
    CHECK(route_of(false, false, vendor, false) == "no_print_host");
    CHECK(route_of(true, false, vendor, false) == "no_print_host");
    // A call the button does not offer is refused for that first.
    CHECK(route_of(true, true, vendor, false) == "all_plates_not_offered");
    CHECK(route_of(false, false, network, false) == "print/ePrintPlate");
    CHECK(route_of(true, true, network, false) == "storage/eSendToPrinterAll");
}
