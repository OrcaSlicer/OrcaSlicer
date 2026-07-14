#include <catch2/catch_all.hpp>

#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginLoader.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PythonFileUtils.hpp>

#include <boost/filesystem.hpp>

#include <fstream>
#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Point data_dir() at a throwaway directory for the lifetime of a test and
// restore the previous value afterwards, so install_plugin() writes into a
// disposable tree and tests don't leak state into each other.
struct ScopedDataDir
{
    std::string previous;
    fs::path    dir;

    explicit ScopedDataDir(const std::string& tag)
    {
        previous = data_dir();
        dir      = fs::temp_directory_path() / fs::unique_path("orca-" + tag + "-%%%%-%%%%");
        fs::create_directories(dir);
        set_data_dir(dir.string());
    }

    ~ScopedDataDir()
    {
        set_data_dir(previous);
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

fs::path write_py_file(const fs::path& dir, const std::string& filename, const std::string& contents)
{
    fs::create_directories(dir);
    const fs::path p = dir / filename;
    std::ofstream out(p.string(), std::ios::binary);
    out << contents;
    return p;
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

    // Reading the sidecar back onto a freshly-scanned descriptor (whose header version is still
    // 1.0.0) must surface the cloud-installed 1.2.0. This is what lets update_cloud_catalog compare
    // the cloud's latest version against the installed version instead of the stale header, so an
    // already-updated plugin no longer looks perpetually out of date.
    PluginDescriptor scanned;
    scanned.version = "1.0.0"; // as parsed from the unchanged PEP723 header
    read_install_state(plugin_dir, scanned);
    CHECK(scanned.installed_version == "1.2.0");
}

TEST_CASE("install_plugin parses [tool.orcaslicer.plugin.settings] into descriptor.settings", "[PluginInstall]")
{
    ScopedDataDir data_dir_guard("plugin-settings");

    // A PEP-723 header with a per-plugin settings sub-table. Values stay strings; the plugin
    // parses what it needs (ctx.params). This is the source Twistify reads its knobs from.
    const std::string contents =
        "# /// script\n"
        "# requires-python = \">=3.12\"\n"
        "#\n"
        "# [tool.orcaslicer.plugin]\n"
        "# name = \"Settings Plugin\"\n"
        "# type = \"slicing-pipeline\"\n"
        "#\n"
        "# [tool.orcaslicer.plugin.settings]\n"
        "# twist_deg_per_mm = \"1.5\"\n"
        "# taper_per_mm = \"-0.004\"\n"
        "# ///\n"
        "print('ok')\n";
    const fs::path py = write_py_file(data_dir_guard.dir / "src", "settings.py", contents);

    PluginDescriptor descriptor;
    std::string error;
    const bool installed = plugin_loader::install_plugin(py, /*cloud_user_id=*/"", descriptor, error);

    REQUIRE(installed);
    CHECK(error.empty());
    REQUIRE(descriptor.settings.count("twist_deg_per_mm") == 1);
    CHECK(descriptor.settings.at("twist_deg_per_mm") == "1.5");
    CHECK(descriptor.settings.at("taper_per_mm") == "-0.004");
    // Identity keys are NOT captured as settings (they belong to [tool.orcaslicer.plugin]).
    CHECK(descriptor.settings.count("name") == 0);
}
