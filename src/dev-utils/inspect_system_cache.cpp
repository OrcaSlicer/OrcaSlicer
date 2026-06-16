#include "libslic3r/PresetBundleCache.hpp"

#include <boost/program_options.hpp>
#include <iostream>
#include <iomanip>

using namespace Slic3r;
namespace po = boost::program_options;

static void print_bar(char c, int n) { std::cout << std::string(n, c) << "\n"; }

int main(int argc, char* argv[])
{
    po::options_description desc("OrcaSlicer Cache Inspector\nUsage");
    desc.add_options()
        ("help,h",    "Show help")
        ("path,p",    po::value<std::string>(), "Path to .cache file (required)")
        ("vendors,V", "List all vendor IDs and versions")
        ("models,m",  "List all printer models per vendor")
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

    PresetBundleCache::SystemPresetsCache cache;
    if (!cache.load(path)) {
        std::cerr << "Failed to load cache: " << path << "\n"
                  << "  (wrong format version, truncated file, or not a .cache file)\n";
        return 1;
    }

    // ---- Summary ----
    print_bar('=', 60);
    std::cout << "Cache file : " << path << "\n";
    std::cout << "Format ver : " << cache.format_version << "\n";
    std::cout << "Config opts: " << cache.config_options_count << "\n";
    print_bar('-', 60);
    std::cout << "Vendors    : " << cache.vendor_versions.size()  << "\n";
    std::cout << "Models     : ";
    size_t total_models = 0;
    for (const auto& vp : cache.vendor_profiles) total_models += vp.models.size();
    std::cout << total_models << "\n";
    std::cout << "Printers   : " << cache.printer_presets.size()  << "\n";
    std::cout << "Filaments  : " << cache.filament_presets.size() << "\n";
    std::cout << "Print proc : " << cache.print_presets.size()    << "\n";
    std::cout << "config_maps: " << cache.config_maps.size()      << "\n";
    std::cout << "filament_id_maps: " << cache.filament_id_maps.size() << "\n";
    print_bar('=', 60);

    bool show_all = !vm.count("vendors") && !vm.count("models") &&
                    !vm.count("presets") && !vm.count("filaments") &&
                    !vm.count("printers") && !vm.count("process");

    // ---- Vendor versions ----
    if (show_all || vm.count("vendors")) {
        std::cout << "\nVENDOR VERSIONS (" << cache.vendor_versions.size() << ")\n";
        print_bar('-', 60);
        for (const auto& [id, ver] : cache.vendor_versions)
            std::cout << "  " << std::left << std::setw(30) << id << " " << ver << "\n";
    }

    // ---- Models per vendor ----
    if (show_all || vm.count("models")) {
        std::cout << "\nVENDOR PROFILES & MODELS\n";
        print_bar('-', 60);
        for (const auto& vp : cache.vendor_profiles) {
            std::cout << "  [" << vp.id << "]  v" << vp.config_version
                      << "  (" << vp.models.size() << " models)\n";
            if (vm.count("models")) {
                for (const auto& m : vp.models) {
                    std::cout << "    " << std::left << std::setw(40) << m.name
                              << "  variants:" << m.variants.size() << "\n";
                }
            }
        }
    }

    // ---- Printer presets ----
    if (vm.count("presets") || vm.count("printers")) {
        std::cout << "\nPRINTER PRESETS (" << cache.printer_presets.size() << ")\n";
        print_bar('-', 60);
        for (const auto& cp : cache.printer_presets) {
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
        std::cout << "\nFILAMENT PRESETS (" << cache.filament_presets.size() << ")\n";
        print_bar('-', 60);
        for (const auto& cp : cache.filament_presets) {
            const auto* fv = cp.config.option<ConfigOptionString>("filament_vendor");
            const auto* ft = cp.config.option<ConfigOptionStrings>("filament_type");
            std::cout << "  " << std::left << std::setw(50) << cp.name
                      << "  vendor=" << (fv ? fv->value : "?")
                      << "  type=" << (ft && !ft->values.empty() ? ft->values[0] : "?")
                      << (cp.is_visible ? "" : "  [hidden]") << "\n";
        }
    }

    // ---- Print process presets ----
    if (vm.count("presets") || vm.count("process")) {
        std::cout << "\nPRINT PROCESS PRESETS (" << cache.print_presets.size() << ")\n";
        print_bar('-', 60);
        for (const auto& cp : cache.print_presets)
            std::cout << "  " << cp.name << (cp.is_visible ? "" : "  [hidden]") << "\n";
    }

    return 0;
}
