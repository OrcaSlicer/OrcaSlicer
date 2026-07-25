#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Platform naming used by BBLNetworkPlugin::scan_plugin_versions().
#if defined(_MSC_VER) || defined(_WIN32)
static const char* PLUGIN_PREFIX = "bambu_networking_";
static const char* PLUGIN_EXT    = ".dll";
#elif defined(__WXMAC__) || defined(__APPLE__)
static const char* PLUGIN_PREFIX = "libbambu_networking_";
static const char* PLUGIN_EXT    = ".dylib";
#else
static const char* PLUGIN_PREFIX = "libbambu_networking_";
static const char* PLUGIN_EXT    = ".so";
#endif

struct PluginFolderFixture
{
    fs::path    root;
    std::string previous_data_dir;

    PluginFolderFixture()
    {
        previous_data_dir = data_dir();
        root = fs::temp_directory_path() / fs::unique_path("orca-netver-%%%%%%%%");
        fs::create_directories(root / "plugins");
        set_data_dir(root.string());
    }

    ~PluginFolderFixture()
    {
        set_data_dir(previous_data_dir);
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }

    void add_plugin(const std::string& version)
    {
        boost::nowide::ofstream f((root / "plugins" / (PLUGIN_PREFIX + version + PLUGIN_EXT)).string());
        f << "stub";
    }
};

int count_version(const std::vector<NetworkLibraryVersionInfo>& versions, const std::string& v)
{
    int n = 0;
    for (const auto& info : versions)
        if (info.version == v)
            ++n;
    return n;
}

} // namespace

TEST_CASE("Series and managed classification", "[NetworkVersions]")
{
    // The AA.BB.CC series is the stored identity of every modern build.
    CHECK(network_plugin_series("02.08.01.53")      == "02.08.01");
    CHECK(network_plugin_series("02.08.01")         == "02.08.01"); // idempotent
    CHECK(network_plugin_series("02.08.01_custom")  == "02.08.01");
    CHECK(network_plugin_series("02.08.01.52-dev")  == "02.08.01");
    CHECK(network_plugin_series(BAMBU_NETWORK_AGENT_VERSION_LEGACY) == BAMBU_NETWORK_AGENT_VERSION_LEGACY);
    CHECK(network_plugin_series("").empty());

    // Only pure dotted-numeric builds collapse into their series entry; legacy and any
    // custom-named build keep their own identity.
    CHECK(is_series_managed_version("02.08.01"));
    CHECK(is_series_managed_version("02.08.01.53"));
    CHECK_FALSE(is_series_managed_version("02.08.01_custom"));
    CHECK_FALSE(is_series_managed_version("02.08.01.52-dev"));
    CHECK_FALSE(is_series_managed_version(BAMBU_NETWORK_AGENT_VERSION_LEGACY));
    CHECK_FALSE(is_series_managed_version(""));
}

TEST_CASE_METHOD(PluginFolderFixture, "Managed builds fold into the series; customs are surfaced", "[NetworkVersions]")
{
    add_plugin("02.08.01.55");          // managed, same series -> folded into the 02.08.01 row
    add_plugin("02.09.00.10");          // managed, unknown series -> not listed
    add_plugin("02.03.00.62");          // managed, series no longer whitelisted -> not listed
    add_plugin("02.08.01_custom");      // custom, whitelisted series -> listed under it
    add_plugin("02.08.01.52-dev");      // custom (dash-suffixed), whitelisted series -> listed

    auto versions = get_all_available_versions();

    // The specific managed build never gets its own row - the series represents it.
    REQUIRE(count_version(versions, "02.08.01.55") == 0);
    REQUIRE(count_version(versions, "02.08.01")    == 1);
    REQUIRE(count_version(versions, "02.09.00.10") == 0);
    REQUIRE(count_version(versions, "02.03.00.62") == 0);
    // Custom-named builds are distinct files kept under their own name.
    REQUIRE(count_version(versions, "02.08.01_custom")  == 1);
    REQUIRE(count_version(versions, "02.08.01.52-dev")  == 1);

    // Newest series first, its customs nested under it (suffix sort: "" < ".52-dev" < "_custom"),
    // legacy last.
    REQUIRE(versions[0].version == "02.08.01");
    REQUIRE(versions[1].version == "02.08.01.52-dev");
    REQUIRE(versions[2].version == "02.08.01_custom");
    REQUIRE(versions.back().version == BAMBU_NETWORK_AGENT_VERSION_LEGACY);

    // Customs sort/render nested under their series (non-empty suffix, base = the series).
    REQUIRE(versions[1].base_version == "02.08.01");
    REQUIRE_FALSE(versions[1].suffix.empty());
    REQUIRE(versions[2].base_version == "02.08.01");
    REQUIRE_FALSE(versions[2].suffix.empty());

    // "(Latest)" is the series row, never a nested custom build.
    REQUIRE(versions[0].suffix.empty());
    REQUIRE(versions[0].is_latest);
    REQUIRE_FALSE(versions[1].is_latest);
    REQUIRE_FALSE(versions[2].is_latest);

    // The stored default that drives download and update-check decisions is now the series.
    REQUIRE(std::string(get_latest_network_version()) == "02.08.01");
}

TEST_CASE_METHOD(PluginFolderFixture, "Only the loaded series is marked installed", "[NetworkVersions]")
{
    add_plugin("02.08.01.55");
    add_plugin("02.08.01_custom");

    // The loaded plug-in reports its full build (02.08.01.55); the series row is what gets marked.
    {
        auto versions = get_all_available_versions("02.08.01.55");
        int marked = 0;
        for (const auto& info : versions)
            if (info.is_loaded) { ++marked; REQUIRE(info.version == "02.08.01"); }
        REQUIRE(marked == 1);
    }

    // A loaded custom build matches its own row, never the bare series.
    {
        auto versions = get_all_available_versions("02.08.01_custom");
        int marked = 0;
        for (const auto& info : versions)
            if (info.is_loaded) { ++marked; REQUIRE(info.version == "02.08.01_custom"); }
        REQUIRE(marked == 1);
    }

    // Nothing loaded marks nothing, even though libraries are on disk.
    for (const auto& info : get_all_available_versions(""))
        REQUIRE_FALSE(info.is_loaded);
}

TEST_CASE("Only whitelisted series pass the load gate", "[NetworkVersions]")
{
    // The whitelisted series, its builds, and custom-named builds of that series.
    REQUIRE(is_supported_network_version("02.08.01"));
    REQUIRE(is_supported_network_version("02.08.01.52"));
    REQUIRE(is_supported_network_version("02.08.01.55"));
    REQUIRE(is_supported_network_version("02.08.01_custom"));
    REQUIRE(is_supported_network_version("02.08.01.52-dev"));
    REQUIRE(is_supported_network_version(BAMBU_NETWORK_AGENT_VERSION_LEGACY));

    // Series whitelisted by previous Orca releases - their ABI no longer matches.
    REQUIRE_FALSE(is_supported_network_version("02.03.00.62"));
    REQUIRE_FALSE(is_supported_network_version("02.01.01.52"));
    REQUIRE_FALSE(is_supported_network_version("02.00.02.50"));

    // Unknown series, legacy siblings, and malformed values.
    REQUIRE_FALSE(is_supported_network_version("02.09.00.10"));
    std::string legacy = BAMBU_NETWORK_AGENT_VERSION_LEGACY;
    std::string legacy_sibling = legacy.substr(0, 9) + (legacy.substr(9) == "99" ? "98" : "99");
    REQUIRE_FALSE(is_supported_network_version(legacy_sibling));
    REQUIRE_FALSE(is_supported_network_version(""));
    REQUIRE_FALSE(is_supported_network_version("02.08"));
}

TEST_CASE_METHOD(PluginFolderFixture, "Legacy series never adopts discovered builds", "[NetworkVersions]")
{
    // A different build of the legacy series must not be surfaced: is_legacy_version()
    // matches exactly, so it would be loaded with the modern struct layout.
    std::string legacy = BAMBU_NETWORK_AGENT_VERSION_LEGACY;
    std::string legacy_sibling = legacy.substr(0, 9) + (legacy.substr(9) == "99" ? "98" : "99");
    add_plugin(legacy_sibling);

    auto versions = get_all_available_versions();

    REQUIRE(count_version(versions, legacy_sibling) == 0);
    REQUIRE(count_version(versions, legacy) == 1);

    // With nothing else on disk, the series holds "(Latest)" even though its library is
    // not installed.
    for (const auto& info : versions) {
        if (info.version == "02.08.01") {
            REQUIRE(info.is_latest);
            REQUIRE_FALSE(info.is_loaded);
        }
    }
}
