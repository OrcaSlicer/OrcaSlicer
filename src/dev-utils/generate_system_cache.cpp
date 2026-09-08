#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
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
    set_data_dir(profiles_path);
    set_resources_dir(fs::path(profiles_path).parent_path().make_preferred().string());

    const fs::path user_dir = fs::path(data_dir()) / PRESET_USER_DIR;
    if (!fs::exists(user_dir))
        fs::create_directories(user_dir);

    AppConfig app_config;
    app_config.set("preset_folder", "default");

    auto preset_bundle = std::make_unique<PresetBundle>();
    preset_bundle->set_is_validation_mode(true);
    preset_bundle->set_default_suppressed(true);
    preset_bundle->set_generate_vendor_caches(true);

    std::cout << "Loading system presets from: " << profiles_path << "\n";

    try {
        // In validation mode data_dir() is the profiles directory set above, so the
        // loader writes each <vendor>.opc next to its <vendor>.json as it parses it.
        preset_bundle->load_presets(app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to load presets: " << ex.what() << "\n";
        return 1;
    }

    size_t cache_count = 0;
    for (auto& entry : fs::directory_iterator(profiles_path))
        if (boost::iends_with(entry.path().string(), ".opc"))
            ++ cache_count;
    if (cache_count == 0) {
        std::cerr << "No vendor cache files were generated under " << profiles_path << "\n";
        return 1;
    }
    std::cout << "Generated " << cache_count << " vendor cache file(s) under " << profiles_path << "\n";
    return 0;
}
