// Automation action catalog tests. Owner: work package S1 (actions core). Tag: [actions].
//
// Every check here runs on the catalog as register_all() builds it, so a domain package's
// descriptor mistakes fail here without a running slicer.
//
// Snapshot: resources/actions/catalog.snapshot.json is the catalog's wire form. Regenerate it with
//   UPDATE_SNAPSHOT=1 slic3rutils_tests "[actions][snapshot]"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "slic3r/GUI/Actions/ActionCatalog.hpp"
#include "slic3r/GUI/Actions/ActionSchema.hpp"
#include "slic3r/GUI/Actions/Register.hpp"

using namespace Slic3r::GUI::Actions;
using json = nlohmann::json;
namespace fs = boost::filesystem;

namespace {

fs::path actions_dir() { return fs::path(PROFILES_DIR).parent_path() / "actions"; }

const Catalog& built()
{
    static Catalog c;
    static bool    done = false;
    if (!done) {
        register_all(c);
        done = true;
    }
    return c;
}

json read_json(const fs::path& p)
{
    std::ifstream in(p.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

ActionDescriptor minimal(const std::string& id)
{
    ActionDescriptor d;
    d.id       = id;
    d.label    = "Test";
    d.summary  = "A test action.";
    d.examples = json::array({{{"params", json::object()}}});
    return d;
}

bool is_outward_or_destructive(Risk r)
{
    return r == Risk::Destructive || r == Risk::OutwardFile || r == Risk::OutwardNetwork || r == Risk::OutwardPrinter;
}

} // namespace

TEST_CASE("Catalog builds and every id is unique and well formed", "[actions][catalog]")
{
    const Catalog& c = built(); // add() throws on a duplicate or malformed id
    REQUIRE(c.size() > 0);
    std::set<std::string> ids;
    for (const ActionEntry& e : c.entries()) {
        INFO(e.descriptor.id);
        CHECK(is_valid_action_id(e.descriptor.id));
        CHECK(ids.insert(e.descriptor.id).second);
        CHECK(e.descriptor.domain == e.descriptor.id.substr(0, e.descriptor.id.find('.')));
        CHECK_FALSE(is_forbidden_domain(e.descriptor.domain));
    }
}

TEST_CASE("Every descriptor is inside the schema dialect and its examples validate", "[actions][catalog]")
{
    for (const ActionEntry& e : built().entries()) {
        const ActionDescriptor& d = e.descriptor;
        INFO(d.id);
        SchemaError err;
        CHECK(check_schema(d.params, err));
        CHECK(d.params.value("type", "") == "object");
        for (const json& ex : d.examples) {
            const json params = ex.contains("params") ? ex["params"] : json::object();
            CHECK(validate(d.params, apply_defaults(d.params, params), err));
        }
        for (const std::string& eff : d.effects)
            CHECK(is_known_effect(eff));
        // An action the catalog itself runs needs an example: it drives the generic end-to-end test.
        if (e.runner)
            CHECK(d.examples.size() >= 1);
        CHECK(d.timeout_ms > 0);
        CHECK(d.timeout_ms <= 50000);
    }
}

TEST_CASE("Destructive and outward actions are confirmed natively or document why not", "[actions][catalog]")
{
    for (const ActionEntry& e : built().entries()) {
        const ActionDescriptor& d = e.descriptor;
        INFO(d.id);
        if (is_outward_or_destructive(d.risk))
            CHECK((d.confirm == Confirm::Native || !d.confirm_skip_rule.empty()));
    }
}

TEST_CASE("Typed-method descriptors name a slicer method", "[actions][catalog]")
{
    std::set<std::string> methods;
    for (const ActionEntry& e : built().entries()) {
        const ActionDescriptor& d = e.descriptor;
        if (d.bridge_method.empty()) {
            INFO(d.id);
            CHECK(e.runner); // a plain action must be runnable
            continue;
        }
        INFO(d.id);
        CHECK(d.bridge_method.rfind("slicer.", 0) == 0);
        CHECK(methods.insert(d.bridge_method).second);
    }
}

TEST_CASE("Catalog::add refuses malformed descriptors", "[actions][catalog]")
{
    Catalog c;
    CHECK_NOTHROW(c.add(minimal("test.ok"), [](InvokeContext&) { return InvokeResult::ok(); }));
    CHECK_THROWS(c.add(minimal("test.ok")));             // duplicate
    CHECK_THROWS(c.add(minimal("nodomain")));            // no verb
    CHECK_THROWS(c.add(minimal("Test.upper")));          // not lowerCamel
    CHECK_THROWS(c.add(minimal("plugin.run")));          // forbidden domain
    CHECK_THROWS(c.add(minimal("account.signOut")));     // forbidden domain

    ActionDescriptor bad_schema = minimal("test.badSchema");
    bad_schema.params           = {{"type", "object"}, {"properties", json::object()}}; // no additionalProperties:false
    CHECK_THROWS(c.add(bad_schema));

    ActionDescriptor bad_example = minimal("test.badExample");
    bad_example.params = {{"type", "object"}, {"additionalProperties", false}, {"required", json::array({"axis"})},
                          {"properties", {{"axis", {{"type", "string"}, {"enum", json::array({"x", "y"})}}}}}};
    bad_example.examples = json::array({{{"params", {{"axis", "z"}}}}});
    CHECK_THROWS(c.add(bad_example));

    ActionDescriptor bad_effect = minimal("test.badEffect");
    bad_effect.effects          = {"everything"};
    CHECK_THROWS(c.add(bad_effect));

    ActionDescriptor destructive = minimal("test.destroy");
    destructive.risk             = Risk::Destructive;
    c.add(destructive);
    CHECK(c.find("test.destroy")->descriptor.confirm == Confirm::Native); // default from the risk
}

TEST_CASE("Catalog version is stable and changes with the descriptors", "[actions][catalog]")
{
    Catalog a, b;
    register_all(a);
    register_all(b);
    CHECK(a.version() == b.version());
    CHECK(a.version().size() == 16);
    b.add(minimal("test.extra"), [](InvokeContext&) { return InvokeResult::ok(); });
    CHECK(a.version() != b.version());
    CHECK(fnv1a_hex("") == "cbf29ce484222325");
}

TEST_CASE("Every action id a coverage manifest names exists in the catalog", "[actions][catalog][manifest]")
{
    const Catalog& c   = built();
    const fs::path dir = actions_dir() / "surface";
    REQUIRE(fs::is_directory(dir));
    auto known = [&c](const std::string& id) { return c.find(id) != nullptr || c.find_by_method(id) != nullptr; };
    std::set<std::string> keys;
    for (fs::directory_iterator it(dir), end; it != end; ++it) {
        if (it->path().extension() != ".json")
            continue;
        const json m = read_json(it->path());
        for (auto kv = m.begin(); kv != m.end(); ++kv) {
            if (kv.key().rfind("$", 0) == 0)
                continue;
            INFO(it->path().filename().string() << ": " << kv.key());
            CHECK(keys.insert(kv.key()).second); // a key lives in exactly one manifest
            const json& v = kv.value();
            REQUIRE(v.is_object());
            if (v.contains("action"))
                CHECK(known(v["action"].get<std::string>()));
            if (v.contains("actions"))
                for (const json& a : v["actions"])
                    CHECK(known(a.get<std::string>()));
            if (v.contains("coveredBy"))
                CHECK(known(v["coveredBy"].get<std::string>()));
        }
    }
}

TEST_CASE("Catalog snapshot matches resources/actions/catalog.snapshot.json", "[actions][catalog][snapshot]")
{
    const json     now  = catalog_snapshot(built());
    const fs::path path = actions_dir() / "catalog.snapshot.json";
    const char*    upd  = std::getenv("UPDATE_SNAPSHOT");
    if ((upd != nullptr && std::string(upd) == "1") || !fs::exists(path)) {
        std::ofstream out(path.string(), std::ios::binary);
        out << now.dump(2) << "\n";
        WARN("wrote " << path.string());
        return;
    }
    const json saved = read_json(path);
    INFO("the catalog changed; regenerate with UPDATE_SNAPSHOT=1 and commit the snapshot");
    CHECK(saved.value("catalogVersion", "") == now["catalogVersion"].get<std::string>());
    CHECK(saved == now);
}
