// ResourcePack plugin type: file validation per kind, the host registry's lifecycle, resolving the
// "orcacloud-asset/<plugin id>/<file>" references printer presets store in bed_custom_model /
// bed_custom_texture, and the printer preset manifest that lets PluginResolver offer a missing pack
// without blocking slicing.
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
#include <string_view>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/PrinterAssetRef.hpp"

#include "slic3r/GUI/TextureLibrary.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginResolver.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include "slic3r/plugin/ResourcePackKinds.hpp"
#include "slic3r/plugin/host/PluginResourcePacks.hpp"

#include "plugin_test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Declare FIRST in a test so it is destroyed last (see test_plugin_resource_texture.cpp).
struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"resource-pack-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

void install_resource_pack_wiring()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    PluginManager& manager = PluginManager::instance();
    manager.subscribe_on_load_callback([](const std::string& plugin_key) {
        PluginResourcePacks::instance().on_plugin_register(plugin_key);
        GUI::bump_texture_library_generation();
    });
    manager.subscribe_on_unload_callback([](const std::string& plugin_key) {
        PluginResourcePacks::instance().on_plugin_deregister(plugin_key);
        GUI::bump_texture_library_generation();
    });
    manager.subscribe_on_capability_load_callback([](const PluginCapabilityId& capability) {
        if (capability.type != PluginCapabilityType::ResourcePack && capability.type != PluginCapabilityType::ResourceTexture)
            return;
        PluginResourcePacks::instance().on_cap_register(capability);
        GUI::bump_texture_library_generation();
    });
    manager.subscribe_on_capability_unload_callback([](const PluginCapabilityId& capability) {
        if (capability.type != PluginCapabilityType::ResourcePack && capability.type != PluginCapabilityType::ResourceTexture)
            return;
        PluginResourcePacks::instance().on_cap_deregister(capability);
        GUI::bump_texture_library_generation();
    });
}

std::string default_resource_dir_body() { return "return os.path.join(os.path.dirname(__file__), \"a\")"; }

std::string default_resource_kinds_body() { return "return {\"bed.stl\": [\"bed_model\"], \"plate.svg\": [\"bed_texture\"]}"; }

// A minimal resource pack shaped like the wheel Orca Cloud builds (pa/__init__.py + pa/a/<file>).
// The body arguments are single Python return statements.
std::string make_resource_pack_source(const std::string& display_name,
                                      const std::string& get_resource_dir_body,
                                      const std::string& get_resource_kinds_body = default_resource_kinds_body())
{
    return std::string{}
        + "# /// script\n"
        + "# requires-python = \">=3.12\"\n"
        + "# dependencies = []\n"
        + "#\n"
        + "# [tool.orcaslicer.plugin]\n"
        + "# name = \"" + display_name + "\"\n"
        + "# description = \"Resource pack fixture.\"\n"
        + "# author = \"tests\"\n"
        + "# version = \"1.0.0\"\n"
        + "# type = \"resource-pack\"\n"
        + "# ///\n"
        + "import os\n"
        + "import orca\n"
        + "\n\n"
        + "class PackResources(orca.resource_pack.ResourcePackPluginCapabilityBase):\n"
        + "    def get_name(self):\n"
        + "        return \"" + display_name + "\"\n"
        + "\n"
        + "    def get_resource_dir(self):\n"
        + "        " + get_resource_dir_body + "\n"
        + "\n"
        + "    def get_resource_kinds(self):\n"
        + "        " + get_resource_kinds_body + "\n"
        + "\n\n"
        + "@orca.plugin\n"
        + "class ResourcePackPlugin(orca.base):\n"
        + "    def register_capabilities(self):\n"
        + "        orca.register_capability(PackResources)\n";
}

// A resource-texture plugin whose capability is named `display_name`, for the same-name case.
std::string make_texture_pack_source(const std::string& display_name)
{
    return std::string{}
        + "# /// script\n"
        + "# requires-python = \">=3.12\"\n"
        + "# dependencies = []\n"
        + "#\n"
        + "# [tool.orcaslicer.plugin]\n"
        + "# name = \"" + display_name + " Package\"\n"
        + "# description = \"Resource texture fixture.\"\n"
        + "# author = \"tests\"\n"
        + "# version = \"1.0.0\"\n"
        + "# type = \"resource-texture\"\n"
        + "# ///\n"
        + "import os\n"
        + "import orca\n"
        + "\n\n"
        + "class PackTextures(orca.resource_texture.ResourceTexturePluginCapabilityBase):\n"
        + "    def get_name(self):\n"
        + "        return \"" + display_name + "\"\n"
        + "\n"
        + "    def get_texture_dir(self):\n"
        + "        return os.path.join(os.path.dirname(__file__), \"textures\")\n"
        + "\n\n"
        + "@orca.plugin\n"
        + "class TexturePack(orca.base):\n"
        + "    def register_capabilities(self):\n"
        + "        orca.register_capability(PackTextures)\n";
}

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

// The validated paths listed for `kind`, empty when the kind has none.
std::vector<std::string> kind_paths(const ResourceFilesByKind& files, std::string_view kind)
{
    const auto it = files.find(std::string(kind));
    return it == files.end() ? std::vector<std::string>{} : it->second;
}

bool has_any_file(const ResourceFilesByKind& files)
{
    return std::any_of(files.begin(), files.end(), [](const auto& kind) { return !kind.second.empty(); });
}

std::string two_digits(int i) { return (i < 10 ? "0" : "") + std::to_string(i); }

// Loads one local pack "Voron Bed" (plugin key Voron_Bed) with a model, a texture and a file its
// kinds do not declare.
std::string load_voron_pack(const ScopedDataDir& data_dir_guard)
{
    const std::string plugin_key = "Voron_Bed";
    const fs::path    plugin_dir = write_plugin(data_dir_guard, plugin_key, make_resource_pack_source("Voron Bed", default_resource_dir_body()));
    write_text_file(plugin_dir / "a" / "bed.stl");
    write_text_file(plugin_dir / "a" / "plate.svg");
    write_text_file(plugin_dir / "a" / "readme.txt");

    PluginManager& manager = PluginManager::instance();
    install_resource_pack_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    const bool  loaded = load_and_wait(manager, plugin_key, error);
    INFO("load error: " << error);
    REQUIRE(loaded);
    return plugin_key;
}

} // namespace

// ── Validation (collect_resource_pack_files, no Python) ─────────────────────────────────────────

TEST_CASE("A file is listed under each kind it is declared for", "[PluginResourcePack]")
{
    ScopedTemporaryDir root("pack-kinds");
    write_text_file(root.path() / "a" / "bed.stl");
    write_text_file(root.path() / "a" / "plate.PNG");
    write_text_file(root.path() / "a" / "plate.svg");
    write_text_file(root.path() / "a" / "photo.jpg");
    write_text_file(root.path() / "a" / "nested" / "deep.stl");

    const ResourceKindMap kinds{
        {"bed.stl", {"bed_model"}},
        {"plate.PNG", {"bed_texture", "displacement_texture"}},
        {"plate.svg", {"bed_texture"}},
        {"photo.jpg", {"displacement_texture"}},
        {"deep.stl", {"bed_model"}}, // in a subdirectory: not listed
    };

    std::string        error;
    const ResourceFilesByKind files = collect_resource_pack_files((root.path() / "a").string(), root.string(), kinds, error);

    CHECK(error.empty());
    CHECK(filenames(kind_paths(files, RESOURCE_KIND_BED_MODEL)) == std::vector<std::string>{"bed.stl"});
    CHECK(filenames(kind_paths(files, RESOURCE_KIND_BED_TEXTURE)) == std::vector<std::string>{"plate.PNG", "plate.svg"});
    CHECK(filenames(kind_paths(files, RESOURCE_KIND_DISPLACEMENT_TEXTURE)) == std::vector<std::string>{"photo.jpg", "plate.PNG"});
}

TEST_CASE("A file is skipped for a kind whose extensions it does not have, and kept for the others", "[PluginResourcePack]")
{
    ScopedTemporaryDir root("pack-extensions");
    write_text_file(root.path() / "a.png");
    write_text_file(root.path() / "b.stl");

    const ResourceKindMap kinds{
        {"a.png", {"bed_model", "displacement_texture"}},
        {"b.stl", {"bed_model", "bed_texture"}},
    };

    std::string        error;
    const ResourceFilesByKind files = collect_resource_pack_files(root.string(), root.string(), kinds, error);

    CHECK(error.empty());
    CHECK(filenames(kind_paths(files, RESOURCE_KIND_BED_MODEL)) == std::vector<std::string>{"b.stl"});
    CHECK(kind_paths(files, RESOURCE_KIND_BED_TEXTURE).empty());
    CHECK(filenames(kind_paths(files, RESOURCE_KIND_DISPLACEMENT_TEXTURE)) == std::vector<std::string>{"a.png"});
}

TEST_CASE("A file missing from the kinds, and a declared file that is not on disk, are ignored", "[PluginResourcePack]")
{
    ScopedTemporaryDir root("pack-undeclared");
    write_text_file(root.path() / "bed.stl");
    write_text_file(root.path() / "plate.png");

    const ResourceKindMap kinds{
        {"bed.stl", {"bed_model"}},
        {"ghost.stl", {"bed_model"}},
    };

    std::string        error;
    const ResourceFilesByKind files = collect_resource_pack_files(root.string(), root.string(), kinds, error);

    CHECK(error.empty());
    CHECK(filenames(kind_paths(files, RESOURCE_KIND_BED_MODEL)) == std::vector<std::string>{"bed.stl"});
    CHECK(kind_paths(files, RESOURCE_KIND_BED_TEXTURE).empty());
    CHECK(kind_paths(files, RESOURCE_KIND_DISPLACEMENT_TEXTURE).empty());
}

TEST_CASE("An unknown kind is skipped", "[PluginResourcePack]")
{
    ScopedTemporaryDir root("pack-unknown-kind");
    write_text_file(root.path() / "bed.stl");

    const ResourceKindMap kinds{{"bed.stl", {"bed_model", "mystery"}}};

    std::string        error;
    const ResourceFilesByKind files = collect_resource_pack_files(root.string(), root.string(), kinds, error);

    CHECK(error.empty());
    CHECK(files.count("mystery") == 0);
    CHECK(filenames(kind_paths(files, RESOURCE_KIND_BED_MODEL)) == std::vector<std::string>{"bed.stl"});
}

TEST_CASE("An asset folder outside the plugin folder is refused", "[PluginResourcePack]")
{
    ScopedTemporaryDir parent("pack-escape");
    const fs::path     root    = parent.path() / "pack";
    const fs::path     outside = parent.path() / "outside";
    fs::create_directories(root);
    write_text_file(outside / "bed.stl");

    const ResourceKindMap kinds{{"bed.stl", {"bed_model"}}};

    std::string error;
    CHECK(collect_resource_pack_files((root / ".." / "outside").string(), root.string(), kinds, error).empty());
    CHECK_FALSE(error.empty());
    CHECK(collect_resource_pack_files("", root.string(), kinds, error).empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Each kind's listing is sorted and stops at its own cap", "[PluginResourcePack]")
{
    ScopedTemporaryDir root("pack-cap");
    ResourceKindMap    kinds;
    for (int i = 0; i < 20; ++i) {
        const std::string n = two_digits(i);
        write_text_file(root.path() / ("bed_" + n + ".stl"));
        kinds["bed_" + n + ".stl"] = {"bed_model"};
        write_text_file(root.path() / ("plate_" + n + ".png"));
        kinds["plate_" + n + ".png"] = {"bed_texture"};
    }
    // Displacement textures count against their own cap, not the bed kinds'.
    for (int i = 0; i < 70; ++i) {
        const std::string n = two_digits(i);
        write_text_file(root.path() / ("photo_" + n + ".jpg"));
        kinds["photo_" + n + ".jpg"] = {"displacement_texture"};
    }

    std::string        error;
    const ResourceFilesByKind files = collect_resource_pack_files(root.string(), root.string(), kinds, error);
    CHECK(error.empty());

    const std::vector<std::string> models = kind_paths(files, RESOURCE_KIND_BED_MODEL);
    REQUIRE(models.size() == 16);
    CHECK(std::is_sorted(models.begin(), models.end()));
    CHECK(fs::path(models.back()).filename() == "bed_15.stl");

    const std::vector<std::string> textures = kind_paths(files, RESOURCE_KIND_BED_TEXTURE);
    REQUIRE(textures.size() == 16);
    CHECK(std::is_sorted(textures.begin(), textures.end()));
    CHECK(fs::path(textures.back()).filename() == "plate_15.png");

    const std::vector<std::string> displacements = kind_paths(files, RESOURCE_KIND_DISPLACEMENT_TEXTURE);
    REQUIRE(displacements.size() == 64);
    CHECK(std::is_sorted(displacements.begin(), displacements.end()));
    CHECK(fs::path(displacements.back()).filename() == "photo_63.jpg");
}

TEST_CASE("Plain bed paths resolve to themselves and packs that are not loaded to nothing", "[PluginResourcePack]")
{
    const std::string missing_ref = "orcacloud-asset/3f2a6c1e-8d4b-4c2a-9e7f-1a2b3c4d5e6f/bed.stl";

    CHECK(resolve_printer_asset_value("", RESOURCE_KIND_BED_MODEL) == "");
    CHECK(resolve_printer_asset_value("/Users/me/bed.stl", RESOURCE_KIND_BED_MODEL) == "/Users/me/bed.stl");
    CHECK(resolve_printer_asset_value(missing_ref, RESOURCE_KIND_BED_MODEL).empty());
}

TEST_CASE("A printer's asset pack names follow its bed files, falling back to its manifest for packs not loaded here",
          "[PluginResourcePack]")
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

TEST_CASE("A pack's files resolve per kind from references and stop resolving on disable and unload",
          "[PluginResourcePack][PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir     data_dir_guard("resource-pack-lifecycle");
    const std::string plugin_key = load_voron_pack(data_dir_guard);
    PluginManager&    manager    = PluginManager::instance();

    const std::vector<ResourcePackEntry> packs = PluginResourcePacks::instance().entries();
    REQUIRE(packs.size() == 1);
    CHECK(packs.front().display_name == "Voron Bed");
    // A local pack has no cloud uuid, so references use its plugin key.
    CHECK(packs.front().pack_id == plugin_key);
    CHECK(filenames(kind_paths(packs.front().files, RESOURCE_KIND_BED_MODEL)) == std::vector<std::string>{"bed.stl"});
    CHECK(filenames(kind_paths(packs.front().files, RESOURCE_KIND_BED_TEXTURE)) == std::vector<std::string>{"plate.svg"});
    CHECK(kind_paths(packs.front().files, RESOURCE_KIND_DISPLACEMENT_TEXTURE).empty());

    const std::string model_ref = make_printer_asset_ref(plugin_key, "bed.stl");
    const std::string model     = resolve_printer_asset_value(model_ref, RESOURCE_KIND_BED_MODEL);
    CHECK(fs::path(model).filename() == "bed.stl");
    CHECK(fs::exists(model));
    // Only files declared for the requested kind resolve; the reference can never leave the asset folder.
    CHECK(resolve_printer_asset_value(model_ref, RESOURCE_KIND_BED_TEXTURE).empty());
    CHECK(resolve_printer_asset_value(make_printer_asset_ref(plugin_key, "readme.txt"), RESOURCE_KIND_BED_MODEL).empty());

    manager.set_capability_enabled({PluginCapabilityType::ResourcePack, "Voron Bed", plugin_key}, false);
    CHECK(PluginResourcePacks::instance().entries().empty());
    CHECK(resolve_printer_asset_value(model_ref, RESOURCE_KIND_BED_MODEL).empty());

    manager.set_capability_enabled({PluginCapabilityType::ResourcePack, "Voron Bed", plugin_key}, true);
    CHECK(resolve_printer_asset_value(model_ref, RESOURCE_KIND_BED_MODEL) == model);

    REQUIRE(manager.unload_plugin(plugin_key));
    CHECK(PluginResourcePacks::instance().entries().empty());
}

TEST_CASE("A file declared for several kinds resolves for each of them, and declarations that do not match drop out",
          "[PluginResourcePack][PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir     data_dir_guard("resource-pack-kinds");
    const std::string plugin_key = "Mixed_Pack";
    const fs::path    plugin_dir = write_plugin(
        data_dir_guard, plugin_key,
        make_resource_pack_source("Mixed Pack", default_resource_dir_body(),
                                  "return {\"plate.png\": [\"bed_texture\", \"displacement_texture\"], \"bed.stl\": [\"bed_model\", \"bed_texture\"], "
                                  "\"ghost.stl\": [\"bed_model\"], \"plate.svg\": [\"mystery\"]}"));
    write_text_file(plugin_dir / "a" / "plate.png");
    write_text_file(plugin_dir / "a" / "bed.stl");
    write_text_file(plugin_dir / "a" / "plate.svg");
    write_text_file(plugin_dir / "a" / "notes.svg");

    PluginManager& manager = PluginManager::instance();
    install_resource_pack_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, plugin_key, error));

    const std::vector<ResourcePackEntry> packs = PluginResourcePacks::instance().entries();
    REQUIRE(packs.size() == 1);
    CHECK(filenames(kind_paths(packs.front().files, RESOURCE_KIND_BED_MODEL)) == std::vector<std::string>{"bed.stl"});
    CHECK(filenames(kind_paths(packs.front().files, RESOURCE_KIND_BED_TEXTURE)) == std::vector<std::string>{"plate.png"});
    CHECK(filenames(kind_paths(packs.front().files, RESOURCE_KIND_DISPLACEMENT_TEXTURE)) == std::vector<std::string>{"plate.png"});

    CHECK_FALSE(resolve_printer_asset_value(make_printer_asset_ref(plugin_key, "plate.png"), RESOURCE_KIND_DISPLACEMENT_TEXTURE).empty());
    CHECK(resolve_printer_asset_value(make_printer_asset_ref(plugin_key, "plate.png"), RESOURCE_KIND_BED_MODEL).empty());
    CHECK(resolve_printer_asset_value(make_printer_asset_ref(plugin_key, "bed.stl"), RESOURCE_KIND_BED_TEXTURE).empty());
    CHECK(resolve_printer_asset_value(make_printer_asset_ref(plugin_key, "plate.svg"), RESOURCE_KIND_BED_TEXTURE).empty());
    CHECK(resolve_printer_asset_value(make_printer_asset_ref(plugin_key, "notes.svg"), RESOURCE_KIND_BED_TEXTURE).empty());
    CHECK(PluginResourcePacks::instance().resolve(PrinterAssetRef{plugin_key, "bed.stl"}, RESOURCE_KIND_BED_MODEL)
          == resolve_printer_asset_value(make_printer_asset_ref(plugin_key, "bed.stl"), RESOURCE_KIND_BED_MODEL));

    manager.unload_plugin(plugin_key);
}

TEST_CASE("A printer preset names its pack in its own manifest, and an inactive pack prompts without blocking slicing",
          "[PluginResourcePack][PluginResolver][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir     data_dir_guard("resource-pack-manifest");
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
    manager.set_capability_enabled({PluginCapabilityType::ResourcePack, "Voron Bed", plugin_key}, false);
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

// ── Hostile overrides ───────────────────────────────────────────────────────────────────────────

TEST_CASE("A pack whose get_resource_dir raises or leaves the plugin folder provides no files",
          "[PluginResourcePack][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir      data_dir_guard("resource-pack-hostile");
    ScopedTemporaryDir elsewhere("resource-pack-elsewhere");
    write_text_file(elsewhere.path() / "bed.stl");

    write_plugin(data_dir_guard, "Raising_Bed", make_resource_pack_source("Raising Bed", "raise RuntimeError(\"no assets today\")"));
    write_plugin(data_dir_guard, "Outside_Bed",
                 make_resource_pack_source("Outside Bed", "return r\"" + elsewhere.path().string() + "\""));

    PluginManager& manager = PluginManager::instance();
    install_resource_pack_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    CHECK(load_and_wait(manager, "Raising_Bed", error));
    CHECK(load_and_wait(manager, "Outside_Bed", error));
    CHECK(PluginResourcePacks::instance().entries().empty());
    CHECK(resolve_printer_asset_value(make_printer_asset_ref("Outside_Bed", "bed.stl"), RESOURCE_KIND_BED_MODEL).empty());
}

TEST_CASE("A pack whose get_resource_kinds raises or does not return a dict provides no files",
          "[PluginResourcePack][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("resource-pack-bad-kinds");

    const fs::path raising_dir = write_plugin(data_dir_guard, "Raising_Kinds",
                                              make_resource_pack_source("Raising Kinds", default_resource_dir_body(),
                                                                        "raise RuntimeError(\"no kinds today\")"));
    write_text_file(raising_dir / "a" / "bed.stl");
    const fs::path list_dir = write_plugin(data_dir_guard, "List_Kinds",
                                           make_resource_pack_source("List Kinds", default_resource_dir_body(), "return [\"bed.stl\"]"));
    write_text_file(list_dir / "a" / "bed.stl");

    PluginManager& manager = PluginManager::instance();
    install_resource_pack_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "Raising_Kinds", error));
    REQUIRE(load_and_wait(manager, "List_Kinds", error));
    CHECK(manager.is_plugin_loaded("Raising_Kinds"));
    CHECK(manager.is_plugin_loaded("List_Kinds"));

    for (const ResourcePackEntry& entry : PluginResourcePacks::instance().entries())
        CHECK_FALSE(has_any_file(entry.files));
    CHECK(resolve_printer_asset_value(make_printer_asset_ref("Raising_Kinds", "bed.stl"), RESOURCE_KIND_BED_MODEL).empty());
    CHECK(resolve_printer_asset_value(make_printer_asset_ref("List_Kinds", "bed.stl"), RESOURCE_KIND_BED_MODEL).empty());

    manager.unload_plugin("Raising_Kinds");
    manager.unload_plugin("List_Kinds");
}

TEST_CASE("A pack's displacement textures reach the texture library under their own section key",
          "[PluginResourcePack][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("resource-pack-textures");

    const std::string kinds = "return {\"wood.png\": [\"displacement_texture\", \"bed_texture\"], \"plate.svg\": [\"bed_texture\"],"
                              " \"bed.stl\": [\"bed_model\"]}";
    const fs::path    pack_dir = write_plugin(data_dir_guard, "Shared_Pack",
                                              make_resource_pack_source("Shared Name", default_resource_dir_body(), kinds));
    const std::vector<uint8_t> pixels(16, 0x40);
    fs::create_directories(pack_dir / "a");
    REQUIRE(png::write_gray_to_file((pack_dir / "a" / "wood.png").string(), 4, 4, pixels));
    write_text_file(pack_dir / "a" / "plate.svg");
    write_text_file(pack_dir / "a" / "bed.stl");

    // A resource-texture pack with the same capability name: names are unique per type only.
    const fs::path texture_dir = write_plugin(data_dir_guard, "Shared_Textures", make_texture_pack_source("Shared Name"));
    fs::create_directories(texture_dir / "textures");
    REQUIRE(png::write_gray_to_file((texture_dir / "textures" / "stone.png").string(), 4, 4, pixels));

    PluginManager& manager = PluginManager::instance();
    install_resource_pack_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "Shared_Pack", error));
    REQUIRE(load_and_wait(manager, "Shared_Textures", error));

    std::vector<GUI::TextureLibraryEntry> plugin_entries;
    for (const GUI::TextureLibraryEntry& entry : GUI::texture_library(true))
        if (entry.source == GUI::TextureSource::Plugin)
            plugin_entries.push_back(entry);

    // wood.png is the pack's only displacement texture; plate.svg and bed.stl are bed files.
    REQUIRE(plugin_entries.size() == 2);
    for (const GUI::TextureLibraryEntry& entry : plugin_entries) {
        CHECK(entry.pack_name == "Shared Name");
        if (entry.name == "wood")
            CHECK(entry.pack_key == "pack:Shared Name");
        else
            CHECK(entry.pack_key == "texture:Shared Name");
    }
    CHECK(plugin_entries[0].pack_key != plugin_entries[1].pack_key);

    manager.unload_plugin("Shared_Pack");
    std::vector<GUI::TextureLibraryEntry> after;
    for (const GUI::TextureLibraryEntry& entry : GUI::texture_library(false))
        if (entry.source == GUI::TextureSource::Plugin)
            after.push_back(entry);
    REQUIRE(after.size() == 1);
    CHECK(after.front().pack_key == "texture:Shared Name");

    manager.unload_plugin("Shared_Textures");
}

TEST_CASE("A resource-texture pack's images sit under displacement_texture and it is never a bed asset pack",
          "[PluginResourcePack][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("resource-pack-texture-only");

    const std::string plugin_key = "Stone_Textures";
    const fs::path    plugin_dir = write_plugin(data_dir_guard, plugin_key, make_texture_pack_source("Stone Textures"));
    const std::vector<uint8_t> pixels(16, 0x40);
    fs::create_directories(plugin_dir / "textures");
    REQUIRE(png::write_gray_to_file((plugin_dir / "textures" / "stone.png").string(), 4, 4, pixels));

    PluginManager& manager = PluginManager::instance();
    install_resource_pack_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, plugin_key, error));

    const std::vector<ResourcePackEntry> packs = PluginResourcePacks::instance().entries();
    REQUIRE(packs.size() == 1);
    CHECK(packs.front().id.type == PluginCapabilityType::ResourceTexture);
    CHECK(filenames(kind_paths(packs.front().files, RESOURCE_KIND_DISPLACEMENT_TEXTURE)) == std::vector<std::string>{"stone.png"});
    CHECK(kind_paths(packs.front().files, RESOURCE_KIND_BED_MODEL).empty());
    CHECK(kind_paths(packs.front().files, RESOURCE_KIND_BED_TEXTURE).empty());

    const PrinterAssetRef ref{plugin_key, "stone.png"};
    CHECK(PluginResourcePacks::instance().capability_name(ref).empty());
    CHECK(PluginResourcePacks::instance().resolve(ref, RESOURCE_KIND_BED_MODEL).empty());
    CHECK(PluginResourcePacks::instance().resolve(ref, RESOURCE_KIND_BED_TEXTURE).empty());
    CHECK(resolve_printer_asset_value(make_printer_asset_ref(plugin_key, "stone.png"), RESOURCE_KIND_BED_TEXTURE).empty());

    manager.unload_plugin(plugin_key);
}
