#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetBundleCache.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>
#include <iostream>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace po = boost::program_options;

int main(int argc, char* argv[])
{
    po::options_description desc("OrcaSlicer System Cache Generator\nUsage");
    // clang-format off
    desc.add_options()
        ("help,h", "Show help")
#ifdef __APPLE__
        ("path,p", po::value<std::string>()->default_value("../../../../../../../resources/profiles"), "Path to profiles directory")
#else
        ("path,p", po::value<std::string>()->default_value("../../../resources/profiles"), "Path to profiles directory")
#endif
        ("log_level,l", po::value<int>()->default_value(2), "Log level (0=trace, 2=info, 4=error)");
    // clang-format on

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help")) { std::cout << desc << "\n"; return 0; }
        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "Error: " << e.what() << "\n" << desc << "\n";
        return 1;
    }

    const std::string profiles_path = vm["path"].as<std::string>();
    const int         log_level     = vm["log_level"].as<int>();

    if (!fs::exists(profiles_path) || !fs::is_directory(profiles_path)) {
        std::cerr << "Error: '" << profiles_path << "' is not a valid directory\n";
        return 1;
    }

    set_logging_level(log_level);
    // In validation_mode, load_system_presets_from_json uses data_dir() directly
    // (no /system/ suffix), so point data_dir at the profiles directory.
    set_data_dir(profiles_path);
    set_resources_dir(fs::path(profiles_path).parent_path().make_preferred().string());

    // load_presets creates user preset dirs under data_dir().
    const fs::path user_dir = fs::path(data_dir()) / PRESET_USER_DIR;
    if (!fs::exists(user_dir))
        fs::create_directories(user_dir);

    AppConfig app_config;
    app_config.set("preset_folder", "default");

    auto preset_bundle = std::make_unique<PresetBundle>();
    preset_bundle->set_is_validation_mode(true);
    preset_bundle->set_default_suppressed(true);

    std::cout << "Loading system presets from: " << profiles_path << "\n";

    try {
        preset_bundle->load_presets(app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to load presets: " << ex.what() << "\n";
        return 1;
    }

    const std::string cache_path =
        (fs::path(profiles_path) / "system_presets_cache.cache").make_preferred().string();

    PresetBundleCache::SystemPresetsCache cache;
    cache.capture(*preset_bundle, profiles_path);
    cache.save(cache_path);

    std::cout << "Cache written: " << cache_path << "\n"
              << "  Vendor profiles:  " << cache.vendor_profiles.size()   << "\n"
              << "  Print presets:    " << cache.print_presets.size()     << "\n"
              << "  Filament presets: " << cache.filament_presets.size()  << "\n"
              << "  Printer presets:  " << cache.printer_presets.size()   << "\n";
    return 0;
}
