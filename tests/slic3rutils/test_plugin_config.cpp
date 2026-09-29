#include <catch2/catch_all.hpp>

#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginConfig.hpp>
#include <slic3r/plugin/PluginManager.hpp>

#include "plugin_test_utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

PluginCapabilityId capability_id(PluginCapabilityType type, const char* name, const char* plugin_key)
{
    return {type, name, plugin_key};
}

json read_config_file()
{
    boost::nowide::ifstream ifs(PluginConfig::plugin_config_file().c_str());
    json root;
    ifs >> root;
    return root;
}

void write_config_file(const std::string& contents)
{
    const fs::path path(PluginConfig::plugin_config_file());
    fs::create_directories(path.parent_path());
    boost::nowide::ofstream ofs(path.string().c_str(), std::ios::out | std::ios::trunc);
    ofs << contents;
}

} // namespace

TEST_CASE("PluginConfig creates, reads back and persists a capability config", "[PluginConfig]")
{
    ScopedDataDir data_dir_guard("plugin-config-roundtrip");

    PluginConfig config;
    const PluginCapabilityId id = capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a");

    // A capability nobody has configured yet reads as an empty record rather than throwing.
    CHECK_FALSE(config.has_config(id));
    CHECK_FALSE(config.get_config(id));

    REQUIRE(config.store_capability_config(id, json{{"speed", 5}}));

    const auto stored = config.get_config(id);
    REQUIRE(stored);
    CHECK(stored->id == id);
    CHECK(stored->config == json{{"speed", 5}});
    CHECK(config.has_config(id));

    // store_capability_config writes through, so a fresh instance (a restart, in effect) sees it.
    PluginConfig reloaded;
    reloaded.load();
    CHECK(reloaded.get_config(id)->config == json{{"speed", 5}});
}

TEST_CASE("PluginConfig updates only the target capability's cap_config", "[PluginConfig]")
{
    ScopedDataDir data_dir_guard("plugin-config-isolation");

    PluginConfig config;
    const PluginCapabilityId a_a = capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a");
    const PluginCapabilityId a_b = capability_id(PluginCapabilityType::Script, "cap_b", "plugin_a");
    const PluginCapabilityId b_a = capability_id(PluginCapabilityType::Script, "cap_a", "plugin_b");
    // The identity is the full (type, capability, plugin_key) tuple, so all three below are separate records.
    REQUIRE(config.store_capability_config(a_a, json{{"value", 1}}));
    REQUIRE(config.store_capability_config(a_b, json{{"value", 2}}));
    REQUIRE(config.store_capability_config(b_a, json{{"value", 3}}));

    REQUIRE(config.store_capability_config(a_a, json{{"value", 99}}));

    CHECK(config.get_config(a_a)->config == json{{"value", 99}});
    CHECK(config.get_config(a_b)->config == json{{"value", 2}});
    CHECK(config.get_config(b_a)->config == json{{"value", 3}});

    // The same holds on disk, not just in memory.
    PluginConfig reloaded;
    reloaded.load();
    CHECK(reloaded.get_config(a_a)->config == json{{"value", 99}});
    CHECK(reloaded.get_config(a_b)->config == json{{"value", 2}});
    CHECK(reloaded.get_config(b_a)->config == json{{"value", 3}});
}

TEST_CASE("PluginConfig isolates same-name capabilities by type", "[PluginConfig]")
{
    ScopedDataDir data_dir_guard("plugin-config-type-isolation");
    const PluginCapabilityId script = capability_id(PluginCapabilityType::Script, "shared", "plugin_a");
    const PluginCapabilityId importer = capability_id(PluginCapabilityType::Importer, "shared", "plugin_a");

    PluginConfig config;
    REQUIRE(config.store_capability_config(script, json{{"value", "script"}}));
    REQUIRE(config.store_capability_config(importer, json{{"value", "importer"}}));
    CHECK(config.get_config(script)->config == json{{"value", "script"}});
    CHECK(config.get_config(importer)->config == json{{"value", "importer"}});

    PluginConfig reloaded;
    reloaded.load();
    CHECK(reloaded.get_config(script)->config == json{{"value", "script"}});
    CHECK(reloaded.get_config(importer)->config == json{{"value", "importer"}});
}

TEST_CASE("PluginConfig serializes the documented on-disk schema", "[PluginConfig]")
{
    ScopedDataDir data_dir_guard("plugin-config-schema");

    PluginConfig config;
    const PluginCapabilityId id = capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a");
    REQUIRE(config.store_capability_config(id, json{{"speed", 5}}));

    // Locks the field names: an existing config.json must keep loading after any future change.
    const json root = read_config_file();
    REQUIRE(root.contains("config"));
    REQUIRE(root.at("config").is_array());
    REQUIRE(root.at("config").size() == 1);

    const json& entry = root.at("config").front();
    CHECK(entry.at("plugin_key") == "plugin_a");
    CHECK(entry.at("capability") == "cap_a");
    CHECK(entry.at("capability_type") == "script");
    CHECK(entry.at("cap_config") == json{{"speed", 5}});
    CHECK(entry.contains("plugin_version"));
    // Only cap_config is user data; the rest of the record is host-managed.
    CHECK(entry.size() == 5);
}

TEST_CASE("PluginConfig keeps a capability's config after its plugin goes away", "[PluginConfig]")
{
    ScopedDataDir data_dir_guard("plugin-config-retention");

    {
        PluginConfig config;
        REQUIRE(config.store_capability_config(capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a"), json{{"token", "keep me"}}));
    }

    // config.json is deliberately not keyed to installed plugins: a record outlives its plugin and is
    // still there on reinstall. Asserts no cleanup path silently drops it.
    PluginConfig after_removal;
    after_removal.load();
    CHECK(after_removal.get_config(capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a"))->config == json{{"token", "keep me"}});
}

TEST_CASE("PluginConfig treats a missing config file as an empty store", "[PluginConfig]")
{
    ScopedDataDir data_dir_guard("plugin-config-missing");

    REQUIRE_FALSE(fs::exists(PluginConfig::plugin_config_file()));

    PluginConfig config;
    REQUIRE_NOTHROW(config.load());
    CHECK_FALSE(config.has_config(capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a")));
    CHECK_FALSE(config.dirty());
}

TEST_CASE("PluginConfig survives a malformed config file", "[PluginConfig]")
{
    SECTION("not JSON at all")
    {
        ScopedDataDir data_dir_guard("plugin-config-garbage");
        write_config_file("this is not json {{{");

        PluginConfig config;
        REQUIRE_NOTHROW(config.load()); // a bad config must not block startup
        CHECK_FALSE(config.has_config(capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a")));
    }

    SECTION("valid JSON without the entries array")
    {
        ScopedDataDir data_dir_guard("plugin-config-noarray");
        write_config_file(R"({"config": {"not": "an array"}})");

        PluginConfig config;
        REQUIRE_NOTHROW(config.load());
        CHECK_FALSE(config.has_config(capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a")));
    }

    SECTION("entries without an identity are skipped, the rest still load")
    {
        ScopedDataDir data_dir_guard("plugin-config-partial");
        write_config_file(R"({"config": [
            {"cap_config": {"orphan": true}},
            {"plugin_key": "plugin_a", "capability": "cap_a", "capability_type": "script", "plugin_version": "1.0.0", "cap_config": {"kept": true}}
        ]})");

        PluginConfig config;
        REQUIRE_NOTHROW(config.load());
        CHECK(config.get_config(capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a"))->config == json{{"kept", true}});
        CHECK(config.get_config(capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a"))->plugin_version == "1.0.0");
    }

    SECTION("an entry with no cap_config reads as an empty object")
    {
        ScopedDataDir data_dir_guard("plugin-config-nocap");
        write_config_file(R"({"config": [
            {"plugin_key": "plugin_a", "capability": "cap_a", "capability_type": "script", "plugin_version": "1.0.0"}
        ]})");

        PluginConfig config;
        REQUIRE_NOTHROW(config.load());
        REQUIRE(config.has_config(capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a")));
        CHECK(config.get_config(capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a"))->config == json::object());
    }

    SECTION("legacy entries without capability_type remain addressable")
    {
        ScopedDataDir data_dir_guard("plugin-config-notype");
        write_config_file(R"({"config": [
            {"plugin_key": "plugin_a", "capability": "cap_a", "plugin_version": "1.0.0", "cap_config": {"old": true}}
        ]})");

        PluginConfig config;
        REQUIRE_NOTHROW(config.load());
        const auto id = capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a");
        REQUIRE(config.has_config(id));
        CHECK(config.get_config(id)->config == json{{"old", true}});

        REQUIRE(config.store_capability_config(id, json{{"migrated", true}}));
        const json root = read_config_file();
        REQUIRE(root.at("config").size() == 1);
        CHECK(root.at("config").front().at("capability_type") == "script");
        CHECK(root.at("config").front().at("cap_config") == json{{"migrated", true}});
    }
}

TEST_CASE("PluginConfig refuses to store a record without an identity", "[PluginConfig]")
{
    ScopedDataDir data_dir_guard("plugin-config-identity");

    PluginConfig config;
    config.save_config(CapabilityConfigEntry{capability_id(PluginCapabilityType::Script, "cap_a", ""), "1.0.0", json::object()});
    config.save_config(CapabilityConfigEntry{capability_id(PluginCapabilityType::Script, "", "plugin_a"), "1.0.0", json::object()});

    // Neither could ever be looked up again, so neither is kept.
    CHECK_FALSE(config.has_config(capability_id(PluginCapabilityType::Script, "cap_a", "")));
    CHECK_FALSE(config.has_config(capability_id(PluginCapabilityType::Script, "", "plugin_a")));
    CHECK_FALSE(config.dirty());
}

TEST_CASE("PluginConfig preserves unknown keys inside cap_config", "[PluginConfig]")
{
    ScopedDataDir data_dir_guard("plugin-config-unknown");

    // The host never interprets cap_config, so a nested/odd shape must round-trip untouched.
    const json nested = json{{"nested", {{"deep", json::array({1, 2, 3})}}}, {"flag", false}, {"name", "x"}};

    PluginConfig config;
    const PluginCapabilityId id = capability_id(PluginCapabilityType::Script, "cap_a", "plugin_a");
    REQUIRE(config.store_capability_config(id, nested));

    PluginConfig reloaded;
    reloaded.load();
    CHECK(reloaded.get_config(id)->config == nested);
}
