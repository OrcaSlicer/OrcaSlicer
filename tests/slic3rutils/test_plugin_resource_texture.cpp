// ResourceTexture plugin type: directory validation, the host registry's lifecycle, the texture
// listing's plugin section, hostile get_texture_dir() overrides, decode parity and the .3mf round
// trip without the pack installed.
//
// The GUI wiring lives in GUI_App::init_plugin_gui_wiring; the test binary has no app, so the same
// registry callbacks are installed here. As in test_plugin_lifecycle.cpp, each Catch2 test case runs
// in its own process (catch_discover_tests), so the PluginManager and interpreter come up at most once
// per test.
//
// Same Windows include prologue as test_texture_color_palette.cpp: wx pulls in <windows.h>, so keep
// WIN32_LEAN_AND_MEAN / NOMINMAX ahead of the Catch2 headers.
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
#include <boost/system/error_code.hpp>
#include <catch2/catch_all.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <wx/image.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Zipper.hpp"

#include "slic3r/GUI/TextureLibrary.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include "slic3r/plugin/host/PluginResourceTextures.hpp"

#include "plugin_test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Brings the plugin system up for the duration of one test and tears the manager down at the end,
// as test_plugin_lifecycle.cpp does. The interpreter is process-wide and finalized once by
// PythonInterpreterRunEnd in slic3rutils_tests_main.cpp; CPython cannot be reinitialized after
// Py_Finalize. Declare this FIRST in a test so it is destroyed last.
struct ScopedPluginManager
{
    // Before initialize(): the interpreter creates {data_dir}/python/packages and {data_dir}/log.
    ScopedDataDir python_data_dir{"texture-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

// The app installs these in GUI_App::init_plugin_gui_wiring. Without an app the callbacks run inline
// on the load worker, which is what lets the tests observe the registry right after the wait.
void install_resource_texture_wiring()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    PluginManager& manager = PluginManager::instance();
    manager.subscribe_on_load_callback([](const std::string& plugin_key) {
        PluginResourceTextures::instance().on_plugin_register(plugin_key);
        GUI::bump_texture_library_generation();
    });
    manager.subscribe_on_unload_callback([](const std::string& plugin_key) {
        PluginResourceTextures::instance().on_plugin_deregister(plugin_key);
        GUI::bump_texture_library_generation();
    });
    manager.subscribe_on_capability_load_callback([](const PluginCapabilityId& capability) {
        if (capability.type != PluginCapabilityType::ResourceTexture)
            return;
        PluginResourceTextures::instance().on_cap_register(capability);
        GUI::bump_texture_library_generation();
    });
    manager.subscribe_on_capability_unload_callback([](const PluginCapabilityId& capability) {
        if (capability.type != PluginCapabilityType::ResourceTexture)
            return;
        PluginResourceTextures::instance().on_cap_deregister(capability);
        GUI::bump_texture_library_generation();
    });
}

// A minimal resource-texture plugin. `display_name` is the PEP723 package name and, unless
// `capability_name` is given, the capability's get_name() too; pass `capability_name` to keep the
// package name and the capability name independent.
std::string make_resource_texture_source(const std::string& display_name,
                                         const std::string& get_texture_dir_body,
                                         const std::string& capability_name = "")
{
    const std::string cap_name = capability_name.empty() ? display_name : capability_name;
    return std::string{}
        + "# /// script\n"
        + "# requires-python = \">=3.12\"\n"
        + "# dependencies = []\n"
        + "#\n"
        + "# [tool.orcaslicer.plugin]\n"
        + "# name = \"" + display_name + "\"\n"
        + "# description = \"Resource texture pack fixture.\"\n"
        + "# author = \"tests\"\n"
        + "# version = \"1.0.0\"\n"
        + "# type = \"resource-texture\"\n"
        + "# ///\n"
        + "import os\n"
        + "import orca\n"
        + "\n\n"
        + "class PackTextures(orca.resource_texture.ResourceTexturePluginCapabilityBase):\n"
        + "    def get_name(self):\n"
        + "        return \"" + cap_name + "\"\n"
        + "\n"
        + "    def get_texture_dir(self):\n"
        + "        " + get_texture_dir_body + "\n"
        + "\n\n"
        + "@orca.plugin\n"
        + "class TexturePack(orca.base):\n"
        + "    def register_capabilities(self):\n"
        + "        orca.register_capability(PackTextures)\n";
}

std::string default_texture_dir_body()
{
    return "return os.path.join(os.path.dirname(__file__), \"textures\")";
}

// Writes {data_dir}/orca_plugins/<stem>/<stem>.py and returns the plugin directory.
fs::path write_plugin(const ScopedDataDir& data_dir_guard, const std::string& stem, const std::string& source)
{
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / stem;
    fs::create_directories(plugin_dir);

    std::ofstream out((plugin_dir / (stem + ".py")).string(), std::ios::binary);
    out << source;
    out.close();

    return plugin_dir;
}

bool load_and_wait(PluginManager& manager, const std::string& plugin_key, std::string& error)
{
    manager.load_plugin(plugin_key, /*skip_deps=*/true);
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(120), error);
}

// The Plugin-sourced slice of the texture listing, which is exactly what the picker's third heading
// draws. Pass force to call texture_library(true); without it the listing is reused unless a pack
// callback moved the generation.
std::vector<GUI::TextureLibraryEntry> plugin_textures(bool force)
{
    std::vector<GUI::TextureLibraryEntry> found;
    for (const GUI::TextureLibraryEntry& entry : GUI::texture_library(force))
        if (entry.source == GUI::TextureSource::Plugin)
            found.push_back(entry);
    return found;
}

void write_text_file(const fs::path& path, const std::string& content = "x")
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::binary);
    out << content;
}

std::vector<unsigned char> read_file_bytes(const fs::path& path)
{
    std::ifstream ifs(path.string(), std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

void write_bytes(const fs::path& path, const std::vector<unsigned char>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

void write_gray_png(const fs::path& path, uint8_t value, size_t w = 4, size_t h = 4)
{
    fs::create_directories(path.parent_path());
    const std::vector<uint8_t> pixels(w * h, value);
    REQUIRE(Slic3r::png::write_gray_to_file(path.string(), w, h, pixels));
}

// A 24-bit gray BMP (R == G == B), synthesised without an image toolkit. Rows are bottom-up and
// padded to four bytes; the image is uniform, so the orientation does not matter.
std::vector<unsigned char> make_gray_bmp(int width, int height, uint8_t value)
{
    const int      row_bytes   = ((width * 3 + 3) / 4) * 4;
    const uint32_t pixel_bytes = uint32_t(row_bytes) * uint32_t(height);
    const uint32_t file_size   = 14 + 40 + pixel_bytes;

    std::vector<unsigned char> bmp(file_size, 0);
    const auto put16 = [&bmp](size_t at, uint16_t v) {
        bmp[at] = uint8_t(v);
        bmp[at + 1] = uint8_t(v >> 8);
    };
    const auto put32 = [&bmp](size_t at, uint32_t v) {
        bmp[at] = uint8_t(v);
        bmp[at + 1] = uint8_t(v >> 8);
        bmp[at + 2] = uint8_t(v >> 16);
        bmp[at + 3] = uint8_t(v >> 24);
    };

    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, file_size);
    put32(10, 54); // pixel data offset
    put32(14, 40); // BITMAPINFOHEADER size
    put32(18, uint32_t(width));
    put32(22, uint32_t(height));
    put16(26, 1); // planes
    put16(28, 24); // bits per pixel
    put32(34, pixel_bytes);

    for (int y = 0; y < height; ++y) {
        unsigned char* row = bmp.data() + 54 + size_t(y) * size_t(row_bytes);
        for (int x = 0; x < width; ++x) {
            row[x * 3]     = value;
            row[x * 3 + 1] = value;
            row[x * 3 + 2] = value;
        }
    }
    return bmp;
}

// The app calls wxInitAllImageHandlers() during startup and, before that, wxEntryStart() runs the
// wxImageModule, which registers the standard handlers. wxInitAllImageHandlers() adds PNG, JPEG and
// the rest but NOT the BMP handler, so a test binary without a wxApp must add the standard set too or
// a .bmp texture cannot be loaded here at all.
void ensure_wx_image_handlers()
{
    static bool initialized = false;
    if (initialized)
        return;
    initialized = true;
    wxImage::InitStandardHandlers();
    wxInitAllImageHandlers();
}

// Stores `model` as a one-plate project, mirroring tests/libslic3r/test_3mf.cpp.
void store_project(Model& model, const std::string& path)
{
    ScopedTemporaryDir backup_dir("orca_tex_pack_src");
    model.set_backup_path(backup_dir.string());

    DynamicPrintConfig cfg;
    PlateData          plate;
    plate.plate_index = 0;
    StoreParams sp;
    sp.path     = path.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    sp.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(sp));
}

bool load_project(const std::string& path, Model& model, const ScopedTemporaryDir& backup_dir)
{
    model.set_backup_path(backup_dir.string());
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    PlateDataPtrs             plates;
    std::vector<Preset*>      project_presets;
    bool                      is_bbl_3mf = false;
    bool                      is_orca_3mf = false;
    Semver                    file_version;
    const bool                loaded = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates, &project_presets,
                                                    &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                                                    LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates);
    return loaded;
}

} // namespace

// ── Validation (collect_resource_texture_files, no Python) ──────────────────────────────────────

TEST_CASE("A texture file directly inside the plugin folder is listed", "[PluginResourceTexture]")
{
    ScopedTemporaryDir root("tex-root");
    write_text_file(root.path() / "textures" / "grain.png");

    std::string error;
    const std::vector<std::string> files =
        collect_resource_texture_files((root.path() / "textures").string(), root.string(), error);

    CHECK(error.empty());
    REQUIRE(files.size() == 1);
    CHECK(fs::path(files.front()).filename() == "grain.png");
}

TEST_CASE("A directory reached with a parent-directory escape is refused", "[PluginResourceTexture]")
{
    ScopedTemporaryDir parent("tex-escape");
    const fs::path     root    = parent.path() / "pack";
    const fs::path     outside = parent.path() / "outside";
    fs::create_directories(root);
    write_text_file(outside / "leak.png");

    std::string error;
    const std::vector<std::string> files =
        collect_resource_texture_files((root / ".." / "outside").string(), root.string(), error);

    CHECK(files.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("An absolute directory outside the plugin folder is refused", "[PluginResourceTexture]")
{
    ScopedTemporaryDir root("tex-abs-root");
    ScopedTemporaryDir elsewhere("tex-abs-elsewhere");
    write_text_file(elsewhere.path() / "leak.png");

    std::string error;
    const std::vector<std::string> files = collect_resource_texture_files(elsewhere.string(), root.string(), error);

    CHECK(files.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("A symlink pointing outside the plugin folder is skipped, not reported as an error", "[PluginResourceTexture]")
{
    ScopedTemporaryDir root("tex-symlink-root");
    ScopedTemporaryDir target("tex-symlink-target");
    write_text_file(target.path() / "secret.png");

    const fs::path link = root.path() / "textures" / "link.png";
    fs::create_directories(link.parent_path());
    boost::system::error_code ec;
    fs::create_symlink(target.path() / "secret.png", link, ec);
    if (ec)
        SKIP("symlinks unavailable on this filesystem: " + ec.message());

    std::string error;
    const std::vector<std::string> files =
        collect_resource_texture_files(link.parent_path().string(), root.string(), error);

    // The directory itself is legitimate; the escaping link is dropped, not an error.
    CHECK(error.empty());
    CHECK(files.empty());
}

TEST_CASE("Files in a subdirectory of the texture directory are not listed", "[PluginResourceTexture]")
{
    ScopedTemporaryDir root("tex-recursive");
    write_text_file(root.path() / "top.png");
    write_text_file(root.path() / "nested" / "deep.png");

    std::string error;
    const std::vector<std::string> files = collect_resource_texture_files(root.string(), root.string(), error);

    CHECK(error.empty());
    REQUIRE(files.size() == 1);
    CHECK(fs::path(files.front()).filename() == "top.png");
}

TEST_CASE("An entry that is not a regular file is skipped", "[PluginResourceTexture]")
{
    ScopedTemporaryDir root("tex-nonregular");
    write_text_file(root.path() / "real.png");
    fs::create_directories(root.path() / "fake.png"); // a directory named like an image

    std::string error;
    const std::vector<std::string> files = collect_resource_texture_files(root.string(), root.string(), error);

    CHECK(error.empty());
    REQUIRE(files.size() == 1);
    CHECK(fs::path(files.front()).filename() == "real.png");
}

TEST_CASE("Image extensions are matched case-insensitively and other extensions are ignored",
          "[PluginResourceTexture]")
{
    ScopedTemporaryDir root("tex-extensions");
    for (const char* name : { "a.png", "b.PNG", "c.jpg", "d.JPG", "e.jpeg", "f.JPEG", "g.bmp", "h.BMP" })
        write_text_file(root.path() / name);
    for (const char* name : { "i.txt", "j.gif", "k.pngx", "l.pn", "m.png.bak" })
        write_text_file(root.path() / name);

    std::string error;
    const std::vector<std::string> files = collect_resource_texture_files(root.string(), root.string(), error);

    CHECK(error.empty());
    CHECK(files.size() == 8);

    std::vector<std::string> names;
    for (const std::string& file : files)
        names.push_back(fs::path(file).filename().string());
    for (const char* accepted : { "a.png", "b.PNG", "c.jpg", "d.JPG", "e.jpeg", "f.JPEG", "g.bmp", "h.BMP" })
        CHECK(std::find(names.begin(), names.end(), accepted) != names.end());
    for (const char* rejected : { "i.txt", "j.gif", "k.pngx", "l.pn", "m.png.bak" })
        CHECK(std::find(names.begin(), names.end(), rejected) == names.end());
}

TEST_CASE("The listing stops at 64 image files", "[PluginResourceTexture]")
{
    ScopedTemporaryDir root("tex-cap");
    for (int i = 0; i < 70; ++i)
        write_text_file(root.path() / ("file" + std::to_string(i) + ".png"));

    std::string error;
    const std::vector<std::string> files = collect_resource_texture_files(root.string(), root.string(), error);

    CHECK(error.empty());
    CHECK(files.size() == 64);
}

TEST_CASE("A missing texture directory yields no files and an error", "[PluginResourceTexture]")
{
    ScopedTemporaryDir root("tex-missing");

    std::string error;
    const std::vector<std::string> files =
        collect_resource_texture_files((root.path() / "textures").string(), root.string(), error);

    CHECK(files.empty());
    CHECK_FALSE(error.empty());
}

// ── Lifecycle through the real listing ──────────────────────────────────────────────────────────

TEST_CASE("A resource-texture pack's images appear in the listing and clear on disable and unload",
          "[PluginResourceTexture][PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("texture-listing");

    const std::string plugin_key = "Texture_Pack";
    const fs::path    plugin_dir =
        write_plugin(data_dir_guard, plugin_key, make_resource_texture_source("Texture Pack", default_texture_dir_body()));

    const fs::path textures = plugin_dir / "textures";
    write_gray_png(textures / "alpha.png", 0x20);
    write_gray_png(textures / "beta.png", 0x80);
    write_text_file(textures / "gamma.jpg"); // the listing only checks the extension; decode is covered below
    write_text_file(textures / "notes.txt");

    // A user-owned texture, so the listing has a non-pack entry whose provenance must be empty.
    write_gray_png(fs::path(GUI::user_texture_dir()) / "user_made.png", 0x40);

    PluginManager& manager = PluginManager::instance();
    install_resource_texture_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, plugin_key, error));
    INFO("load error: " << error);
    CHECK(error.empty());

    CHECK(PluginResourceTextures::instance().entries().size() == 1);

    const uint64_t loaded_generation = GUI::texture_library_generation();
    const auto     loaded             = plugin_textures(true);
    REQUIRE(loaded.size() == 3);
    std::vector<std::string> names;
    for (const GUI::TextureLibraryEntry& entry : loaded)
        names.push_back(entry.name);
    CHECK(std::find(names.begin(), names.end(), "alpha") != names.end());
    CHECK(std::find(names.begin(), names.end(), "beta") != names.end());
    CHECK(std::find(names.begin(), names.end(), "gamma") != names.end());
    CHECK(std::find(names.begin(), names.end(), "notes") == names.end());

    // Every pack entry names its contributing pack; shipped and user entries carry no pack.
    for (const GUI::TextureLibraryEntry& entry : GUI::texture_library(true)) {
        if (entry.source == GUI::TextureSource::Plugin) {
            CHECK_FALSE(entry.pack_name.empty());
            CHECK(entry.pack_name == "Texture Pack");
        } else {
            CHECK(entry.pack_name.empty());
        }
    }

    // A no-argument call reuses the cached listing until a pack callback moves the generation.
    CHECK(plugin_textures(false).size() == 3);

    manager.set_capability_enabled({ PluginCapabilityType::ResourceTexture, "Texture Pack", plugin_key }, false);
    CHECK(GUI::texture_library_generation() > loaded_generation);
    CHECK(PluginResourceTextures::instance().entries().empty());
    CHECK(plugin_textures(false).empty());

    manager.set_capability_enabled({ PluginCapabilityType::ResourceTexture, "Texture Pack", plugin_key }, true);
    CHECK(plugin_textures(false).size() == 3);

    REQUIRE(manager.unload_plugin(plugin_key));
    CHECK(PluginResourceTextures::instance().entries().empty());
    CHECK(plugin_textures(false).empty());
}

TEST_CASE("Two distinctly named packs each list as one contiguous run",
          "[PluginResourceTexture][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("texture-contiguous");

    // Loaded out of name order, so it is the listing's pack-name sort that orders the runs.
    const std::string key_zeta  = "Pack_Zeta";
    const std::string key_alpha = "Pack_Alpha";

    const fs::path dir_zeta = write_plugin(
        data_dir_guard, key_zeta, make_resource_texture_source("Zeta Pack", default_texture_dir_body()));
    const fs::path dir_alpha = write_plugin(
        data_dir_guard, key_alpha, make_resource_texture_source("Alpha Pack", default_texture_dir_body()));

    write_gray_png(dir_zeta / "textures" / "zeta_one.png", 0x20);
    write_gray_png(dir_zeta / "textures" / "zeta_two.png", 0x40);
    write_gray_png(dir_alpha / "textures" / "alpha_one.png", 0x60);
    write_gray_png(dir_alpha / "textures" / "alpha_two.png", 0x80);

    PluginManager& manager = PluginManager::instance();
    install_resource_texture_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, key_zeta, error));
    REQUIRE(load_and_wait(manager, key_alpha, error));

    std::vector<GUI::TextureLibraryEntry> pack_entries;
    for (const GUI::TextureLibraryEntry& entry : GUI::texture_library(true))
        if (entry.source == GUI::TextureSource::Plugin)
            pack_entries.push_back(entry);
    REQUIRE(pack_entries.size() == 4);

    const auto count_pack = [&pack_entries](const std::string& name) {
        return std::count_if(pack_entries.begin(), pack_entries.end(),
                             [&name](const GUI::TextureLibraryEntry& e) { return e.pack_name == name; });
    };
    CHECK(count_pack("Zeta Pack") == 2);
    CHECK(count_pack("Alpha Pack") == 2);

    // Collapsing consecutive entries by pack name must leave one run per pack: a name never reappears
    // after another name intervenes, which is what lets the picker draw one section per pack.
    std::vector<std::string> runs;
    for (const GUI::TextureLibraryEntry& entry : pack_entries)
        if (runs.empty() || runs.back() != entry.pack_name)
            runs.push_back(entry.pack_name);

    std::vector<std::string> sorted_runs = runs;
    std::sort(sorted_runs.begin(), sorted_runs.end());
    CHECK(std::unique(sorted_runs.begin(), sorted_runs.end()) == sorted_runs.end());
    CHECK(runs == std::vector<std::string>({ "Alpha Pack", "Zeta Pack" }));

    manager.unload_plugin(key_zeta);
    manager.unload_plugin(key_alpha);
}

TEST_CASE("A second pack declaring a loaded capability name is refused",
          "[PluginResourceTexture][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("texture-capability-clash");

    // Distinct package names, one capability name: the loader allows only the first.
    const std::string key_first  = "Clash_First";
    const std::string key_second = "Clash_Second";
    const std::string capability = "Shared Textures";

    const fs::path dir_first = write_plugin(
        data_dir_guard, key_first,
        make_resource_texture_source("Clash Package One", default_texture_dir_body(), capability));
    write_plugin(data_dir_guard, key_second,
                 make_resource_texture_source("Clash Package Two", default_texture_dir_body(), capability));

    write_gray_png(dir_first / "textures" / "one.png", 0x20);
    write_gray_png(dir_first / "textures" / "two.png", 0x40);

    PluginManager& manager = PluginManager::instance();
    install_resource_texture_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, key_first, error));
    REQUIRE(manager.is_plugin_loaded(key_first));

    const std::vector<GUI::TextureLibraryEntry> before = plugin_textures(true);
    REQUIRE(before.size() == 2);
    for (const GUI::TextureLibraryEntry& entry : before)
        CHECK(entry.pack_name == capability);

    // The second package claims the capability name the first already owns, so the load is refused.
    std::string second_error;
    CHECK_FALSE(load_and_wait(manager, key_second, second_error));
    CHECK_FALSE(manager.is_plugin_loaded(key_second));

    // The first pack's textures are untouched by the refused load.
    const std::vector<GUI::TextureLibraryEntry> after = plugin_textures(true);
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK(after[i].name == before[i].name);
        CHECK(after[i].path == before[i].path);
        CHECK(after[i].pack_name == before[i].pack_name);
    }

    manager.unload_plugin(key_first);
}

TEST_CASE("A resource-texture pack with no images contributes no textures", "[PluginResourceTexture][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("texture-empty");

    const std::string plugin_key = "Empty_Pack";
    const fs::path    plugin_dir =
        write_plugin(data_dir_guard, plugin_key, make_resource_texture_source("Empty Pack", default_texture_dir_body()));
    fs::create_directories(plugin_dir / "textures"); // valid, but empty

    PluginManager& manager = PluginManager::instance();
    install_resource_texture_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, plugin_key, error));
    CHECK(manager.is_plugin_loaded(plugin_key));
    CHECK(plugin_textures(true).empty());

    manager.unload_plugin(plugin_key);
}

// ── Hostile get_texture_dir() overrides ─────────────────────────────────────────────────────────

TEST_CASE("A texture directory outside the plugin root contributes no textures", "[PluginResourceTexture][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir      data_dir_guard("texture-hostile");
    ScopedResourcesDir resources_dir_guard("texture-hostile-res");
    write_text_file(fs::path(resources_dir_guard.dir) / "sentinel.png"); // would list if it were scanned

    const std::string data_pack = "Data_Dir_Pack";
    write_plugin(data_dir_guard, data_pack,
                 make_resource_texture_source("Data Dir Pack", "return r\"" + data_dir_guard.dir.string() + "\""));
    const std::string resources_pack = "Resources_Dir_Pack";
    write_plugin(data_dir_guard, resources_pack,
                 make_resource_texture_source("Resources Dir Pack", "return r\"" + resources_dir_guard.dir.string() + "\""));

    PluginManager& manager = PluginManager::instance();
    install_resource_texture_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, data_pack, error));
    REQUIRE(load_and_wait(manager, resources_pack, error));

    // The packs load; their out-of-root directories are simply refused.
    CHECK(manager.is_plugin_loaded(data_pack));
    CHECK(manager.is_plugin_loaded(resources_pack));
    CHECK(PluginResourceTextures::instance().entries().empty());
    CHECK(plugin_textures(true).empty());

    manager.unload_plugin(data_pack);
    manager.unload_plugin(resources_pack);
}

TEST_CASE("A raising get_texture_dir leaves the pack loaded with no textures", "[PluginResourceTexture][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("texture-raising");

    const std::string plugin_key = "Raising_Pack";
    write_plugin(data_dir_guard, plugin_key,
                 make_resource_texture_source("Raising Pack", "raise RuntimeError(\"no textures today\")"));

    PluginManager& manager = PluginManager::instance();
    install_resource_texture_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, plugin_key, error));
    CHECK(error.empty());
    CHECK(manager.is_plugin_loaded(plugin_key));
    CHECK(PluginResourceTextures::instance().entries().empty());
    CHECK(plugin_textures(true).empty());

    manager.unload_plugin(plugin_key);
}

// ── Decoding parity ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pack images decode through the same path as hand-picked files", "[PluginResourceTexture]")
{
    ensure_wx_image_handlers();

    ScopedTemporaryDir pack_root("tex-pack");
    ScopedTemporaryDir hand_root("tex-hand");
    const fs::path     pack_dir = pack_root.path() / "textures";
    fs::create_directories(pack_dir);

    // PNG: grayscale, so load_texture_image_data passes it through verbatim.
    const fs::path pack_png = pack_dir / "grain.png";
    write_gray_png(pack_png, 0x40);
    const std::vector<unsigned char> png_bytes = read_file_bytes(pack_png);
    REQUIRE_FALSE(png_bytes.empty());
    write_bytes(hand_root.path() / "grain.png", png_bytes);

    // BMP: not a PNG, so it goes through wxImage and is re-encoded; the same file must give the same
    // bytes wherever it sits.
    const std::vector<unsigned char> bmp_bytes = make_gray_bmp(4, 4, 0x40);
    const fs::path                   pack_bmp  = pack_dir / "bump.bmp";
    write_bytes(pack_bmp, bmp_bytes);
    const fs::path hand_bmp = hand_root.path() / "bump.bmp";
    write_bytes(hand_bmp, bmp_bytes);

    // The collector lists both pack files, so these are the paths a pack would serve.
    std::string                    collect_error;
    const std::vector<std::string> pack_files =
        collect_resource_texture_files(pack_dir.string(), pack_root.string(), collect_error);
    REQUIRE(collect_error.empty());
    CHECK(pack_files.size() == 2);

    std::string pack_error;
    std::string hand_error;

    const auto pack_png_decoded = GUI::load_texture_image_data(pack_png.string(), pack_error);
    const auto hand_png_decoded = GUI::load_texture_image_data((hand_root.path() / "grain.png").string(), hand_error);
    REQUIRE(pack_png_decoded != nullptr);
    REQUIRE(hand_png_decoded != nullptr);
    CHECK(*pack_png_decoded == png_bytes); // supported PNG is passed through unchanged
    CHECK(*pack_png_decoded == *hand_png_decoded);

    const auto pack_bmp_decoded = GUI::load_texture_image_data(pack_bmp.string(), pack_error);
    const auto hand_bmp_decoded = GUI::load_texture_image_data(hand_bmp.string(), hand_error);
    REQUIRE(pack_bmp_decoded != nullptr);
    REQUIRE(hand_bmp_decoded != nullptr);
    REQUIRE_FALSE(pack_bmp_decoded->empty());
    CHECK(*pack_bmp_decoded == *hand_bmp_decoded);
}

TEST_CASE("Reinstalling a pack with different bytes at the same path changes the decoded content",
          "[PluginResourceTexture][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("texture-reinstall");

    const std::string plugin_key = "Reinstall_Pack";
    const fs::path    plugin_dir =
        write_plugin(data_dir_guard, plugin_key, make_resource_texture_source("Reinstall Pack", default_texture_dir_body()));
    const fs::path texture = plugin_dir / "textures" / "grain.png";
    write_gray_png(texture, 0x10);

    PluginManager& manager = PluginManager::instance();
    install_resource_texture_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, plugin_key, error));

    auto loaded = plugin_textures(true);
    REQUIRE(loaded.size() == 1);
    const std::string path = loaded.front().path;

    std::string decode_error;
    const auto  first = GUI::load_texture_image_data(path, decode_error);
    REQUIRE(first != nullptr);

    REQUIRE(manager.unload_plugin(plugin_key));

    // The reinstalled pack writes different bytes to the same file path.
    write_gray_png(texture, 0xE0);
    REQUIRE(load_and_wait(manager, plugin_key, error));

    loaded = plugin_textures(true);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded.front().path == path);

    const auto second = GUI::load_texture_image_data(path, decode_error);
    REQUIRE(second != nullptr);
    CHECK(*first != *second);
}

// ── Wheel metadata ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("A minimal resource-texture wheel is accepted", "[PluginResourceTexture][Wheel]")
{
    ScopedTemporaryDir dir("texture-wheel");
    const fs::path     wheel = dir.path() / "texture_pack-1.0-py3-none-any.whl";

    const std::string dist_info = "texture_pack-1.0.dist-info";
    const std::string metadata =
        "Metadata-Version: 2.1\n"
        "Name: texture_pack\n"
        "Version: 1.0\n"
        "Summary: Resource texture pack fixture\n"
        "Author: tests\n";
    const std::string wheel_metadata =
        "Wheel-Version: 1.0\n"
        "Generator: manual\n"
        "Root-Is-Purelib: true\n"
        "Tag: py3-none-any\n";
    const std::string record = dist_info + "/METADATA,,\n" + dist_info + "/WHEEL,,\n" + dist_info + "/RECORD,,\n";
    const std::string top_level = "texture_pack\n";

    {
        Zipper zipper(wheel.string(), Zipper::NO_COMPRESSION);
        zipper.add_entry(dist_info + "/METADATA", metadata.data(), metadata.size());
        zipper.add_entry(dist_info + "/WHEEL", wheel_metadata.data(), wheel_metadata.size());
        zipper.add_entry(dist_info + "/RECORD", record.data(), record.size());
        // read_wheel_plugin_metadata leaves `error` set when an optional top_level.txt is missing.
        zipper.add_entry(dist_info + "/top_level.txt", top_level.data(), top_level.size());
        zipper.finalize();
    }

    PluginDescriptor descriptor;
    std::string      error;
    REQUIRE(read_wheel_plugin_metadata(wheel, descriptor, error));
    INFO("wheel metadata error: " << error);
    CHECK(error.empty());
    CHECK(descriptor.name == "texture_pack");
    CHECK(descriptor.version == "1.0");
    CHECK(descriptor.entry_package == "texture_pack");
}

// ── .3mf round trip ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("A project keeps a pack texture after the pack is uninstalled",
          "[PluginResourceTexture][PluginLifecycle][Python][3mf]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("texture-3mf");

    const std::string plugin_key = "Texture_Pack_3mf";
    const fs::path    plugin_dir =
        write_plugin(data_dir_guard, plugin_key, make_resource_texture_source("Texture Pack", default_texture_dir_body()));
    write_gray_png(plugin_dir / "textures" / "grain.png", 0x60);

    PluginManager& manager = PluginManager::instance();
    install_resource_texture_wiring();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, plugin_key, error));

    const auto loaded = plugin_textures(true);
    REQUIRE(loaded.size() == 1);

    std::string decode_error;
    const auto  pack_bytes = GUI::load_texture_image_data(loaded.front().path, decode_error);
    REQUIRE(pack_bytes != nullptr);
    REQUIRE_FALSE(pack_bytes->empty());

    // Build a project the way the gizmo would: the layer carries the pack texture's bytes.
    Model               model;
    ModelObject*        object = model.add_object();
    ModelVolume*        volume = object->add_volume(make_cube(10., 10., 10.));
    object->add_instance();
    TextureDisplacementLayer layer;
    layer.slot       = 0;
    layer.name       = loaded.front().name;
    layer.path       = loaded.front().path;
    layer.image_data = pack_bytes;
    volume->texture_displacement_layers.push_back(layer);

    ScopedTemporaryFile first(".3mf");
    store_project(model, first.string());

    // Uninstall the pack; the project still has to open and re-save with the same image bytes.
    REQUIRE(manager.unload_plugin(plugin_key));
    CHECK(plugin_textures(true).empty());

    ScopedTemporaryDir backup_dir("orca_tex_pack_dst");
    Model              reloaded;
    REQUIRE(load_project(first.string(), reloaded, backup_dir));
    REQUIRE(reloaded.objects.size() == 1);
    REQUIRE(reloaded.objects.front()->volumes.size() == 1);
    const std::vector<TextureDisplacementLayer>& reloaded_layers =
        reloaded.objects.front()->volumes.front()->texture_displacement_layers;
    REQUIRE(reloaded_layers.size() == 1);
    REQUIRE(reloaded_layers.front().image_data);
    CHECK(*reloaded_layers.front().image_data == *pack_bytes);

    ScopedTemporaryFile second(".3mf");
    store_project(reloaded, second.string());

    ScopedTemporaryDir backup_dir2("orca_tex_pack_dst2");
    Model              reloaded_again;
    REQUIRE(load_project(second.string(), reloaded_again, backup_dir2));
    REQUIRE(reloaded_again.objects.size() == 1);
    REQUIRE(reloaded_again.objects.front()->volumes.size() == 1);
    const std::vector<TextureDisplacementLayer>& again_layers =
        reloaded_again.objects.front()->volumes.front()->texture_displacement_layers;
    REQUIRE(again_layers.size() == 1);
    REQUIRE(again_layers.front().image_data);
    CHECK(*again_layers.front().image_data == *pack_bytes);
}
