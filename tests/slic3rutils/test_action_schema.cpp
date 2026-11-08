// Action params dialect, turn log and target resolution. Owner: work package S1 (actions core).
// Tag: [actions]. The schema vectors are shared with the server side (resources/actions/schema_vectors.json).

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "slic3r/GUI/Actions/ActionRuntime.hpp"
#include "slic3r/GUI/Actions/ActionSchema.hpp"
#include "slic3r/GUI/Actions/ActionTargets.hpp"
#include "slic3r/GUI/Actions/TurnLog.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI::Actions;
using json = nlohmann::json;
namespace fs = boost::filesystem;

namespace {

json read_vectors()
{
    const fs::path p = fs::path(PROFILES_DIR).parent_path() / "actions" / "schema_vectors.json";
    std::ifstream  in(p.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

} // namespace

TEST_CASE("Schema vectors", "[actions][schema]")
{
    const json vectors = read_vectors();
    REQUIRE(vectors.contains("vectors"));
    for (const json& v : vectors["vectors"]) {
        INFO(v["name"].get<std::string>());
        SchemaError err;
        const bool  schema_ok = check_schema(v["schema"], err);
        CHECK(schema_ok == v["schemaValid"].get<bool>());
        if (!schema_ok || !v.contains("cases"))
            continue;
        for (const json& c : v["cases"]) {
            INFO(c["value"].dump());
            SchemaError e;
            const bool  ok = validate(v["schema"], c["value"], e);
            CHECK(ok == c["valid"].get<bool>());
            if (!ok && c.contains("path"))
                CHECK(e.path == c["path"].get<std::string>());
            if (!ok && c.contains("needs"))
                CHECK(e.needs == c["needs"].get<std::string>());
        }
    }
}

TEST_CASE("apply_defaults fills absent properties, never alternatives", "[actions][schema]")
{
    const json schema = json::parse(R"({"type":"object","additionalProperties":false,"x-exactlyOne":[["a"],["b"]],
        "properties":{"a":{"type":"integer","default":1},"b":{"type":"integer"},"keep":{"type":"string","default":"both"},
        "inner":{"type":"object","additionalProperties":false,"properties":{"z":{"type":"number","default":2}}}}})");
    const json out = apply_defaults(schema, json{{"b", 5}, {"inner", json::object()}});
    CHECK(out["keep"] == "both");
    CHECK(out["inner"]["z"] == 2);
    CHECK_FALSE(out.contains("a"));
    CHECK(apply_defaults(schema, json{{"keep", "upper"}})["keep"] == "upper");
}

TEST_CASE("Turn log records snapshots and settings, and decides roll-back", "[actions][turnlog]")
{
    TurnLog log;
    log.note_snapshots("t1", {1, 2}, {1, 2, 5, 6});
    REQUIRE(log.find("t1") != nullptr);
    CHECK(log.find("t1")->snapshots == std::vector<size_t>{5, 6});

    size_t      undo_to = 0;
    std::string why;
    CHECK(log.can_roll_back("t1", {1, 2, 5, 6}, undo_to, why));
    CHECK(undo_to == 5);
    CHECK_FALSE(log.can_roll_back("t1", {1, 2, 5, 6, 9}, undo_to, why)); // a later edit
    CHECK_FALSE(log.can_roll_back("t1", {1, 2, 5}, undo_to, why));       // undone in part
    CHECK_FALSE(log.can_roll_back("nope", {1}, undo_to, why));

    log.note_setting("t2", {"print", "0.2mm", "wall_loops", "2", "3"});
    log.note_setting("t2", {"print", "0.2mm", "wall_loops", "3", "4"});
    REQUIRE(log.find("t2")->settings.size() == 1);
    CHECK(log.find("t2")->settings[0].previous == "2"); // the value before the TURN
    CHECK(log.find("t2")->settings[0].written == "4");
    CHECK(log.can_roll_back("t2", {1, 2}, undo_to, why)); // settings only: nothing on the stack
    CHECK(undo_to == 0);

    CHECK_FALSE(TurnLog::valid_turn_id(""));
    CHECK_FALSE(TurnLog::valid_turn_id(std::string(200, 'a')));
    for (int i = 0; i < int(TurnLog::kMaxTurns) + 5; ++i)
        log.note_setting("bulk-" + std::to_string(i), {"print", "p", "k", "a", "b"});
    CHECK(log.find("t1") == nullptr); // oldest dropped
    log.forget("bulk-70");
    CHECK(log.find("bulk-70") == nullptr);
}

TEST_CASE("Targets resolve by id, index and name", "[actions][targets]")
{
    Model model;
    TriangleMesh cube = TriangleMesh(its_make_cube(10., 10., 10.));
    for (const char* name : {"Benchy", "Cube", "Cube2"}) {
        ModelObject* o = model.add_object(name, "", TriangleMesh(cube));
        o->add_instance();
    }
    model.objects[1]->add_instance();
    const json objects_spec = {{"kind", "objects"}, {"required", true}};

    ResolvedTargets t;
    std::string     message;
    json            data;
    REQUIRE(resolve_targets(model, 1, objects_spec, json{{"objects", {{{"id", model.objects[2]->id().id}}, {{"index", 0}}}}}, nullptr, t,
                            message, data) == 0);
    CHECK(t.objects == std::vector<int>{0, 2});

    CHECK(resolve_targets(model, 1, objects_spec, json{{"object", {{"name", "Cube"}}}}, nullptr, t, message, data) == 0);
    CHECK(t.objects == std::vector<int>{1});

    CHECK(resolve_targets(model, 1, objects_spec, json{{"objects", {{{"name", "Nope"}}}}}, nullptr, t, message, data) == 1008);
    CHECK(data["missing"] == "/target/objects/0");
    CHECK(resolve_targets(model, 1, objects_spec, json{{"objects", {{{"index", "0"}}}}}, nullptr, t, message, data) == 1002);
    CHECK(resolve_targets(model, 1, objects_spec, json{{"bogus", 1}}, nullptr, t, message, data) == 1002);
    CHECK(resolve_targets(model, 1, objects_spec, nullptr, nullptr, t, message, data) == 1002); // three objects: say which

    const json volume_spec = {{"kind", "volume"}, {"required", true}};
    CHECK(resolve_targets(model, 1, volume_spec, json{{"volume", {{"object", {{"index", 1}}}, {"index", 0}}}}, nullptr, t, message, data) == 0);
    CHECK(t.volumes == std::vector<std::pair<int, int>>{{1, 0}});
    CHECK(resolve_targets(model, 1, volume_spec, json{{"objects", {{{"index", 2}}}}}, nullptr, t, message, data) == 0); // single part implied
    CHECK(t.volumes == std::vector<std::pair<int, int>>{{2, 0}});

    const json inst_spec = {{"kind", "instances"}, {"required", true}};
    CHECK(resolve_targets(model, 1, inst_spec, json{{"objects", {{{"index", 1}}}}}, nullptr, t, message, data) == 0);
    CHECK(t.instances.size() == 2);

    const json plate_spec = {{"kind", "plate"}, {"required", true}};
    CHECK(resolve_targets(model, 2, plate_spec, json{{"plate", 1}}, nullptr, t, message, data) == 0);
    CHECK(t.plate == 1);
    CHECK(resolve_targets(model, 2, plate_spec, json{{"plate", 2}}, nullptr, t, message, data) == 1008);

    ResolvedTargets sel;
    sel.objects = {2};
    CHECK(resolve_targets(model, 1, objects_spec, json{{"selection", true}}, &sel, t, message, data) == 0);
    CHECK(t.objects == std::vector<int>{2});

    CHECK(object_index_by_id(model, model.objects[1]->id().id) == 1);
    CHECK(object_ref_json(model, 1)["name"] == "Cube");
}

TEST_CASE("A Deferred finished before the runtime attaches still delivers its result", "[actions][deferred]")
{
    using Slic3r::GUI::Actions::Deferred;
    using Slic3r::GUI::Actions::InvokeResult;

    Deferred early;
    early.finish(InvokeResult::error(1009, "file type 'exe' is not accepted here"));
    CHECK(early.finished());
    int delivered = 0, code = 0;
    early.on_finish([&](InvokeResult&& r) { ++delivered; code = r.code; });
    CHECK(delivered == 1);
    CHECK(code == 1009);
    early.on_finish([&](InvokeResult&&) { ++delivered; }); // delivered once only
    CHECK(delivered == 1);

    Deferred later;
    later.on_finish([&](InvokeResult&& r) { ++delivered; code = r.code; });
    CHECK(delivered == 1);
    later.finish(InvokeResult::ok());
    later.finish(InvokeResult::error(1012, "twice")); // ignored
    CHECK(delivered == 2);
    CHECK(code == 0);
}
