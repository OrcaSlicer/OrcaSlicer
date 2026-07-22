#include <catch2/catch_all.hpp>

#include <libslic3r/Utils.hpp>
#include <libslic3r/libslic3r.h>         // GCODEVIEWER_APP_KEY, SLIC3R_APP_KEY (via libslic3r_version.h)
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp> // secret_constants::USER_SECRET_FILENAME

#include "plugin_test_utils.hpp"

#include <boost/filesystem.hpp>

#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Seed the deny registry with the same list install_hook() uses. Both draw from
// PluginAuditManager::default_denied_filenames(), so the test and production seeding cannot
// drift apart. The registry is a process singleton, so repeated seeding only appends harmless
// duplicates; matching is unaffected.
void seed_denied_names()
{
    PluginAuditManager& mgr = PluginAuditManager::instance();
    for (const auto& name : PluginAuditManager::default_denied_filenames())
        mgr.add_denied_filename(name);
}

} // namespace

TEST_CASE("Plugin audit denies app config and token filenames anywhere", "[audit]")
{
    seed_denied_names();
    const PluginAuditManager& mgr = PluginAuditManager::instance();

    SECTION("the seeded names are denied by their base name")
    {
        CHECK(mgr.is_denied_filename(fs::path(SLIC3R_APP_KEY ".conf")));
        CHECK(mgr.is_denied_filename(fs::path(GCODEVIEWER_APP_KEY ".conf")));
        CHECK(mgr.is_denied_filename(fs::path(SLIC3R_APP_KEY ".ini")));
        CHECK(mgr.is_denied_filename(fs::path(GCODEVIEWER_APP_KEY ".ini")));
        CHECK(mgr.is_denied_filename(fs::path(secret_constants::USER_SECRET_FILENAME)));
    }

    SECTION("companions holding the same secrets are denied by the prefix rule")
    {
        CHECK(mgr.is_denied_filename(fs::path(SLIC3R_APP_KEY ".conf.bak")));
        CHECK(mgr.is_denied_filename(fs::path(std::string(secret_constants::USER_SECRET_FILENAME) + ".tmp")));
        // Windows alternate data streams share the same base name.
        CHECK(mgr.is_denied_filename(fs::path(SLIC3R_APP_KEY ".conf:stream")));
    }

    SECTION("the denial ignores the directory the file lives in")
    {
        CHECK(mgr.is_denied_filename(fs::path("/tmp") / (SLIC3R_APP_KEY ".conf")));
        CHECK(mgr.is_denied_filename(fs::path("/some/plugin/dir") / (SLIC3R_APP_KEY ".conf")));
        // Traversal is handled for free: filename() of the path below is already the denied name.
        CHECK(mgr.is_denied_filename(fs::path(data_dir()) / "plugins" / ".." / (SLIC3R_APP_KEY ".conf")));
    }

    SECTION("matching is case-insensitive on every platform")
    {
        CHECK(mgr.is_denied_filename(fs::path("orcaslicer.conf")));
        CHECK(mgr.is_denied_filename(fs::path("ORCASLICER.CONF")));
        CHECK(mgr.is_denied_filename(fs::path("ORCA_REFRESH_TOKEN.SEC")));
    }

    SECTION("an unrelated name that merely shares a stem is not denied")
    {
        // The prefix is the full registered name ("OrcaSlicer.conf"), not the stem "OrcaSlicer",
        // so a sibling file with a different extension/suffix stays allowed.
        CHECK_FALSE(mgr.is_denied_filename(fs::path(data_dir()) / (SLIC3R_APP_KEY "_other.txt")));
        CHECK_FALSE(mgr.is_denied_filename(fs::path(data_dir()) / (SLIC3R_APP_KEY ".json")));
        CHECK_FALSE(mgr.is_denied_filename(fs::path("orca_refresh_token.txt")));
    }

    SECTION("an empty path is not denied")
    {
        CHECK_FALSE(mgr.is_denied_filename(fs::path()));
    }
}

TEST_CASE("Plugin audit deny beats allowed roots and the Loading read exemption", "[audit]")
{
    ScopedDataDir data_dir_guard("plugin-audit-deny");
    seed_denied_names();

    PluginAuditManager& mgr = PluginAuditManager::instance();
    // Reproduce install_hook()'s grant: data_dir() is a global allowed root, so both the app
    // config and the token would otherwise be reachable simply by living inside it.
    mgr.add_global_allowed_root(data_dir());

    // Enter a plugin context. The deny must hold in Loading mode, which every scope runs in.
    ScopedPluginAuditContext ctx("test_plugin", "", PluginAuditManager::AuditMode::Loading);

    const fs::path conf  = fs::path(data_dir()) / (SLIC3R_APP_KEY ".conf");
    const fs::path token = fs::path(data_dir()) / secret_constants::USER_SECRET_FILENAME;

    SECTION("a non-denied file inside the allowed root is writable (root really grants writes)")
    {
        AuditDecision decision = mgr.check_open((fs::path(data_dir()) / "plugin_data.txt").string(), "w");
        CHECK(decision.allowed);
    }

    SECTION("writing the app config is blocked despite data_dir() being allowed")
    {
        AuditDecision decision = mgr.check_open(conf.string(), "w");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied filename");
    }

    SECTION("reading the app config is blocked even though Loading exempts reads")
    {
        // Without the deny, a read in Loading mode short-circuits to allow. The deny sits above
        // that exemption, so this must still be blocked.
        AuditDecision decision = mgr.check_open(conf.string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied filename");
    }

    SECTION("reading the cloud refresh token is blocked in Loading mode")
    {
        AuditDecision decision = mgr.check_open(token.string(), "r");
        CHECK_FALSE(decision.allowed);
    }

    SECTION("the token staging companion (.tmp) is blocked too")
    {
        AuditDecision decision = mgr.check_open((token.string() + ".tmp"), "w");
        CHECK_FALSE(decision.allowed);
    }

    SECTION("a traversal path resolving to the config is blocked")
    {
        const fs::path traversal = fs::path(data_dir()) / "plugins" / ".." / (SLIC3R_APP_KEY ".conf");
        AuditDecision  decision  = mgr.check_open(traversal.string(), "r");
        CHECK_FALSE(decision.allowed);
    }
}

TEST_CASE("Plugin audit deny beats a plugin's own scoped root", "[audit]")
{
    ScopedDataDir data_dir_guard("plugin-audit-scoped");
    seed_denied_names();

    PluginAuditManager& mgr = PluginAuditManager::instance();

    // A plugin's private directory, granted as a scoped root while it runs.
    const fs::path plugin_dir = fs::path(data_dir()) / "plugins" / "test_plugin";
    fs::create_directories(plugin_dir);

    ScopedPluginAuditContext ctx("test_plugin", "", PluginAuditManager::AuditMode::Loading);
    mgr.add_scoped_allowed_root(plugin_dir);

    SECTION("the plugin's own non-denied file opens for read and write")
    {
        const std::string own_file = (plugin_dir / "state.json").string();
        CHECK(mgr.check_open(own_file, "r").allowed);
        CHECK(mgr.check_open(own_file, "w").allowed);
    }

    SECTION("a denied name stashed inside the plugin's own root is still blocked")
    {
        const std::string smuggled = (plugin_dir / (SLIC3R_APP_KEY ".conf")).string();
        AuditDecision      decision = mgr.check_open(smuggled, "w");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied filename");
    }
}

TEST_CASE("Plugin audit does not constrain non-plugin code", "[audit]")
{
    ScopedDataDir data_dir_guard("plugin-audit-noplugin");
    seed_denied_names();

    PluginAuditManager& mgr = PluginAuditManager::instance();
    mgr.clear_current_plugin(); // no plugin context: this is OrcaSlicer's own C++/internal Python

    const fs::path conf = fs::path(data_dir()) / (SLIC3R_APP_KEY ".conf");

    // The name is still recognised as denied...
    CHECK(mgr.is_denied_filename(conf));
    // ...but with no current plugin the access check allows it: denies constrain plugin code only.
    CHECK(mgr.check_open(conf.string(), "w").allowed);
    CHECK(mgr.check_open(conf.string(), "r").allowed);
}
