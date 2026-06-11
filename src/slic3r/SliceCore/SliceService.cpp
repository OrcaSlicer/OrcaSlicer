// SliceService.cpp
//
// Real headless FFF slice-to-gcode pipeline. This is the DRY backbone shared by
// the CLI and the (future) HTTP server. It must NOT depend on wxGetApp() or any
// running wxApp / event loop — every value that the GUI CLI normally pulls from
// wxGetApp() is read here from the resolved DynamicPrintConfig instead.
//
// The control flow mirrors the proven slice path currently inlined in CLI::run
// (src/OrcaSlicer.cpp). Signatures bound to (verified against the headers):
//
//   Model::read_from_file(input_file, &config, &config_substitutions, strategy,
//                         &plate_data, ...)              libslic3r/Model.hpp:1598
//   GUI::PartPlateList(Plater*, Model*, PrinterTechnology) src/slic3r/GUI/PartPlate.hpp:689
//   PartPlateList::load_from_3mf_structure(PlateDataPtrs&, int)
//                                                          src/slic3r/GUI/PartPlate.hpp:899
//   PartPlateList::get_plate_count() const                src/slic3r/GUI/PartPlate.hpp:772
//   PartPlate::get_print(PrintBase**, GCodeResult**, int*) src/slic3r/GUI/PartPlate.hpp:307
//   Print::process(long long*, bool)                      libslic3r/Print.hpp:911
//   Print::export_gcode(const std::string&, GCodeProcessorResult*,
//                       ThumbnailsGeneratorCallback)       libslic3r/Print.hpp:914
//   PrintBase::apply(const Model&, const DynamicPrintConfig&)  (PrintBaseWithState)
//   Print::validate(StringObjectException*)               libslic3r/Print.hpp
//   PrintStatistics::total_used_filament (mm)             libslic3r/Print.hpp:794
//   PrintObject::total_layer_count()                      libslic3r/Print.hpp:376
//
// Exit-code values reused from libslic3r/Utils.hpp (CLI_* macros):
//   CLI_SUCCESS(0), CLI_FILE_NOTFOUND(-3), CLI_DATA_FILE_ERROR(-6),
//   CLI_INVALID_PRINTER_TECH(-7), CLI_NO_SUITABLE_OBJECTS(-50),
//   CLI_EXPORT_3MF_ERROR(-13), CLI_SLICING_ERROR(-100).

#include "SliceService.hpp"

#include "PresetResolver.hpp"
#include "OutputTargetDeliver.hpp"
#include "ModelTransforms.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"   // CLI_* exit codes, set_resources_dir()
#include "libslic3r/Format/STL.hpp"       // store_stl()
#include "libslic3r/Format/bbs_3mf.hpp"   // store_bbs_3mf(), StoreParams, SaveStrategy

#include "slic3r/GUI/PartPlate.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include <chrono>
#include <fstream>

namespace Slic3r {
namespace SliceCore {

namespace {

// Monotonic millisecond clock for per-plate slice timing (sliced_ms).
long long now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Read printer technology from the resolved config, defaulting to ptFFF.
// Mirrors get_printer_technology() in OrcaSlicer.cpp:390 but without any wx use.
PrinterTechnology printer_tech_from_config(const DynamicPrintConfig &config)
{
    const auto *opt = config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
    if (opt != nullptr)
        return opt->value;
    return ptFFF;
}

// Compute a layer count for stats by taking the max total_layer_count() over all
// print objects. PrintStatistics has no layer-count field, so this is the cheap
// readily-available source.
int layer_count_of(const Print *print)
{
    int max_layers = 0;
    for (const PrintObject *obj : print->objects()) {
        const int n = static_cast<int>(obj->total_layer_count());
        if (n > max_layers)
            max_layers = n;
    }
    return max_layers;
}

} // namespace

SliceService::SliceService(std::string resources_dir)
    : m_resources_dir(std::move(resources_dir))
{}

SliceResult SliceService::run(const SliceRequest &req)
{
    SliceResult result;
    result.ok        = false;
    result.exit_code = CLI_SUCCESS;

    // Wrap the entire pipeline: NEVER let an exception escape run().
    try {
        // ------------------------------------------------------------------
        // 0) Make resources_dir available to libslic3r lookups (profiles,
        //    text fonts, etc.). Harmless to set repeatedly; pure libslic3r,
        //    no wx involvement.
        // ------------------------------------------------------------------
        if (!m_resources_dir.empty())
            set_resources_dir(m_resources_dir);

        // ------------------------------------------------------------------
        // 1) Resolve the input file path. The request may carry either an
        //    on-disk path or raw bytes (server path). For raw bytes we spill
        //    to a temp file because Model::read_from_file() takes a path.
        //    Uses boost::filesystem — NOT wxFileName::GetTempDir().
        // ------------------------------------------------------------------
        std::string input_file = req.input_path;
        boost::filesystem::path temp_input;       // cleaned up at scope end
        bool have_temp_input = false;

        if (input_file.empty() && !req.input_bytes.empty()) {
            std::string fname = req.input_filename.empty() ? std::string("input.3mf")
                                                           : req.input_filename;
            temp_input = boost::filesystem::temp_directory_path() /
                         boost::filesystem::unique_path("orca-%%%%-%%%%-" + fname);
            {
                boost::filesystem::ofstream ofs(temp_input, std::ios::binary);
                if (!ofs) {
                    result.exit_code = CLI_DATA_FILE_ERROR;
                    result.error     = "failed to create temp input file";
                    return result;
                }
                ofs.write(reinterpret_cast<const char *>(req.input_bytes.data()),
                          static_cast<std::streamsize>(req.input_bytes.size()));
            }
            input_file      = temp_input.string();
            have_temp_input = true;
        }

        if (input_file.empty()) {
            result.exit_code = CLI_INVALID_PARAMS;
            result.error     = "no input provided (neither input_path nor input_bytes)";
            return result;
        }
        if (!boost::filesystem::exists(input_file)) {
            result.exit_code = CLI_FILE_NOTFOUND;
            result.error     = "input file not found: " + input_file;
            return result;
        }

        const bool is_3mf = boost::algorithm::iends_with(input_file, ".3mf");

        if (req.progress) req.progress(0, "loading model");

        // ------------------------------------------------------------------
        // 2) Load the model + embedded config + plate data.
        //    Mirrors OrcaSlicer.cpp:1640 (Model::read_from_file). The clean,
        //    wx-free libslic3r entry point. For .3mf we load model+config+
        //    plate structure; for .stl just the model with a default instance.
        // ------------------------------------------------------------------
        DynamicPrintConfig          embedded_config;   // config embedded in the 3mf (if any)
        ConfigSubstitutionContext   config_substitutions(ForwardCompatibilitySubstitutionRule::Enable);
        PlateDataPtrs               plate_data_src;
        std::vector<Preset *>       project_presets;
        bool                        is_bbl_3mf = false;
        Semver                      file_version;

        LoadStrategy strategy;
        if (is_3mf)
            strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                       LoadStrategy::AddDefaultInstances | LoadStrategy::LoadAuxiliary;
        else
            strategy = LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances;

        Model model;
        try {
            model = Model::read_from_file(
                input_file, &embedded_config, &config_substitutions, strategy,
                &plate_data_src, &project_presets, &is_bbl_3mf, &file_version,
                nullptr, nullptr, nullptr, /*plate_id=*/0);
        } catch (const std::exception &ex) {
            result.exit_code = CLI_DATA_FILE_ERROR;
            result.error     = std::string("failed to read input model: ") + ex.what();
            return result;
        }

        // ------------------------------------------------------------------
        // 3) Build the effective config with the required precedence
        //    (high -> low):
        //        req.presets.overrides  >  resolve() output  >  3MF embedded
        //
        //    Implementation: start from the embedded 3mf config (lowest),
        //    apply the resolved preset config on top, then apply the explicit
        //    overrides last (highest). apply()/apply_only() overwrite keys, so
        //    last-applied wins — giving exactly that precedence.
        //
        //    NOTE: resolve() is implemented by the PresetResolver worker; if no
        //    presets/load files are supplied it returns an empty config, and the
        //    3mf embedded config remains the base (correct for plain .3mf jobs).
        // ------------------------------------------------------------------
        std::string resolve_err;
        DynamicPrintConfig resolved = resolve(req.presets, req.datadir, resolve_err);
        // resolve() reports a hard failure (preset name not found, or a
        // --load_settings / --load_filaments JSON failed to load) via resolve_err.
        // Fail loud — never slice with a wrong/default profile. This mirrors
        // CLI::run exactly (OrcaSlicer.cpp:3644-3648). resolve_err is empty for
        // plain 3mf jobs with no by-name presets / load files, so this is a no-op
        // there and the embedded 3mf config remains the base.
        if (!resolve_err.empty()) {
            result.ok        = false;
            result.exit_code = CLI_CONFIG_FILE_ERROR;
            result.error     = resolve_err;
            return result;
        }

        DynamicPrintConfig config;                 // the effective, merged config
        config.apply(embedded_config, /*ignore_nonexistent=*/true);   // base
        config.apply(resolved,        /*ignore_nonexistent=*/true);   // mid
        config.apply(req.presets.overrides, /*ignore_nonexistent=*/true); // top

        // Normalize after merge — same as OrcaSlicer.cpp:3617.
        config.normalize_fdm();

        // Determine printer technology from config (NOT from wxGetApp()).
        PrinterTechnology printer_technology = printer_tech_from_config(config);
        if (printer_technology == ptUnknown)
            printer_technology = ptFFF;
        if (printer_technology != ptFFF) {
            // SLA is out of scope (TODO below); only FFF supported here.
            result.exit_code = CLI_INVALID_PRINTER_TECH;
            result.error     = "only FFF printer technology is supported";
            return result;
        }
        // Persist the resolved technology back into the config.
        config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology", true)->value =
            printer_technology;

        // Fill in FFF defaults and synchronize, mirroring OrcaSlicer.cpp:3627-3629.
        {
            FullPrintConfig fff_defaults;
            fff_defaults.apply(config, /*ignore_nonexistent=*/true);
            config.apply(fff_defaults, /*ignore_nonexistent=*/true);
        }

        // Apply per-object transforms (scale, rotate, orient, convert_unit,
        // ensure_on_bed) in CLI order. Must run BEFORE PartPlateList is built
        // so the plate geometry reflects the transformed model.
        {
            std::string transforms_err;
            if (!apply_model_transforms(model, req.transforms, transforms_err)) {
                result.exit_code = CLI_INVALID_PARAMS;
                result.error     = transforms_err;
                return result;
            }
        }

        // Apply per-object placement overrides (position, rotation, scale,
        // mirror, explicit instances, orient, ensure_on_bed, skip_objects).
        // Runs after apply_model_transforms so global transforms are already
        // in place; runs before PartPlateList so instance positions are
        // finalised before plate assignment.
        if (!req.objects.empty() || !req.transforms.skip_objects.empty()) {
            std::string placement_err;
            if (!apply_object_placements(model, req.objects,
                                         req.transforms.skip_objects,
                                         req.transforms.assemble,
                                         config,
                                         result.warnings,
                                         placement_err)) {
                result.exit_code = CLI_INVALID_PARAMS;
                result.error     = placement_err;
                return result;
            }
        }

        // Determine whether any ObjectPlacement carries explicit per-instance
        // positions.  When such explicit instances exist the global arrange /
        // repetitions step must be skipped for those objects; because
        // arrange_objects / duplicate operate on ALL objects simultaneously
        // (not object-by-object) the safest policy is to skip the entire
        // arrange step when ANY object has explicit instances.  Callers that
        // need both explicit per-object instances AND global arrangement should
        // run two separate slice jobs.
        const bool has_explicit_instances = [&req]() {
            for (const ObjectPlacement &op : req.objects)
                if (!op.instances.empty())
                    return true;
            return false;
        }();

        // Apply arrange / duplicate (best-effort; non-fatal if bed shape is
        // unavailable from the config). Must also run before PartPlateList so
        // the instance positions are finalised before plate assignment.
        // Skipped when any ObjectPlacement carries explicit instances (see above).
        if (!has_explicit_instances) {
            std::string arrange_err;
            apply_arrange_or_duplicate(model, req.transforms, config, arrange_err);
            // arrange_err is informational only; the call never returns false for
            // hard failures — bed-shape absence is treated as a no-op.
        } else {
            result.warnings.push_back(
                "global arrange/duplicate skipped because one or more objects "
                "have explicit per-instance placements");
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore] arrange skipped — explicit instances present";
        }

        if (req.progress) req.progress(10, "preparing plates");

        // ------------------------------------------------------------------
        // 4) Build the PartPlateList (NULL plater -> headless-safe; thumbnail
        //    branches inside self-skip when plater is NULL).
        //    OrcaSlicer.cpp:3705 — PartPlateList(NULL, &model, printer_technology).
        // ------------------------------------------------------------------
        Slic3r::GUI::PartPlateList partplate_list(/*plater=*/nullptr, &model, printer_technology);

        if (!plate_data_src.empty()) {
            // 3mf path: populate plates from the embedded plate structure.
            // OrcaSlicer.cpp:4057.
            partplate_list.load_from_3mf_structure(plate_data_src);
        } else {
            // STL / non-3mf path: there is no embedded plate structure. The list
            // already created a default plate. Attach the loaded model objects to
            // whichever plate they intersect.
            //
            // TODO(WU-arrange): without arrange the object sits at its loaded
            // position and may not intersect the bed; proper placement requires the
            // arrange WU. reload_all_objects() is best-effort and correct when the
            // geometry already lies on the plate.
            //
            // A non-zero return means object loading failed — fail loud rather than
            // falling through to an empty-print (the print->empty() check below is
            // only a partial backstop).
            const int reload_ret = partplate_list.reload_all_objects();
            if (reload_ret != 0) {
                result.ok        = false;
                result.exit_code = CLI_DATA_FILE_ERROR;
                result.error     = "failed to load objects from input";
                return result;
            }
        }

        const int plate_count = partplate_list.get_plate_count();
        if (plate_count <= 0) {
            result.exit_code = CLI_NO_SUITABLE_OBJECTS;
            result.error     = "no plates to slice";
            return result;
        }

        // Which plates? req.plate == 0 -> all plates; otherwise the 1-based index.
        int first_plate = 0;
        int last_plate  = plate_count - 1;     // inclusive
        if (req.plate != 0) {
            const int idx = req.plate - 1;     // request is 1-based
            if (idx < 0 || idx >= plate_count) {
                result.exit_code = CLI_INVALID_PARAMS;
                result.error     = "plate index out of range: " + std::to_string(req.plate);
                return result;
            }
            first_plate = idx;
            last_plate  = idx;
        }

        // Resolve the output directory for File mode. Default to CWD if unset.
        boost::filesystem::path out_dir;
        if (req.output.mode == OutputMode::File) {
            out_dir = req.output.outputdir.empty()
                          ? boost::filesystem::current_path()
                          : boost::filesystem::path(req.output.outputdir);
            boost::system::error_code ec;
            boost::filesystem::create_directories(out_dir, ec);
            if (ec) {
                result.exit_code = CLI_EXPORT_3MF_ERROR;
                result.error     = "failed to create output directory: " + out_dir.string();
                return result;
            }
        }

        // Temp dir for Stdout / PrintHost intermediate gcode.
        boost::filesystem::path temp_dir = boost::filesystem::temp_directory_path();

        // ------------------------------------------------------------------
        // 5) Slice each target plate.
        // ------------------------------------------------------------------
        bool any_export_failed = false;

        for (int index = first_plate; index <= last_plate; ++index) {
            const long long plate_t0 = now_ms();

            Slic3r::GUI::PartPlate *part_plate = partplate_list.get_plate(index);
            if (part_plate == nullptr) {
                result.exit_code = CLI_DATA_FILE_ERROR;
                result.error     = "null plate at index " + std::to_string(index);
                return result;
            }

            // get_print() returns the plate's owned Print + GCodeResult by pointer.
            // OrcaSlicer.cpp:5696 / PartPlate.hpp:307 — pure pointer return.
            PrintBase                *print_base   = nullptr;
            Slic3r::GUI::GCodeResult *gcode_result = nullptr;
            int                       print_index  = 0;
            part_plate->get_print(&print_base, &gcode_result, &print_index);

            Print *print = dynamic_cast<Print *>(print_base);   // FFF
            if (print == nullptr) {
                result.exit_code = CLI_INVALID_PRINTER_TECH;
                result.error     = "plate " + std::to_string(index + 1) +
                                   ": not an FFF print";
                return result;
            }

            if (req.progress)
                req.progress(20, "applying config to plate " + std::to_string(index + 1));

            // Fold in per-plate config overrides (plate-level print_sequence, bed
            // type, and any plate-specific settings stored in the 3MF plate
            // structure) on TOP of the global effective config. The CLI does the
            // same — new_print_config.apply(*part_plate->config()) at
            // OrcaSlicer.cpp:6026 — so every plate of a multi-plate job slices with
            // its own settings, not just the global ones. part_plate->config() is a
            // plain wx-free accessor (PartPlate.hpp:244) returning DynamicPrintConfig*.
            DynamicPrintConfig plate_cfg = config;
            if (const DynamicPrintConfig *pc = part_plate->config()) {
                if (!pc->empty())
                    plate_cfg.apply(*pc);   // plate keys win over global
            }

            // Apply model + per-plate effective config to the print (PrintBase::apply).
            // OrcaSlicer.cpp:6087.
            print->apply(model, plate_cfg);

            // Set the is_BBL_printer flag BEFORE validate() — validation depends on
            // it (OrcaSlicer.cpp:6091-6106). Derive it exactly like the CLI: from the
            // "printer_model" string ("Bambu Lab" prefix). The CLI's fallback to the
            // printer-name string is only used when printer_model is empty; here the
            // resolved/embedded config always carries printer_model (added "true"
            // creates it if missing), so the prefix test is the faithful mirror.
            {
                const std::string &printer_model =
                    plate_cfg.opt_string("printer_model", /*create=*/true);
                const bool is_bbl_vendor_preset =
                    (printer_model.compare(0, 9, "Bambu Lab") == 0);
                print->is_BBL_printer() = is_bbl_vendor_preset;
            }

            // Validate before slicing (catches incompatible params). OrcaSlicer.cpp:6110.
            StringObjectException warning;
            StringObjectException err = print->validate(&warning);
            if (!err.string.empty()) {
                result.exit_code = CLI_SLICING_ERROR;
                result.error     = "plate " + std::to_string(index + 1) +
                                   " validation failed: " + err.string;
                return result;
            }

            if (print->empty()) {
                // Nothing inside the print volume — same condition as
                // OrcaSlicer.cpp:6101. For STL this is the most likely failure
                // mode until the arrange WU lands.
                result.exit_code = CLI_NO_SUITABLE_OBJECTS;
                result.error     = "plate " + std::to_string(index + 1) +
                                   ": nothing to slice (print empty or no object "
                                   "fully inside the print volume)";
                return result;
            }

            if (req.progress)
                req.progress(30, "slicing plate " + std::to_string(index + 1));

            // ----- Slice (always required regardless of export kind).
            try {
                // Slice. Pure compute, wx-free, TBB-parallel. OrcaSlicer.cpp:6161.
                print->process();
            } catch (const std::exception &ex) {
                result.ok        = false;
                result.exit_code = CLI_SLICING_ERROR;
                result.error     = "plate " + std::to_string(index + 1) +
                                   " slicing failed: " + ex.what();
                return result;
            }

            // ----- Export: branch on req.export_kind.
            //       STL and 3MF are only meaningful for OutputMode::File.
            //       Stdout / PrintHost always fall through to Gcode (export_kind
            //       is ignored for non-file modes).
            std::string written_path;

            if (req.export_kind == ExportKind::Gcode ||
                req.output.mode != OutputMode::File) {
                // ---- G-code path (default, or forced for non-file output modes).
                boost::filesystem::path gcode_path;
                const std::string plate_file =
                    "plate_" + std::to_string(index + 1) + ".gcode";
                if (req.output.mode == OutputMode::File) {
                    gcode_path = out_dir / plate_file;
                } else {
                    gcode_path = temp_dir /
                        boost::filesystem::unique_path("orca-%%%%-%%%%-" + plate_file);
                }

                if (req.progress)
                    req.progress(80, "exporting gcode for plate " + std::to_string(index + 1));

                // Export gcode. Empty thumbnail callback (nullptr) — GL/GLFW skipped.
                // OrcaSlicer.cpp:6215.
                try {
                    written_path =
                        print->export_gcode(gcode_path.string(), gcode_result,
                                            /*thumbnail_cb=*/nullptr);
                } catch (const std::exception &ex) {
                    result.ok        = false;
                    result.exit_code = CLI_SLICING_ERROR;
                    result.error     = "plate " + std::to_string(index + 1) +
                                       " gcode export: " + ex.what();
                    return result;
                }

                if (written_path.empty()) {
                    result.ok        = false;
                    result.exit_code = CLI_SLICING_ERROR;
                    result.error     = "export_gcode produced no output for plate " +
                                       std::to_string(index + 1);
                    return result;
                }

            } else if (req.export_kind == ExportKind::Stl) {
                // ---- STL export: one file per model object.
                //      store_stl(path, ModelObject*, bool binary)
                //      Signature confirmed in libslic3r/Format/STL.hpp:16.
                if (req.progress)
                    req.progress(80, "exporting STL for plate " + std::to_string(index + 1));

                // Export each object to its own .stl file; record the last path
                // written in written_path for the stat record.
                bool stl_ok = true;
                for (size_t oi = 0; oi < model.objects.size(); ++oi) {
                    const std::string stl_name =
                        "object_" + std::to_string(oi) + ".stl";
                    const boost::filesystem::path stl_path = out_dir / stl_name;
                    if (!Slic3r::store_stl(stl_path.string().c_str(),
                                           model.objects[oi], /*binary=*/true)) {
                        result.ok        = false;
                        result.exit_code = CLI_EXPORT_STL_ERROR;
                        result.error     = "store_stl failed for object " +
                                           std::to_string(oi) + " on plate " +
                                           std::to_string(index + 1);
                        stl_ok = false;
                        break;
                    }
                    written_path = stl_path.string();
                }
                if (!stl_ok)
                    return result;
                if (written_path.empty()) {
                    // No objects — treat as slicing error.
                    result.ok        = false;
                    result.exit_code = CLI_NO_SUITABLE_OBJECTS;
                    result.error     = "no model objects to export as STL for plate " +
                                       std::to_string(index + 1);
                    return result;
                }

            } else {
                // ---- 3MF export.
                //      store_bbs_3mf(StoreParams&) -> bool
                //      StoreParams fields confirmed in libslic3r/Format/bbs_3mf.hpp:227.
                //      Modelled on CLI::export_project (OrcaSlicer.cpp:7448-7465).
                if (req.progress)
                    req.progress(80, "exporting 3MF for plate " + std::to_string(index + 1));

                const std::string tmf_name =
                    "plate_" + std::to_string(index + 1) + ".3mf";
                const boost::filesystem::path tmf_path = out_dir / tmf_name;
                // Keep the std::string alive for the duration of the StoreParams use.
                const std::string tmf_path_str = tmf_path.string();

                StoreParams store_params;
                store_params.path             = tmf_path_str.c_str();
                store_params.model            = &model;
                store_params.export_plate_idx = index;
                // No pre-sliced plate data / thumbnails available in headless mode;
                // use a minimal save strategy (Silence suppresses progress output).
                store_params.strategy =
                    SaveStrategy::Zip64 | SaveStrategy::Silence;
                // config pointer: point at the effective merged config.
                store_params.config = const_cast<DynamicPrintConfig *>(&config);

                if (!Slic3r::store_bbs_3mf(store_params)) {
                    result.ok        = false;
                    result.exit_code = CLI_EXPORT_3MF_ERROR;
                    result.error     = "store_bbs_3mf failed for plate " +
                                       std::to_string(index + 1);
                    return result;
                }
                written_path = tmf_path_str;
            }

            const long long plate_t1 = now_ms();

            // ----- Collect stats.
            PlateStat stat;
            stat.plate_id  = index + 1;                       // 1-based, like the CLI
            stat.sliced_ms = plate_t1 - plate_t0;             // wall time for this plate
            // total_used_filament is in mm (PrintStatistics). OrcaSlicer convention.
            stat.filament_used_mm = print->print_statistics().total_used_filament;
            stat.layer_count      = layer_count_of(print);    // max object layer count

            // ----- Tier-1 structured preview stats from GCodeProcessorResult.
            // gcode_result is non-null only after export_gcode for the Gcode path;
            // the Stl/3MF export paths don't produce gcode, so guard carefully.
            // GCodeResult is typedef'd to GCodeProcessorResult (PartPlate.hpp:75).
            //
            // Confirmed fields (GCodeProcessor.hpp:46-260):
            //   print_statistics.modes[0].time     — float, seconds, Normal mode
            //   initial_layer_time                 — float, seconds
            //   custom_gcode_per_print_z           — std::vector<CustomGCode::Item>
            //   print_statistics.model_volumes_per_extruder — std::map<size_t,double>
            if (gcode_result != nullptr &&
                (req.export_kind == ExportKind::Gcode ||
                 req.output.mode != OutputMode::File)) {
                const auto &ps = gcode_result->print_statistics;
                // modes is std::array<Mode, 2>; [0]=Normal, [1]=Stealth
                // (GCodeProcessor.hpp:48 — ETimeMode::Normal=0).
                stat.estimated_print_time_s =
                    static_cast<double>(ps.modes[0].time);
                stat.initial_layer_time_s =
                    static_cast<double>(gcode_result->initial_layer_time);
                stat.color_change_count =
                    static_cast<int>(gcode_result->custom_gcode_per_print_z.size());
                // model_volumes_per_extruder: map<size_t, double>.
                // Cast size_t key to int for the public PlateStat API.
                for (const auto &kv : ps.model_volumes_per_extruder)
                    stat.filament_volume_per_extruder[static_cast<int>(kv.first)] =
                        kv.second;
                // thumbnail_generated stays false — thumbnail work is a separate task.
            }

            // ----- Deliver / record output path.
            //       For non-Gcode export kinds the output is already written to
            //       out_dir; deliver() is only relevant for Gcode Stdout/PrintHost.
            if (req.output.mode == OutputMode::File ||
                req.export_kind != ExportKind::Gcode) {
                stat.gcode_path = written_path;
            } else {
                // Stdout / PrintHost Gcode — hand off to deliver().
                std::string deliver_err;
                const bool delivered = deliver(req.output, written_path, deliver_err);
                if (!delivered) {
                    any_export_failed = true;
                    if (!result.error.empty()) result.error += "; ";
                    result.error += "plate " + std::to_string(index + 1) +
                                    " deliver failed: " + deliver_err;
                }
                stat.gcode_path = written_path;
            }

            result.plates.push_back(std::move(stat));

            if (req.progress)
                req.progress(100, "plate " + std::to_string(index + 1) + " done");
        }

        // ------------------------------------------------------------------
        // 6) Final status.
        // ------------------------------------------------------------------
        if (any_export_failed) {
            // deliver() failed for at least one plate (e.g. not-yet-implemented
            // PrintHost routing). Slicing itself succeeded, but delivery did not.
            result.ok        = false;
            result.exit_code = CLI_EXPORT_3MF_ERROR;   // generic export/delivery failure
            // result.error already populated above.
        } else {
            result.ok        = true;
            result.exit_code = CLI_SUCCESS;
        }

        // ------------------------------------------------------------------
        // Temp input cleanup (best-effort).
        // ------------------------------------------------------------------
        if (have_temp_input) {
            boost::system::error_code ec;
            boost::filesystem::remove(temp_input, ec);
        }

        return result;

        // TODO(WU-sla): SLA technology (ptSLA) — currently rejected above.
    }
    catch (const std::bad_alloc &) {
        result.ok        = false;
        result.exit_code = CLI_OUT_OF_MEMORY;
        result.error     = "out of memory during slicing";
        return result;
    }
    catch (const std::exception &ex) {
        // Any slicing/export error -> CLI_SLICING_ERROR, matching OrcaSlicer.cpp:6290.
        result.ok        = false;
        result.exit_code = CLI_SLICING_ERROR;
        result.error     = std::string("slicing failed: ") + ex.what();
        return result;
    }
    catch (...) {
        result.ok        = false;
        result.exit_code = CLI_SLICING_ERROR;
        result.error     = "slicing failed: unknown exception";
        return result;
    }
}

} // namespace SliceCore
} // namespace Slic3r
