// Automation action tests: slice.* (SliceActions.cpp) and project.* (ProjectActions.cpp). Tag: [actions].
//
// The pure rules behind them: what the slicing service options dialog would store (SliceService::options_to_store),
// the Project tab's licences and auxiliary folders, and file-name checks. No running application.

#include <catch2/catch_all.hpp>

#include "slic3r/GUI/Actions/ActionCatalog.hpp"
#include "slic3r/GUI/Actions/ActionSchema.hpp"
#include "slic3r/GUI/Actions/ProjectCommon.hpp"
#include "slic3r/GUI/Actions/Register.hpp"
#include "slic3r/GUI/SliceServiceOptionsDialog.hpp"

#include <string>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Actions;
using nlohmann::json;

namespace {

// A service form in the ui-schema vocabulary: a build field, an enum shown only for one build, a
// bool, a ranged number with a per-axes default, and an optional number.
json test_schema()
{
    return json::parse(R"({
      "title": "Test service",
      "fields": [
        {"dest": "build", "type": "enum", "default": "planar",
         "choices": [{"value": "planar", "label": "Planar"}, {"value": "conical", "label": "Conical", "axes": 5},
                     {"value": "wave", "label": "Wave"}]},
        {"dest": "wave_amp", "type": "number", "min": 0.1, "max": 5, "default": 1, "show_if": {"build": ["wave"]}},
        {"dest": "ironing", "type": "bool", "default": false},
        {"dest": "tilt", "type": "number", "min": 0, "max": 60, "default_by_axes": {"3": 0, "5": 45}},
        {"dest": "seed", "type": "number", "optional": true},
        {"dest": "future", "type": "colour"}
      ]})");
}

json store(const json& saved, const json& changes, int axes = 3, std::string* bad = nullptr)
{
    json        out;
    std::string dest, why;
    const bool  ok = SliceService::options_to_store(test_schema(), saved, changes, json{{"machine_axes", axes}}, json::object(), out, dest, why);
    if (bad != nullptr)
        *bad = ok ? std::string() : dest;
    return ok ? out : json();
}

} // namespace

TEST_CASE("slice.setOptions stores what the slicing service options dialog's OK would", "[actions]")
{
    // Nothing changed from the defaults: nothing stored (the form's fields are all generic; a
    // "build" field is one like any other).
    CHECK(store(json::object(), {{"ironing", false}}) == json::object());
    // Non-defaults are stored; a value back at its default is dropped again.
    CHECK(store(json::object(), {{"ironing", true}, {"tilt", 30}}) == json{{"ironing", true}, {"tilt", 30}});
    CHECK(store({{"ironing", true}, {"tilt", 30}}, {{"tilt", 0}}) == json{{"ironing", true}});
    // null returns a field to its default (blank for an optional number).
    CHECK(store({{"ironing", true}, {"seed", 7}}, {{"seed", nullptr}}) == json{{"ironing", true}});
    // A field shown only under another field's value applies once that value is chosen in the same call.
    CHECK(store(json::object(), {{"build", "wave"}, {"wave_amp", 2}}) == json{{"build", "wave"}, {"wave_amp", 2}});
    // The default depends on the printer's axes: 45 is the 5-axis default, so it is not stored there.
    CHECK(store(json::object(), {{"tilt", 45}}, 5) == json::object());
    // Leaving a value drops what applied only to it.
    CHECK(store({{"build", "wave"}, {"wave_amp", 2}}, {{"build", "planar"}}) == json::object());
}

TEST_CASE("slice.setOptions refuses what the dialog cannot hold", "[actions]")
{
    std::string bad;
    CHECK(store(json::object(), {{"nonsense", 1}}, 3, &bad).is_null());
    CHECK(bad == "nonsense");
    // A field kind this build cannot draw is not settable either.
    CHECK(store(json::object(), {{"future", "red"}}, 3, &bad).is_null());
    CHECK(bad == "future");
    // Out of range, wrong type, a choice not offered on a 3-axis printer.
    CHECK(store(json::object(), {{"tilt", 90}}, 3, &bad).is_null());
    CHECK(bad == "tilt");
    CHECK(store(json::object(), {{"ironing", "yes"}}, 3, &bad).is_null());
    CHECK(bad == "ironing");
    CHECK(store(json::object(), {{"build", "conical"}}, 3, &bad).is_null());
    CHECK(bad == "build");
    CHECK_FALSE(store(json::object(), {{"build", "conical"}}, 5).is_null());
    // A field that does not apply under the resulting values.
    CHECK(store(json::object(), {{"wave_amp", 2}}, 3, &bad).is_null());
    CHECK(bad == "wave_amp");
}

TEST_CASE("Project tab rules: licences, folders, file names", "[actions]")
{
    const auto& licenses = project_licenses();
    REQUIRE(licenses.size() == 8);
    CHECK(licenses.front().empty());
    CHECK(std::find(licenses.begin(), licenses.end(), "BY-NC-SA") != licenses.end());

    REQUIRE(find_aux_folder("pictures") != nullptr);
    CHECK(find_aux_folder("pictures")->dir == "Model Pictures");
    CHECK(find_aux_folder("bom")->dir == "Bill of Materials");
    CHECK(find_aux_folder("assembly")->extensions == std::vector<std::string>{"pdf"});
    CHECK(find_aux_folder("others")->extensions == std::vector<std::string>{"txt"});
    CHECK(find_aux_folder(".thumbnails") == nullptr);

    CHECK(aux_file_name_ok("cover.png"));
    CHECK_FALSE(aux_file_name_ok(""));
    CHECK_FALSE(aux_file_name_ok(".."));
    CHECK_FALSE(aux_file_name_ok("../etc/passwd"));
    CHECK_FALSE(aux_file_name_ok("a\\b.txt"));
    CHECK_FALSE(aux_file_name_ok("C:x.txt"));
    CHECK_FALSE(aux_file_name_ok(std::string("a\nb")));

    CHECK(aux_unique_name("cover.png", {}) == "cover.png");
    CHECK(aux_unique_name("cover.png", {"cover.png"}) == "cover_2.png");
    CHECK(aux_unique_name("cover.png", {"cover.png", "cover_2.png"}) == "cover_3.png");
    CHECK(aux_unique_name("README", {"README"}) == "README_2");
}

TEST_CASE("slice.* and project.* descriptors added for 0.2.0 are in the catalog", "[actions]")
{
    Catalog c;
    register_all(c);
    for (const char* id : {"slice.cancel", "slice.getOptions", "slice.setOptions", "project.restoreAside", "project.setInfo",
                           "project.listFiles", "project.addFile", "project.setCover", "project.removeFile", "project.forgetRecent"}) {
        INFO(id);
        const ActionEntry* e = c.find(id);
        REQUIRE(e != nullptr);
        CHECK(static_cast<bool>(e->runner));
    }
    // Cancelling must run while a slice is busy: not a write in the runtime's guard.
    CHECK(c.find("slice.cancel")->descriptor.risk == Risk::Ui);
    CHECK(c.find("project.removeFile")->descriptor.confirm == Confirm::Native);

    SchemaError err;
    const json& set_options = c.find("slice.setOptions")->descriptor.params;
    CHECK(validate(set_options, {{"values", json::array({{{"dest", "tilt"}, {"number", 30}}})}}, err));
    CHECK_FALSE(validate(set_options, {{"values", json::array({{{"dest", "tilt"}, {"number", 30}, {"on", true}}})}}, err));
    CHECK_FALSE(validate(set_options, {{"values", json::array({{{"number", 30}}})}}, err));
    const json& set_info = c.find("project.setInfo")->descriptor.params;
    CHECK(validate(set_info, {{"license", ""}}, err));
    CHECK_FALSE(validate(set_info, {{"license", "MIT"}}, err));
}
