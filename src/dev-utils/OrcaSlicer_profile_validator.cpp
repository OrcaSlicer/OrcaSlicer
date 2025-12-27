#include "libslic3r/GCode.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include <string>

using namespace Slic3r;
namespace po = boost::program_options;

void generate_custom_presets(PresetBundle* preset_bundle, AppConfig& app_config)
{
    struct cus_preset
    {
        std::string name;
        std::string parent_name;
    };
    // create user presets
    auto createCustomPrinters = [&](Preset::Type type) {
        std::vector<cus_preset> custom_preset;
        PresetCollection*                            collection = nullptr;
        if (type == Preset::TYPE_PRINT)
            collection = &preset_bundle->prints;
        else if (type == Preset::TYPE_FILAMENT)
            collection = &preset_bundle->filaments;
        else if (type == Preset::TYPE_PRINTER)
            collection = &preset_bundle->printers;
        else
            return;
        custom_preset.reserve(collection->size());
        for (auto& parent : collection->get_presets()) {
            if (!parent.is_system)
                continue;
            auto new_name = parent.name + "_orca_test";
            if (parent.vendor)
                new_name = parent.vendor->name + "_" + new_name;
            custom_preset.push_back({new_name, parent.name});
        }
        for (auto p : custom_preset) {
            // Creating a new preset.
            auto parent = collection->find_preset(p.parent_name);
            auto vendor = collection->get_preset_with_vendor_profile(*parent);
            if (type == Preset::TYPE_FILAMENT) {
                parent->config.set_key_value("filament_start_gcode",
                                             new ConfigOptionStrings({"this_is_orca_test_filament_start_gcode_mock"}));
                parent->config.set_key_value("filament_notes", new ConfigOptionString(vendor.vendor->name));
            } else if (type == Preset::TYPE_PRINT) {
                parent->config.set_key_value("filename_format", new ConfigOptionString("this_is_orca_test_filename_format_mock"));
                parent->config.set_key_value("notes", new ConfigOptionString(vendor.vendor->name));
            } else if (type == Preset::TYPE_PRINTER) {
                parent->config.set_key_value("machine_start_gcode", new ConfigOptionString("this_is_orca_test_machine_start_gcode_mock"));
                parent->config.set_key_value("printer_notes", new ConfigOptionString(vendor.vendor->name));
            }

            collection->save_current_preset(p.name, false, false, parent);

        }
    };
    createCustomPrinters(Preset::TYPE_PRINTER);
    createCustomPrinters(Preset::TYPE_FILAMENT);
    createCustomPrinters(Preset::TYPE_PRINT);

    std::string       user_sub_folder  = DEFAULT_USER_FOLDER_NAME;
    const std::string dir_user_presets = data_dir() + "/" + PRESET_USER_DIR + "/" + user_sub_folder;

    fs::path user_folder(data_dir() + "/" + PRESET_USER_DIR);
    if (!fs::exists(user_folder))
        fs::create_directory(user_folder);

    fs::path folder(dir_user_presets);
    if (!fs::exists(folder))
        fs::create_directory(folder);
    std::vector<std::string> need_to_delete_list; // store setting ids of preset

    preset_bundle->prints.save_user_presets(dir_user_presets, PRESET_PRINT_NAME, need_to_delete_list);
    preset_bundle->filaments.save_user_presets(dir_user_presets, PRESET_FILAMENT_NAME, need_to_delete_list);
    preset_bundle->printers.save_user_presets(dir_user_presets, PRESET_PRINTER_NAME, need_to_delete_list);

    std::cout << "Custom presets generated successfully" << std::endl;
}
int main(int argc, char* argv[])
{
    po::options_description desc("Orca Profile Validator\nUsage");
    // clang-format off
    desc.add_options()("help,h", "help")
#ifdef __APPLE__
    ("path,p", po::value<std::string>()->default_value("../../../../../../../resources/profiles"), "profile folder")
#else
    ("path,p", po::value<std::string>()->default_value("../../../resources/profiles"), "profile folder")
#endif
    ("vendor,v", po::value<std::string>()->default_value(""), "Vendor name. Optional, all profiles present in the folder will be validated if not specified")
    ("generate_presets,g", po::value<bool>()->default_value(false), "Generate user presets for mock test")
    ("log_level,l", po::value<int>()->default_value(2), "Log level. Optional, default is 2 (warning). Higher values produce more detailed logs.");
    // clang-format on

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            return 1;
        }

        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "Error: " << e.what() << "\n";
        std::cerr << desc << "\n";
        return 1;
    }

    std::string path                 = vm["path"].as<std::string>();
    std::string vendor               = vm["vendor"].as<std::string>();
    int         log_level            = vm["log_level"].as<int>();
    bool        generate_user_preset = vm["generate_presets"].as<bool>();

    //  check if path is valid, and return error if not
    if (!fs::exists(path) || !fs::is_directory(path)) {
        std::cerr << "Error: " << path << " is not a valid directory\n";
        return 1;
    }

    // std::cout<<"path: "<<path<<std::endl;
    // std::cout<<"vendor: "<<vendor<<std::endl;
    // std::cout<<"log_level: "<<log_level<<std::endl;

    set_data_dir(path);

    auto user_dir = fs::path(Slic3r::data_dir()) / PRESET_USER_DIR;
    user_dir.make_preferred();
    if (!fs::exists(user_dir))
        fs::create_directory(user_dir);

    set_logging_level(log_level);
    auto preset_bundle = new PresetBundle();
    // preset_bundle->setup_directories();
    preset_bundle->set_is_validation_mode(true);
    preset_bundle->set_vendor_to_validate(vendor);

    preset_bundle->set_default_suppressed(true);
    AppConfig app_config;
    app_config.set("preset_folder", "default");

    if(generate_user_preset)
        preset_bundle->remove_user_presets_directory("default");

    try {
        auto preset_substitutions = preset_bundle->load_presets(app_config, ForwardCompatibilitySubstitutionRule::Disable);
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << ex.what();
        std::cout << "Validation failed" << std::endl;
        return 1;
    }
    // Report loaded presets
    std::cout << "Total loaded vendors: " << preset_bundle->vendors.size() << std::endl;

    if (generate_user_preset) {
        generate_custom_presets(preset_bundle, app_config);
        return 0;
    }

    if (preset_bundle->has_errors()) {
        std::cout << "Validation failed" << std::endl;
        return 1;
    }

    std::cout << "Validation completed successfully" << std::endl;
    return 0;
}
