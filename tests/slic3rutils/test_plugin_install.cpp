#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <catch2/catch_all.hpp>

#include <ios>
#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginLoader.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "catch2/catch_message.hpp"
#include "plugin_test_utils.hpp"
#include "test_utils.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <string>
#include <vector>

#include <libslic3r/Zipper.hpp>
#include <nlohmann/json.hpp>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

fs::path write_py_file(const fs::path& dir, const std::string& filename, const std::string& contents)
{
    fs::create_directories(dir);
    const fs::path p = dir / filename;
    std::ofstream out(p.string(), std::ios::binary);
    out << contents;
    return p;
}

// Write a sidecar verbatim so a test can exercise values the canonical writer never emits,
// such as the legacy permission schema.
void write_raw_install_state(const fs::path& plugin_dir, const nlohmann::json& state)
{
    fs::create_directories(plugin_dir);
    std::ofstream out((plugin_dir / ".install_state.json").string(), std::ios::binary);
    out << state.dump(2);
}

// The shared production helper: makes the path absolute and lexically normalized and, on
// Windows, prefixes it with the extended-length "\\?\" form, so the test can create, verify and
// clean up a tree past MAX_PATH even with LongPathsEnabled off. Identity on other platforms.
fs::path to_extended_path(const fs::path& p)
{
    return Slic3r::extended_path(p);
}

} // namespace

TEST_CASE("install_plugin rejects a cloud UUID containing path traversal", "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("cor2");

    // Package contents are irrelevant: the UUID is validated before metadata is read.
    const fs::path py = write_py_file(data_dir_guard.dir / "src", "evil.py", "print('hi')\n");



    PluginDescriptor descriptor;
    // is_cloud_plugin() -> true; cloud_uuid() -> the traversal string.
    descriptor.cloud = CloudPluginState{"../../escape", true, false, false, false};

    std::string error;
    const bool installed = plugin_loader::install_plugin(py, /*cloud_user_id=*/"test-user", descriptor, error);

    REQUIRE_FALSE(installed);
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("valid identifier"));
}

TEST_CASE("install_plugin rejects a side-loaded .py with no PEP 723 metadata", "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("cor3-bad");

    // No `# /// script` block -> name stays empty and type stays Unknown.
    const fs::path py = write_py_file(data_dir_guard.dir / "src", "nameless.py", "print('no metadata here')\n");


    PluginDescriptor descriptor;
    std::string error;
    const bool installed = plugin_loader::install_plugin(py, /*cloud_user_id=*/"", descriptor, error);

    REQUIRE_FALSE(installed);
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("PEP 723"));
}

TEST_CASE("install_plugin accepts a side-loaded .py with complete PEP 723 metadata", "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("cor3-good");

    const std::string contents =
        "# /// script\n"
        "# requires-python = \">=3.12\"\n"
        "#\n"
        "# [tool.orcaslicer.plugin]\n"
        "# name = \"Test Plugin\"\n"
        "# type = \"script\"\n"
        "# ///\n"
        "print('ok')\n";
    const fs::path py = write_py_file(data_dir_guard.dir / "src", "good.py", contents);


    PluginDescriptor descriptor;
    std::string error;
    const bool installed = plugin_loader::install_plugin(py, /*cloud_user_id=*/"", descriptor, error);

    // Positive control: a complete side-loaded .py must still install (guards against over-rejection).
    REQUIRE(installed);
    CHECK(error.empty());
}

TEST_CASE("install-state sidecar is the source of truth for a cloud plugin's installed version", "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("installed-version");

    const fs::path plugin_dir = data_dir_guard.dir / "plugin";
    fs::create_directories(plugin_dir);

    // A cloud plugin whose local manifest/PEP723 header lags the version actually fetched from
    // the cloud: the user bumped the version on the cloud without touching the local header.
    PluginDescriptor descriptor;
    descriptor.name             = "Versioned Plugin";
    descriptor.version          = "1.0.0"; // stale header version
    descriptor.installed_version = "1.2.0"; // version fetched from the cloud at install time
    descriptor.cloud            = CloudPluginState{"uuid-1", true, false, false, false};

    REQUIRE(write_install_state(plugin_dir, descriptor));

    // The writer must persist the installed_version (1.2.0), not the header version (1.0.0),
    // so a subsequent re-write from a freshly-scanned descriptor cannot clobber it.
    PluginInstallState state;
    REQUIRE(read_install_state(plugin_dir, state));
    CHECK(state.installed_version == "1.2.0");

    state.permissions.fs_read       = {"/path/to/read"};
    state.permissions.fs_readwrite  = {"/path/to/readwrite"};
    state.permissions.network_http  = {"https://api.example.com"};
    state.permissions.network_socket = {"192.168.45.6:443"};
    state.permissions.process        = {"/usr/bin/curl"};
    state.permissions.threading      = true;
    REQUIRE(write_install_state(plugin_dir, state));

    // Permission data is persisted in the same sidecar as the installation metadata.
    PluginInstallState persisted;
    REQUIRE(read_install_state(plugin_dir, persisted));
    CHECK(persisted.permissions.fs_read == state.permissions.fs_read);
    CHECK(persisted.permissions.fs_readwrite == state.permissions.fs_readwrite);
    CHECK(persisted.permissions.network_http == state.permissions.network_http);
    CHECK(persisted.permissions.network_socket == state.permissions.network_socket);
    CHECK(persisted.permissions.process == state.permissions.process);
    CHECK(persisted.permissions.threading == state.permissions.threading);

    // Reading the sidecar back onto a freshly-scanned descriptor (whose header version is still
    // 1.0.0) must surface the cloud-installed 1.2.0. This is what lets update_cloud_metadata compare
    // the cloud's latest version against the installed version instead of the stale header, so an
    // already-updated plugin no longer looks perpetually out of date.
    PluginDescriptor scanned;
    scanned.version = "1.0.0"; // as parsed from the unchanged PEP723 header
    read_install_state(plugin_dir, scanned);
    CHECK(scanned.installed_version == "1.2.0");
}

TEST_CASE("install-state reads legacy threading values with nonempty meaning granted", "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("threading-permission-read");
    const fs::path plugin_dir = data_dir_guard.dir / "plugin";

    const auto read_threading = [&plugin_dir]() {
        PluginInstallState state;
        REQUIRE(read_install_state(plugin_dir, state));
        return state.permissions.threading;
    };

    SECTION("booleans are read directly")
    {
        write_raw_install_state(plugin_dir, {{"permissions", {{"threading", true}}}});
        CHECK(read_threading());

        write_raw_install_state(plugin_dir, {{"permissions", {{"threading", false}}}});
        CHECK_FALSE(read_threading());
    }

    SECTION("a nonempty string grants and an empty string does not")
    {
        write_raw_install_state(plugin_dir, {{"permissions", {{"threading", "thread"}}}});
        CHECK(read_threading());

        write_raw_install_state(plugin_dir, {{"permissions", {{"threading", "other"}}}});
        CHECK(read_threading());

        write_raw_install_state(plugin_dir, {{"permissions", {{"threading", ""}}}});
        CHECK_FALSE(read_threading());
    }

    SECTION("a nonempty array grants regardless of its contents")
    {
        const std::vector<nlohmann::json> arrays = {
            nlohmann::json::array({"thread"}), nlohmann::json::array({"other"}),
            nlohmann::json::array({1}),        nlohmann::json::array({true}),
            nlohmann::json::array({"thread", 1})};
        for (const nlohmann::json& value : arrays) {
            write_raw_install_state(plugin_dir, {{"permissions", {{"threading", value}}}});
            CHECK(read_threading());
        }
    }

    SECTION("an empty array does not grant")
    {
        write_raw_install_state(plugin_dir, {{"permissions", {{"threading", nlohmann::json::array()}}}});
        CHECK_FALSE(read_threading());
    }

    SECTION("numeric, object and null values do not grant")
    {
        const std::vector<nlohmann::json> malformed = {
            0, 1, nlohmann::json::object({{"thread", true}}), nullptr};
        for (const nlohmann::json& value : malformed) {
            write_raw_install_state(plugin_dir, {{"permissions", {{"threading", value}}}});
            CHECK_FALSE(read_threading());
        }
    }
}

TEST_CASE("install-state writes threading as a boolean and reads it back", "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("threading-permission-write");
    const fs::path plugin_dir = data_dir_guard.dir / "plugin";

    PluginInstallState state;
    state.permissions.threading = true;
    REQUIRE(write_install_state(plugin_dir, state));

    std::ifstream in((plugin_dir / ".install_state.json").string(), std::ios::binary);
    const nlohmann::json written = nlohmann::json::parse(in);
    REQUIRE(written["permissions"].contains("threading"));
    CHECK(written["permissions"]["threading"].is_boolean());
    CHECK(written["permissions"]["threading"].get<bool>());
    in.close();

    PluginInstallState read_back;
    REQUIRE(read_install_state(plugin_dir, read_back));
    CHECK(read_back.permissions.threading);

    // The false grant is also written as a boolean and round-trips as false.
    state.permissions.threading = false;
    REQUIRE(write_install_state(plugin_dir, state));

    {
        std::ifstream false_in((plugin_dir / ".install_state.json").string(), std::ios::binary);
        const nlohmann::json written_false = nlohmann::json::parse(false_in);
        REQUIRE(written_false["permissions"].contains("threading"));
        CHECK(written_false["permissions"]["threading"].is_boolean());
        CHECK_FALSE(written_false["permissions"]["threading"].get<bool>());
    }

    REQUIRE(read_install_state(plugin_dir, read_back));
    CHECK_FALSE(read_back.permissions.threading);
}

TEST_CASE("reading a legacy threading value does not rewrite the sidecar and a normal write migrates it",
          "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("threading-permission-migrate");
    const fs::path plugin_dir = data_dir_guard.dir / "plugin";
    const fs::path sidecar    = plugin_dir / ".install_state.json";

    nlohmann::json capabilities = nlohmann::json::array();
    capabilities.push_back({{"Script", true}});

    const nlohmann::json legacy = {
        {"installed_from", "local"},
        {"installed_version", "1.0.0"},
        {"plugin_name", "Migrating Plugin"},
        {"cloud_uuid", "uuid-legacy"},
        {"enabled", false},
        {"permissions",
         {{"fs_read", nlohmann::json::array({"/allowed/read"})},
          {"fs_readwrite", nlohmann::json::array()},
          {"network_http", nlohmann::json::array()},
          {"network_socket", nlohmann::json::array()},
          {"process", nlohmann::json::array()},
          {"threading", nlohmann::json::array({"thread"})},
          {"events", nlohmann::json::array({"FsRead:open"})}}},
        {"capabilities", capabilities}};
    write_raw_install_state(plugin_dir, legacy);

    const auto read_raw = [&sidecar]() {
        std::ifstream in(sidecar.string(), std::ios::binary);
        return nlohmann::json::parse(in);
    };

    PluginInstallState state;
    REQUIRE(read_install_state(plugin_dir, state));
    CHECK(state.permissions.threading);
    CHECK(state.permissions.fs_read == std::vector<std::string>{"/allowed/read"});
    CHECK(state.permissions.events == std::vector<std::string>{"FsRead:open"});
    // Reading only maps the value in memory: the file keeps its legacy form until a write.
    CHECK(read_raw() == legacy);
    CHECK(read_raw()["permissions"]["threading"].is_array());

    REQUIRE(write_install_state(plugin_dir, state));
    const nlohmann::json migrated = read_raw();
    REQUIRE(migrated["permissions"].contains("threading"));
    CHECK(migrated["permissions"]["threading"].is_boolean());
    CHECK(migrated["permissions"]["threading"].get<bool>());
    CHECK(migrated["installed_from"] == "local");
    CHECK(migrated["installed_version"] == "1.0.0");
    CHECK(migrated["plugin_name"] == "Migrating Plugin");
    CHECK(migrated["cloud_uuid"] == "uuid-legacy");
    CHECK_FALSE(migrated["enabled"].get<bool>());
    CHECK(migrated["permissions"]["fs_read"] == legacy["permissions"]["fs_read"]);
    CHECK(migrated["permissions"]["events"] == legacy["permissions"]["events"]);
    CHECK(migrated["capabilities"] == legacy["capabilities"]);
}

TEST_CASE("install-state reads full access as a boolean and defaults to off", "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("full-access-read");
    const fs::path plugin_dir = data_dir_guard.dir / "plugin";

    const auto read_full_access = [&plugin_dir]() {
        PluginInstallState state;
        REQUIRE(read_install_state(plugin_dir, state));
        return state.permissions.full_access;
    };

    SECTION("a missing flag is off")
    {
        write_raw_install_state(plugin_dir, {{"permissions", nlohmann::json::object()}});
        CHECK_FALSE(read_full_access());
    }

    SECTION("explicit booleans are read directly")
    {
        write_raw_install_state(plugin_dir, {{"permissions", {{"full_access", true}}}});
        CHECK(read_full_access());

        write_raw_install_state(plugin_dir, {{"permissions", {{"full_access", false}}}});
        CHECK_FALSE(read_full_access());
    }

    SECTION("non-boolean values are off")
    {
        const std::vector<nlohmann::json> malformed = {
            "true", 1, 0, nlohmann::json::array({true}), nlohmann::json::object({{"enabled", true}}), nullptr};
        for (const nlohmann::json& value : malformed) {
            write_raw_install_state(plugin_dir, {{"permissions", {{"full_access", value}}}});
            CHECK_FALSE(read_full_access());
        }
    }
}

TEST_CASE("install-state writes full access as a boolean and reads it back", "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("full-access-write");
    const fs::path plugin_dir = data_dir_guard.dir / "plugin";

    PluginInstallState state;
    state.permissions.full_access = true;
    state.permissions.fs_read     = {"/allowed/read"};
    REQUIRE(write_install_state(plugin_dir, state));

    std::ifstream in((plugin_dir / ".install_state.json").string(), std::ios::binary);
    const nlohmann::json written = nlohmann::json::parse(in);
    REQUIRE(written["permissions"].contains("full_access"));
    CHECK(written["permissions"]["full_access"].is_boolean());
    CHECK(written["permissions"]["full_access"].get<bool>());
    CHECK(written["permissions"]["fs_read"] == nlohmann::json::array({"/allowed/read"}));
    in.close();

    PluginInstallState read_back;
    REQUIRE(read_install_state(plugin_dir, read_back));
    CHECK(read_back.permissions.full_access);
    CHECK(read_back.permissions.fs_read == std::vector<std::string>{"/allowed/read"});

    // Turning full access off is also written as a boolean and preserves the other grants.
    state.permissions.full_access = false;
    REQUIRE(write_install_state(plugin_dir, state));
    {
        std::ifstream false_in((plugin_dir / ".install_state.json").string(), std::ios::binary);
        const nlohmann::json written_false = nlohmann::json::parse(false_in);
        REQUIRE(written_false["permissions"].contains("full_access"));
        CHECK(written_false["permissions"]["full_access"].is_boolean());
        CHECK_FALSE(written_false["permissions"]["full_access"].get<bool>());
        CHECK(written_false["permissions"]["fs_read"] == nlohmann::json::array({"/allowed/read"}));
    }

    REQUIRE(read_install_state(plugin_dir, read_back));
    CHECK_FALSE(read_back.permissions.full_access);
}

TEST_CASE("a local plugin's installed version follows its entry file header", "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("local-installed-version");

    const fs::path plugin_dir = data_dir_guard.dir / "plugin";
    fs::create_directories(plugin_dir);

    // Sidecar written when the local plugin was first installed at 1.0.0.
    PluginInstallState state;
    state.installed_from    = "local";
    state.installed_version = "1.0.0";
    REQUIRE(write_install_state(plugin_dir, state));

    // The user then edited the plugin file: the freshly scanned header says 1.1.0.
    PluginDescriptor scanned;
    scanned.version = "1.1.0";
    REQUIRE(read_install_state(plugin_dir, scanned));
    CHECK(scanned.installed_version == "1.1.0");
}

TEST_CASE("an unsubscribed cloud plugin follows its header version", "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("unsubscribed-installed-version");

    const fs::path plugin_dir = data_dir_guard.dir / "plugin";
    fs::create_directories(plugin_dir);

    // What keep_installed_plugin_as_local leaves behind: a normal local package. The sidecar still
    // holds the version fetched from the cloud (1.2.0), but the header (1.0.0) is the source of truth.
    PluginInstallState state;
    state.installed_from    = "local";
    state.installed_version = "1.2.0";
    REQUIRE(write_install_state(plugin_dir, state));

    PluginDescriptor scanned;
    scanned.version = "1.0.0";
    REQUIRE(read_install_state(plugin_dir, scanned));
    CHECK(scanned.installed_version == "1.0.0");
}

TEST_CASE("extract_zip_to_directory writes entries under a destination longer than MAX_PATH",
          "[PluginInstall][PluginFsUtils]")
{
    ScopedTemporaryDir dir("deep-extract");

    const fs::path wheel = dir.path() / "deep_pkg-1.0-py3-none-any.whl";
    const std::string payload = "print('hello from a deep path')\n";
    {
        Zipper zipper(wheel.string(), Zipper::NO_COMPRESSION);
        zipper.add_entry("pkg/", "", 0); // explicit directory entry
        zipper.add_entry("pkg/module.py", payload.data(), payload.size());
        zipper.finalize();
    }

    // A single 240-char component keeps every filename below the NTFS 255 limit while pushing the
    // full destination (temp dir + component) past the 260-char MAX_PATH the Win32 APIs enforce
    // when LongPathsEnabled is off.
    const fs::path destination = dir.path() / std::string(240, 'd');
    REQUIRE(destination.string().size() > 260);

    std::string error;
    REQUIRE(extract_zip_to_directory(wheel, destination, error));
    INFO("extract error: " << error);
    CHECK(error.empty());

    // Verify through a long-path-capable path, otherwise the check itself is bounded by MAX_PATH.
    const fs::path module_py = destination / "pkg" / "module.py";
    REQUIRE(fs::exists(to_extended_path(module_py)));
    REQUIRE(fs::is_regular_file(to_extended_path(module_py)));

    // Remove the deep subtree here: ScopedTemporaryDir's plain remove_all cannot reach past
    // MAX_PATH on Windows.
    boost::system::error_code ec;
    fs::remove_all(to_extended_path(destination), ec);
    CHECK_FALSE(ec);
}

TEST_CASE("extract_zip_to_directory rejects an entry that escapes the destination",
          "[PluginInstall][PluginFsUtils]")
{
    ScopedTemporaryDir dir("escape-extract");

    const fs::path wheel = dir.path() / "evil-1.0-py3-none-any.whl";
    {
        Zipper zipper(wheel.string(), Zipper::NO_COMPRESSION);
        zipper.add_entry("pkg/module.py", "print('ok')\n", 12);
        zipper.add_entry("../escape.txt", "escaped\n", 8);
        zipper.finalize();
    }

    std::string error;
    CHECK_FALSE(extract_zip_to_directory(wheel, dir.path() / "out", error));
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("escapes the plugin package"));
    CHECK_FALSE(fs::exists(dir.path() / "escape.txt"));
}
