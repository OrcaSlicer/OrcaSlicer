// ResourcePrinterAsset plugin type: asset folder validation, the host registry's lifecycle,
// resolving the "orcacloud-asset/<plugin id>/<file>" references printer presets store in
// bed_custom_model / bed_custom_texture, and the printer preset manifest that lets PluginResolver
// offer a missing pack without blocking slicing.
//
// The GUI wiring lives in GUI_App::init_plugin_gui_wiring; the test binary has no app, so the same
// registry callbacks are installed here. Each Catch2 test case runs in its own process
// (catch_discover_tests), so the PluginManager and interpreter come up at most once per test.
#include "test_utils.hpp"
#include <slic3r/plugin/PluginDescriptor.hpp>
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <catch2/catch_all.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrinterAssetRef.hpp"

#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginResolver.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include "slic3r/plugin/host/PluginPrinterAssets.hpp"

#include "plugin_test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Declare FIRST in a test so it is destroyed last (see test_plugin_resource_texture.cpp).
struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"asset-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

void install_printer_asset_wiring()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    PluginManager& manager = PluginManager::instance();
    manager.subscribe_on_load_callback([](const std::string& plugin_key) { PluginPrinterAssets::instance().on_plugin_register(plugin_key); });
    manager.subscribe_on_unload_callback([](const std::string& plugin_key) { PluginPrinterAssets::instance().on_plugin_deregister(plugin_key); });
    manager.subscribe_on_capability_load_callback([](const PluginCapabilityId& capability) {
        if (capability.type == PluginCapabilityType::ResourcePrinterAsset)
            PluginPrinterAssets::instance().on_cap_register(capability);
    });
    manager.subscribe_on_capability_unload_callback([](const PluginCapabilityId& capability) {
        if (capability.type == PluginCapabilityType::ResourcePrinterAsset)
            PluginPrinterAssets::instance().on_cap_deregister(capability);
    });
}

// A minimal printer asset pack shaped like the wheel Orca Cloud builds (pa/__init__.py + pa/a/<file>).
std::string make_printer_asset_source(const std::string& display_name, const std::string& get_asset_dir_body)
{
    return std::string{}
        + "# /// script\n"
        + "# requires-python = \">=3.12\"\n"
        + "# dependencies = []\n"
        + "#\n"
        + "# [tool.orcaslicer.plugin]\n"
        + "# name = \"" + display_name + "\"\n"
        + "# description = \"Printer asset pack fixture.\"\n"
        + "# author = \"tests\"\n"
        + "# version = \"1.0.0\"\n"
        + "# type = \"resource-printer-asset\"\n"
        + "# ///\n"
        + "import os\n"
        + "import orca\n"
        + "\n\n"
        + "class PrinterAssets(orca.resource_printer_asset.ResourcePrinterAssetPluginCapabilityBase):\n"
        + "    def get_name(self):\n"
        + "        return \"" + display_name + "\"\n"
        + "\n"
        + "    def get_asset_dir(self):\n"
        + "        " + get_asset_dir_body + "\n"
        + "\n\n"
        + "@orca.plugin\n"
        + "class PrinterAssetPack(orca.base):\n"
        + "    def register_capabilities(self):\n"
        + "        orca.register_capability(PrinterAssets)\n";
}

std::string default_asset_dir_body() { return "return os.path.join(os.path.dirname(__file__), \"a\")"; }

fs::path write_plugin(const ScopedDataDir& data_dir_guard, const std::string& stem, const std::string& source)
{
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / stem;
    fs::create_directories(plugin_dir);
    std::ofstream out((plugin_dir / (stem + ".py")).string(), std::ios::binary);
    out << source;
    return plugin_dir;
}

void write_text_file(const fs::path& path, const std::string& content = "x")
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::binary);
    out << content;
}

bool load_and_wait(PluginManager& manager, const std::string& plugin_key, std::string& error)
{
    manager.load_plugin(plugin_key, /*skip_deps=*/true);
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(120), error);
}

std::vector<std::string> filenames(const std::vector<std::string>& paths)
{
    std::vector<std::string> names;
    for (const std::string& path : paths)
        names.push_back(fs::path(path).filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

// Loads one local pack "Voron Bed" (plugin key Voron_Bed) with a model, a texture and a stray file.
std::string load_voron_pack(const ScopedDataDir& data_dir_guard)
{
    const std::string plugin_key = "Voron_Bed";
    const fs::path    plugin_dir = write_plugin(data_dir_guard, plugin_key, make_printer_asset_source("Voron Bed", default_asset_dir_body()));
    write_text_file(plugin_dir / "a" / "bed.stl");
    write_text_file(plugin_dir / "a" / "plate.svg");
    write_text_file(plugin_dir / "a" / "readme.txt");

    PluginManager& manager = PluginManager::instance();
    install_printer_asset_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    const bool  loaded = load_and_wait(manager, plugin_key, error);
    INFO("load error: " << error);
    REQUIRE(loaded);
    return plugin_key;
}

} // namespace

// ── Validation (collect_printer_asset_files, no Python) ─────────────────────────────────────────

TEST_CASE("An asset folder lists only its .stl, .png and .svg files", "[PluginPrinterAsset]")
{
    ScopedTemporaryDir root("asset-root");
    write_text_file(root.path() / "a" / "bed.stl");
    write_text_file(root.path() / "a" / "plate.PNG");
    write_text_file(root.path() / "a" / "plate.svg");
    write_text_file(root.path() / "a" / "notes.txt");
    write_text_file(root.path() / "a" / "photo.jpg");
    write_text_file(root.path() / "a" / "nested" / "deep.stl");

    std::string                    error;
    const std::vector<std::string> files = collect_printer_asset_files((root.path() / "a").string(), root.string(), error);

    CHECK(error.empty());
    CHECK(filenames(files) == std::vector<std::string>{"bed.stl", "plate.PNG", "plate.svg"});
}

TEST_CASE("An asset folder outside the plugin folder is refused", "[PluginPrinterAsset]")
{
    ScopedTemporaryDir parent("asset-escape");
    const fs::path     root    = parent.path() / "pack";
    const fs::path     outside = parent.path() / "outside";
    fs::create_directories(root);
    write_text_file(outside / "bed.stl");

    std::string error;
    CHECK(collect_printer_asset_files((root / ".." / "outside").string(), root.string(), error).empty());
    CHECK_FALSE(error.empty());
    CHECK(collect_printer_asset_files("", root.string(), error).empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("The asset listing stops at 16 files", "[PluginPrinterAsset]")
{
    ScopedTemporaryDir root("asset-cap");
    for (int i = 0; i < 20; ++i)
        write_text_file(root.path() / ("plate_" + std::to_string(i) + ".stl"));

    // Sorted before the cap, so every file system keeps the same 16.
    std::string                    error;
    const std::vector<std::string> files = collect_printer_asset_files(root.string(), root.string(), error);
    REQUIRE(files.size() == 16);
    CHECK(std::is_sorted(files.begin(), files.end()));
    CHECK(fs::path(files.back()).filename() == "plate_5.stl");
}

TEST_CASE("Plain bed paths resolve to themselves and packs that are not loaded to nothing", "[PluginPrinterAsset]")
{
    const std::string missing_ref = "orcacloud-asset/3f2a6c1e-8d4b-4c2a-9e7f-1a2b3c4d5e6f/bed.stl";

    CHECK(resolve_printer_asset_value("") == "");
    CHECK(resolve_printer_asset_value("/Users/me/bed.stl") == "/Users/me/bed.stl");
    CHECK(resolve_printer_asset_value(missing_ref).empty());
}

TEST_CASE("A printer's asset pack names follow its bed files, falling back to its manifest for packs not loaded here",
          "[PluginPrinterAsset]")
{
    std::unique_ptr<DynamicPrintConfig> config(DynamicPrintConfig::new_from_defaults_keys(Preset::printer_options()));
    const auto names = [&config]() { return config->option<ConfigOptionStrings>("printer_asset_plugins")->values; };

    // Plain paths name no pack, so a stale name is dropped.
    config->option<ConfigOptionStrings>("printer_asset_plugins")->values = {"Old Pack"};
    config->set_key_value("bed_custom_model", new ConfigOptionString("/Users/me/bed.stl"));
    update_printer_asset_plugins(*config);
    CHECK(names().empty());

    // A pack that is not loaded here keeps the name its manifest entry (matched by uuid) holds.
    config->set_key_value("bed_custom_model", new ConfigOptionString("orcacloud-asset/3f2a6c1e-8d4b-4c2a-9e7f-1a2b3c4d5e6f/bed.stl"));
    config->set_key_value("bed_custom_texture", new ConfigOptionString("orcacloud-asset/9b1d0000-0000-4000-8000-000000000000/plate.png"));
    config->option<ConfigOptionStrings>("printer_plugins")->values = {"Voron Bed;3F2A6C1E-8D4B-4C2A-9E7F-1A2B3C4D5E6F;Voron Bed by sam"};
    update_printer_asset_plugins(*config);
    // The texture's pack is neither loaded nor in the manifest: its name is unknown.
    CHECK(names() == std::vector<std::string>{"Voron Bed by sam"});

    // The saved preset's manifest names a pack this config's manifest has lost (e.g. bed files
    // transferred from another preset). Entries carry no type: the plugin's other capability
    // ("Voron Agent") loses to the name the config already lists as an asset pack.
    config->option<ConfigOptionStrings>("printer_plugins")->values = {};
    config->option<ConfigOptionStrings>("printer_asset_plugins")->values = {"Voron Bed by sam"};
    const ConfigOptionStrings saved_manifest({"Voron Bed;3f2a6c1e-8d4b-4c2a-9e7f-1a2b3c4d5e6f;Voron Agent",
                                              "Voron Bed;3f2a6c1e-8d4b-4c2a-9e7f-1a2b3c4d5e6f;Voron Bed by sam",
                                              "Other Agent;77777777-0000-4000-8000-000000000000;Agent"});
    update_printer_asset_plugins(*config, &saved_manifest);
    CHECK(names() == std::vector<std::string>{"Voron Bed by sam"});
    // Only the bed files' packs are taken over from the fallback, even without a resolver to rebuild
    // the manifest.
    const std::vector<std::string> manifest = config->option<ConfigOptionStrings>("printer_plugins")->values;
    CHECK(std::find(manifest.begin(), manifest.end(), "Other Agent;77777777-0000-4000-8000-000000000000;Agent") == manifest.end());
    CHECK(std::find(manifest.begin(), manifest.end(), "Voron Bed;3f2a6c1e-8d4b-4c2a-9e7f-1a2b3c4d5e6f;Voron Bed by sam") != manifest.end());

    // Not a printer config: nothing to do.
    std::unique_ptr<DynamicPrintConfig> print(DynamicPrintConfig::new_from_defaults_keys({"bed_custom_model"}));
    update_printer_asset_plugins(*print);
    CHECK(print->option("printer_asset_plugins") == nullptr);
}

// ── Lifecycle (Python) ──────────────────────────────────────────────────────────────────────────

TEST_CASE("A pack's files resolve from references and stop resolving on disable and unload",
          "[PluginPrinterAsset][PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir     data_dir_guard("asset-lifecycle");
    const std::string plugin_key = load_voron_pack(data_dir_guard);
    PluginManager&    manager    = PluginManager::instance();

    const std::vector<PrinterAssetPackEntry> packs = PluginPrinterAssets::instance().entries();
    REQUIRE(packs.size() == 1);
    CHECK(packs.front().display_name == "Voron Bed");
    // A local pack has no cloud uuid, so references use its plugin key.
    CHECK(packs.front().pack_id == plugin_key);
    CHECK(filenames(packs.front().files) == std::vector<std::string>{"bed.stl", "plate.svg"});

    const std::string model_ref = make_printer_asset_ref(plugin_key, "bed.stl");
    const std::string model     = resolve_printer_asset_value(model_ref);
    CHECK(fs::path(model).filename() == "bed.stl");
    CHECK(fs::exists(model));
    // Only files the pack lists resolve; the reference can never leave the asset folder.
    CHECK(resolve_printer_asset_value(make_printer_asset_ref(plugin_key, "readme.txt")).empty());

    manager.set_capability_enabled({PluginCapabilityType::ResourcePrinterAsset, "Voron Bed", plugin_key}, false);
    CHECK(PluginPrinterAssets::instance().entries().empty());
    CHECK(resolve_printer_asset_value(model_ref).empty());

    manager.set_capability_enabled({PluginCapabilityType::ResourcePrinterAsset, "Voron Bed", plugin_key}, true);
    CHECK(resolve_printer_asset_value(model_ref) == model);

    REQUIRE(manager.unload_plugin(plugin_key));
    CHECK(PluginPrinterAssets::instance().entries().empty());
}

TEST_CASE("A printer preset names its pack in its own manifest, and an inactive pack prompts without blocking slicing",
          "[PluginPrinterAsset][PluginResolver][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir     data_dir_guard("asset-manifest");
    const std::string plugin_key = load_voron_pack(data_dir_guard);
    PluginManager&    manager    = PluginManager::instance();

    // What the Bed Shape dialog stores: references to the pack's files. The pack's capability name and
    // manifest entry follow from them. A file from a pack this machine knows nothing about adds no name
    // but does not drop the loaded pack's.
    std::unique_ptr<DynamicPrintConfig> config(DynamicPrintConfig::new_from_defaults_keys(Preset::printer_options()));
    config->set_key_value("bed_custom_texture", new ConfigOptionString("orcacloud-asset/9b1d0000-0000-4000-8000-000000000000/plate.png"));
    config->set_key_value("bed_custom_model", new ConfigOptionString(make_printer_asset_ref(plugin_key, "bed.stl")));
    update_printer_asset_plugins(*config);
    CHECK(config->option<ConfigOptionStrings>("printer_asset_plugins")->values == std::vector<std::string>{"Voron Bed"});

    // The printer keeps its manifest apart from the process preset's "plugins", which the full print
    // config would otherwise overwrite with it.
    const std::vector<std::string> expected_manifest{plugin_key + ";;Voron Bed"};
    CHECK(config->option("plugins") == nullptr);
    CHECK(config->option<ConfigOptionStrings>("printer_plugins")->values == expected_manifest);
    CHECK(config->plugin_manifest_key() == "printer_plugins");

    Preset printer(Preset::TYPE_PRINTER, "Voron with bed pack");
    printer.config = *config;
    const auto refresh = [&printer]() {
        refresh_missing_plugins(Preset::TYPE_PRINTER, printer.config.option<ConfigOptionStrings>("printer_plugins"), &printer);
    };

    refresh();
    CHECK_FALSE(has_inactive_plugins());

    // Turned off: offered for activation, never blocking slicing. The installed pack still names itself
    // when the manifest has lost its entry.
    manager.set_capability_enabled({PluginCapabilityType::ResourcePrinterAsset, "Voron Bed", plugin_key}, false);
    refresh();
    CHECK(has_inactive_plugins());
    CHECK_FALSE(has_blocking_plugins());
    {
        DynamicPrintConfig stripped = *config;
        stripped.option<ConfigOptionStrings>("printer_plugins")->values = {};
        update_printer_asset_plugins(stripped);
        CHECK(stripped.option<ConfigOptionStrings>("printer_asset_plugins")->values == std::vector<std::string>{"Voron Bed"});
        CHECK(stripped.option<ConfigOptionStrings>("printer_plugins")->values == expected_manifest);
    }

    // Unloaded, as on a machine where the pack is not installed yet: saving the preset there keeps the
    // pack's name and the manifest entry other machines need.
    REQUIRE(manager.unload_plugin(plugin_key));
    update_printer_asset_plugins(*config);
    CHECK(config->option<ConfigOptionStrings>("printer_asset_plugins")->values == std::vector<std::string>{"Voron Bed"});
    CHECK(config->option<ConfigOptionStrings>("printer_plugins")->values == expected_manifest);
    printer.config = *config;
    refresh();
    CHECK(has_inactive_plugins());
    CHECK_FALSE(has_blocking_plugins());

    refresh_missing_plugins(Preset::TYPE_PRINTER, nullptr);
    CHECK_FALSE(has_inactive_plugins());
}

TEST_CASE("A pack whose get_asset_dir raises or leaves the plugin folder provides no files",
          "[PluginPrinterAsset][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir      data_dir_guard("asset-hostile");
    ScopedTemporaryDir elsewhere("asset-elsewhere");
    write_text_file(elsewhere.path() / "bed.stl");

    write_plugin(data_dir_guard, "Raising_Bed", make_printer_asset_source("Raising Bed", "raise RuntimeError(\"no assets today\")"));
    write_plugin(data_dir_guard, "Outside_Bed",
                 make_printer_asset_source("Outside Bed", "return r\"" + elsewhere.path().string() + "\""));

    PluginManager& manager = PluginManager::instance();
    install_printer_asset_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    CHECK(load_and_wait(manager, "Raising_Bed", error));
    CHECK(load_and_wait(manager, "Outside_Bed", error));
    CHECK(PluginPrinterAssets::instance().entries().empty());
    CHECK(resolve_printer_asset_value(make_printer_asset_ref("Outside_Bed", "bed.stl")).empty());
}
