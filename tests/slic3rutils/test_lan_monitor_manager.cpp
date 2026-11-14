#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/LanMonitorManager.hpp"
#include "slic3r/Utils/BBLNetworkPlugin.hpp"
#include "plugin_test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <string>

TEST_CASE("LAN monitoring requires both Multi-device and networking to be enabled", "[LanMonitor][Regression]")
{
    CHECK_FALSE(Slic3r::GUI::lan_monitoring_enabled(nullptr));
    Slic3r::AppConfig config;
    const bool multi = GENERATE(false, true);
    const bool networking = GENERATE(false, true);
    config.set("enable_multi_machine", std::string(multi ? "true" : "false"));
    config.set_bool("installed_networking", networking);
    const bool expected = multi && networking;
    CHECK(Slic3r::GUI::lan_monitoring_enabled(&config) == expected);
    // Disabling networking after a previously enabled session also stops monitoring.
    config.set_bool("installed_networking", false);
    CHECK_FALSE(Slic3r::GUI::lan_monitoring_enabled(&config));
}

TEST_CASE("A managed networking series resolves to its installed build without crossing series", "[LanMonitor][Regression]")
{
    using Slic3r::BBLNetworkPlugin;
    Slic3r::ScopedDataDir data("lan-sdk-resolution");
    boost::filesystem::create_directories(data.dir / "plugins");
    const auto create_module = [](const std::string& version) {
        const auto path = BBLNetworkPlugin::get_versioned_library_path(version);
        boost::nowide::ofstream file(path);
        file << "test fixture, not loaded";
        return path;
    };
    const auto older = create_module("02.08.01.20");
    const auto newest = create_module("02.08.01.53");
    create_module("02.09.00.99");
    CHECK(BBLNetworkPlugin::resolve_library_path("02.08.01") == newest);
    CHECK(BBLNetworkPlugin::resolve_library_path("02.08.01.20") == older);
    const auto missing = BBLNetworkPlugin::get_versioned_library_path("02.09.01");
    CHECK(BBLNetworkPlugin::resolve_library_path("02.09.01") == missing);
    CHECK_FALSE(boost::filesystem::exists(missing));
    const auto exact = create_module("02.08.01");
    CHECK(BBLNetworkPlugin::resolve_library_path("02.08.01") == exact);
}
