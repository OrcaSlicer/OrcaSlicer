#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include <iomanip>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace po = boost::program_options;

static void print_bar(char c, int n) { std::cout << std::string(n, c) << "\n"; }

static void inspect_one(const std::string& path, const po::variables_map& vm)
{
    Slic3r::VendorCache vc;
    if (!vc.load(path)) {
        std::cerr << "Failed to load cache: " << path << "\n"
                  << "  (wrong format version, truncated file, CRC mismatch, or not a .cache file)\n";
        return;
    }

    bool show_all = !vm.count("vendors") && !vm.count("models") &&
                    !vm.count("presets") && !vm.count("filaments") &&
                    !vm.count("printers") && !vm.count("process");

    // ---- Summary ----
    print_bar('=', 60);
    std::cout << "Cache file  : " << path << "\n";
    std::cout << "Vendor      : " << vc.profile.id << "  v" << vc.profile.config_version << "\n";
    std::cout << "JSON version: " << (vc.vendor_json_version.empty() ? "(none)" : vc.vendor_json_version) << "\n";
    std::cout << "Cache ver   : " << vc.cache_version << "\n";
    std::cout << "Config opts : " << vc.config_options_count << "\n";
    print_bar('-', 60);
    std::cout << "Models      : " << vc.profile.models.size()       << "\n";
    std::cout << "Printers    : " << vc.printer_presets.size()      << "\n";
    std::cout << "Filaments   : " << vc.filament_presets.size()     << "\n";
    std::cout << "Print proc  : " << vc.print_presets.size()        << "\n";
    std::cout << "SLA print   : " << vc.sla_print_presets.size()    << "\n";
    std::cout << "SLA material: " << vc.sla_material_presets.size() << "\n";
    std::cout << "config_maps : " << vc.config_maps.size()          << "\n";
    std::cout << "filament_id_maps: " << vc.filament_id_maps.size() << "\n";
    print_bar('=', 60);

    // ---- Models ----
    if (show_all || vm.count("vendors") || vm.count("models")) {
        std::cout << "\nVENDOR PROFILE  [" << vc.profile.id << "]\n";
        print_bar('-', 60);
        std::cout << "  Name: " << vc.profile.name << "\n";
        std::cout << "  Config version: " << vc.profile.config_version << "\n";
        if (vm.count("models") || show_all) {
            for (const auto& m : vc.profile.models) {
                std::cout << "    " << std::left << std::setw(40) << m.name
                          << "  variants:" << m.variants.size() << "\n";
            }
        }
    }

    // ---- Printer presets ----
    if (vm.count("presets") || vm.count("printers")) {
        std::cout << "\nPRINTER PRESETS (" << vc.printer_presets.size() << ")\n";
        print_bar('-', 60);
        for (const auto& cp : vc.printer_presets) {
            const auto* pm = cp.config.option<ConfigOptionString>("printer_model");
            const auto* pv = cp.config.option<ConfigOptionString>("printer_variant");
            std::cout << "  " << std::left << std::setw(50) << cp.name
                      << "  model=" << (pm ? pm->value : "?")
                      << "  nozzle=" << (pv ? pv->value : "?")
                      << (cp.is_visible ? "" : "  [hidden]") << "\n";
        }
    }

    // ---- Filament presets ----
    if (vm.count("presets") || vm.count("filaments")) {
        std::cout << "\nFILAMENT PRESETS (" << vc.filament_presets.size() << ")\n";
        print_bar('-', 60);
        for (const auto& cp : vc.filament_presets) {
            const auto* fv = cp.config.option<ConfigOptionStrings>("filament_vendor");
            const auto* ft = cp.config.option<ConfigOptionStrings>("filament_type");
            std::cout << "  " << std::left << std::setw(50) << cp.name
                      << "  vendor=" << (fv && !fv->values.empty() ? fv->values[0] : "?")
                      << "  type="   << (ft && !ft->values.empty() ? ft->values[0] : "?")
                      << (cp.is_visible ? "" : "  [hidden]") << "\n";
        }
    }

    // ---- Print process presets ----
    if (vm.count("presets") || vm.count("process")) {
        std::cout << "\nPRINT PROCESS PRESETS (" << vc.print_presets.size() << ")\n";
        print_bar('-', 60);
        for (const auto& cp : vc.print_presets)
            std::cout << "  " << cp.name << (cp.is_visible ? "" : "  [hidden]") << "\n";
    }
}

int main(int argc, char* argv[])
{
    po::options_description desc("OrcaSlicer Cache Inspector\nUsage");
    desc.add_options()
        ("help,h",    "Show help")
        ("path,p",    po::value<std::string>(), "Path to a .cache file or a directory of .cache files (required)")
        ("vendors,V", "Show vendor profile summary")
        ("models,m",  "List all printer models")
        ("presets,P", "List all preset names")
        ("filaments,f", "List filament presets")
        ("printers,r",  "List printer presets")
        ("process,p2",  "List print process presets");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help") || !vm.count("path")) { std::cout << desc << "\n"; return 0; }
        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "Error: " << e.what() << "\n" << desc << "\n"; return 1;
    }

    const std::string path = vm["path"].as<std::string>();

    if (fs::is_directory(path)) {
        // Inspect all .cache files in the directory.
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(path))
            if (e.path().extension() == ".cache")
                files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files)
            inspect_one(f.string(), vm);
    } else {
        inspect_one(path, vm);
    }

    return 0;
}
