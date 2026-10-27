#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/system/error_code.hpp>
#include <catch2/catch_all.hpp>

#include <libslic3r/Utils.hpp>
#include <libslic3r/libslic3r.h>         // GCODEVIEWER_APP_KEY, SLIC3R_APP_KEY (via libslic3r_version.h)
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginBridge.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/Utils/IPrinterAgent.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp> // secret_constants::USER_SECRET_FILENAME

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "libslic3r_version.h"
#include "plugin_test_utils.hpp"
#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <pybind11/cast.h>
#include <pybind11/embed.h>
#include <pybind11/eval.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>

#include <fstream>
#include <functional>
#include <ios>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace py = pybind11;

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

// Seed the keyword registry with the same list install_hook() uses. Same rationale as
// seed_denied_names(): a process-singleton registry, seeded from the single shared source so
// production and tests cannot drift apart.
void seed_denied_keywords()
{
    PluginAuditManager& mgr = PluginAuditManager::instance();
    for (const auto& keyword : PluginAuditManager::default_denied_path_keywords())
        mgr.add_denied_path_keyword(keyword);
}

} // namespace

TEST_CASE("Plugin audit denies host-managed filenames anywhere", "[audit]")
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
        CHECK(mgr.is_denied_filename(fs::path(".install_state.json")));
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

TEST_CASE("Plugin audit deny beats allowed roots", "[audit]")
{
    ScopedDataDir data_dir_guard("plugin-audit-deny");
    seed_denied_names();

    PluginAuditManager& mgr = PluginAuditManager::instance();
    // Reproduce install_hook()'s grant: data_dir() is a global allowed root, so both the app
    // config and the token would otherwise be reachable simply by living inside it.
    mgr.add_global_allowed_root(data_dir());

    // Enter a plugin context. The deny must hold even inside a globally allowed root.
    ScopedPluginAuditContext ctx("test_plugin", "");

    const fs::path conf  = fs::path(data_dir()) / (SLIC3R_APP_KEY ".conf");
    const fs::path token = fs::path(data_dir()) / secret_constants::USER_SECRET_FILENAME;
    const fs::path install_state = fs::path(data_dir()) / "plugins" / "example" / ".install_state.json";

    SECTION("a non-denied file inside the allowed root is writable (root really grants writes)")
    {
        AuditDecision decision = mgr.check_open((fs::path(data_dir()) / "plugin_data.txt").string(), "w");
        CHECK(decision.allowed);
    }

    SECTION("a file outside the allowed root is blocked for reads as well as writes")
    {
        AuditDecision decision = mgr.check_open((fs::path(data_dir()).parent_path() / "outside.txt").string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "outside allowed root");
    }

    SECTION("writing the app config is blocked despite data_dir() being allowed")
    {
        AuditDecision decision = mgr.check_open(conf.string(), "w");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied filename");
    }

    SECTION("reading the app config is blocked despite the allowed root")
    {
        AuditDecision decision = mgr.check_open(conf.string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied filename");
    }

    SECTION("reading the cloud refresh token is blocked")
    {
        AuditDecision decision = mgr.check_open(token.string(), "r");
        CHECK_FALSE(decision.allowed);
    }

    SECTION("the token staging companion (.tmp) is blocked too")
    {
        AuditDecision decision = mgr.check_open((token.string() + ".tmp"), "w");
        CHECK_FALSE(decision.allowed);
    }

    SECTION("plugin install state is blocked for reads and writes despite the allowed root")
    {
        AuditDecision read_decision = mgr.check_open(install_state.string(), "r");
        CHECK_FALSE(read_decision.allowed);
        CHECK(read_decision.reason == "denied filename");

        AuditDecision write_decision = mgr.check_open(install_state.string(), "w");
        CHECK_FALSE(write_decision.allowed);
        CHECK(write_decision.reason == "denied filename");
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

    ScopedPluginAuditContext ctx("test_plugin", "");
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

TEST_CASE("Plugin audit denies secret/certificate/config-like paths by keyword", "[audit]")
{
    seed_denied_keywords();
    const PluginAuditManager& mgr = PluginAuditManager::instance();

    SECTION("a 'secrets' directory component is denied")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/secrets/api_key.json")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/secret/token.txt")));
    }

    SECTION("a 'certificate(s)' directory component is denied")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/resources/cert/slicer_base64.cer")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/resources/certificates/ca.pem")));
    }

    SECTION("a 'conf'/'config' directory or config file component is denied")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/conf/settings.json")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/config/settings.json")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/plugin.conf")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/plugin.ini")));
    }

    SECTION("matching is case-insensitive")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/SECRETS/token.txt")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/resources/Certificates/ca.pem")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/PLUGIN.CONF")));
    }

    SECTION("matching is not limited to the base name -- any ancestor component counts")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/data/secrets/nested/deep/file.txt")));
    }

    SECTION("an unrelated path is not denied")
    {
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/plugin/output/model.gcode")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/plugin/storage/state.json")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/packages/cp312/numpy/__config__.py")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/packages/cp312/numpy/_core/_ufunc_config.py")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/Lib/configparser.py")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/Lib/sysconfig.py")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/Lib/logging/config.py")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/packages/cp312/certifi/cacert.pem")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/users/Conference/output.txt")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/users/Concert/output.txt")));
    }

    SECTION("an empty path is not denied")
    {
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path()));
    }
}

TEST_CASE("Plugin audit checks denied keywords below data_dir, not in the directories above it", "[PluginAudit]")
{
    // Flatpak keeps the app's data folder under <home>/.var/app/<app-id>/config/OrcaSlicer, so
    // data_dir() itself has a "config" component. That ancestor describes the sandbox's layout,
    // not what the folder holds, and must not hard-deny every path inside it.
    ScopedDataDir data_dir_guard("plugin-audit-data-dir-keyword");
    const fs::path flatpak_data_dir = data_dir_guard.dir / "config" / "OrcaSlicer";
    set_data_dir(flatpak_data_dir.string());

    seed_denied_names();
    seed_denied_keywords();

    PluginAuditManager& mgr = PluginAuditManager::instance();
    // Reproduce install_hook()'s grant: data_dir() is a global allowed root.
    mgr.add_global_allowed_root(data_dir());

    ScopedPluginAuditContext ctx("test_plugin", "");

    SECTION("the root and the paths below it that carry no keyword are allowed")
    {
        CHECK_FALSE(mgr.is_denied_path_keyword(flatpak_data_dir / "orca_plugins" / "state.json"));
        CHECK(mgr.check_open(data_dir(), "r").allowed);
        CHECK(mgr.check_open((flatpak_data_dir / "orca_plugins" / "test_plugin" / "state.json").string(), "r").allowed);
        CHECK(mgr.check_open((flatpak_data_dir / "orca_plugins" / "test_plugin" / "state.json").string(), "w").allowed);
        CHECK(mgr.check_open((flatpak_data_dir / "system" / "profiles" / "vendor.json").string(), "r").allowed);
    }

    SECTION("a 'secrets' component below data_dir() is still denied")
    {
        AuditDecision decision = mgr.check_open((flatpak_data_dir / "secrets" / "token.json").string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied path keyword");
    }

    SECTION("a 'config' component below data_dir() is still denied")
    {
        AuditDecision decision = mgr.check_open((flatpak_data_dir / "config" / "settings.json").string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied path keyword");
    }

    SECTION("the app config below data_dir() is still denied by name")
    {
        AuditDecision decision = mgr.check_open((flatpak_data_dir / (SLIC3R_APP_KEY ".conf")).string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied filename");
    }

    SECTION("an unrelated resources root keeps the full-path keyword check")
    {
        ScopedResourcesDir resources_dir_guard("plugin-audit-data-dir-keyword-resources");
        mgr.add_global_allowed_root(resources_dir(), /*allow_write=*/false);

        CHECK(mgr.check_open((fs::path(resources_dir()) / "profiles" / "vendor.json").string(), "r").allowed);

        AuditDecision decision = mgr.check_open((fs::path(resources_dir()) / "cert" / "slicer_base64.cer").string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied path keyword");
    }
}

TEST_CASE("Plugin audit is_denied_path combines the filename and keyword registries", "[audit]")
{
    seed_denied_names();
    seed_denied_keywords();
    const PluginAuditManager& mgr = PluginAuditManager::instance();

    SECTION("a filename-registry match is denied")
    {
        CHECK(mgr.is_denied_path(fs::path(SLIC3R_APP_KEY ".conf")));
    }

    SECTION("a keyword-registry match is denied")
    {
        CHECK(mgr.is_denied_path(fs::path("/plugin/secrets/token.txt")));
    }

    SECTION("a path matching neither registry is not denied")
    {
        CHECK_FALSE(mgr.is_denied_path(fs::path("/plugin/output/model.gcode")));
    }
}

TEST_CASE("Plugin audit a read-only allowed root blocks writes but not reads", "[audit]")
{
    ScopedDataDir      data_dir_guard("plugin-audit-readonly");
    ScopedResourcesDir resources_dir_guard("plugin-audit-readonly-resources");
    seed_denied_names();
    seed_denied_keywords();

    PluginAuditManager& mgr = PluginAuditManager::instance();
    mgr.add_global_allowed_root(resources_dir(), /*allow_write=*/false);

    ScopedPluginAuditContext ctx("test_plugin", "");

    const fs::path readonly_file = fs::path(resources_dir()) / "profiles" / "vendor.json";

    SECTION("a read inside the read-only root is allowed")
    {
        CHECK(mgr.check_open(readonly_file.string(), "r").allowed);
    }

    SECTION("a write inside the read-only root is blocked")
    {
        AuditDecision decision = mgr.check_open(readonly_file.string(), "w");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "outside allowed root");
    }

    SECTION("a create inside the read-only root is blocked")
    {
        AuditDecision decision = mgr.check_path_access(readonly_file, /*is_write=*/true);
        CHECK_FALSE(decision.allowed);
    }

    SECTION("the bundled cert underneath the read-only root is denied even for reads")
    {
        const fs::path cert = fs::path(resources_dir()) / "cert" / "slicer_base64.cer";
        AuditDecision  decision = mgr.check_open(cert.string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied path keyword");
    }
}

TEST_CASE("Plugin audit network permissions match by scheme and host", "[audit]")
{
    SECTION("a URL is reduced to its scheme and host")
    {
        CHECK(PluginAuditManager::normalize_network_url("https://API.Example.com:8443/v1/items?page=2#top") ==
              "https://api.example.com");
        CHECK(PluginAuditManager::normalize_network_url("http://example.com") == "http://example.com");
        CHECK(PluginAuditManager::normalize_network_url("https://[2001:db8::1]:443/path") == "https://[2001:db8::1]");
    }

    SECTION("non-http schemes and targets without a scheme are not network URLs")
    {
        CHECK(PluginAuditManager::normalize_network_url("ftp://example.com").empty());
        CHECK(PluginAuditManager::normalize_network_url("api.example.com").empty());
        CHECK(PluginAuditManager::normalize_network_url("https://").empty());
    }

    SECTION("a URL target matches the same scheme and host regardless of path and port")
    {
        const std::vector<std::string> granted{"https://api.example.com"};
        CHECK(PluginAuditManager::network_permission_matches(granted, "https://api.example.com/v1/items?page=2"));
        CHECK(PluginAuditManager::network_permission_matches(granted, "https://api.example.com:8443/upload"));
    }

    SECTION("canonical host grants are broad but persisted full-URL grants stay exact")
    {
        const std::vector<std::string> declared_host{"https://api.example.com"};
        CHECK(PluginAuditManager::network_permission_matches(declared_host, "https://api.example.com/v1"));

        const std::vector<std::string> saved_url{"https://api.example.com/v1"};
        CHECK(PluginAuditManager::network_permission_matches(saved_url, "https://api.example.com/v1"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_url, "https://api.example.com/v2"));
    }

    SECTION("a URL target does not match a different host or a different scheme")
    {
        const std::vector<std::string> granted{"https://api.example.com"};
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "https://evil.example.com/v1"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "http://api.example.com/v1"));
    }

    SECTION("a bare host target matches the host of a grant")
    {
        const std::vector<std::string> granted{"https://api.example.com"};
        CHECK(PluginAuditManager::network_permission_matches(granted, "api.example.com"));
        CHECK(PluginAuditManager::network_permission_matches(granted, "api.example.com:443"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "other.example.com"));
    }

    SECTION("a saved full URL does not authorize raw socket access to its host")
    {
        const std::vector<std::string> saved_url{"https://api.example.com/v1"};
        CHECK(PluginAuditManager::network_permission_matches(saved_url, "https://api.example.com/v1"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_url, "api.example.com"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_url, "api.example.com:443"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_url, "('api.example.com', 443)"));

        const std::vector<std::string> saved_ipv6{"https://[2001:db8::1]/v1"};
        CHECK(PluginAuditManager::network_permission_matches(saved_ipv6, "https://[2001:db8::1]/v1"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_ipv6, "2001:db8::1"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_ipv6, "[2001:db8::1]:443"));

        // Canonical scheme://host grants still cover raw socket targets, IPv6 brackets aside.
        const std::vector<std::string> canonical{"https://api.example.com", "https://[2001:db8::1]"};
        CHECK(PluginAuditManager::network_permission_matches(canonical, "api.example.com"));
        CHECK(PluginAuditManager::network_permission_matches(canonical, "2001:db8::1"));

        // Normalization lowercases the scheme and host, so case alone keeps a grant canonical.
        const std::vector<std::string> uppercase{"HTTPS://API.Example.com", "HTTPS://[2001:DB8::1]"};
        CHECK(PluginAuditManager::network_permission_matches(uppercase, "api.example.com"));
        CHECK(PluginAuditManager::network_permission_matches(uppercase, "2001:db8::1"));
    }

    SECTION("legacy persisted address tuples keep matching their host")
    {
        const std::vector<std::string> legacy_granted{"('api.example.com', 443)"};
        CHECK(PluginAuditManager::network_permission_matches(legacy_granted, "api.example.com"));

        const std::vector<std::string> granted{"https://api.example.com"};
        CHECK(PluginAuditManager::network_permission_matches(granted, "('api.example.com', 443)"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "('93.184.216.34', 443)"));
    }

    SECTION("an IPv6 literal matches across bracketed and bare forms")
    {
        const std::vector<std::string> granted{"https://[2001:db8::1]"};
        CHECK(PluginAuditManager::network_permission_matches(granted, "2001:db8::1"));
        CHECK(PluginAuditManager::network_permission_matches(granted, "[2001:db8::1]:443"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "2001:db8::2"));
    }

    SECTION("a non-http URL only matches an exact grant")
    {
        const std::vector<std::string> granted{"ftp://example.com/file"};
        CHECK(PluginAuditManager::network_permission_matches(granted, "ftp://example.com/file"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "ftp://example.com/other"));

        const std::vector<std::string> https{"https://api.example.com"};
        CHECK_FALSE(PluginAuditManager::network_permission_matches(https, "ftp://api.example.com/file"));
    }

    SECTION("an empty grant list or an empty target never matches")
    {
        const std::vector<std::string> granted{};
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "https://api.example.com"));

        const std::vector<std::string> any{"https://api.example.com"};
        CHECK_FALSE(PluginAuditManager::network_permission_matches(any, ""));
    }
}

TEST_CASE("Plugin audit a scoped root can also be registered read-only", "[audit]")
{
    ScopedDataDir data_dir_guard("plugin-audit-scoped-readonly");
    seed_denied_names();
    seed_denied_keywords();

    PluginAuditManager& mgr = PluginAuditManager::instance();

    const fs::path readonly_dir = fs::path(data_dir()) / "readonly_scope";
    fs::create_directories(readonly_dir);

    ScopedPluginAuditContext ctx("test_plugin", "");
    mgr.add_scoped_allowed_root(readonly_dir, /*allow_write=*/false);

    SECTION("a read inside the scoped read-only root is allowed")
    {
        CHECK(mgr.check_open((readonly_dir / "vendor.json").string(), "r").allowed);
    }

    SECTION("a write inside the scoped read-only root is blocked")
    {
        CHECK_FALSE(mgr.check_open((readonly_dir / "vendor.json").string(), "w").allowed);
    }
}

// ---------------------------------------------------------------------------
// The audit hook, run in the embedded interpreter
// ---------------------------------------------------------------------------
//
// Each TEST_CASE runs in its own process (catch_discover_tests), so each brings the plugin system
// up with the audit hook installed, once, and shuts it down at the end. The tests loop over their
// cases instead of using SECTIONs, which would restart the interpreter for every section.

namespace {

// Discovered, never loaded: the hook only needs the plugin's root and its sidecar.
const char* const AUDIT_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Audit Plugin"
# description = "Plugin audit hook fixture"
# author = "OrcaSlicer"
# version = "1.0"
# type = "script"
# ///
)PY";

// A third-party library, installed outside the stdlib and outside the plugin.
const char* const LIBRARY_SOURCE = R"PY(
import socket
import urllib.request

def read_all(paths):
    for path in paths:
        with open(path) as f:
            f.read()

def connect_from_host(port):
    socket.gethostname()
    socket.create_connection(("127.0.0.1", port)).close()

def fetch(url):
    with urllib.request.urlopen(url) as response:
        return response.read()

def fetch_each(urls):
    for url in urls:
        yield fetch(url)

class ReadingHandler(urllib.request.BaseHandler):
    """Reads a file while each response is processed."""
    def __init__(self, path):
        self.path = path
        self.outcome = None

    def http_response(self, request, response):
        try:
            open(self.path).close()
            self.outcome = "ok"
        except PermissionError:
            self.outcome = "denied"
        return response
)PY";

// Test-side Python, run outside any plugin context: files outside every allowed root, a local
// HTTP server, the library, and every module the tests use imported up front, so that no import
// happens inside a plugin context. Code from exec() runs as "<string>", which is not stdlib, so
// attempt() and fetch() stand in for plugin code.
const char* const AUDIT_SETUP = R"PY(
import http.client, http.server, os, pathlib, shutil, socket, subprocess, sys, tempfile, threading
import urllib.request, zipfile
if os.name != "nt":
    import pty

APPROVED = os.path.join(OUTSIDE, "approved.txt")
PRIVATE = os.path.join(OUTSIDE, "private.txt")
MISSING = os.path.join(OUTSIDE, "missing")
TREE = os.path.join(OUTSIDE, "tree")
TRUE_COMMAND = ["cmd", "/c", "exit 0"] if os.name == "nt" else ["true"]
for path in (APPROVED, PRIVATE):
    with open(path, "w") as f:
        f.write("x")
for sub in ("one", "two"):
    os.makedirs(os.path.join(TREE, sub))
    with open(os.path.join(TREE, sub, "f.txt"), "w") as f:
        f.write("x")

LIBRARY = os.path.join(OUTSIDE, "library")
os.makedirs(LIBRARY)
with open(os.path.join(LIBRARY, "netlib.py"), "w") as f:
    f.write(LIBRARY_SOURCE)
sys.path.insert(0, LIBRARY)
import netlib

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", URL_B)
            body = b""
        else:
            self.send_response(200)
            body = b"ok"
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server_thread = threading.Thread(target=server.serve_forever, daemon=True)
server_thread.start()
PORT = server.server_address[1]
BASE = "http://127.0.0.1:%d" % PORT
URL_A, URL_B, URL_REDIRECT = BASE + "/a", BASE + "/b", BASE + "/redirect"
urllib.request.install_opener(urllib.request.build_opener(urllib.request.ProxyHandler({})))

def fetch(url):
    with urllib.request.urlopen(url) as response:
        return response.read()

fetch(URL_A)  # urllib imports some modules on its first request

def attempt(action):
    try:
        action()
        return "ok"
    except PermissionError:
        return "denied"

def spawn_checked(argv):
    parent = os.getpid()
    try:
        return pty.spawn(argv)
    finally:
        if os.getpid() != parent:
            os._exit(1)  # a child whose exec was refused must not go on running the tests

finalizer_outcomes = []

class TrackedRequest(urllib.request.Request):
    def __del__(self):
        finalizer_outcomes.append(attempt(lambda: open(PRIVATE).close()))
)PY";

struct Prompt
{
    std::string              plugin;
    std::string              event;
    std::vector<std::string> targets;
};

// One line per prompt, "event: target ...", so a failed check shows every prompt.
std::string describe(const std::vector<Prompt>& prompts)
{
    std::string text;
    for (const Prompt& prompt : prompts) {
        text += prompt.event + ":";
        for (const std::string& target : prompt.targets)
            text += " " + target;
        text += "\n";
    }
    return text;
}

// The plugin system with the audit hook installed, and the permission dialog replaced by a
// recorder that answers with `approve`. Declare it first in a test, so it is destroyed last.
//
// Only the manager is torn down here; the interpreter is finalized once per test process by
// PythonInterpreterRunEnd in slic3rutils_tests_main.cpp.
struct AuditHarness
{
    // Before initialize(): the interpreter creates {data_dir}/python/packages and {data_dir}/log.
    // data_dir() is a global allowed root from then on -- the first test to initialize Python in
    // this process (under ctest, this one) -- so the test files live in `outside`.
    ScopedDataDir                      data{"plugin-audit-hook"};
    bool                               initialized = PluginManager::instance().initialize();
    ScopedTemporaryDir                 outside{"orca-plugin-audit-outside"};
    std::vector<Prompt>                prompts;
    std::function<bool(const Prompt&)> approve = [](const Prompt&) { return false; };

    AuditHarness()
    {
        PluginAuditManager& audit = PluginAuditManager::instance();
        audit.clear_current_plugin();
        audit.clear_current_capability();
        audit.clear_audit_denial();
        audit.clear_last_violation();
        PluginAuditManager::set_permission_prompt_for_testing(
            [this](const std::string& event, const std::vector<std::string>& targets) {
                prompts.push_back({PluginAuditManager::instance().current_plugin(), event, targets});
                return approve(prompts.back());
            });
    }

    ~AuditHarness()
    {
        PluginAuditManager::set_permission_prompt_for_testing(nullptr);
        PluginManager::instance().shutdown();
    }

    // Call before taking the GIL: discovery is not Python work.
    void add_plugin(const std::string& key)
    {
        const fs::path root = data.plugins_dir() / key;
        fs::create_directories(root);
        std::ofstream((root / (key + ".py")).string(), std::ios::binary) << AUDIT_PLUGIN_SOURCE;
        save_permissions(key, {});
        PluginManager::instance().discover_plugins(/*async=*/false, /*clear=*/true);
    }

    // Grants as if saved in an earlier session.
    void save_permissions(const std::string& key, const PluginPermissions& permissions)
    {
        PluginInstallState state;
        state.installed_from    = "local";
        state.installed_version = "1.0";
        state.plugin_name       = key;
        state.permissions       = permissions;
        REQUIRE(write_install_state(data.plugins_dir() / key, state));
    }

    PluginPermissions saved_permissions(const std::string& key) const
    {
        PluginInstallState state;
        read_install_state(data.plugins_dir() / key, state);
        return state.permissions;
    }
};

// The test-side Python globals. Construct and destroy it with the GIL held; it stops the HTTP
// server before the test case ends, so nothing keeps serving once the test's temp dirs are gone.
struct PythonSide
{
    py::dict globals;

    explicit PythonSide(const AuditHarness& harness)
    {
        globals["OUTSIDE"]        = harness.outside.path().string();
        globals["LIBRARY_SOURCE"] = LIBRARY_SOURCE;
        exec(AUDIT_SETUP);
    }

    // A Python error fails the test here, as text: an error_already_set left to propagate would
    // outlive the test case and keep a traceback with references into the interpreter.
    void exec(const char* code)
    {
        std::string error;
        try {
            py::exec(code, globals);
        } catch (py::error_already_set& raised) {
            error = raised.what();
        }
        if (!error.empty())
            FAIL(error);
    }

    ~PythonSide()
    {
        try {
            // Stop the server thread while Python is still up: the interpreter now outlives the
            // test case, so a daemon thread must not keep serving (and touching test temp dirs).
            globals["server"].attr("shutdown")();
            globals["server"].attr("server_close")();
            globals["server_thread"].attr("join")();
        } catch (...) {
        }
    }

    std::string              str(const char* name) const { return py::str(globals[name]).cast<std::string>(); }
    std::string              eval(const char* expression) const { return py::str(py::eval(expression, globals)).cast<std::string>(); }
    std::vector<std::string> strings(const char* name) const { return globals[name].cast<std::vector<std::string>>(); }

    // Runs `code` as `plugin_key`, as a trampoline call does: inside the caller's GIL, in an
    // audit scope of its own.
    void run_as(const std::string& plugin_key, const char* code)
    {
        ScopedPluginAuditContext audit(plugin_key);
        exec(code);
    }
};

PluginPermissions http_grants(std::vector<std::string> urls)
{
    PluginPermissions permissions;
    permissions.network_http = std::move(urls);
    return permissions;
}

const char* const PLUGIN = "AuditPlugin";

} // namespace

TEST_CASE("A saved request grant covers the request's own connection in a later session", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    harness.save_permissions(PLUGIN, http_grants({python.str("URL_A")}));

    python.run_as(PLUGIN, "outcome = attempt(lambda: fetch(URL_A))");

    CHECK(python.str("outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A saved request grant does not cover another URL or a redirect", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    harness.save_permissions(PLUGIN, http_grants({python.str("URL_A"), python.str("URL_REDIRECT")}));

    python.run_as(PLUGIN, R"PY(
outcomes = [attempt(lambda: fetch(url)) for url in (URL_A, URL_B, URL_REDIRECT)]
outcomes += [attempt(lambda: netlib.fetch(url)) for url in (URL_A, URL_B)]
outcomes.append(attempt(lambda: list(netlib.fetch_each([URL_A, URL_B]))))
)PY");

    // The redirect target is asked for inside the approved redirecting request, and a library's
    // wrapper or generator asks for the second URL as plain code does.
    CHECK(python.strings("outcomes") == std::vector<std::string>{"ok", "denied", "denied", "ok", "denied", "denied"});
    const std::string url_b = "urllib.Request: " + python.str("URL_B") + "\n";
    CHECK(describe(harness.prompts) == url_b + url_b + url_b + url_b);
}

TEST_CASE("Code an approved operation calls back into is judged on its own", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    python.exec(R"PY(
CALLBACK_COPY = os.path.join(OUTSIDE, "callback-copy")
handler = netlib.ReadingHandler(PRIVATE)
handler_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), handler)
walk_outcomes = []
copy_outcomes = []

def on_walk_error(error):
    walk_outcomes.append(attempt(lambda: open(PRIVATE).close()))

def copy_file(source, destination):
    copy_outcomes.append(attempt(lambda: open(PRIVATE).close()))
)PY");
    PluginPermissions permissions = http_grants({python.str("URL_A")});
    permissions.fs_read           = {python.str("APPROVED"), python.str("MISSING")};
    permissions.fs_readwrite      = {python.str("TREE"), python.str("CALLBACK_COPY")};
    harness.save_permissions(PLUGIN, permissions);

    // A library handler that urllib calls while the approved request runs, the plugin's own
    // os.walk error callback and copytree copy function (called once per file), and a library
    // loop over a saved file and an unsaved one.
    python.run_as(PLUGIN, R"PY(
request_outcome = attempt(lambda: handler_opener.open(URL_A).read())
for _ in os.walk(MISSING, onerror=on_walk_error):
    pass
shutil.copytree(TREE, CALLBACK_COPY, copy_function=copy_file)
loop_outcome = attempt(lambda: netlib.read_all([APPROVED, PRIVATE]))
)PY");

    CHECK(python.str("request_outcome") == "ok");
    CHECK(python.eval("handler.outcome") == "denied");
    CHECK(python.strings("walk_outcomes") == std::vector<std::string>{"denied"});
    CHECK(python.strings("copy_outcomes") == std::vector<std::string>{"denied", "denied"});
    CHECK(python.str("loop_outcome") == "denied");
    const std::string private_file = "open: " + python.str("PRIVATE") + "\n";
    CHECK(describe(harness.prompts) == private_file + private_file + private_file + private_file + private_file);
}

TEST_CASE("A saved directory walk covers each directory it lists", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginPermissions permissions;
    permissions.fs_read = {python.str("TREE"), python.str("APPROVED")};
    harness.save_permissions(PLUGIN, permissions);

    // The walk lists the tree's two subdirectories on later resumes, after the plugin's own
    // audited reads in between. Abandoned walks leave frames whose generator is gone, which the
    // next audited event lets go.
    python.run_as(PLUGIN, R"PY(
walked = []
for root, dirs, files in os.walk(TREE):
    walked.append(root)
    open(APPROVED).close()
globbed = list(pathlib.Path(TREE).glob("**/*"))

abandoned = os.walk(TREE)
next(abandoned)
del abandoned
open(APPROVED).close()
for _ in pathlib.Path(TREE).glob("*"):
    break
open(APPROVED).close()
)PY");

    CHECK(py::len(python.globals["walked"]) == 3);
    CHECK(py::len(python.globals["globbed"]) == 4);
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A freshly approved operation covers its nested events", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    struct Case
    {
        const char* prompts;
        const char* code;
    };
    // Each operation names targets no earlier one saved, so each is asked once. FsCreate and
    // FsDelete grants are never saved, so those operations are asked every time. Only the
    // expected prompts are approved, so a nested event left uncovered fails the operation.
    std::vector<Case> cases = {
        {"shutil.copytree", "shutil.copytree(TREE, os.path.join(OUTSIDE, 'copy'))"},
        {"shutil.copyfile", "shutil.copyfile(APPROVED, os.path.join(OUTSIDE, 'copied.txt'))"},
        {"shutil.move", "shutil.move(os.path.join(OUTSIDE, 'copied.txt'), os.path.join(OUTSIDE, 'moved.txt'))"},
        {"shutil.copystat", "shutil.copystat(os.path.join(TREE, 'one', 'f.txt'), PRIVATE)"},
        {"shutil.make_archive", "shutil.make_archive(os.path.join(OUTSIDE, 'archive'), 'zip', root_dir=TREE)"},
        {"shutil.unpack_archive", "shutil.unpack_archive(os.path.join(OUTSIDE, 'archive.zip'), os.path.join(OUTSIDE, 'unpacked'))"},
        {"pathlib.Path.glob", "list(pathlib.Path(TREE).glob('**/*'))"},
        {"subprocess.Popen", "subprocess.run(TRUE_COMMAND, check=True)"},
        {"tempfile.mkdtemp", "tempfile.mkdtemp(dir=OUTSIDE)"},
        {"tempfile.mkstemp", "os.close(tempfile.mkstemp(dir=OUTSIDE)[0])"},
        {"shutil.rmtree", "shutil.rmtree(os.path.join(OUTSIDE, 'copy'))"},
        // The host grant from connect() also authorizes targetless send() events.
        {"http.client.connect",
         "connection = http.client.HTTPConnection('127.0.0.1', PORT)\n"
         "connection.request('GET', '/a')\n"
         "connection.getresponse().read()\n"
         "connection.close()"},
    };
#ifndef _WIN32
    cases.push_back({"os.fwalk", "list(os.fwalk(os.path.join(OUTSIDE, 'unpacked')))"});
    // The forked child's exec is covered by the spawn too: had it prompted, the child's own
    // denial would end it with status 1.
    cases.push_back({"pty.spawn", "spawn_status = spawn_checked(TRUE_COMMAND + ['spawned'])"});
#endif

    for (const Case& operation : cases) {
        INFO(operation.code);
        const std::string expected = std::string(operation.prompts) + "\n";
        harness.approve = [&expected](const Prompt& prompt) {
            return expected.find(prompt.event + "\n") != std::string::npos;
        };
        harness.prompts.clear();
        python.run_as(PLUGIN, operation.code);

        std::string asked;
        for (const Prompt& prompt : harness.prompts)
            asked += (asked.empty() ? "" : "\n") + prompt.event;
        CHECK(asked == operation.prompts);
    }
#ifndef _WIN32
    CHECK(python.eval("spawn_status") == "0");
#endif
}

TEST_CASE("An event with no target is granted by name and remembered", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    harness.approve = [](const Prompt& prompt) { return prompt.event != "socket.sendto"; };
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.run_as(PLUGIN, R"PY(
socket.socket().close()
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sendto_outcome = attempt(lambda: udp.sendto(b"x", ("127.0.0.1", 9)))
udp.close()
)PY");

    // The socket is asked for once, without a target to show; the datagram's address is a target.
    CHECK(python.str("sendto_outcome") == "denied");
    CHECK(describe(harness.prompts) == "socket.__new__:\nsocket.sendto: ('127.0.0.1', 9)\n");
    CHECK(harness.saved_permissions(PLUGIN).events == std::vector<std::string>{"Socket:socket.__new__"});
    CHECK(harness.saved_permissions(PLUGIN).network_socket.empty());

    harness.prompts.clear();
    python.run_as(PLUGIN, "socket.socket().close()");
    CHECK(describe(harness.prompts).empty());

    // A grant for an event with no target covers nothing else: a library's approved
    // gethostname() does not carry the connection it makes next.
    harness.approve = [](const Prompt& prompt) { return prompt.event == "socket.gethostname"; };
    python.run_as(PLUGIN, "connect_outcome = attempt(lambda: netlib.connect_from_host(PORT))");
    CHECK(python.str("connect_outcome") == "denied");
    CHECK(describe(harness.prompts) == "socket.gethostname:\nsocket.getaddrinfo: 127.0.0.1\n");
    harness.prompts.clear();
    harness.approve = [](const Prompt&) { return true; };

#ifndef _WIN32
    // sendmsg() without an address names no target either.
    python.run_as(PLUGIN, R"PY(
left, right = socket.socketpair()
left.sendmsg([b"x"])
left.close()
right.close()
)PY");
    CHECK(describe(harness.prompts) == "socket.sendmsg:\n");
    CHECK(harness.saved_permissions(PLUGIN).events ==
          std::vector<std::string>{"Socket:socket.__new__", "Socket:socket.gethostname", "Socket:socket.sendmsg"});
#endif
}

TEST_CASE("A saved network grant covers a network event that names no target", "[Audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // The connection is opened outside the plugin, so only the send itself is judged: it names
    // no host, just the connection it writes to.
    python.exec(R"PY(
connection = http.client.HTTPConnection("127.0.0.1", PORT)
connection.connect()
)PY");
    harness.save_permissions(PLUGIN, http_grants({"http://127.0.0.1"}));

    python.run_as(PLUGIN, R"PY(
send_outcome = attempt(lambda: connection.send(b"GET /"))
)PY");

    // The host grant already covers the connection's send: no prompt, and no event grant.
    CHECK(python.str("send_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK(harness.saved_permissions(PLUGIN).events.empty());

    // With no network grant there is nothing covering the send, so it is asked for by name.
    harness.save_permissions(PLUGIN, http_grants({}));
    python.run_as(PLUGIN, R"PY(
denied_outcome = attempt(lambda: connection.send(b"GET /"))
)PY");
    CHECK(python.str("denied_outcome") == "denied");
    CHECK(describe(harness.prompts) == "http.client.send:\n");
    CHECK(harness.saved_permissions(PLUGIN).events.empty());
    CHECK(harness.saved_permissions(PLUGIN).network_http.empty());

    python.exec("connection.close()");
}

TEST_CASE("An event grant does not stand in for another access or a target of the same name", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    harness.approve = [asked = 0](const Prompt&) mutable { return asked++ == 0; };
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    python.exec(R"PY(
READ_FD = os.open(APPROVED, os.O_RDONLY)
WRITE_FD = os.open(os.path.join(OUTSIDE, "scratch.txt"), os.O_WRONLY | os.O_CREAT)
)PY");

    // A file descriptor has no path to show or save: a granted read of one is not a write.
    python.run_as(PLUGIN, R"PY(
fd_outcomes = [attempt(lambda: open(READ_FD, "r", closefd=False).close()),
               attempt(lambda: open(WRITE_FD, "w", closefd=False).close())]
)PY");
    CHECK(python.strings("fd_outcomes") == std::vector<std::string>{"ok", "denied"});
    CHECK(describe(harness.prompts) == "open:\nopen:\n");
    CHECK(harness.saved_permissions(PLUGIN).events == std::vector<std::string>{"FsRead:open"});

    // A saved os.fork event grant is not an os.system command named "os.fork".
    PluginPermissions permissions;
    permissions.events = {"FsRead:open", "ProcessCreate:os.fork"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
reread_outcome = attempt(lambda: open(READ_FD, "r", closefd=False).close())
system_outcome = attempt(lambda: os.system("os.fork"))
os.close(READ_FD)
os.close(WRITE_FD)
)PY");
    CHECK(python.str("reread_outcome") == "ok");
    CHECK(python.str("system_outcome") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().event == "os.system");
    CHECK(harness.saved_permissions(PLUGIN).process.empty());
}

TEST_CASE("A finalizer run when a root frame is let go is judged as the plugin without a prompt", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginPermissions permissions = http_grants({python.str("URL_A")});
    permissions.fs_read           = {python.str("APPROVED")};
    harness.save_permissions(PLUGIN, permissions);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    // Once the request returns, only its held frame keeps the request object alive, and the
    // request's finalizer reads a file the plugin was never granted.
    python.run_as(PLUGIN, "fetch(TrackedRequest(URL_A))");
    CHECK(python.strings("finalizer_outcomes") == std::vector<std::string>{"denied"}); // at scope end
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(mgr.audit_denial_pending());

    python.exec("finalizer_outcomes.clear()");
    python.run_as(PLUGIN, R"PY(
fetch(TrackedRequest(URL_A))
open(APPROVED).close()  # the next event lets the returned request's frame go
outcomes_before_scope_end = list(finalizer_outcomes)
)PY");
    CHECK(python.strings("outcomes_before_scope_end") == std::vector<std::string>{"denied"});
    CHECK(describe(harness.prompts).empty());

    // The finalizer's denial during another event's decision leaves that event's own denial intact.
    python.exec("finalizer_outcomes.clear()");
    python.run_as(PLUGIN, R"PY(
fetch(TrackedRequest(URL_A))
try:
    os.listdir(OUTSIDE)
    outer_error = ""
except PermissionError as error:
    outer_error = str(error)
)PY");
    CHECK(python.strings("finalizer_outcomes") == std::vector<std::string>{"denied"});
    CHECK_THAT(python.str("outer_error"), Catch::Matchers::ContainsSubstring("os.listdir"));
    CHECK(describe(harness.prompts) == "os.listdir: " + python.str("OUTSIDE") + "\n");
    AuditViolation violation;
    REQUIRE(mgr.last_violation(violation));
    CHECK(violation.event_name == "os.listdir");
    CHECK(mgr.audit_denial_pending());
}

TEST_CASE("A nested audit scope does not see the outer scope's running operations", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin("PluginA");
    harness.add_plugin("PluginB");
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginPermissions permissions;
    permissions.fs_read = {python.str("TREE")};
    harness.save_permissions("PluginA", permissions);

    {
        // Two walks plugin A started and suspended after listing the tree itself.
        ScopedPluginAuditContext plugin_a("PluginA");
        python.exec("first = os.walk(TREE)\nnext(first)\nsecond = os.walk(TREE)\nnext(second)");
        {
            // Plugin B resuming one of them is asked for each subdirectory, as plugin B.
            ScopedPluginAuditContext plugin_b("PluginB");
            python.exec("listed_by_b = len(list(first))");
        }
        // Back in plugin A's scope, its walks are covered again.
        python.exec("listed_by_a = len(list(second))");
    }

    CHECK(python.globals["listed_by_b"].cast<int>() == 0);
    CHECK(python.globals["listed_by_a"].cast<int>() == 2);
    REQUIRE(harness.prompts.size() == 2);
    for (const Prompt& prompt : harness.prompts) {
        CHECK(prompt.plugin == "PluginB");
        CHECK(prompt.event == "os.scandir");
    }
}

TEST_CASE("Root frames held when a capability call ends are let go with the GIL, as the plugin", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.walk"; };
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    (void) PythonPluginBridge::instance(); // force the embedded module registration into the binary
    python.globals["orca"] = py::module_::import("orca");

    // walk_and_drop() leaves a returned walk's frame held by the audit scope alone, with a marker
    // in its locals; the frame is let go when the capability call ends, which runs the marker's
    // finalizer.
    python.exec(R"PY(
class Marker:
    def __init__(self, action=None):
        self.action = action
    def __del__(self):
        finalized.append(self.action() if self.action else "ran")

finalized = []

def walk_and_drop(marker):
    walker = os.walk(TREE, onerror=marker)
    next(walker)
    del walker

class ConfigCapability(orca.PythonPluginBase):
    def get_name(self):
        return "config_cap"
    def get_default_config(self):
        walk_and_drop(Marker())
        return {"finalized_during_call": len(finalized)}
    def has_config_ui(self):
        walker = os.walk(TREE, onerror=Marker(lambda: attempt(lambda: open(PRIVATE).close())))
        next(walker)
        attempt(lambda: os.listdir(OUTSIDE))  # denied: the trampoline clears the plugin key
        del walker  # the walk's frame is now held by the audit scope alone
        raise RuntimeError("the capability fails after a denial")

class TicketAgent(orca.printer_agent.PrinterAgentBase):
    def get_name(self):
        return "ticket_agent"
    def request_bind_ticket(self):
        walk_and_drop(Marker())
        return (0, "ticket")
)PY");

    py::object config_instance = python.globals["ConfigCapability"]();
    auto       config_cap      = config_instance.cast<std::shared_ptr<PluginCapabilityInterface>>();
    config_cap->set_audit_plugin_key("GilPlugin");
    config_cap->set_resolved_identity("config_cap", PluginCapabilityType::Script);

    py::object agent_instance = python.globals["TicketAgent"]();
    auto       agent_cap      = agent_instance.cast<std::shared_ptr<PluginCapabilityInterface>>();
    agent_cap->set_audit_plugin_key("GilPlugin");
    agent_cap->set_resolved_identity("ticket_agent", PluginCapabilityType::PrinterConnection);
    auto agent = std::dynamic_pointer_cast<IPrinterAgent>(agent_cap);
    REQUIRE(agent);

    // A host call arrives without the GIL; both calls take it before their audit scope.
    nlohmann::json config;
    int            ticket_result = -1;
    std::string    ticket;
    {
        py::gil_scoped_release released;
        config        = config_cap->get_default_config();
        ticket_result = agent->request_bind_ticket(&ticket);
    }
    CHECK(config == nlohmann::json{{"finalized_during_call", 0}});
    CHECK(ticket_result == 0);
    CHECK(ticket == "ticket");
    CHECK(python.strings("finalized") == std::vector<std::string>{"ran", "ran"});

    // On the trampoline's denial path the frame is let go as the plugin, though the ambient key
    // was cleared: the finalizer's read is audited and denied without a prompt.
    python.exec("finalized.clear()");
    harness.prompts.clear();
    CHECK_THROWS_AS(config_cap->has_config_ui(), py::error_already_set);
    CHECK(python.strings("finalized") == std::vector<std::string>{"denied"});
    CHECK(describe(harness.prompts) == "os.walk: " + python.str("TREE") + "\nos.listdir: " + python.str("OUTSIDE") + "\n");
}

TEST_CASE("Stdlib code is recognized through a symlinked Python home", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    const py::dict paths   = py::module_::import("sysconfig").attr("get_paths")();
    const fs::path stdlib  = py::str(paths["stdlib"]).cast<std::string>();
    const fs::path purelib = py::str(paths["purelib"]).cast<std::string>();
    const fs::path link    = harness.outside.path() / "linked-stdlib";
    boost::system::error_code ec;
    fs::create_directory_symlink(stdlib, link, ec);
    if (ec)
        SKIP("Cannot create a directory symlink here: " + ec.message());

    CHECK(PluginAuditManager::is_stdlib_file((stdlib / "os.py").string()));
    CHECK(PluginAuditManager::is_stdlib_file((link / "urllib" / "request.py").string()));
    CHECK(PluginAuditManager::is_stdlib_file("<frozen os>"));
    CHECK_FALSE(PluginAuditManager::is_stdlib_file((purelib / "requests" / "api.py").string()));
    CHECK_FALSE(PluginAuditManager::is_stdlib_file((link / "site-packages" / "requests" / "api.py").string()));
    CHECK_FALSE(PluginAuditManager::is_stdlib_file((harness.outside.path() / "plugin.py").string()));
    CHECK_FALSE(PluginAuditManager::is_stdlib_file("<string>"));
}

TEST_CASE("A plugin may read the CPython stdlib without a prompt, but not site-packages or for writing", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    const py::dict paths   = py::module_::import("sysconfig").attr("get_paths")();
    const fs::path stdlib  = py::str(paths["stdlib"]).cast<std::string>();
    const fs::path purelib = py::str(paths["purelib"]).cast<std::string>();
    // os.py ships with every CPython installation. A site-packages path is third-party code even
    // when the directory is empty: the prompt must come before the missing file is noticed.
    const fs::path stdlib_file = stdlib / "os.py";
    REQUIRE(fs::exists(stdlib_file));

    python.globals["STDLIB_FILE"]       = stdlib_file.string();
    python.globals["SITE_PACKAGE_FILE"] = (purelib / "regression_probe.py").string();
    python.run_as(PLUGIN, R"PY(
read_outcome = attempt(lambda: open(STDLIB_FILE).read())
write_outcome = attempt(lambda: open(STDLIB_FILE, "r+").close())
site_packages_outcome = attempt(lambda: open(SITE_PACKAGE_FILE).close())
)PY");

    CHECK(python.str("read_outcome") == "ok");
    CHECK(python.str("write_outcome") == "denied");
    CHECK(python.str("site_packages_outcome") == "denied");
    // Only the write and the site-packages read are asked about; the stdlib read is implicit.
    const std::string write_prompt         = "open: " + python.str("STDLIB_FILE") + "\n";
    const std::string site_packages_prompt = "open: " + python.str("SITE_PACKAGE_FILE") + "\n";
    CHECK(describe(harness.prompts) == write_prompt + site_packages_prompt);
}

TEST_CASE("A filesystem wildcard grant covers the folder and its descendants at any depth", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
DEEP_FILE = os.path.join(TREE, "one", "deep", "deeper", "f.txt")
os.makedirs(os.path.dirname(DEEP_FILE))
with open(DEEP_FILE, "w") as f:
    f.write("x")
NESTED_FILE = os.path.join(TREE, "one", "f.txt")
SIBLING = TREE + "-sibling"
SIBLING_FILE = os.path.join(SIBLING, "f.txt")
os.makedirs(SIBLING)
with open(SIBLING_FILE, "w") as f:
    f.write("x")
# Lexically under TREE, but ".." resolves outside the granted folder.
ESCAPE_FILE = os.path.join(TREE, "..", "private.txt")
)PY");

    const std::string tree = python.str("TREE");
    PluginPermissions permissions;
    permissions.fs_read = {tree + "/*"};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
deep_outcome = attempt(lambda: open(DEEP_FILE).read())
root_outcome = attempt(lambda: os.listdir(TREE))
sibling_outcome = attempt(lambda: open(SIBLING_FILE).read())
escape_outcome = attempt(lambda: open(ESCAPE_FILE).read())
)PY");

    // The tree grant covers the folder itself (listing it) and a file two directories below it.
    CHECK(python.str("deep_outcome") == "ok");
    CHECK(python.str("root_outcome") == "ok");
    // A path sharing the folder's prefix is not a descendant, and ".." is resolved before the
    // comparison, so neither escapes the grant; both are asked about.
    CHECK(python.str("sibling_outcome") == "denied");
    CHECK(python.str("escape_outcome") == "denied");
    const std::string sibling_prompt = "open: " + python.str("SIBLING_FILE") + "\n";
    const std::string escape_prompt  = "open: " + python.str("ESCAPE_FILE") + "\n";
    CHECK(describe(harness.prompts) == sibling_prompt + escape_prompt);

    // Only the final component may be a wildcard: a "*" inside it is an ordinary character.
    permissions.fs_read = {tree + "/*.txt"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, "literal_outcome = attempt(lambda: open(NESTED_FILE).read())");
    CHECK(python.str("literal_outcome") == "denied");
    const std::string literal_prompt = "open: " + python.str("NESTED_FILE") + "\n";
    CHECK(describe(harness.prompts) == literal_prompt);
}

TEST_CASE("A tree grant compares canonical paths on both sides", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // A folder outside TREE, reached through a symlink planted inside it.
    const fs::path tree        = python.str("TREE");
    const fs::path outside_dir = harness.outside.path() / "symlink-target";
    const fs::path link        = tree / "escape";
    fs::create_directories(outside_dir);
    std::ofstream((outside_dir / "outside.txt").string(), std::ios::binary) << "x";

    boost::system::error_code ec;
    fs::create_directory_symlink(outside_dir, link, ec);
    if (ec)
        SKIP("Cannot create a directory symlink here: " + ec.message());

    PluginPermissions permissions;
    permissions.fs_read = {tree.string() + "/*"};
    harness.save_permissions(PLUGIN, permissions);

    const std::string escaped_file = (link / "outside.txt").string();
    python.globals["ESCAPED_FILE"] = escaped_file;
    python.run_as(PLUGIN, "escaped_outcome = attempt(lambda: open(ESCAPED_FILE).read())");

    // The target canonicalizes outside the canonical grant root, so the link does not inherit
    // the tree grant. The read/write wildcard test covers a non-existing write tail.
    CHECK(python.str("escaped_outcome") == "denied");
    CHECK(describe(harness.prompts) == "open: " + escaped_file + "\n");

    // A grant rooted at the link applies to what the link points at, not just to its path.
    permissions.fs_read = {link.string() + "/*"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.globals["TARGET_FILE"] = (outside_dir / "outside.txt").string();
    python.run_as(PLUGIN, "via_target_outcome = attempt(lambda: open(TARGET_FILE).read())");

    CHECK(python.str("via_target_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A write through a dangling symlink inside a granted tree is denied", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // Two links inside TREE whose targets do not exist yet, so both dangle:
    //  - "dangling" points outside the tree, the reported escape;
    //  - "dangling-inside" points at a not-yet-created file that would be inside the tree, to pin
    //    the fail-closed choice: a dangling link is rejected whatever its target.
    const fs::path tree          = python.str("TREE");
    const fs::path outside_file  = harness.outside.path() / "written-through-link.txt";
    const fs::path link          = tree / "dangling";
    const fs::path inside_target = tree / "not-created.txt";
    const fs::path inside_link   = tree / "dangling-inside";
    boost::system::error_code ec;
    fs::create_symlink(outside_file, link, ec);
    if (ec)
        SKIP("Cannot create a file symlink here: " + ec.message());
    fs::create_symlink(inside_target, inside_link, ec);
    if (ec)
        SKIP("Cannot create a file symlink here: " + ec.message());

    PluginPermissions permissions;
    permissions.fs_readwrite = {tree.string() + "/*"};
    harness.save_permissions(PLUGIN, permissions);

    python.globals["DANGLING_LINK"]        = link.string();
    python.globals["DANGLING_INSIDE_LINK"] = inside_link.string();
    python.run_as(PLUGIN, R"PY(
inside_outcome = attempt(lambda: open(os.path.join(TREE, "inside.txt"), "w").close())
outcome = attempt(lambda: open(DANGLING_LINK, "w").close())
inside_link_outcome = attempt(lambda: open(DANGLING_INSIDE_LINK, "w").close())
)PY");

    // The wildcard still covers an ordinary new file in the tree. A dangling final symlink is not
    // covered: the grant describes the path the plugin spelled, while a write through the link
    // would land at its target, so the prompt is refused and neither destination is created.
    CHECK(python.str("inside_outcome") == "ok");
    CHECK(python.str("outcome") == "denied");
    CHECK(python.str("inside_link_outcome") == "denied");
    CHECK(describe(harness.prompts) == "open: " + link.string() + "\nopen: " + inside_link.string() + "\n");
    CHECK_FALSE(fs::exists(outside_file));
    CHECK_FALSE(fs::exists(inside_target));
}

TEST_CASE("A relative wildcard permission request is shown and stored as an absolute grant", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    // The plugin declares the grant relative to the working directory of the request, while the
    // audit hook reports absolute targets. Resolve against the directory the request is made in.
    const std::string relative_grant = "tree/*";
    PluginPermissions requested;
    requested.fs_readwrite = {relative_grant};

    const fs::path original_cwd = fs::current_path();
    boost::system::error_code ec;
    fs::current_path(harness.outside.path(), ec);
    REQUIRE_FALSE(ec);
    const fs::path request_dir = fs::current_path();

    std::string shown;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&shown](const std::string& message) { shown = message; return true; });
    const bool granted = mgr.request_permissions(PLUGIN, requested);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);
    fs::current_path(original_cwd, ec);

    const std::string absolute_grant = (request_dir / "tree" / "*").string();

    REQUIRE(granted);
    // The prompt names the resolved scope, not the relative declaration.
    CHECK(shown.find(absolute_grant) != std::string::npos);
    CHECK(shown.find("\n" + relative_grant + "\n") == std::string::npos);
    CHECK(harness.saved_permissions(PLUGIN).fs_readwrite == std::vector<std::string>{absolute_grant});

    // The saved absolute grant is the effective one: an absolute target below the resolved
    // directory is covered, so the write is not asked about again.
    python.globals["RESOLVED_FILE"] = (harness.outside.path() / "tree" / "written.txt").string();
    python.run_as(PLUGIN, "outcome = attempt(lambda: open(RESOLVED_FILE, 'w').close())");
    CHECK(python.str("outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A filesystem wildcard keeps read and write grants separate", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
NESTED_FILE = os.path.join(TREE, "one", "f.txt")
NEW_FILE = os.path.join(TREE, "one", "new.txt")
)PY");
    const std::string tree = python.str("TREE");
    PluginPermissions permissions;
    permissions.fs_read = {tree + "/*"};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
read_outcome = attempt(lambda: open(NESTED_FILE).read())
write_outcome = attempt(lambda: open(NEW_FILE, "w").close())
)PY");
    CHECK(python.str("read_outcome") == "ok");
    CHECK(python.str("write_outcome") == "denied");
    // A read grant does not cover the write-shaped open, so that one is asked about.
    const std::string write_prompt = "open: " + python.str("NEW_FILE") + "\n";
    CHECK(describe(harness.prompts) == write_prompt);

    // The reverse direction is separate too: a read/write wildcard leaves plain reads in the
    // read bucket, which is now empty.
    permissions.fs_read.clear();
    permissions.fs_readwrite = {tree + "/*"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
write_outcome = attempt(lambda: open(NEW_FILE, "w").close())
reread_outcome = attempt(lambda: open(NESTED_FILE).read())
)PY");
    CHECK(python.str("write_outcome") == "ok");
    CHECK(python.str("reread_outcome") == "denied");
    const std::string reread_prompt = "open: " + python.str("NESTED_FILE") + "\n";
    CHECK(describe(harness.prompts) == reread_prompt);
}

TEST_CASE("A filesystem wildcard does not override a denied path", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    python.exec(R"PY(
DENIED_DIR = os.path.join(OUTSIDE, "secrets")
os.makedirs(DENIED_DIR)
with open(os.path.join(DENIED_DIR, "token.txt"), "w") as f:
    f.write("x")
)PY");

    // A grant wide enough to cover the deny registry's target still loses to the deny.
    PluginPermissions permissions;
    permissions.fs_read = {python.str("OUTSIDE") + "/*"};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, "denied_outcome = attempt(lambda: open(os.path.join(DENIED_DIR, 'token.txt')).read())");

    CHECK(python.str("denied_outcome") == "denied");
    CHECK(describe(harness.prompts).empty());
    AuditViolation violation;
    REQUIRE(mgr.last_violation(violation));
    CHECK(violation.reason == "denied path");
}
