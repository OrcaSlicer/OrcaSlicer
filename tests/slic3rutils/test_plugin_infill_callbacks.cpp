// Failure handling for the optional per-surface infill callbacks. Fill invokes generate(),
// no_sort() and aligned_to_origin() at different points, so all three must translate a plugin raise
// the same way: cancellation stays a cancellation, and any other exception is recorded in the
// plugin's diagnostic store and surfaced as the generic non-critical slicing error.
//
// As in the other plugin suites, each Catch2 test case runs in its own process
// (catch_discover_tests), so the PluginManager and interpreter come up at most once per test.
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <fstream>
#include <ios>
#include <memory>
#include <stdexcept>
#include <string>

#include "libslic3r/Exception.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintBase.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/pluginTypes/infill/InfillPluginCapability.hpp"

#include "plugin_test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Brings the plugin system up for one test and tears it down at the end. Declare this FIRST in a test
// so it is destroyed last.
struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"infill-callback-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

bool load_and_wait(PluginManager& manager, const std::string& plugin_key, std::string& error)
{
    manager.load_plugin(plugin_key, /*skip_deps=*/true);
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(120), error);
}

void write_callback_failure_plugin(const ScopedDataDir& data_dir_guard,
                                   const std::string&   stem,
                                   const std::string&   capability_name)
{
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / stem;
    fs::create_directories(plugin_dir);

    std::ofstream out((plugin_dir / (stem + ".py")).string(), std::ios::binary);
    out << "# /// script\n"
           "# requires-python = \">=3.12\"\n"
           "# dependencies = []\n"
           "#\n"
           "# [tool.orcaslicer.plugin]\n"
           "# name = \"" + capability_name + " Package\"\n"
           "# description = \"Infill callback failure test fixture.\"\n"
           "# author = \"tests\"\n"
           "# version = \"1.0.0\"\n"
           "# type = \"infill\"\n"
           "# ///\n"
           "import orca\n"
           "\n\n"
           "class Cap" + stem + "(orca.infill.InfillPluginCapabilityBase):\n"
           "    def get_name(self):\n"
           "        return \"" + capability_name + "\"\n"
           "    def patterns(self):\n"
           "        return [orca.infill.PatternDescriptor('callback-pattern', 'Callback Pattern')]\n"
           "    def generate(self, pattern_id, request):\n"
           "        return []\n"
           "    def no_sort(self, pattern_id):\n"
           "        raise RuntimeError('boom from no_sort')\n"
           "    def aligned_to_origin(self, pattern_id):\n"
           "        raise RuntimeError('boom from aligned_to_origin')\n"
           "\n\n"
           "@orca.plugin\n"
           "class Plugin" + stem + "(orca.base):\n"
           "    def register_capabilities(self):\n"
           "        orca.register_capability(Cap" + stem + ")\n";
    out.close();
}

// Native provider whose optional callbacks fail on demand. No interpreter is involved, so the
// translation and cancellation rules can be checked on their own.
class FailingCallbackInfillCapability : public InfillPluginCapability
{
public:
    enum class Failure
    {
        RuntimeError,
        Cancelled,
    };

    explicit FailingCallbackInfillCapability(Failure failure = Failure::RuntimeError)
        : m_failure(failure) {}

    std::string get_name() const override { return "Failing Callback Infill"; }
    Polylines   generate(const std::string&, const InfillPluginRequest&) override { return {}; }
    bool        no_sort(const std::string&) const override { fail(); }
    bool        aligned_to_origin(const std::string&) const override { fail(); }

private:
    [[noreturn]] void fail() const
    {
        if (m_failure == Failure::Cancelled)
            throw CanceledException();
        throw std::runtime_error("native callback failure");
    }

    Failure m_failure;
};

} // namespace

TEST_CASE("A failing optional infill callback becomes the generic noncritical slicing error", "[PluginInfill]")
{
    auto capability = std::make_shared<FailingCallbackInfillCapability>();
    InfillPluginFill fill(capability, "callback-pattern");

    const std::string expected = PluginManager::plugin_infill_failure_message();
    CHECK_THROWS_WITH(fill.no_sort(), Catch::Matchers::Equals(expected));
    CHECK_THROWS_AS(fill.no_sort(), SlicingError);
    CHECK_THROWS_WITH(fill.aligned_to_origin(), Catch::Matchers::Equals(expected));
    CHECK_THROWS_AS(fill.aligned_to_origin(), SlicingError);
}

TEST_CASE("A cancelled optional infill callback stays a cancellation", "[PluginInfill]")
{
    auto capability = std::make_shared<FailingCallbackInfillCapability>(
        FailingCallbackInfillCapability::Failure::Cancelled);
    InfillPluginFill fill(capability, "callback-pattern");

    // CanceledException is not a slicing error and must reach the slicing job untouched.
    CHECK_THROWS_AS(fill.no_sort(), CanceledException);
    CHECK_THROWS_AS(fill.aligned_to_origin(), CanceledException);
}

TEST_CASE("A Python no_sort or aligned_to_origin exception records the plugin diagnostic", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-callback-failure");
    write_callback_failure_plugin(data_dir_guard, "CallbackFailureInfill", "Callback Failure Infill");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "CallbackFailureInfill", error));
    INFO("load error: " << error);

    const auto capability = manager.get_plugin_capability("Callback Failure Infill",
                                                          PluginCapabilityType::Infill, /*only_enabled=*/true);
    REQUIRE(capability != nullptr);
    auto infill = std::dynamic_pointer_cast<InfillPluginCapability>(capability);
    REQUIRE(infill != nullptr);

    std::unique_ptr<Fill> fill(infill->create_fill(infill, "callback-pattern"));
    REQUIRE(fill != nullptr);

    const std::string expected = PluginManager::plugin_infill_failure_message();
    CHECK_THROWS_WITH(fill->no_sort(), Catch::Matchers::Equals(expected));

    PluginDescriptor descriptor;
    REQUIRE(manager.try_get_plugin_descriptor("CallbackFailureInfill", descriptor));
    CHECK(descriptor.error.find("boom from no_sort") != std::string::npos);
    CHECK(descriptor.error.find("no_sort()") != std::string::npos);

    CHECK_THROWS_WITH(fill->aligned_to_origin(), Catch::Matchers::Equals(expected));

    REQUIRE(manager.try_get_plugin_descriptor("CallbackFailureInfill", descriptor));
    CHECK(descriptor.error.find("boom from aligned_to_origin") != std::string::npos);
    CHECK(descriptor.error.find("aligned_to_origin()") != std::string::npos);

    manager.unload_plugin("CallbackFailureInfill");
}
