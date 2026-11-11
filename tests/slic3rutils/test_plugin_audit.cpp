#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/filesystem.hpp>
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

#include <clocale>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <locale>
#include <memory>
#include <stdexcept>
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

std::string read_text_file(const fs::path& path)
{
    std::ifstream in(path.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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

namespace {

// Converts paths as UTF-8 for the test's lifetime, as the app does from startup
// (boost::nowide::nowide_filesystem() in OrcaSlicer.cpp).
struct Utf8PathConversion
{
    std::locale previous = boost::nowide::nowide_filesystem();
    ~Utf8PathConversion() { fs::path::imbue(previous); }
};

// Switches the C library's character classification to a single-byte code page, as the app's UI
// language does on Windows (Russian_Russia.1251), when the system has one.
struct SingleByteCtypeLocale
{
    std::string previous;
    bool        applied = false;

    SingleByteCtypeLocale()
    {
        if (const char* current = std::setlocale(LC_CTYPE, nullptr))
            previous = current;
        applied = std::setlocale(LC_CTYPE, ".1251") != nullptr;
    }
    ~SingleByteCtypeLocale()
    {
        if (applied)
            std::setlocale(LC_CTYPE, previous.c_str());
    }
};

} // namespace

TEST_CASE("Plugin audit keyword check keeps non-Latin path names intact", "[audit]")
{
    seed_denied_keywords();
    const PluginAuditManager& mgr = PluginAuditManager::instance();

    // Lowering these names byte by byte in a single-byte C locale broke their UTF-8, and turning
    // the result back into a path threw out of the audit hook: the plugin call never returned.
    const Utf8PathConversion    utf8_paths;
    const SingleByteCtypeLocale ctype;
    INFO("single-byte C locale in use: " << (ctype.applied ? "yes" : "no"));

    const std::string folder   = "\xD0\x9D\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x8F \xD0\xBF\xD0\xB0\xD0\xBF\xD0\xBA\xD0\xB0"; // "Новая папка"
    const std::string part     = "\xD0\xB4\xD0\xB5\xD1\x82\xD0\xB0\xD0\xBB\xD1\x8C.stl";                          // "деталь.stl"
    const std::string settings = "\xD0\xBD\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB8.INI";      // "настройки.INI"

    SECTION("a path with non-Latin names is checked without an error and allowed")
    {
        bool denied = true;
        CHECK_NOTHROW(denied = mgr.is_denied_path_keyword(fs::path("/models/" + folder + "/" + part)));
        CHECK_FALSE(denied);
    }

    SECTION("keywords and config files next to non-Latin names are still denied, in any case")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/models/" + folder + "/CONFIG/a.stl")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/models/" + folder + "/" + settings)));
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

def start_and_join_thread():
    worker = threading.Thread(target=lambda: None)
    worker.start()
    worker.join()

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

TEST_CASE("A permission prompt that throws denies the event", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    harness.approve = [](const Prompt&) -> bool { throw std::runtime_error("prompt failed"); };

    python.run_as(PLUGIN, "outcome = attempt(lambda: open(PRIVATE).close())");

    CHECK(python.str("outcome") == "denied");
    CHECK(describe(harness.prompts) == "open: " + python.str("PRIVATE") + "\n");
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
    // Each case starts with no saved grants, so its root event is asked once. A freshly approved
    // create or delete saves the parent folder, so the stored grants are reset between cases.
    // Only the expected prompts are approved, so a nested event left uncovered fails the
    // operation.
    std::vector<Case> cases = {
        {"shutil.copytree", "shutil.copytree(TREE, os.path.join(OUTSIDE, 'copy'))"},
        {"shutil.copyfile", "shutil.copyfile(APPROVED, os.path.join(OUTSIDE, 'copied.txt'))"},
        {"shutil.move", "shutil.move(os.path.join(OUTSIDE, 'copied.txt'), os.path.join(OUTSIDE, 'moved.txt'))"},
        {"shutil.copystat", "shutil.copystat(os.path.join(TREE, 'one', 'f.txt'), PRIVATE)"},
        {"shutil.make_archive", "shutil.make_archive(os.path.join(OUTSIDE, 'archive'), 'zip', root_dir=TREE)"},
        {"shutil.unpack_archive",
         "shutil.unpack_archive(os.path.join(OUTSIDE, 'archive.zip'), os.path.join(OUTSIDE, 'unpacked'))"},
        {"pathlib.Path.glob", "list(pathlib.Path(TREE).glob('**/*'))"},
        {"subprocess.Popen", "subprocess.run(TRUE_COMMAND, check=True)"},
        {"tempfile.mkdtemp", "tempfile.mkdtemp(dir=OUTSIDE)"},
        {"tempfile.mkstemp", "os.close(tempfile.mkstemp(dir=OUTSIDE)[0])"},
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
        // Dropping the grants from the previous case keeps its persisted create/delete parent
        // folder from covering this one.
        harness.save_permissions(PLUGIN, {});
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

TEST_CASE("A finalizer hardlink to an ungranted source is denied without prompting during frame release", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginPermissions permissions = http_grants({python.str("URL_A")});
    harness.save_permissions(PLUGIN, permissions);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    python.exec(R"PY(
FINALIZER_LINK_SOURCE = os.path.join(OUTSIDE, "finalizer-link-source.txt")
with open(FINALIZER_LINK_SOURCE, "w") as f:
    f.write("x")
FINALIZER_LINK_DEST = os.path.join(OUTSIDE, "finalizer-link-dest.txt")
finalizer_link_outcomes = []

class LinkTrackedRequest(urllib.request.Request):
    def __del__(self):
        finalizer_link_outcomes.append(attempt(lambda: os.link(FINALIZER_LINK_SOURCE, FINALIZER_LINK_DEST)))
)PY");

    // The request is kept alive by the approved fetch's root frame. When the frame is released, its
    // finalizer tries to hardlink a source that was never granted. Releasing frames runs finalizers
    // where a modal cannot open, so the source check must fail closed without prompting.
    python.run_as(PLUGIN, "fetch(LinkTrackedRequest(URL_A))");

    CHECK(python.strings("finalizer_link_outcomes") == std::vector<std::string>{"denied"});
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("FINALIZER_LINK_DEST"))));
    CHECK_FALSE(mgr.audit_denial_pending());
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

TEST_CASE("A saved exact file grant covers an equivalent spelling of the same file", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // The granted file, a file beside it, a lookalike name, the not-yet-existing target of a
    // dangling link, and the not-yet-existing target of a write.
    python.exec(R"PY(
FILE = os.path.join(OUTSIDE, "approved-file.txt")
OTHER = os.path.join(OUTSIDE, "other-file.txt")
LOOKALIKE = os.path.join(OUTSIDE, "approved-file.txt.bak")
NOT_CREATED = os.path.join(OUTSIDE, "not-created.txt")
NEW_FILE = os.path.join(OUTSIDE, "new-file.txt")
ESCAPE_PATH = os.path.join(OUTSIDE, "sub", "..", "other-file.txt")
os.makedirs(os.path.join(OUTSIDE, "sub"))
for path in (FILE, OTHER, LOOKALIKE):
    with open(path, "w") as f:
        f.write("x")
)PY");

    const fs::path            outside     = harness.outside.path();
    const fs::path            file        = python.str("FILE");
    const fs::path            alias       = outside / "alias.txt";
    const fs::path            other_alias = outside / "other-alias.txt";
    const fs::path            dangling    = outside / "dangling.txt";
    boost::system::error_code ec;
    // A link to the granted file, a link to a different file, and a link whose target does not
    // exist yet.
    fs::create_symlink(file, alias, ec);
    if (ec)
        SKIP("Cannot create a file symlink here: " + ec.message());
    fs::create_symlink(outside / "other-file.txt", other_alias, ec);
    if (ec)
        SKIP("Cannot create a file symlink here: " + ec.message());
    fs::create_symlink(outside / "not-created.txt", dangling, ec);
    if (ec)
        SKIP("Cannot create a file symlink here: " + ec.message());

    python.globals["ALIAS"]      = alias.string();
    python.globals["OTHER_LINK"] = other_alias.string();
    python.globals["DANGLING"]   = dangling.string();

    PluginPermissions permissions;
    permissions.fs_read      = {python.str("FILE")};
    permissions.fs_readwrite = {python.str("NOT_CREATED"), python.str("NEW_FILE")};
    harness.save_permissions(PLUGIN, permissions);

    // A relative target is resolved against the working directory the open resolves in, so the
    // grant matches the file it names even though the audit target carries no directory.
    const fs::path original_cwd = fs::current_path();
    fs::current_path(outside, ec);
    REQUIRE_FALSE(ec);
    python.run_as(PLUGIN, R"PY(
relative_read_outcome = attempt(lambda: open("approved-file.txt").read())
relative_write_outcome = attempt(lambda: open("new-file.txt", "w").close())
)PY");
    fs::current_path(original_cwd, ec);
    REQUIRE_FALSE(ec);

    python.run_as(PLUGIN, R"PY(
verbatim_outcome = attempt(lambda: open(FILE).read())
dot_outcome = attempt(lambda: open(os.path.join(OUTSIDE, ".", "approved-file.txt")).read())
updown_outcome = attempt(lambda: open(os.path.join(OUTSIDE, "sub", "..", "approved-file.txt")).read())
alias_outcome = attempt(lambda: open(ALIAS).read())
other_outcome = attempt(lambda: open(OTHER).read())
escape_outcome = attempt(lambda: open(ESCAPE_PATH).read())
other_link_outcome = attempt(lambda: open(OTHER_LINK).read())
lookalike_outcome = attempt(lambda: open(LOOKALIKE).read())
dangling_outcome = attempt(lambda: open(DANGLING, "w").close())
)PY");

    CHECK(python.str("verbatim_outcome") == "ok");
    CHECK(python.str("relative_read_outcome") == "ok");
    CHECK(python.str("relative_write_outcome") == "ok");
    CHECK(python.str("dot_outcome") == "ok");
    CHECK(python.str("updown_outcome") == "ok");
    CHECK(python.str("alias_outcome") == "ok");

    // Full path equality, not containment: a sibling, a lookalike name, a link to another file,
    // and a ".." spelling that lands on another file are all asked about.
    CHECK(python.str("other_outcome") == "denied");
    CHECK(python.str("escape_outcome") == "denied");
    CHECK(python.str("other_link_outcome") == "denied");
    CHECK(python.str("lookalike_outcome") == "denied");
    // Fail closed on a dangling final link: the write would create its target, not the path the
    // grant names, even though that target itself carries a read/write grant.
    CHECK(python.str("dangling_outcome") == "denied");
    CHECK_FALSE(fs::exists(outside / "not-created.txt"));

    const std::string open_prompt = "open: ";
    const std::string expected_prompts =
        open_prompt + python.str("OTHER") + "\n" + open_prompt + python.str("ESCAPE_PATH") + "\n" +
        open_prompt + python.str("OTHER_LINK") + "\n" + open_prompt + python.str("LOOKALIKE") + "\n" +
        open_prompt + python.str("DANGLING") + "\n";
    CHECK(describe(harness.prompts) == expected_prompts);

#ifdef _WIN32
    // Windows compares path names case-insensitively and accepts either separator, so a different
    // spelling of the granted file matches without another prompt.
    std::string forward_slashes;
    for (const char c : file.string())
        forward_slashes += c == '\\' ? '/' : c;
    python.globals["UPPERCASE_FILE"]     = (file.parent_path() / "APPROVED-FILE.TXT").string();
    python.globals["FORWARD_SLASH_FILE"] = forward_slashes;
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
uppercase_outcome = attempt(lambda: open(UPPERCASE_FILE).read())
forward_slash_outcome = attempt(lambda: open(FORWARD_SLASH_FILE).read())
)PY");
    CHECK(python.str("uppercase_outcome") == "ok");
    CHECK(python.str("forward_slash_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
#endif

    // A grant saved as a link applies to the file it points at, not just to the link's spelling.
    permissions.fs_read      = {alias.string()};
    permissions.fs_readwrite.clear();
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, "grant_via_link_outcome = attempt(lambda: open(FILE).read())");
    CHECK(python.str("grant_via_link_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // An exact grant names one file: it does not become a tree grant, so it does not cover what
    // lives inside the folder it names.
    permissions.fs_read = {python.str("OUTSIDE")};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, "inside_granted_folder_outcome = attempt(lambda: open(FILE).read())");
    CHECK(python.str("inside_granted_folder_outcome") == "denied");
    CHECK(describe(harness.prompts) == open_prompt + python.str("FILE") + "\n");

    // A grant that names a dangling link also covers only its verbatim spelling: an equivalent
    // spelling of the target the write would create is not matched by equivalence.
    permissions.fs_read.clear();
    permissions.fs_readwrite = {dangling.string()};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, "through_dangling_grant_outcome = attempt(lambda: open(NOT_CREATED, 'w').close())");
    CHECK(python.str("through_dangling_grant_outcome") == "denied");
    CHECK(describe(harness.prompts) == open_prompt + python.str("NOT_CREATED") + "\n");
    CHECK_FALSE(fs::exists(outside / "not-created.txt"));
}

TEST_CASE("A saved exact file grant covers an equivalent spelling in a permission request", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    const fs::path file = harness.outside.path() / "requested-file.txt";
    PluginPermissions stored;
    stored.fs_read = {file.string()};
    harness.save_permissions(PLUGIN, stored);

    std::vector<std::string> prompted;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&prompted](const std::string& message) {
            prompted.push_back(message);
            return false;
        });
    PluginAuditManager& mgr = PluginAuditManager::instance();

    // An absolute declaration that is a different spelling of the granted path is already covered,
    // so it is neither shown nor appended as a duplicate grant.
    PluginPermissions dotted;
    dotted.fs_read = {file.parent_path().string() + "/./" + file.filename().string()};
    CHECK(mgr.request_permissions(PLUGIN, dotted));
    CHECK(prompted.empty());
    CHECK(harness.saved_permissions(PLUGIN).fs_read == std::vector<std::string>{file.string()});

    // A relative declaration resolves against the directory the request is made in.
    const fs::path            original_cwd = fs::current_path();
    boost::system::error_code ec;
    fs::current_path(harness.outside.path(), ec);
    REQUIRE_FALSE(ec);
    PluginPermissions relative;
    relative.fs_read = {file.filename().string()};
    const bool relative_granted = mgr.request_permissions(PLUGIN, relative);
    fs::current_path(original_cwd, ec);
    REQUIRE_FALSE(ec);
    CHECK(relative_granted);
    CHECK(prompted.empty());

    // A different file still needs its own grant, so the equivalence above did not widen anything.
    PluginPermissions different;
    different.fs_read = {(harness.outside.path() / "different-file.txt").string()};
    CHECK_FALSE(mgr.request_permissions(PLUGIN, different));
    REQUIRE(prompted.size() == 1);
    CHECK(prompted.front().find("different-file.txt") != std::string::npos);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);
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

TEST_CASE("A create or delete tree declaration is stored as an absolute subtree grant", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    const fs::path folder = harness.outside.path() / "declared";
    fs::create_directories(folder);

    std::vector<std::string> prompted;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&prompted](const std::string& message) {
            prompted.push_back(message);
            return true;
        });

    // "<dir>/*" asks for the folder and every folder below it. The prompt names, and the sidecar
    // stores, the canonical absolute scope the grant actually covers.
    PluginPermissions tree_create;
    tree_create.fs_create = {(folder / "*").string()};
    CHECK(mgr.request_permissions(PLUGIN, tree_create));
    REQUIRE(prompted.size() == 1);
    const std::vector<std::string> saved_create = harness.saved_permissions(PLUGIN).fs_create;
    REQUIRE(saved_create.size() == 1);
    const fs::path saved_create_path(saved_create.front());
    CHECK(saved_create_path.filename() == "*");
    CHECK(fs::equivalent(saved_create_path.parent_path(), folder));
    CHECK(prompted.front().find(saved_create.front()) != std::string::npos);

    // The saved tree grant also covers a later literal request for the same folder.
    PluginPermissions literal_create;
    literal_create.fs_create = {folder.string()};
    CHECK(mgr.request_permissions(PLUGIN, literal_create));
    CHECK(prompted.size() == 1);
    CHECK(harness.saved_permissions(PLUGIN).fs_create == saved_create);

    // Delete is an independent bucket with the same subtree syntax.
    PluginPermissions tree_delete;
    tree_delete.fs_delete = {(folder / "*").string()};
    CHECK(mgr.request_permissions(PLUGIN, tree_delete));
    REQUIRE(prompted.size() == 2);
    const std::vector<std::string> saved_delete = harness.saved_permissions(PLUGIN).fs_delete;
    REQUIRE(saved_delete.size() == 1);
    CHECK(fs::path(saved_delete.front()).filename() == "*");
    CHECK(fs::equivalent(fs::path(saved_delete.front()).parent_path(), folder));
    CHECK(harness.saved_permissions(PLUGIN).fs_create == saved_create);

    // A saved tree grant already covers a nested tree request, so it is neither prompted nor
    // stored again.
    const fs::path nested = folder / "nested";
    fs::create_directories(nested);
    PluginPermissions nested_create;
    nested_create.fs_create = {(nested / "*").string()};
    CHECK(mgr.request_permissions(PLUGIN, nested_create));
    CHECK(prompted.size() == 2);
    CHECK(harness.saved_permissions(PLUGIN).fs_create == saved_create);

    // A wildcard that is not the final tree marker does not name a scope the prompt could show.
    PluginPermissions pattern_delete;
    pattern_delete.fs_delete = {(folder / "*.txt").string()};
    CHECK_FALSE(mgr.request_permissions(PLUGIN, pattern_delete));
    CHECK(prompted.size() == 2);
    CHECK(harness.saved_permissions(PLUGIN).fs_delete == saved_delete);

    // The all-folders marker is only valid on its own.
    PluginPermissions mixed;
    mixed.fs_delete = {"*", folder.string()};
    CHECK_FALSE(mgr.request_permissions(PLUGIN, mixed));
    CHECK(prompted.size() == 2);
    CHECK(harness.saved_permissions(PLUGIN).fs_delete == saved_delete);

    // Positive control: the one-element all-folders form is accepted and stored alone.
    PluginPermissions full;
    full.fs_delete = {"*"};
    CHECK(mgr.request_permissions(PLUGIN, full));
    const std::vector<std::string> saved_full = harness.saved_permissions(PLUGIN).fs_delete;
    REQUIRE(saved_full.size() == 1);
    CHECK(saved_full.front() == "*");
    CHECK(prompted.size() == 3);

    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);
}

#if !defined(_WIN32)
TEST_CASE("A tree declaration rooted at / discloses the subtree scope, not full access", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    std::string shown;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&shown](const std::string& message) {
            shown = message;
            return true;
        });

    // "/*" is the tree grant rooted at "/". It is a subtree scope, not the "*" all-folders marker.
    PluginPermissions requested;
    requested.fs_create = {"/*"};
    CHECK(mgr.request_permissions(PLUGIN, requested));
    CHECK(harness.saved_permissions(PLUGIN).fs_create == std::vector<std::string>{"/*"});

    // The prompt keeps the root and discloses its filesystem-wide scope.
    CHECK(shown.find("/*") != std::string::npos);
    CHECK(shown.find("entire filesystem, drive, or network share") != std::string::npos);
    CHECK(shown.find("anywhere on this computer") == std::string::npos);

    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);
}
#endif

TEST_CASE("A threading permission request is prompted once and saved as a boolean", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    PluginPermissions requested;
    requested.threading = true;

    std::string shown;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&shown](const std::string& message) { shown = message; return true; });
    const bool granted = mgr.request_permissions(PLUGIN, requested);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);

    REQUIRE(granted);
    CHECK(shown.find("create a thread") != std::string::npos);
    CHECK(harness.saved_permissions(PLUGIN).threading);

    // The saved boolean grant covers a repeated declaration without asking again.
    bool prompted_again = false;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&prompted_again](const std::string&) {
            prompted_again = true;
            return false;
        });
    CHECK(mgr.request_permissions(PLUGIN, requested));
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);
    CHECK_FALSE(prompted_again);
}

TEST_CASE("A permission request creates a sidecar for a discovered plugin that has none", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    // A sandbox plugin may be discovered without ever having been given a sidecar. An approval
    // must still be saved, so the next session does not ask again.
    const fs::path sidecar = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    boost::system::error_code ec;
    fs::remove(sidecar, ec);
    REQUIRE_FALSE(ec);

    PluginPermissions requested;
    requested.fs_read = {"/newly/granted"};

    PluginAuditManager::set_permission_message_prompt_for_testing(
        [](const std::string&) { return true; });
    const bool granted = PluginAuditManager::instance().request_permissions(PLUGIN, requested);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);

    REQUIRE(granted);
    CHECK(fs::exists(sidecar));
    CHECK(harness.saved_permissions(PLUGIN).fs_read == std::vector<std::string>{"/newly/granted"});
}

TEST_CASE("A permission request does not recreate a sidecar that disappears while prompting", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    const fs::path sidecar = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    REQUIRE(fs::exists(sidecar));

    bool removed = false;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&sidecar, &removed](const std::string&) {
            boost::system::error_code ec;
            removed = fs::remove(sidecar, ec);
            return true;
        });

    PluginPermissions requested;
    requested.fs_read = {"/new/grant"};
    const bool granted = PluginAuditManager::instance().request_permissions(PLUGIN, requested);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);

    // Readable at the first read, absent at the second: that is lost state, not a new install, so
    // the approval must not create a fresh sidecar.
    REQUIRE(removed);
    CHECK_FALSE(granted);
    CHECK_FALSE(fs::exists(sidecar));
}

TEST_CASE("A permission request leaves an unreadable install-state sidecar untouched", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    const fs::path    sidecar   = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    const std::string malformed = "{ \"permissions\": {";
    {
        std::ofstream out(sidecar.string(), std::ios::binary | std::ios::trunc);
        out << malformed;
    }

    bool prompted = false;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&prompted](const std::string&) {
            prompted = true;
            return true;
        });

    PluginPermissions requested;
    requested.fs_read = {"/new/grant"};
    const bool granted = PluginAuditManager::instance().request_permissions(PLUGIN, requested);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);

    // The file cannot be read, so no grant can be saved safely: fail closed instead of replacing
    // whatever it holds.
    CHECK_FALSE(granted);
    CHECK_FALSE(prompted);
    CHECK(read_text_file(sidecar) == malformed);
}

TEST_CASE("An approved audited operation creates a missing install-state sidecar", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    harness.approve = [](const Prompt&) { return true; };

    const fs::path sidecar = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    boost::system::error_code ec;
    fs::remove(sidecar, ec);
    REQUIRE_FALSE(ec);

    // An event grant seeds a sidecar when none exists.
    python.run_as(PLUGIN, "read_outcome = attempt(lambda: open(PRIVATE).read())");
    CHECK(python.str("read_outcome") == "ok");
    REQUIRE(fs::exists(sidecar));
    CHECK_FALSE(harness.saved_permissions(PLUGIN).fs_read.empty());

    // The threading helper does the same when the sidecar disappears again.
    fs::remove(sidecar, ec);
    REQUIRE_FALSE(ec);
    python.run_as(PLUGIN, "thread_outcome = attempt(start_and_join_thread)");
    CHECK(python.str("thread_outcome") == "ok");
    CHECK(fs::exists(sidecar));
    CHECK(harness.saved_permissions(PLUGIN).threading);
}

TEST_CASE("An approved audited operation does not replace an unreadable install-state sidecar", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    harness.approve = [](const Prompt&) { return true; };

    // Both the event-grant path (persist_permission) and the boolean threading path
    // (persist_threading_grant) must preserve a sidecar they cannot read.
    const fs::path    sidecar   = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    const std::string malformed = "not an install state";
    {
        std::ofstream out(sidecar.string(), std::ios::binary | std::ios::trunc);
        out << malformed;
    }

    // The operation itself is approved for this call; only the persistence is refused.
    python.run_as(PLUGIN, "read_outcome = attempt(lambda: open(PRIVATE).read())");
    python.run_as(PLUGIN, "thread_outcome = attempt(start_and_join_thread)");

    CHECK(python.str("read_outcome") == "ok");
    CHECK(python.str("thread_outcome") == "ok");
    CHECK(read_text_file(sidecar) == malformed);
}

TEST_CASE("Thread creation is denied without a grant and remembered after approval", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // The audit hook's thread event has no user-supplied target; the fixed sentinel is what the
    // prompt and the persisted grant use. The helper joins the worker so no test thread outlives
    // the test case.
    python.run_as(PLUGIN, "thread_outcome = attempt(start_and_join_thread)");
    CHECK(python.str("thread_outcome") == "denied");
    CHECK(describe(harness.prompts) == "_thread.start_new_thread: thread\n");
    CHECK_FALSE(harness.saved_permissions(PLUGIN).threading);

    harness.approve = [](const Prompt&) { return true; };
    harness.prompts.clear();
    python.run_as(PLUGIN, "thread_outcome = attempt(start_and_join_thread)");
    CHECK(python.str("thread_outcome") == "ok");
    CHECK(describe(harness.prompts) == "_thread.start_new_thread: thread\n");
    CHECK(harness.saved_permissions(PLUGIN).threading);

    // The saved grant is effective for later events without another prompt.
    harness.prompts.clear();
    python.run_as(PLUGIN, "thread_outcome = attempt(start_and_join_thread)");
    CHECK(python.str("thread_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("Full access allows an audited operation without a grant", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    PluginPermissions permissions;
    permissions.full_access = true;
    harness.save_permissions(PLUGIN, permissions);

    // PRIVATE sits outside every allowed root, so without full access this read would prompt.
    python.run_as(PLUGIN, "outcome = attempt(lambda: open(PRIVATE).read())");

    CHECK(python.str("outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("Without full access an audited operation still prompts", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    PluginPermissions permissions;
    permissions.full_access = false;
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, "outcome = attempt(lambda: open(PRIVATE).read())");

    CHECK(python.str("outcome") == "denied");
    CHECK(describe(harness.prompts) == "open: " + python.str("PRIVATE") + "\n");
}

TEST_CASE("Full access does not bypass an unconditional denied path", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    seed_denied_names();
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    PluginPermissions permissions;
    permissions.full_access = true;
    harness.save_permissions(PLUGIN, permissions);

    // The install-state sidecar stays host-managed: full access only switches off permission
    // auditing, it does not make the plugin's own state file reachable.
    python.globals["INSTALL_STATE"] = (harness.data.plugins_dir() / PLUGIN / ".install_state.json").string();
    python.run_as(PLUGIN, "denied_outcome = attempt(lambda: open(INSTALL_STATE).read())");

    CHECK(python.str("denied_outcome") == "denied");
    CHECK(describe(harness.prompts).empty());
    AuditViolation violation;
    REQUIRE(PluginAuditManager::instance().last_violation(violation));
    CHECK(violation.reason == "denied path");
}

TEST_CASE("A full access plugin's permission declaration is not prompted", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    PluginPermissions stored;
    stored.full_access = true;
    stored.fs_read     = {"/previously/granted"};
    harness.save_permissions(PLUGIN, stored);

    bool prompted = false;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&prompted](const std::string&) {
            prompted = true;
            return false;
        });

    PluginPermissions requested;
    requested.fs_read = {"/new/target"};
    const bool granted = PluginAuditManager::instance().request_permissions(PLUGIN, requested);

    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);

    // The declaration is accepted without a prompt and without adding a grant: full access already
    // covers the event, and the stored list stays as it was.
    CHECK(granted);
    CHECK_FALSE(prompted);
    CHECK(harness.saved_permissions(PLUGIN).fs_read == std::vector<std::string>{"/previously/granted"});
}

TEST_CASE("Toggling full access preserves the other stored grants", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    PluginPermissions stored;
    stored.fs_read        = {"/kept/read"};
    stored.network_http   = {"https://kept.example"};
    harness.save_permissions(PLUGIN, stored);

    PluginManager& manager = PluginManager::instance();
    PluginPermissionMutation toggle;
    toggle.full_access = true;
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, toggle));
    PluginPermissions after_enable = harness.saved_permissions(PLUGIN);
    CHECK(after_enable.full_access);
    CHECK(after_enable.fs_read == std::vector<std::string>{"/kept/read"});
    CHECK(after_enable.network_http == std::vector<std::string>{"https://kept.example"});

    toggle.full_access = false;
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, toggle));
    PluginPermissions after_disable = harness.saved_permissions(PLUGIN);
    CHECK_FALSE(after_disable.full_access);
    CHECK(after_disable.fs_read == std::vector<std::string>{"/kept/read"});

    // An unknown package has no sidecar to modify: the toggle fails closed.
    toggle.full_access = true;
    CHECK_FALSE(manager.mutate_plugin_permissions("NoSuchPlugin", toggle));
}

TEST_CASE("A permission mutation changes only the named entries and preserves the rest", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    const fs::path plugin_dir = harness.data.plugins_dir() / PLUGIN;
    PluginInstallState initial;
    initial.installed_from    = "local";
    initial.installed_version = "1.2.3";
    initial.plugin_name       = "Audit Plugin";
    initial.enabled           = false;
    initial.capabilities      = {{"Echo", false}, {"Other", true}};
    initial.permissions.fs_read      = {"/kept/read", "/removed/read"};
    initial.permissions.fs_readwrite = {"/kept/write"};
    initial.permissions.fs_create    = {"/kept/create", "/removed/create"};
    initial.permissions.fs_delete    = {"/kept/delete", "/removed/delete"};
    initial.permissions.network_http = {"https://kept.example"};
    REQUIRE(write_install_state(plugin_dir, initial));

    PluginPermissionMutation mutation;
    mutation.add = {
        {PluginPermissionCategory::FsRead, "/added/read"},
        {PluginPermissionCategory::FsRead, "/added/read"}, // duplicate is collapsed
        {PluginPermissionCategory::FsRead, ""},            // empty values are skipped
        {PluginPermissionCategory::FsRead, "/removed/read"},
        {PluginPermissionCategory::FsCreate, "/added/create"},
        {PluginPermissionCategory::FsCreate, "/removed/create"},
        {PluginPermissionCategory::FsDelete, "/added/delete"},
        {PluginPermissionCategory::NetworkSocket, "127.0.0.1:8080"},
    };
    mutation.remove = {
        {PluginPermissionCategory::FsRead, "/removed/read"},
        {PluginPermissionCategory::FsRead, "/never/granted"},
        {PluginPermissionCategory::FsRead, ""},
        {PluginPermissionCategory::FsCreate, "/removed/create"},
        {PluginPermissionCategory::FsDelete, "/removed/delete"},
        {PluginPermissionCategory::Process, "/never/granted"},
    };

    PluginPermissions applied;
    REQUIRE(PluginManager::instance().mutate_plugin_permissions(PLUGIN, mutation, &applied));
    // Removals run first, so a value named in both lists ends present; absent removals are ignored.
    const std::vector<std::string> expected_fs_read{"/kept/read", "/added/read", "/removed/read"};
    const std::vector<std::string> expected_fs_create{"/kept/create", "/added/create", "/removed/create"};
    const std::vector<std::string> expected_fs_delete{"/kept/delete", "/added/delete"};
    const std::vector<std::string> expected_sockets{"127.0.0.1:8080"};
    CHECK(applied.fs_read == expected_fs_read);
    CHECK(applied.fs_create == expected_fs_create);
    CHECK(applied.fs_delete == expected_fs_delete);
    CHECK(applied.network_socket == expected_sockets);

    PluginInstallState stored;
    REQUIRE(read_install_state(plugin_dir, stored));
    CHECK(stored.permissions.fs_read == expected_fs_read);
    CHECK(stored.permissions.fs_create == expected_fs_create);
    CHECK(stored.permissions.fs_delete == expected_fs_delete);
    CHECK(stored.permissions.network_socket == expected_sockets);

    // Every grant the mutation did not name stays as it was.
    CHECK(stored.permissions.fs_readwrite == initial.permissions.fs_readwrite);
    CHECK(stored.permissions.network_http == initial.permissions.network_http);
    CHECK(stored.permissions.process.empty());
    CHECK_FALSE(stored.permissions.threading);
    CHECK_FALSE(stored.permissions.full_access);

    // The install metadata, enabled flag and capability flags are unrelated state and survive too.
    CHECK(stored.installed_from == initial.installed_from);
    CHECK(stored.installed_version == initial.installed_version);
    CHECK(stored.plugin_name == initial.plugin_name);
    CHECK_FALSE(stored.enabled);
    REQUIRE(stored.capabilities.size() == 2);
    CHECK(stored.capabilities[0].first == "Echo");
    CHECK_FALSE(stored.capabilities[0].second);
    CHECK(stored.capabilities[1].first == "Other");
    CHECK(stored.capabilities[1].second);
}

TEST_CASE("A permission mutation applies to the latest stored contents", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    PluginPermissions initial;
    initial.fs_read = {"/caller/saw"};
    harness.save_permissions(PLUGIN, initial);

    PluginManager& manager = PluginManager::instance();
    PluginInstallState observed;
    REQUIRE(manager.get_install_state(PLUGIN, observed));
    CHECK(observed.permissions.fs_read == std::vector<std::string>{"/caller/saw"});

    // Another writer grants a target after the caller read the state.
    PluginPermissionMutation concurrent;
    concurrent.add = {{PluginPermissionCategory::FsRead, "/other/writer"}};
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, concurrent));

    // The caller's edit is applied against the sidecar as it stands now, not against `observed`,
    // so the concurrent grant is not dropped.
    PluginPermissionMutation mine;
    mine.add = {{PluginPermissionCategory::FsRead, "/caller/adds"}};
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, mine));

    PluginInstallState stored;
    REQUIRE(read_install_state(harness.data.plugins_dir() / PLUGIN, stored));
    const std::vector<std::string> expected_fs_read{"/caller/saw", "/other/writer", "/caller/adds"};
    CHECK(stored.permissions.fs_read == expected_fs_read);
}

TEST_CASE("A permission mutation changes boolean grants only when asked", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    PluginPermissions initial;
    initial.fs_read     = {"/kept"};
    initial.full_access = true;
    harness.save_permissions(PLUGIN, initial);

    PluginManager& manager = PluginManager::instance();

    // A mutation that names no booleans leaves both flags alone.
    PluginPermissions applied;
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, PluginPermissionMutation{}, &applied));
    CHECK_FALSE(applied.threading);
    CHECK(applied.full_access);
    CHECK(applied.fs_read == std::vector<std::string>{"/kept"});

    PluginPermissionMutation booleans;
    booleans.threading   = true;
    booleans.full_access = false;
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, booleans, &applied));
    CHECK(applied.threading);
    CHECK_FALSE(applied.full_access);
    CHECK(applied.fs_read == std::vector<std::string>{"/kept"});

    const PluginPermissions stored = harness.saved_permissions(PLUGIN);
    CHECK(stored.threading);
    CHECK_FALSE(stored.full_access);
    CHECK(stored.fs_read == std::vector<std::string>{"/kept"});
}

TEST_CASE("A permission mutation fails closed for a plugin it cannot update", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    PluginPermissionMutation mutation;
    mutation.add = {{PluginPermissionCategory::FsRead, "/new"}};
    PluginManager& manager = PluginManager::instance();

    CHECK_FALSE(manager.mutate_plugin_permissions("", mutation));
    CHECK_FALSE(manager.mutate_plugin_permissions("NoSuchPlugin", mutation));

    // A discovered package whose sidecar is gone has nothing safe to update.
    const fs::path sidecar = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    fs::remove(sidecar);
    CHECK_FALSE(manager.mutate_plugin_permissions(PLUGIN, mutation));
}

TEST_CASE("A permission edit applies to the next audited operation without a reload", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.run_as(PLUGIN, "first_outcome = attempt(lambda: open(PRIVATE).read())");
    CHECK(python.str("first_outcome") == "denied");
    harness.prompts.clear();

    PluginPermissionMutation mutation;
    mutation.full_access = true;
    REQUIRE(PluginManager::instance().mutate_plugin_permissions(PLUGIN, mutation));

    // The running audit re-reads the sidecar, so the edit covers the next operation.
    python.run_as(PLUGIN, "second_outcome = attempt(lambda: open(PRIVATE).read())");
    CHECK(python.str("second_outcome") == "ok");
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

TEST_CASE("Create and delete folder grants cover direct entries but not subfolders", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
GRANTED = os.path.realpath(os.path.join(OUTSIDE, "granted"))
SUBFOLDER = os.path.join(GRANTED, "sub")
os.makedirs(SUBFOLDER)
DELETE_FILE = os.path.join(GRANTED, "delete-me.txt")
with open(DELETE_FILE, "w") as f:
    f.write("x")
NESTED_DELETE_FILE = os.path.join(SUBFOLDER, "delete-me-too.txt")
with open(NESTED_DELETE_FILE, "w") as f:
    f.write("x")
NEW_DIR = os.path.join(GRANTED, "new-dir")
OTHER_DIR = os.path.join(GRANTED, "other-dir")
NESTED_NEW_DIR = os.path.join(SUBFOLDER, "new-dir")
)PY");

    PluginPermissions permissions;
    permissions.fs_create = {python.str("GRANTED")};
    permissions.fs_delete = {python.str("GRANTED")};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
create_outcome = attempt(lambda: os.mkdir(NEW_DIR))
delete_outcome = attempt(lambda: os.remove(DELETE_FILE))
nested_create_outcome = attempt(lambda: os.mkdir(NESTED_NEW_DIR))
nested_delete_outcome = attempt(lambda: os.remove(NESTED_DELETE_FILE))
)PY");

    // The literal grant names the exact folder, so an operation directly inside it is covered; an
    // operation in a child folder is not. The tree form below covers both.
    CHECK(python.str("create_outcome") == "ok");
    CHECK(python.str("delete_outcome") == "ok");
    CHECK(python.str("nested_create_outcome") == "denied");
    CHECK(python.str("nested_delete_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("SUBFOLDER") + "\n" +
                                            "os.remove: " + python.str("SUBFOLDER") + "\n");
    CHECK(fs::is_directory(fs::path(python.str("NEW_DIR"))));
    CHECK(fs::exists(fs::path(python.str("NESTED_DELETE_FILE"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("NESTED_NEW_DIR"))));

    // Create and delete are independent buckets: a delete grant does not authorize a create in
    // the same folder, and the reverse.
    permissions.fs_create.clear();
    permissions.fs_delete = {python.str("GRANTED")};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
other_create_outcome = attempt(lambda: os.mkdir(OTHER_DIR))
other_delete_outcome = attempt(lambda: os.rmdir(NEW_DIR))
)PY");

    CHECK(python.str("other_create_outcome") == "denied");
    CHECK(python.str("other_delete_outcome") == "ok");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("GRANTED") + "\n");
}

TEST_CASE("A create and delete tree grant covers direct entries and subfolders but not a sibling", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
GRANTED = os.path.realpath(os.path.join(OUTSIDE, "tree-granted"))
SIBLING = os.path.realpath(os.path.join(OUTSIDE, "tree-granted-sibling"))
SUBFOLDER = os.path.join(GRANTED, "sub")
os.makedirs(SUBFOLDER)
os.makedirs(SIBLING)
TREE_GRANT = GRANTED + os.sep + "*"
DIRECT_FILE = os.path.join(GRANTED, "direct.txt")
NESTED_FILE = os.path.join(SUBFOLDER, "nested.txt")
SIBLING_FILE = os.path.join(SIBLING, "sibling.txt")
for path in (DIRECT_FILE, NESTED_FILE, SIBLING_FILE):
    with open(path, "w") as f:
        f.write("x")
DIRECT_NEW = os.path.join(GRANTED, "direct-new")
NESTED_NEW = os.path.join(SUBFOLDER, "nested-new")
SIBLING_NEW = os.path.join(SIBLING, "sibling-new")
)PY");

    // "<dir>/*" is a subtree grant, unlike the literal folder grant in the previous test.
    PluginPermissions permissions;
    permissions.fs_create = {python.str("TREE_GRANT")};
    permissions.fs_delete = {python.str("TREE_GRANT")};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
direct_create_outcome = attempt(lambda: os.mkdir(DIRECT_NEW))
nested_create_outcome = attempt(lambda: os.mkdir(NESTED_NEW))
direct_delete_outcome = attempt(lambda: os.remove(DIRECT_FILE))
nested_delete_outcome = attempt(lambda: os.remove(NESTED_FILE))
sibling_create_outcome = attempt(lambda: os.mkdir(SIBLING_NEW))
sibling_delete_outcome = attempt(lambda: os.remove(SIBLING_FILE))
)PY");

    CHECK(python.str("direct_create_outcome") == "ok");
    CHECK(python.str("nested_create_outcome") == "ok");
    CHECK(python.str("direct_delete_outcome") == "ok");
    CHECK(python.str("nested_delete_outcome") == "ok");
    // A sibling whose name only shares the granted folder's text prefix is a different folder.
    CHECK(python.str("sibling_create_outcome") == "denied");
    CHECK(python.str("sibling_delete_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("SIBLING") + "\n" +
                                            "os.remove: " + python.str("SIBLING") + "\n");
    CHECK(fs::is_directory(fs::path(python.str("DIRECT_NEW"))));
    CHECK(fs::is_directory(fs::path(python.str("NESTED_NEW"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("DIRECT_FILE"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("NESTED_FILE"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("SIBLING_NEW"))));
    CHECK(fs::exists(fs::path(python.str("SIBLING_FILE"))));

    // Create and delete stay separate: dropping the create grant denies a later create in the
    // same subtree while the delete grant keeps working.
    permissions.fs_create.clear();
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
cross_create_outcome = attempt(lambda: os.mkdir(os.path.join(GRANTED, "cross-new")))
cross_delete_outcome = attempt(lambda: os.rmdir(NESTED_NEW))
)PY");
    CHECK(python.str("cross_create_outcome") == "denied");
    CHECK(python.str("cross_delete_outcome") == "ok");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("GRANTED") + "\n");
}

TEST_CASE("A create and delete tree grant does not follow a symlink out of its root", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
GRANTED = os.path.realpath(os.path.join(OUTSIDE, "tree-link-granted"))
ESCAPE_TARGET = os.path.realpath(os.path.join(OUTSIDE, "tree-link-target"))
os.makedirs(GRANTED)
os.makedirs(ESCAPE_TARGET)
TREE_GRANT = GRANTED + os.sep + "*"
VICTIM = os.path.join(ESCAPE_TARGET, "victim.txt")
with open(VICTIM, "w") as f:
    f.write("x")
LINK = os.path.join(GRANTED, "escape")
try:
    os.symlink(ESCAPE_TARGET, LINK, target_is_directory=True)
    SYMLINK_OK = True
except (OSError, NotImplementedError):
    SYMLINK_OK = False
)PY");
    if (python.eval("SYMLINK_OK") != "True")
        SKIP("Cannot create a directory symlink here");

    PluginPermissions permissions;
    permissions.fs_create = {python.str("TREE_GRANT")};
    permissions.fs_delete = {python.str("TREE_GRANT")};
    harness.save_permissions(PLUGIN, permissions);

    // The link is inside the granted tree, but the folder it resolves to is outside it. Creating
    // or deleting through the link must be judged against that outside folder.
    python.run_as(PLUGIN, R"PY(
escaped_create_outcome = attempt(lambda: os.mkdir(os.path.join(LINK, "new")))
escaped_delete_outcome = attempt(lambda: os.remove(os.path.join(LINK, "victim.txt")))
)PY");

    CHECK(python.str("escaped_create_outcome") == "denied");
    CHECK(python.str("escaped_delete_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("ESCAPE_TARGET") + "\n" +
                                            "os.remove: " + python.str("ESCAPE_TARGET") + "\n");
    CHECK(fs::exists(fs::path(python.str("VICTIM"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("ESCAPE_TARGET")) / "new"));

    // Removing the link itself acts on the link inside the granted folder, not on its target.
    harness.prompts.clear();
    python.run_as(PLUGIN, "link_delete_outcome = attempt(lambda: os.remove(LINK))");
    CHECK(python.str("link_delete_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::is_symlink(fs::path(python.str("LINK"))));
    CHECK(fs::exists(fs::path(python.str("VICTIM"))));
}

#if !defined(_WIN32)
TEST_CASE("A create or delete target inside a folder literally named * fails closed without a tree grant", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
BASE = os.path.realpath(os.path.join(OUTSIDE, "star-parent"))
STAR = os.path.join(BASE, "*")
os.makedirs(STAR)
os.makedirs(os.path.join(BASE, "sibling"))
STAR_FILE = os.path.join(STAR, "kept.txt")
with open(STAR_FILE, "w") as f:
    f.write("x")
STAR_NEW = os.path.join(STAR, "new")
SIBLING_NEW = os.path.join(BASE, "sibling", "new")
)PY");

    // The parent folder is spelled "/.../*". Storing it as-is would be read back as a tree grant
    // for the whole parent, so an approval here must fail closed instead.
    harness.approve = [](const Prompt&) { return true; };
    python.run_as(PLUGIN, "star_create_outcome = attempt(lambda: os.mkdir(STAR_NEW))");

    CHECK(python.str("star_create_outcome") == "denied");
    CHECK_FALSE(fs::exists(fs::path(python.str("STAR_NEW"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_create.empty());

    // Nothing was widened to the parent: an operation beside the literal "*" folder is still
    // prompted and denied, and no tree grant for BASE was stored.
    harness.prompts.clear();
    harness.approve = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, "sibling_outcome = attempt(lambda: os.mkdir(SIBLING_NEW))");

    CHECK(python.str("sibling_outcome") == "denied");
    CHECK_FALSE(fs::exists(fs::path(python.str("SIBLING_NEW"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_create.empty());

    // Delete resolves the same parent folder and must not persist a tree grant either.
    harness.prompts.clear();
    harness.approve = [](const Prompt&) { return true; };
    python.run_as(PLUGIN, "star_delete_outcome = attempt(lambda: os.remove(STAR_FILE))");

    CHECK(python.str("star_delete_outcome") == "denied");
    CHECK(fs::exists(fs::path(python.str("STAR_FILE"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_delete.empty());
}
#endif

TEST_CASE("A full create and delete grant allows any folder but not a denied path", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
FIRST = os.path.join(OUTSIDE, "full-create")
os.makedirs(FIRST)
NESTED = os.path.join(TREE, "one", "full-create")
DELETE_FILES = [os.path.join(FIRST, "a.txt"), os.path.join(TREE, "two", "b.txt")]
for path in DELETE_FILES:
    with open(path, "w") as f:
        f.write("x")
DENIED_DIR = os.path.join(OUTSIDE, "secrets")
os.makedirs(DENIED_DIR)
DENIED_FILE = os.path.join(DENIED_DIR, "token.txt")
with open(DENIED_FILE, "w") as f:
    f.write("x")
DENIED_NEW = os.path.join(DENIED_DIR, "new")
)PY");

    // "*" is the stored form of "all folders", while a "<dir>/*" entry grants one subtree.
    PluginPermissions permissions;
    permissions.fs_create = {"*"};
    permissions.fs_delete = {"*"};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
create_outcome = attempt(lambda: os.mkdir(NESTED))
delete_outcome = attempt(lambda: [os.remove(path) for path in DELETE_FILES])
denied_create_outcome = attempt(lambda: os.mkdir(DENIED_NEW))
denied_delete_outcome = attempt(lambda: os.remove(DENIED_FILE))
)PY");

    CHECK(python.str("create_outcome") == "ok");
    CHECK(python.str("delete_outcome") == "ok");
    CHECK(python.str("denied_create_outcome") == "denied");
    CHECK(python.str("denied_delete_outcome") == "denied");
    CHECK(describe(harness.prompts).empty());
    CHECK(fs::is_directory(fs::path(python.str("NESTED"))));
    CHECK(fs::exists(fs::path(python.str("DENIED_FILE"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("DENIED_NEW"))));

    AuditViolation violation;
    REQUIRE(PluginAuditManager::instance().last_violation(violation));
    CHECK(violation.reason == "denied path");
}

TEST_CASE("An approved create or delete prompt saves the parent folder and is not asked again", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
CREATE_ROOT = os.path.realpath(os.path.join(OUTSIDE, "approved-create"))
os.makedirs(CREATE_ROOT)
NESTED_FOLDER = os.path.join(CREATE_ROOT, "sub")
os.makedirs(NESTED_FOLDER)
FIRST_CREATE = os.path.join(CREATE_ROOT, "first")
SECOND_CREATE = os.path.join(CREATE_ROOT, "second")
NESTED_CREATE = os.path.join(NESTED_FOLDER, "nested")
DELETE_ROOT = os.path.realpath(os.path.join(OUTSIDE, "approved-delete"))
os.makedirs(DELETE_ROOT)
FIRST_DELETE = os.path.join(DELETE_ROOT, "one.txt")
SECOND_DELETE = os.path.join(DELETE_ROOT, "two.txt")
for path in (FIRST_DELETE, SECOND_DELETE):
    with open(path, "w") as f:
        f.write("x")
CROSS_CREATE = os.path.join(DELETE_ROOT, "cross")
)PY");

    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.mkdir"; };
    python.run_as(PLUGIN, "first_create_outcome = attempt(lambda: os.mkdir(FIRST_CREATE))");

    CHECK(python.str("first_create_outcome") == "ok");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("CREATE_ROOT") + "\n");
    PluginPermissions after_create = harness.saved_permissions(PLUGIN);
    // The prompt approval is saved as the folder containing the target, not the target itself.
    REQUIRE(after_create.fs_create.size() == 1);
    CHECK(fs::equivalent(fs::path(after_create.fs_create.front()), fs::path(python.str("CREATE_ROOT"))));
    CHECK(after_create.fs_delete.empty());

    // The saved parent folder covers another operation directly inside it, without a prompt.
    harness.prompts.clear();
    python.run_as(PLUGIN, "second_create_outcome = attempt(lambda: os.mkdir(SECOND_CREATE))");
    CHECK(python.str("second_create_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // A subfolder is a descendant, not the granted folder, so it still asks.
    harness.approve = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, "nested_create_outcome = attempt(lambda: os.mkdir(NESTED_CREATE))");
    CHECK(python.str("nested_create_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("NESTED_FOLDER") + "\n");
    const PluginPermissions after_denial = harness.saved_permissions(PLUGIN);
    REQUIRE(after_denial.fs_create.size() == 1);
    CHECK(fs::equivalent(fs::path(after_denial.fs_create.front()), fs::path(python.str("CREATE_ROOT"))));

    // Delete approval persists its own folder and leaves the create grant alone.
    harness.prompts.clear();
    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.remove"; };
    python.run_as(PLUGIN, "first_delete_outcome = attempt(lambda: os.remove(FIRST_DELETE))");
    CHECK(python.str("first_delete_outcome") == "ok");
    CHECK(describe(harness.prompts) == "os.remove: " + python.str("DELETE_ROOT") + "\n");
    PluginPermissions after_delete = harness.saved_permissions(PLUGIN);
    REQUIRE(after_delete.fs_delete.size() == 1);
    CHECK(fs::equivalent(fs::path(after_delete.fs_delete.front()), fs::path(python.str("DELETE_ROOT"))));
    REQUIRE(after_delete.fs_create.size() == 1);
    CHECK(fs::equivalent(fs::path(after_delete.fs_create.front()), fs::path(python.str("CREATE_ROOT"))));

    harness.prompts.clear();
    python.run_as(PLUGIN, "second_delete_outcome = attempt(lambda: os.remove(SECOND_DELETE))");
    CHECK(python.str("second_delete_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // The delete grant does not authorize a create in the same folder.
    harness.approve = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, "cross_create_outcome = attempt(lambda: os.mkdir(CROSS_CREATE))");
    CHECK(python.str("cross_create_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("DELETE_ROOT") + "\n");
}

TEST_CASE("A create prompt discloses the canonical folder the approval will grant", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
REAL_FOLDER = os.path.realpath(os.path.join(OUTSIDE, "prompt-real"))
os.makedirs(REAL_FOLDER)
LINK = os.path.join(OUTSIDE, "prompt-link")
CREATED = os.path.join(REAL_FOLDER, "new")
RAW_CREATE = os.path.join(LINK, "new")
try:
    os.symlink(REAL_FOLDER, LINK, target_is_directory=True)
    SYMLINK_OK = True
except (OSError, NotImplementedError):
    SYMLINK_OK = False
)PY");
    if (python.eval("SYMLINK_OK") != "True")
        SKIP("Cannot create a directory symlink here");

    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.mkdir"; };
    python.run_as(PLUGIN, "outcome = attempt(lambda: os.mkdir(RAW_CREATE))");

    CHECK(python.str("outcome") == "ok");
    // The user approved creating inside the symlink's target. Showing the raw path would hide
    // which folder is actually stored and granted.
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("REAL_FOLDER")});
    CHECK(harness.saved_permissions(PLUGIN).fs_create == std::vector<std::string>{python.str("REAL_FOLDER")});
    CHECK(fs::is_directory(fs::path(python.str("CREATED"))));
}

TEST_CASE("A relative create target is resolved against the working directory and disclosed absolutely", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
RELATIVE_ROOT = os.path.realpath(os.path.join(OUTSIDE, "relative-root"))
os.makedirs(RELATIVE_ROOT)
RELATIVE_VICTIM = os.path.join(RELATIVE_ROOT, "victim.txt")
RELATIVE_SECOND_VICTIM = os.path.join(RELATIVE_ROOT, "victim2.txt")
for path in (RELATIVE_VICTIM, RELATIVE_SECOND_VICTIM):
    with open(path, "w") as f:
        f.write("x")
)PY");

    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.mkdir"; };
    python.run_as(PLUGIN, R"PY(
os.chdir(RELATIVE_ROOT)
try:
    first_outcome = attempt(lambda: os.mkdir("direct"))
finally:
    os.chdir(OUTSIDE)
)PY");

    CHECK(python.str("first_outcome") == "ok");
    // A relative target is resolved when the event is judged, and the prompt shows the absolute
    // folder whose grant the approval will store.
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("RELATIVE_ROOT")});
    CHECK(harness.saved_permissions(PLUGIN).fs_create == std::vector<std::string>{python.str("RELATIVE_ROOT")});

    // The saved absolute folder covers another relative operation in the same directory.
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(RELATIVE_ROOT)
try:
    second_outcome = attempt(lambda: os.mkdir("second"))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("second_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // A relative entry saved by hand never matches: its meaning would move with the working
    // directory. The operation asks again for the canonical folder.
    PluginPermissions relative_only;
    relative_only.fs_create = {"relative-root"};
    harness.save_permissions(PLUGIN, relative_only);
    harness.approve = [](const Prompt&) { return false; };
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(RELATIVE_ROOT)
try:
    third_outcome = attempt(lambda: os.mkdir("third"))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("third_outcome") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("RELATIVE_ROOT")});

    // Delete targets resolve the same way. A relative removal inside the granted folder is
    // allowed without a prompt.
    PluginPermissions delete_root;
    delete_root.fs_delete = {python.str("RELATIVE_ROOT")};
    harness.save_permissions(PLUGIN, delete_root);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(RELATIVE_ROOT)
try:
    relative_delete_outcome = attempt(lambda: os.remove("victim.txt"))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("relative_delete_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // Without a grant the prompt names the resolved folder, not the relative spelling.
    harness.save_permissions(PLUGIN, {});
    harness.approve = [](const Prompt&) { return false; };
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(RELATIVE_ROOT)
try:
    relative_delete_denied = attempt(lambda: os.remove("victim2.txt"))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("relative_delete_denied") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("RELATIVE_ROOT")});
}

TEST_CASE("A link create grant covers only the destination folder", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
SOURCE_FOLDER = os.path.realpath(os.path.join(OUTSIDE, "link-source"))
DEST_FOLDER = os.path.realpath(os.path.join(OUTSIDE, "link-dest"))
os.makedirs(SOURCE_FOLDER)
os.makedirs(DEST_FOLDER)
SOURCE_FILE = os.path.join(SOURCE_FOLDER, "source.txt")
with open(SOURCE_FILE, "w") as f:
    f.write("x")
SYMLINK_DEST = os.path.join(DEST_FOLDER, "symlink.txt")
HARDLINK_DEST = os.path.join(DEST_FOLDER, "hardlink.txt")
SECOND_LINK = os.path.join(DEST_FOLDER, "second-link.txt")
try:
    os.symlink(SOURCE_FILE, os.path.join(OUTSIDE, "probe-link"))
    os.remove(os.path.join(OUTSIDE, "probe-link"))
    SYMLINK_OK = True
except (OSError, NotImplementedError):
    SYMLINK_OK = False
try:
    os.link(SOURCE_FILE, os.path.join(OUTSIDE, "probe-hardlink"))
    os.remove(os.path.join(OUTSIDE, "probe-hardlink"))
    HARDLINK_OK = True
except (OSError, NotImplementedError):
    HARDLINK_OK = False
)PY");
    if (python.eval("SYMLINK_OK") != "True" || python.eval("HARDLINK_OK") != "True")
        SKIP("Symbolic or hard links are unavailable");

    // Only the path being created is a create target; the source the link points at is not. A
    // hardlink still needs read/write access to its source, so grant that separately to keep this
    // test focused on the destination folder grant.
    PluginPermissions source_access;
    source_access.fs_readwrite = {python.str("SOURCE_FILE")};
    harness.save_permissions(PLUGIN, source_access);
    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.symlink" || prompt.event == "os.link"; };
    python.run_as(PLUGIN, R"PY(
symlink_outcome = attempt(lambda: os.symlink(SOURCE_FILE, SYMLINK_DEST))
hardlink_outcome = attempt(lambda: os.link(SOURCE_FILE, HARDLINK_DEST))
)PY");

    CHECK(python.str("symlink_outcome") == "ok");
    CHECK(python.str("hardlink_outcome") == "ok");
    const PluginPermissions after_links = harness.saved_permissions(PLUGIN);
    REQUIRE(after_links.fs_create.size() == 1);
    CHECK(fs::equivalent(fs::path(after_links.fs_create.front()), fs::path(python.str("DEST_FOLDER"))));
    CHECK(after_links.fs_readwrite == source_access.fs_readwrite);

    // The source folder alone does not authorize creating an entry in another folder.
    PluginPermissions source_only;
    source_only.fs_create = {python.str("SOURCE_FOLDER")};
    harness.save_permissions(PLUGIN, source_only);
    harness.approve = [](const Prompt&) { return false; };
    harness.prompts.clear();
    python.run_as(PLUGIN, "second_outcome = attempt(lambda: os.symlink(SOURCE_FILE, SECOND_LINK))");

    CHECK(python.str("second_outcome") == "denied");
    CHECK_FALSE(fs::exists(fs::path(python.str("SECOND_LINK"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_create == std::vector<std::string>{python.str("SOURCE_FOLDER")});
}

TEST_CASE("A hardlink nested in an approved read/write root needs no extra prompt", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    const fs::path destination = harness.outside.path() / "hardlinked-copy";
    python.globals["HARDLINKED_COPY"] = destination.string();
    python.exec(R"PY(
try:
    PROBE = os.path.join(OUTSIDE, "copytree-hardlink-probe.txt")
    os.link(os.path.join(TREE, "one", "f.txt"), PROBE)
    os.remove(PROBE)
    COPY_HARDLINK_OK = True
except OSError:
    COPY_HARDLINK_OK = False
)PY");
    if (python.eval("COPY_HARDLINK_OK") != "True")
        SKIP("Hard links are unavailable here");

    // copytree is the approved read/write root. os.link is called from its stdlib frames, so the
    // hardlink source inside TREE must not need a separate read/write prompt.
    harness.approve = [](const Prompt& prompt) { return prompt.event == "shutil.copytree"; };
    python.run_as(PLUGIN, "shutil.copytree(TREE, HARDLINKED_COPY, copy_function=os.link)");

    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().event == "shutil.copytree");
    const fs::path copied_file = destination / "one" / "f.txt";
    REQUIRE(fs::exists(copied_file));
    CHECK(fs::equivalent(copied_file, fs::path(python.str("TREE")) / "one" / "f.txt"));
    CHECK(harness.saved_permissions(PLUGIN).fs_read.empty());
    CHECK(harness.saved_permissions(PLUGIN).fs_readwrite.empty());
}

TEST_CASE("A hardlink needs read/write permission for a source outside a writable root", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    python.globals["DATA_DIR"] = harness.data.dir.string();

    python.exec(R"PY(
SOURCE = os.path.join(OUTSIDE, "hardlink-source.txt")
with open(SOURCE, "w") as f:
    f.write("secret")
DATA_SOURCE = os.path.join(DATA_DIR, "hardlink-data-source.txt")
with open(DATA_SOURCE, "w") as f:
    f.write("data")
DEST_DENIED = os.path.join(DATA_DIR, "hardlink-denied.txt")
DEST_READ_ONLY = os.path.join(DATA_DIR, "hardlink-read-only.txt")
DEST_GRANTED = os.path.join(DATA_DIR, "hardlink-granted.txt")
DEST_FROM_DATA_SOURCE = os.path.join(DATA_DIR, "hardlink-from-data-source.txt")
try:
    PROBE = os.path.join(DATA_DIR, "hardlink-probe.txt")
    os.link(SOURCE, PROBE)
    os.remove(PROBE)
    HARDLINK_OK = True
except OSError:
    HARDLINK_OK = False
)PY");
    if (python.eval("HARDLINK_OK") != "True")
        SKIP("Hard links are unavailable here");

    // The destination sits inside data_dir(), an implicitly writable allowed root. That must not
    // authorize writing through a hardlink to a source outside it.
    python.run_as(PLUGIN, "denied_outcome = attempt(lambda: os.link(SOURCE, DEST_DENIED))");
    CHECK(python.str("denied_outcome") == "denied");
    CHECK_FALSE(fs::exists(fs::path(python.str("DEST_DENIED"))));

    // A read grant is not enough either: the hardlink still has to ask for write access to the
    // source, and denying that prompt leaves the operation blocked.
    PluginPermissions read_only;
    read_only.fs_read = {python.str("SOURCE")};
    harness.save_permissions(PLUGIN, read_only);
    harness.approve = [](const Prompt&) { return false; };
    harness.prompts.clear();
    python.run_as(PLUGIN, "read_only_outcome = attempt(lambda: os.link(SOURCE, DEST_READ_ONLY))");
    CHECK(python.str("read_only_outcome") == "denied");
    CHECK_FALSE(fs::exists(fs::path(python.str("DEST_READ_ONLY"))));
    CHECK(describe(harness.prompts) == "os.link: " + python.str("SOURCE") + "\n");

    // A read/write grant for the source makes the same link creation succeed without another
    // prompt.
    PluginPermissions read_write;
    read_write.fs_readwrite = {python.str("SOURCE")};
    harness.save_permissions(PLUGIN, read_write);
    harness.prompts.clear();
    python.run_as(PLUGIN, "granted_outcome = attempt(lambda: os.link(SOURCE, DEST_GRANTED))");
    CHECK(python.str("granted_outcome") == "ok");
    CHECK(fs::exists(fs::path(python.str("DEST_GRANTED"))));
    CHECK(describe(harness.prompts).empty());

    // A source inside the writable allowed root itself needs no separate grant: writing through
    // the new name stays inside a folder the plugin may already write.
    harness.save_permissions(PLUGIN, {});
    harness.prompts.clear();
    python.run_as(PLUGIN, "data_source_outcome = attempt(lambda: os.link(DATA_SOURCE, DEST_FROM_DATA_SOURCE))");
    CHECK(python.str("data_source_outcome") == "ok");
    CHECK(fs::exists(fs::path(python.str("DEST_FROM_DATA_SOURCE"))));
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("Recursive rmtree does not let a direct parent delete grant cover subfolder contents", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
DELETE_PARENT = os.path.realpath(os.path.join(OUTSIDE, "rmtree-parent"))
DELETE_SUBFOLDER = os.path.join(DELETE_PARENT, "sub")
os.makedirs(DELETE_SUBFOLDER)
DELETE_A = os.path.join(DELETE_SUBFOLDER, "a.txt")
DELETE_B = os.path.join(DELETE_SUBFOLDER, "b.txt")
for path in (DELETE_A, DELETE_B):
    with open(path, "w") as f:
        f.write("x")
)PY");

    PluginPermissions permissions;
    // The grant names the parent, so deleting the direct entry "sub" is allowed. It does not
    // grant deleting the entries inside "sub".
    permissions.fs_delete = {python.str("DELETE_PARENT")};
    harness.save_permissions(PLUGIN, permissions);

    // Reads needed by rmtree may still be approved if the implementation asks; deletes are left
    // denied. The recursive walk must not inherit the parent's delete grant for its children.
    harness.approve = [](const Prompt& prompt) { return prompt.event != "os.remove" && prompt.event != "os.rmdir"; };
    python.run_as(PLUGIN, "outcome = attempt(lambda: shutil.rmtree(DELETE_SUBFOLDER))");

    CHECK(python.str("outcome") == "denied");
    CHECK(fs::is_directory(fs::path(python.str("DELETE_SUBFOLDER"))));
    const int remaining = (fs::exists(fs::path(python.str("DELETE_A"))) ? 1 : 0) +
                          (fs::exists(fs::path(python.str("DELETE_B"))) ? 1 : 0);
    CHECK(remaining >= 1);

    // A full delete grant still allows the recursive operation.
    permissions.fs_delete = {"*"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    harness.approve = [](const Prompt& prompt) { return prompt.event != "os.remove" && prompt.event != "os.rmdir"; };
    python.run_as(PLUGIN, "full_outcome = attempt(lambda: shutil.rmtree(DELETE_SUBFOLDER))");
    CHECK(python.str("full_outcome") == "ok");
    CHECK_FALSE(fs::exists(fs::path(python.str("DELETE_SUBFOLDER"))));
}

TEST_CASE("A trailing symlink is judged by its containing folder, not its target", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
LINK_PARENT = os.path.realpath(os.path.join(OUTSIDE, "link-parent"))
LINK_TARGET = os.path.realpath(os.path.join(OUTSIDE, "link-target"))
os.makedirs(LINK_PARENT)
os.makedirs(LINK_TARGET)
TARGET_FILE = os.path.join(LINK_TARGET, "target.txt")
with open(TARGET_FILE, "w") as f:
    f.write("x")
LINK = os.path.join(LINK_PARENT, "alias.txt")
DANGLING = os.path.join(LINK_PARENT, "missing-link.txt")
try:
    os.symlink(TARGET_FILE, LINK)
    os.symlink(os.path.join(LINK_TARGET, "missing.txt"), DANGLING)
    SYMLINKS_OK = True
except (OSError, NotImplementedError):
    SYMLINKS_OK = False
)PY");
    if (python.eval("SYMLINKS_OK") != "True")
        SKIP("Cannot create symbolic links here");

    // Deleting a link acts on the link in LINK_PARENT, never on what it points at.
    PluginPermissions permissions;
    permissions.fs_delete = {python.str("LINK_TARGET")};
    harness.save_permissions(PLUGIN, permissions);
    python.run_as(PLUGIN, R"PY(
target_folder_outcome = attempt(lambda: os.remove(LINK))
dangling_target_folder_outcome = attempt(lambda: os.remove(DANGLING))
)PY");

    CHECK(python.str("target_folder_outcome") == "denied");
    CHECK(python.str("dangling_target_folder_outcome") == "denied");
    CHECK(fs::is_symlink(fs::path(python.str("LINK"))));
    CHECK(fs::is_symlink(fs::path(python.str("DANGLING"))));
    CHECK(fs::exists(fs::path(python.str("TARGET_FILE"))));

    // Granting the containing folder allows deleting the links themselves, without touching the
    // link target or creating the dangling link's missing destination.
    permissions.fs_delete = {python.str("LINK_PARENT")};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
link_outcome = attempt(lambda: os.remove(LINK))
dangling_link_outcome = attempt(lambda: os.remove(DANGLING))
)PY");

    CHECK(python.str("link_outcome") == "ok");
    CHECK(python.str("dangling_link_outcome") == "ok");
    CHECK_FALSE(fs::is_symlink(fs::path(python.str("LINK"))));
    CHECK_FALSE(fs::is_symlink(fs::path(python.str("DANGLING"))));
    CHECK(fs::exists(fs::path(python.str("TARGET_FILE"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("LINK_TARGET")) / "missing.txt"));
}

#if !defined(_WIN32) && (defined(__linux__) || defined(__APPLE__))
TEST_CASE("A non-default dir_fd is checked against the folder it actually names", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    python.exec(R"PY(
FD_GRANTED = os.path.realpath(os.path.join(OUTSIDE, "fd-granted"))
FD_TARGET = os.path.realpath(os.path.join(OUTSIDE, "fd-target"))
FD_DENIED = os.path.join(OUTSIDE, "secrets")
os.makedirs(FD_GRANTED)
os.makedirs(FD_TARGET)
os.makedirs(FD_DENIED)
FD_VICTIM = os.path.join(FD_TARGET, "victim.txt")
with open(FD_VICTIM, "w") as f:
    f.write("x")
# Open the descriptors before entering any plugin audit scope: os.open itself is an
# FsRead/FsReadWrite audited event and must not fail the test before the descriptor is used.
FD_TARGET_FD = os.open(FD_TARGET, os.O_RDONLY)
FD_DENIED_FD = os.open(FD_DENIED, os.O_RDONLY)
)PY");

    // The working directory is granted, but the operations happen in FD_TARGET through dir_fd.
    // Resolving the relative path against the working directory would authorize or deny the
    // wrong folder, so the hook must resolve the descriptor first.
    PluginPermissions permissions;
    permissions.fs_create = {python.str("FD_GRANTED")};
    permissions.fs_delete = {python.str("FD_GRANTED")};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
os.chdir(FD_GRANTED)
try:
    plain_outcome = attempt(lambda: os.mkdir("plain"))
    prompted_create = attempt(lambda: os.mkdir("child", dir_fd=FD_TARGET_FD))
    prompted_delete = attempt(lambda: os.remove("victim.txt", dir_fd=FD_TARGET_FD))
finally:
    os.chdir(OUTSIDE)
)PY");

    // A default dir_fd must still be allowed: the audit arguments include the mode, which must
    // not be mistaken for a descriptor.
    CHECK(python.str("plain_outcome") == "ok");
    CHECK(fs::is_directory(fs::path(python.str("FD_GRANTED")) / "plain"));
    // Without a grant for FD_TARGET, the prompt names that actual folder and denial leaves both
    // folders untouched.
    CHECK(python.str("prompted_create") == "denied");
    CHECK(python.str("prompted_delete") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("FD_TARGET") + "\n" +
                                            "os.remove: " + python.str("FD_TARGET") + "\n");
    CHECK_FALSE(fs::exists(fs::path(python.str("FD_TARGET")) / "child"));
    CHECK(fs::exists(fs::path(python.str("FD_VICTIM"))));

    // A grant naming the resolved folder covers the operations.
    permissions.fs_create = {python.str("FD_TARGET")};
    permissions.fs_delete = {python.str("FD_TARGET")};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(FD_GRANTED)
try:
    granted_create = attempt(lambda: os.mkdir("child", dir_fd=FD_TARGET_FD))
    granted_delete = attempt(lambda: os.remove("victim.txt", dir_fd=FD_TARGET_FD))
    granted_rmdir = attempt(lambda: os.rmdir("child", dir_fd=FD_TARGET_FD))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("granted_create") == "ok");
    CHECK(python.str("granted_delete") == "ok");
    CHECK(python.str("granted_rmdir") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("FD_TARGET")) / "child"));
    CHECK_FALSE(fs::exists(fs::path(python.str("FD_VICTIM"))));

    // The resolved target still goes through the deny registry before any folder grant applies.
    permissions.fs_create = {"*"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(FD_GRANTED)
try:
    denied_create = attempt(lambda: os.mkdir("child", dir_fd=FD_DENIED_FD))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("denied_create") == "denied");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("FD_DENIED")) / "child"));
    python.exec("os.close(FD_TARGET_FD)\nos.close(FD_DENIED_FD)");
    AuditViolation violation;
    REQUIRE(mgr.last_violation(violation));
    CHECK(violation.reason == "denied path");
}

TEST_CASE("A dir_fd rmtree succeeds when full access allows the resolved folder", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
import inspect
RMTREE_DIR_FD_SUPPORTED = "dir_fd" in inspect.signature(shutil.rmtree).parameters
RMTREE_PARENT = os.path.join(OUTSIDE, "rmtree-fd-parent")
RMTREE_TREE = os.path.join(RMTREE_PARENT, "tree")
os.makedirs(RMTREE_TREE)
for name in ("a.txt", "b.txt"):
    with open(os.path.join(RMTREE_TREE, name), "w") as f:
        f.write("x")
# Open outside the plugin scope: os.open is audited and would otherwise need a read grant.
RMTREE_FD = os.open(RMTREE_PARENT, os.O_RDONLY)
)PY");
    if (python.eval("RMTREE_DIR_FD_SUPPORTED") != "True")
        SKIP("This Python's shutil.rmtree has no dir_fd parameter");

    // Full access is the "always allowed" counterpart: descriptor resolution must not turn an
    // otherwise authorized recursive delete into a denial.
    PluginPermissions permissions;
    permissions.full_access = true;
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
outcome = attempt(lambda: shutil.rmtree("tree", dir_fd=RMTREE_FD))
)PY");

    CHECK(python.str("outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_TREE"))));
    python.exec("os.close(RMTREE_FD)");
}
#elif !defined(_WIN32)
TEST_CASE("A non-default dir_fd fails closed when the platform cannot resolve it", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
import inspect
DIR_FD_SUPPORTED = "dir_fd" in inspect.signature(os.mkdir).parameters
FD_GRANTED = os.path.realpath(os.path.join(OUTSIDE, "fd-granted"))
FD_TARGET = os.path.realpath(os.path.join(OUTSIDE, "fd-target"))
os.makedirs(FD_GRANTED)
os.makedirs(FD_TARGET)
FD_VICTIM = os.path.join(FD_TARGET, "victim.txt")
with open(FD_VICTIM, "w") as f:
    f.write("x")
# Open outside the plugin scope so the audited os.open cannot fail before descriptor use.
FD_TARGET_FD = os.open(FD_TARGET, os.O_RDONLY)
)PY");
    if (python.eval("DIR_FD_SUPPORTED") != "True")
        SKIP("This Python does not support dir_fd here");

    // Even a grant naming the descriptor's folder cannot make an unresolvable descriptor safe.
    PluginPermissions permissions;
    permissions.fs_create = {python.str("FD_GRANTED"), python.str("FD_TARGET")};
    permissions.fs_delete = {python.str("FD_GRANTED"), python.str("FD_TARGET")};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
os.chdir(FD_GRANTED)
try:
    plain_outcome = attempt(lambda: os.mkdir("plain"))
    created_outcome = attempt(lambda: os.mkdir("child", dir_fd=FD_TARGET_FD))
    removed_outcome = attempt(lambda: os.remove("victim.txt", dir_fd=FD_TARGET_FD))
finally:
    os.chdir(OUTSIDE)
)PY");

    CHECK(python.str("plain_outcome") == "ok");
    CHECK(python.str("created_outcome") == "denied");
    CHECK(python.str("removed_outcome") == "denied");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("FD_TARGET")) / "child"));
    CHECK(fs::exists(fs::path(python.str("FD_VICTIM"))));
    python.exec("os.close(FD_TARGET_FD)");
}
#endif

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
