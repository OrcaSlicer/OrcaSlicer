// This single-TU executable links libslic3r, whose SVG/emboss objects (pulled in by the slice mode
// below) reference the header-only nanosvg implementation. Provide it here BEFORE any libslic3r header:
// several of them transitively include nanosvg.h without the implementation macro, and its include
// guard would then suppress the implementation if the macro were defined afterwards. Same pattern as
// the test mains.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

#include <vector>
#include <boost/filesystem/path.hpp>
#include <map>
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <utility>
#include "libslic3r/CustomGCode.hpp"
#include <iterator>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/expressions/message.hpp>
#include <cstddef>
#include <exception>
#include "libslic3r/PlaceholderParser.hpp"
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/program_options/errors.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTowerEstimate.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/core/null_deleter.hpp>
#include <boost/make_shared.hpp>
#include <boost/program_options.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/STEP.hpp"

namespace fs = boost::filesystem;

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
    std::map<std::string, std::string> need_to_delete_list; // store setting ids of preset

    preset_bundle->prints.save_user_presets(dir_user_presets, PRESET_PRINT_NAME, need_to_delete_list);
    preset_bundle->filaments.save_user_presets(dir_user_presets, PRESET_FILAMENT_NAME, need_to_delete_list);
    preset_bundle->printers.save_user_presets(dir_user_presets, PRESET_PRINTER_NAME, need_to_delete_list);

    std::cout << "Custom presets generated successfully" << std::endl;
}

namespace {

Vec2d printable_area_center(const DynamicPrintConfig &cfg)
{
    const auto *opt = cfg.option<ConfigOptionPoints>("printable_area");
    if (opt == nullptr || opt->values.empty())
        return Vec2d(100., 100.);
    Vec2d lo = opt->values.front(), hi = opt->values.front();
    for (const Vec2d &p : opt->values) { lo = lo.cwiseMin(p); hi = hi.cwiseMax(p); }
    return 0.5 * (lo + hi);
}

// Put the prime tower where the GUI and CLI would before slicing. The config default (x 15, y 220)
// lies off any bed shallower than the tower, and generation rejects an off-plate tower instead of
// exporting it. Beside the cube at the bed centre, clear of the edge exclusion strips some beds
// carry, with a few mm of clearance so the conflict checker never sees the two touch. The cube and
// the tower's estimated footprint are then pulled inside the printable outline as one rigid pair:
// moving the tower alone would push it back onto the cube on a narrow bed. Returns that move for
// the cube.
Vec2d place_wipe_tower(DynamicPrintConfig &cfg, const Vec2d &center)
{
    const auto *area = cfg.option<ConfigOptionPoints>("printable_area");
    if (area == nullptr || area->values.size() < 3)
        return Vec2d::Zero();
    const WipeTowerFootprint footprint = estimate_wipe_tower_footprint(cfg, resolve_wipe_tower_type(cfg), {0, 1}, cfg.opt_float("layer_height"), 10.);
    if (footprint.depth < EPSILON)
        return Vec2d::Zero();
    const double margin = WIPE_TOWER_MARGIN + footprint.brim_width;
    // The position is the tower's own origin; a rotated tower extends from it in another
    // direction, so place the rotated box's extents rather than the origin.
    Slic3r::Polygon box({Point::new_scale(0., 0.), Point::new_scale(footprint.width, 0.), Point::new_scale(footprint.width, footprint.depth), Point::new_scale(0., footprint.depth)});
    box.rotate(Geometry::deg2rad(cfg.opt_float("wipe_tower_rotation_angle")));
    const BoundingBox local = get_extents(box);
    const Vec2d       lo    = unscale(local.min);
    const Vec2d       size  = unscale(local.max) - lo;
    Vec2d             pos(center.x() + 5. + margin + 5. - lo.x(), center.y() - size.y() / 2. - lo.y());
    box.translate(Point::new_scale(pos.x(), pos.y()));
    // A bed too small for the pair keeps the cube at its centre and places the tower alone.
    const Polygons    bed{Polygon::new_scale(area->values)};
    const BoundingBox tower = get_extents(box);
    BoundingBox       pair  = tower;
    pair.merge(Point::new_scale(center.x() - 5., center.y() - 5.));
    pair.merge(Point::new_scale(center.x() + 5., center.y() + 5.));
    const Point       room  = get_extents(bed).size() - Point::new_scale(2. * margin, 2. * margin);
    const bool        rigid = pair.size().x() < room.x() && pair.size().y() < room.y();
    const Vec2d       move  = WipeTower::move_box_inside_polygon(rigid ? pair : tower, bed, scaled<coord_t>(margin)).cast<double>();
    pos += move;
    cfg.option<ConfigOptionFloats>("wipe_tower_x", true)->values = {pos.x()};
    cfg.option<ConfigOptionFloats>("wipe_tower_y", true)->values = {pos.y()};
    return rigid ? move : Vec2d::Zero();
}

// Slice cubes that switch from filament 1 to filament 2 partway up, so a filament change fires, then
// export. The change drives the printer's own change_filament_gcode: on a single-nozzle machine it rides
// the AMS prime tower (append_tcr), on a multi-nozzle machine it routes through the nozzle swap
// (set_extruder / append_tcr2) - the engine picks the path from the printer's topology, so one model
// covers both. The layer-by-layer slice also pauses at z 2 and runs the template custom g-code at z 7, so
// machine_pause_gcode and template_custom_gcode expand, and turns clumping detection on for a printer that
// ships wrapping_detection_gcode. The by-object slice prints two cubes in turn without a prime tower:
// printing_by_object_gcode fires before the second one, and the filament change goes through set_extruder.
// Either way the output file name is built after export, which expands filename_format with the final
// print statistics. An undefined placeholder in any shipped custom g-code throws
// Slic3r::PlaceholderParserError.
std::string slice_two_color_cube_and_export(DynamicPrintConfig cfg, bool is_bbl, bool by_object)
{
    const Vec2d        center = printable_area_center(cfg);
    std::vector<Vec2d> cube_mins;
    if (by_object) {
        // By-object printing fires the hook only without a wipe tower, and rules out clumping detection and
        // smooth timelapse. No skirt, so the two cubes' own skirts cannot overlap.
        cfg.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
        cfg.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
        cfg.set_key_value("enable_wrapping_detection", new ConfigOptionBool(false));
        cfg.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
        cfg.set_key_value("skirt_loops", new ConfigOptionInt(0));
        cube_mins = {center + Vec2d(-20., -5.), center + Vec2d(10., -5.)};
    } else {
        // Clumping detection changes the tower footprint, so turn it on before placing the tower.
        if (!cfg.opt_string("wrapping_detection_gcode").empty())
            cfg.set_key_value("enable_wrapping_detection", new ConfigOptionBool(true));
        cube_mins = {center - Vec2d(5., 5.) + place_wipe_tower(cfg, center)};
    }

    Model  model;
    Print  print;
    print.is_BBL_printer() = is_bbl;
    for (const Vec2d &cube_min : cube_mins) {
        TriangleMesh m = make_cube(10, 10, 10);
        m.translate(static_cast<float>(cube_min.x()), static_cast<float>(cube_min.y()), 0.f);
        ModelObject *obj = model.add_object();
        obj->name = "cube"; // populates [input_filename_base] the way a loaded model does
        obj->add_volume(m);
        obj->add_instance();
        // Filament 2 is used only above z=4, so the upper layers carry a single filament change.
        DynamicPrintConfig range_config;
        range_config.set_key_value("extruder",     new ConfigOptionInt(2));
        // Every range must carry a layer_height; use the process's own so a fine nozzle (e.g. 0.15 mm
        // printing ~0.1 mm layers) isn't forced to a height its extrusion width can't support - that
        // trips Flow::with_spacing.
        range_config.set_key_value("layer_height", new ConfigOptionFloat(cfg.opt_float("layer_height")));
        obj->layer_config_ranges[{4.0, 10.0}].assign_config(std::move(range_config));
        obj->ensure_on_bed();
        print.auto_assign_extruders(obj);
    }
    // Custom g-codes per print_z apply to layer-by-layer printing only (ToolOrdering::assign_custom_gcodes).
    if (!by_object)
        model.plates_custom_gcodes[model.curr_plate_index].gcodes = {{2., CustomGCode::PausePrint, 1, "", ""},
                                                                     {7., CustomGCode::Template, 1, "", ""}};
    print.apply(model, cfg);
    print.validate();

    // Process + export to a temp file, then read it back (the app's own export path is where the
    // custom *_gcode placeholders expand).
    print.set_status_silent();
    print.process();
    const fs::path tmp = fs::temp_directory_path() / fs::unique_path("orca-validate-%%%%-%%%%.gcode");
    print.export_gcode(tmp.string(), nullptr, nullptr);
    std::ifstream in(tmp.string());
    std::string   out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    boost::system::error_code ec;
    fs::remove(tmp, ec);
    print.output_filename(); // names the export as the app does, expanding filename_format
    return out;
}

// Select the printer's OWN default process + filament (as the app does on a printer change) so we
// slice with settings the printer actually ships, not the generic "Default Setting" that stays
// selected because it is compatible with every printer.
void select_printer_default_presets(PresetBundle &bundle)
{
    const Preset &printer_preset = bundle.printers.get_selected_preset();
    const std::string def_print = printer_preset.config.opt_string("default_print_profile");
    if (!def_print.empty())
        bundle.prints.select_preset_by_name(def_print, /*force=*/true);
    if (const auto *def_fil = printer_preset.config.option<ConfigOptionStrings>("default_filament_profile");
        def_fil != nullptr && !def_fil->values.empty())
        bundle.filaments.select_preset_by_name(def_fil->values.front(), /*force=*/true);
    // Re-seed the per-slot filament list from that selection, or the sweep's result depends on the
    // printer sliced before it. Once there are 2+ slots, full_config() builds the filament config from
    // filament_presets and ignores the selected preset (PresetBundle::full_fff_config), while
    // update_compatible() only replaces a slot that has gone *incompatible* - and when it does, it ranks
    // the outgoing preset's alias, then its filament type, above the printer's own default. The sweep
    // grows every printer to 2 slots and update_multi_material_filament_presets() never shrinks them, so
    // a material picked up on the first printer rides the whole run. With all vendors loaded the first
    // printer inherits a TPU (the load-time pick is whichever filament sorts first), the type match
    // re-resolves it to "Generic TPU @System", and its alias then pins every later printer to that
    // vendor's own "Generic TPU @..." - which the BBL dual-nozzle profiles rightly refuse to group.
    bundle.filament_presets.assign(1, bundle.filaments.get_selected_preset_name());
}

// The vendor/printer currently being sliced, stamped onto every engine log record by the sink below so
// the interleaved [error] lines can be attributed to a profile. Updated once per loop iteration; safe as
// a plain global because the sweep is single-threaded and synchronous (see slice_all_printers).
static std::string g_slice_context;

// Route Boost.Log through a sink that prefixes every record with g_slice_context. Without this the
// engine's [error] lines (emitted deep inside process()/export_gcode()) carry no printer context, so a
// failing profile cannot be told apart from the ~1000 others in the sweep. Drops the default trivial
// sink's timestamp/thread columns - noise here - in favour of the vendor/printer tag.
void install_slice_context_log_sink()
{
    namespace logging = boost::log;
    namespace sinks   = boost::log::sinks;
    namespace expr    = boost::log::expressions;

    auto backend = boost::make_shared<sinks::text_ostream_backend>();
    backend->add_stream(boost::shared_ptr<std::ostream>(&std::clog, boost::null_deleter()));
    backend->auto_flush(true);

    auto sink = boost::make_shared<sinks::synchronous_sink<sinks::text_ostream_backend>>(backend);
    sink->set_formatter([](const logging::record_view &rec, logging::formatting_ostream &strm) {
        strm << "[" << rec[logging::trivial::severity] << "]";
        if (!g_slice_context.empty())
            strm << " [" << g_slice_context << "]";
        strm << " " << rec[expr::smessage];
    });

    logging::core::get()->remove_all_sinks(); // drop the default trivial sink so lines are not doubled
    logging::core::get()->add_sink(sink);
}

// Size the filament slots for the current selection and build the config every slice uses.
DynamicPrintConfig slice_config(PresetBundle &bundle)
{
    // Grow to a 2nd filament so the cube can change colour; never shrink a multi-nozzle printer
    // below its nozzle count, or full_config()'s flush-volume matrix no longer matches validate().
    const size_t nozzles = bundle.printers.get_selected_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->size();
    bundle.set_num_filaments((unsigned int) std::max<size_t>(2, nozzles));

    // Mirror the app's manual filament->nozzle assignment for a multi-nozzle BBL printer: put each
    // filament on its own nozzle and pin the map (fmmManual) so full_config() collapses every filament to
    // the variant of the nozzle it actually prints from, and the engine keeps that assignment instead of
    // auto-remapping it during process(). Without this the synthetic 2nd filament keeps nozzle 1's variant
    // while the auto map moves it to nozzle 2 - harmless, but on the one printer whose nozzles differ in
    // type (Direct Drive + Bowden) the mismatched lookup spams [error] lines. Single-nozzle and non-BBL
    // printers keep the default map (their toolchange rides the AMS/tool-changer path unchanged).
    const bool pin_filament_map = bundle.is_bbl_vendor() && nozzles > 1;
    auto &fmap = bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values;
    if (pin_filament_map) {
        for (size_t i = 0; i < fmap.size(); ++i)
            fmap[i] = int(i % nozzles) + 1;
    }

    // A fresh printer selection uses its declared nozzle volumes, just like the
    // app. Otherwise a high-flow preset is silently sliced with Standard tuning.
    bundle.reset_default_nozzle_volume_type();
    bundle.project_config.option<ConfigOptionInts>("filament_volume_map", true)->values =
        bundle.get_default_nozzle_volume_types_for_filaments(fmap);

    DynamicPrintConfig cfg = bundle.full_config();
    cfg.set_key_value("enable_prime_tower", new ConfigOptionBool(true)); // force a purge tower so the change is detectable
    // The map above drives full_config()'s per-filament variant collapse; fmmManual on the sliced config
    // stops process() from auto-remapping filaments back onto a different nozzle (which would re-introduce
    // the variant mismatch this pinning avoids).
    if (pin_filament_map)
        cfg.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));

    // full_config() grows filament_extruder_variant to one entry per filament, but because the synthetic
    // 2nd filament is a duplicate of the first (set_num_filaments copies the same preset), it leaves
    // filament_self_index at size 1. That makes update_values_to_printer_extruders_for_multiple_filaments
    // fail to resolve the 2nd filament's variant - a benign fallback that spams [error] lines. A real
    // 2-colour project ships filament_self_index = 1,2,...; mirror that so the sweep log stays clean. The
    // slice output is unaffected: the duplicated filament's per-variant values are identical to the first.
    if (auto *variants = cfg.option<ConfigOptionStrings>("filament_extruder_variant")) {
        auto &self_index = cfg.option<ConfigOptionInts>("filament_self_index", true)->values;
        if (self_index.size() != variants->size()) {
            self_index.resize(variants->size());
            for (size_t i = 0; i < self_index.size(); ++i)
                self_index[i] = int(i) + 1;
        }
    }
    return cfg;
}

// Slice the current selection and log any failure against `what` (the printer, and the extra preset if
// any). With an outdir, the g-code is also saved there as "<file_base>.gcode". Returns the g-code, or an
// empty string on failure.
std::string slice_selection(PresetBundle &bundle, const std::string &what, bool by_object, const std::string &outdir, const std::string &file_base)
{
    try {
        const std::string out = slice_two_color_cube_and_export(slice_config(bundle), bundle.is_bbl_vendor(), by_object);
        if (!outdir.empty() && !out.empty())
            save_string_file(fs::path(outdir) / (file_base + ".gcode"), out);
        if (out.empty() || out.find("G1") == std::string::npos) {
            BOOST_LOG_TRIVIAL(error) << what << " produced no g-code";
            return {};
        }
        return out;
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << what << " failed to slice: " << ex.what();
        return {};
    }
}

// The templates a preset carries, as (key, text): every non-empty *_gcode text, and filename_format. Each
// value of a per-variant key counts, though a slice expands only the values of the variants its printer uses.
using Templates = std::set<std::pair<std::string, std::string>>;
Templates preset_templates(const DynamicPrintConfig &cfg)
{
    Templates out;
    for (const std::string &key : cfg.keys()) {
        if (key != "filename_format" && !boost::algorithm::ends_with(key, "_gcode"))
            continue;
        const ConfigOption *opt = cfg.option(key);
        if (opt->type() == coString) {
            if (const std::string &text = static_cast<const ConfigOptionString *>(opt)->value; !text.empty())
                out.emplace(key, text);
        } else if (opt->type() == coStrings) {
            for (const std::string &text : static_cast<const ConfigOptionStrings *>(opt)->values)
                if (!text.empty())
                    out.emplace(key, text);
        }
    }
    return out;
}

// Slice-and-export a two-colour cube through every shipped printer (optionally scoped to one vendor via
// -v). Unlike the static reference/placeholder checks, this expands every custom *_gcode - including
// change_filament_gcode at the one filament change - against the printer's fully-resolved config, so
// undefined-placeholder / invalid-flow bugs surface here. PlaceholderParser::check_inactive_branches is
// set for the sweep, so the names in {if} branches a slice does not take must resolve too.
//
// Each printer is sliced with its own default process and filament. Every other compatible system
// process and filament whose templates (filename_format, filament_start_gcode, ...) no slice has expanded
// yet is then sliced once on that printer - a filament that compatible_prints limits to another process
// with that process - so each template text shipped in any system preset is expanded once. That check runs on the first compatible printer the sweep reaches only, in that printer's hook
// contexts: a value-dependent error on another printer, or a hook call site there that sets fewer
// variables (a multi-nozzle tool change, say), is not covered. The first printer shipping each
// printing_by_object_gcode also slices two cubes by object, the only way that hook fires.
//
// Reports every failure and returns 1 if any slice failed, 0 otherwise. When outdir is non-empty, each
// g-code is also written there as "<vendor>__<printer>[__<preset>].gcode" for manual inspection. The
// sweep is SEQUENTIAL by necessity: Print::process() keeps process-global state, so slicing printers
// concurrently in one process races even with per-slice Model+Print. Load in validation mode so the
// vendors are read straight from the -p profiles dir (no data_dir/system tree) and -v scoping is honoured
// for free.
int slice_all_printers(const std::string &vendor, const std::string &outdir)
{
    install_slice_context_log_sink();
    PlaceholderParser::check_inactive_branches = true;

    if (!outdir.empty()) {
        boost::system::error_code ec;
        fs::create_directories(outdir, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << "Could not create output directory \"" << outdir << "\": " << ec.message();
            std::cout << "Validation failed" << std::endl;
            return 1;
        }
        std::cout << "Saving sliced g-code to " << outdir << std::endl;
    }

    PresetBundle bundle;
    bundle.set_is_validation_mode(true);
    bundle.set_vendor_to_validate(vendor); // empty == all vendors
    AppConfig app_config;
    app_config.set("preset_folder", "default");
    try {
        bundle.load_presets(app_config, ForwardCompatibilitySubstitutionRule::Disable);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << ex.what();
        std::cout << "Validation failed" << std::endl;
        return 1;
    }

    // Enable every instantiable model/variant in AppConfig - system printers are hidden until enabled;
    // without this select_preset_by_name silently falls back to the "Default Printer".
    std::vector<std::pair<std::string, std::string>> printers; // (vendor name, preset name)
    for (const Preset &p : bundle.printers.get_presets()) {
        if (p.vendor == nullptr) continue;                 // skips the Default Printer
        const std::string model   = p.config.opt_string("printer_model");
        const std::string variant = p.config.opt_string("printer_variant");
        if (model.empty() || variant.empty()) continue;    // skip non-instantiable base/common configs
        app_config.set_variant(p.vendor->id, model, variant, true);
        printers.push_back({p.vendor->name, p.name});
    }
    bundle.load_installed_printers(app_config);

    if (printers.empty()) {
        BOOST_LOG_TRIVIAL(error) << "No instantiable printer presets found"
            << (vendor.empty() ? "" : " for vendor " + vendor);
        std::cout << "Validation failed" << std::endl;
        return 1;
    }
    std::cout << "Slicing " << printers.size() << " printer preset(s)"
              << (vendor.empty() ? "" : " for vendor " + vendor) << "..." << std::endl;

    Templates covered; // the process and filament templates some slice has expanded
    auto cover = [&covered](const Preset &preset) {
        const Templates templates = preset_templates(preset.config);
        covered.insert(templates.begin(), templates.end());
    };
    auto is_covered = [&covered](const Preset &preset) {
        const Templates templates = preset_templates(preset.config);
        return std::includes(covered.begin(), covered.end(), templates.begin(), templates.end());
    };
    int failures = 0, extra_processes = 0, extra_filaments = 0, by_object_slices = 0;
    for (const auto &[vendor_name, printer] : printers) {
        g_slice_context = vendor_name + " / " + printer; // tag every engine log line from this slice
        const bool selected = bundle.printers.select_preset_by_name(printer, /*force=*/true);
        if (!selected || bundle.printers.get_selected_preset_name() != printer) {
            BOOST_LOG_TRIVIAL(error) << "Printer preset \"" << printer << "\" could not be selected";
            ++failures;
            continue;
        }

        select_printer_default_presets(bundle);          // slice with the printer's shipped process/filament
        bundle.update_multi_material_filament_presets();  // size filament_presets to nozzle count
        bundle.update_compatible(PresetSelectCompatibleType::Always);

        // Never slice with a generic default preset - that would validate stand-in settings, not the
        // real profile (a legit per-profile error).
        if (bundle.prints.get_selected_preset().is_default || bundle.filaments.get_selected_preset().is_default) {
            BOOST_LOG_TRIVIAL(error) << "Printer \"" << printer << "\" fell back to a default preset (process=\""
                << bundle.prints.get_selected_preset_name() << "\", filament=\""
                << bundle.filaments.get_selected_preset_name() << "\")";
            ++failures;
            continue;
        }

        const std::string print_name    = bundle.prints.get_selected_preset_name();
        const std::string filament_name = bundle.filaments.get_selected_preset_name();
        const std::string what          = "Printer \"" + printer + "\"";
        const std::string file_base     = sanitize_filename(vendor_name) + "__" + sanitize_filename(printer);
        if (const std::string out = slice_selection(bundle, what, false, outdir, file_base); out.empty())
            ++failures;
        else if (out.find("CP TOOLCHANGE START") == std::string::npos) {
            // The filament change never rode the tower, so change_filament_gcode was not exercised.
            BOOST_LOG_TRIVIAL(error) << what << " sliced but the filament change never fired (no CP TOOLCHANGE START)";
            ++failures;
        }
        cover(bundle.prints.get_selected_preset());
        cover(bundle.filaments.get_selected_preset());

        const std::string &by_object_gcode = bundle.printers.get_selected_preset().config.opt_string("printing_by_object_gcode");
        if (!by_object_gcode.empty() && covered.emplace("printing_by_object_gcode", by_object_gcode).second) {
            ++by_object_slices;
            g_slice_context = vendor_name + " / " + printer + " / by object";
            if (slice_selection(bundle, what + " printing by object", true, outdir, file_base + "__by_object").empty())
                ++failures;
        }

        // The first process of this printer that compatible_prints lets an otherwise incompatible filament
        // use, judged as update_compatible() would with that process selected; empty if there is none.
        const PresetWithVendorProfile printer_with_vendor = bundle.printers.get_edited_preset_with_vendor_profile();
        auto limiting_process = [&bundle, &printer_with_vendor](const Preset &filament) -> std::string {
            const auto *processes = filament.config.option<ConfigOptionStrings>("compatible_prints");
            if (processes == nullptr || processes->values.empty())
                return {};
            const PresetWithVendorProfile filament_with_vendor = bundle.filaments.get_preset_with_vendor_profile(filament);
            if (!is_compatible_with_printer(filament_with_vendor, printer_with_vendor))
                return {};
            for (const std::string &name : processes->values)
                if (const Preset *process = bundle.prints.find_preset(name);
                    process != nullptr && process->is_visible && process->is_compatible &&
                    is_compatible_with_print(filament_with_vendor, bundle.prints.get_preset_with_vendor_profile(*process), printer_with_vendor))
                    return name;
            return {};
        };
        // The compatibility flags are still this printer's, from update_compatible() above, which judged the
        // filaments against the printer's own process. A hidden preset cannot be selected, so it is counted,
        // not sliced.
        std::vector<std::pair<const Preset *, std::string>> extras; // (extra preset, process to slice it with)
        size_t                                              hidden = 0;
        for (const PresetCollection *presets : {&bundle.prints, &bundle.filaments})
            for (const Preset &preset : presets->get_presets()) {
                if (!preset.is_system || preset.is_default)
                    continue;
                std::string process;
                if (preset.is_compatible)
                    process = preset.type == Preset::TYPE_PRINT ? preset.name : print_name;
                else if (preset.type == Preset::TYPE_FILAMENT)
                    process = limiting_process(preset);
                if (process.empty() || is_covered(preset))
                    continue;
                if (preset.is_visible)
                    extras.emplace_back(&preset, process);
                else
                    ++hidden;
            }
        if (hidden > 0)
            BOOST_LOG_TRIVIAL(warning) << "Printer \"" << printer << "\": " << hidden
                << " compatible system preset(s) with unexpanded templates are hidden and were not sliced";

        // Each extra slice swaps one preset into the printer's own selection. update_compatible() is not
        // run again: it could swap filament slots. The filament change is not required to ride the tower
        // here (a vase-mode process has none): the printer's own slice above already expanded its hooks.
        for (const auto &[preset, process] : extras) {
            if (is_covered(*preset))
                continue; // an earlier extra slice expanded the same texts
            const bool         is_print = preset->type == Preset::TYPE_PRINT;
            const std::string &filament = is_print ? filament_name : preset->name;
            std::string        extra    = std::string(is_print ? "process" : "filament") + " \"" + preset->name + "\"";
            if (!is_print && process != print_name)
                extra += " and process \"" + process + "\"";
            g_slice_context = vendor_name + " / " + printer + " / " + extra;
            if (is_print)
                ++extra_processes;
            else
                ++extra_filaments;
            cover(*preset);
            bundle.prints.select_preset_by_name(process, /*force=*/true);
            bundle.filaments.select_preset_by_name(filament, /*force=*/true);
            bundle.filament_presets.assign(1, bundle.filaments.get_selected_preset_name());
            bundle.update_multi_material_filament_presets();
            // select_preset_by_name() falls back to another preset, and still returns true, when it cannot select this one.
            if (bundle.prints.get_selected_preset_name() != process || bundle.filaments.get_selected_preset_name() != filament) {
                BOOST_LOG_TRIVIAL(error) << what << " could not select " << extra;
                ++failures;
                continue;
            }
            if (slice_selection(bundle, what + " with " + extra, false, outdir, file_base + "__" + sanitize_filename(preset->name)).empty())
                ++failures;
        }
        // Leave the printer's own selection for the next printer to start from, as without the extra slices.
        bundle.prints.select_preset_by_name(print_name, /*force=*/true);
        bundle.filaments.select_preset_by_name(filament_name, /*force=*/true);
    }
    g_slice_context.clear();

    const int slices = int(printers.size()) + extra_processes + extra_filaments + by_object_slices;
    std::cout << "Sliced " << printers.size() << " printer preset(s), " << extra_processes << " more process preset(s), "
              << extra_filaments << " more filament preset(s) and " << by_object_slices << " printer(s) by object" << std::endl;
    if (failures > 0) {
        std::cout << failures << " of " << slices << " slice(s) failed" << std::endl;
        std::cout << "Validation failed" << std::endl;
        return 1;
    }
    std::cout << "All " << slices << " slice(s) succeeded" << std::endl;
    std::cout << "Validation completed successfully" << std::endl;
    return 0;
}

} // namespace

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
    ("slice,s", po::bool_switch()->default_value(false), "Slice a two-colour cube through every printer, and through every other system process and filament preset whose templates no printer's own slice expands, so every custom g-code and filename_format shipped is expanded, names in {if} branches not taken included (catches placeholder/flow errors that static checks miss). Off unless this flag is present.")
    ("outdir,o", po::value<std::string>()->default_value(""), "With -s, also save each slice's g-code to this folder (as <vendor>__<printer>[__<preset>].gcode) for manual inspection. Optional.")
    ("check_filament_subtypes,f", po::bool_switch()->default_value(true), "Also flag printers with duplicate (ambiguous) filament subtypes. Off unless this flag is present.")
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
    bool        slice_mode           = vm["slice"].as<bool>();
    std::string slice_outdir         = vm["outdir"].as<std::string>();
    bool        check_filament_subtypes = vm["check_filament_subtypes"].as<bool>();

    //  check if path is valid, and return error if not
    if (!fs::exists(path) || !fs::is_directory(path)) {
        std::cerr << "Error: " << path << " is not a valid directory\n";
        return 1;
    }

    // std::cout<<"path: "<<path<<std::endl;
    // std::cout<<"vendor: "<<vendor<<std::endl;
    // std::cout<<"log_level: "<<log_level<<std::endl;

    set_data_dir(path);
    // Orca: the profiles folder lives at <resources>/profiles, so point resources_dir() at that
    // <resources> parent. Without this, resources_dir() is empty and slice mode's HRC lookup
    // (info/nozzle_info.json) resolves to a non-existent relative path and falls back to a
    // built-in table (logging a spurious parse error and dropping the E3D entry).
    if (fs::exists(fs::path(path).parent_path() / "info"))
        set_resources_dir(fs::path(path).parent_path().string());

    auto user_dir = fs::path(Slic3r::data_dir()) / PRESET_USER_DIR;
    user_dir.make_preferred();
    if (!fs::exists(user_dir))
        fs::create_directory(user_dir);

    set_logging_level(log_level);

    // Slice mode expands every printer's custom g-code by actually slicing (see slice_all_printers).
    // A distinct opt-in mode so the default static checks stay fast for every profile PR.
    if (slice_mode)
        return slice_all_printers(vendor, slice_outdir);

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

    if (preset_bundle->has_errors(check_filament_subtypes)) {
        std::cout << "Validation failed" << std::endl;
        return 1;
    }

    std::cout << "Validation completed successfully" << std::endl;
    return 0;
}
