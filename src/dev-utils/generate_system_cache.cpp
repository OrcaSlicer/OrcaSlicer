#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>
#include <boost/system/error_code.hpp>
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

    // Collect all vendor names from JSON files in the profiles directory.
    std::vector<std::string> vendor_names;
    for (const auto& e : fs::directory_iterator(profiles_path)) {
        if (e.path().extension() == ".json")
            vendor_names.push_back(e.path().stem().string());
    }

    // Sort: PresetBundle::ORCA_FILAMENT_LIBRARY first, rest alphabetical.
    std::sort(vendor_names.begin(), vendor_names.end(),
              [](const std::string& a, const std::string& b) {
                  if (a == PresetBundle::ORCA_FILAMENT_LIBRARY) return true;
                  if (b == PresetBundle::ORCA_FILAMENT_LIBRARY) return false;
                  return a < b;
              });

    size_t total_print = 0, total_filament = 0, total_printer = 0;
    int    saved = 0, failed = 0;

    for (const auto& vendor_name : vendor_names) {
        try {
            const std::string json_path  = (fs::path(profiles_path) / (vendor_name + ".json")).string();
            const std::string cache_path = (fs::path(profiles_path) / (vendor_name + ".cache")).make_preferred().string();
            const bool is_orca_lib = (vendor_name == PresetBundle::ORCA_FILAMENT_LIBRARY);

            const auto stats = preset_bundle->save_bundled_vendor_cache(vendor_name, json_path, is_orca_lib, cache_path);

            if (!stats.ok) {
                std::cerr << "ERROR: " << vendor_name << ": verification failed\n";
                ++failed;
            } else {
                std::cout << "  [ok] " << vendor_name << ".cache"
                          << "  (" << stats.print_presets    << " print, "
                          <<           stats.filament_presets << " filament, "
                          <<           stats.printer_presets  << " printer)\n";
                total_print    += stats.print_presets;
                total_filament += stats.filament_presets;
                total_printer  += stats.printer_presets;
                ++saved;
            }
        } catch (const std::exception& ex) {
            std::cerr << "ERROR: " << vendor_name << ": " << ex.what() << "\n";
            ++failed;
        }
    }

    std::cout << "\nDone: " << saved << " cache(s) written";
    if (failed) std::cout << ", " << failed << " FAILED";
    std::cout << "\n"
              << "  Total print presets:    " << total_print    << "\n"
              << "  Total filament presets: " << total_filament << "\n"
              << "  Total printer presets:  " << total_printer  << "\n";
    return failed ? 1 : 0;
}
