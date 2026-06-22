#include "GPXExport.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace {

std::string shell_quote(const std::string& s)
{
#ifdef _WIN32
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

std::string getenv_string(const char* name)
{
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

// Generischer Options-Zugriff über die gemeinsame ConfigBase-Schnittstelle -
// funktioniert identisch für PrintConfig (StaticConfig) und DynamicPrintConfig
// (DynamicConfig), im Gegensatz zu .has()/.opt_string(), die nur auf
// DynamicConfig existieren.
std::string config_string_if_present(const PrintConfig& config, const std::string& key)
{
    if (const auto* opt = config.option(key))
        if (const auto* s = dynamic_cast<const ConfigOptionString*>(opt))
            return s->value;
    return {};
}

std::string printer_model_string(const PrintConfig& config)
{
    std::string model = config_string_if_present(config, "printer_model");
    if (model.empty())
        model = config_string_if_present(config, "model_id");
    if (model.empty())
        model = config_string_if_present(config, "printer_notes");
    return model;
}

// Anzahl konfigurierter Extruder (Düsendurchmesser-Array-Länge) - dient zur
// Single-/Dual-Extruder-Unterscheidung innerhalb derselben Baureihe
// (z.B. Replicator 1 single vs. dual, TOM single vs. dual).
int extruder_count(const PrintConfig& config)
{
    if (const auto* opt = config.option("nozzle_diameter"))
        if (const auto* v = dynamic_cast<const ConfigOptionFloats*>(opt))
            return std::max<int>(1, static_cast<int>(v->values.size()));
    return 1;
}

bool contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

std::string GPXExport::find_gpx_binary()
{
    // Developer override, useful for AppImage / local dev builds.
    if (std::string env = getenv_string("ORCA_GPX_BIN"); !env.empty())
        return env;

#ifdef _WIN32
    return "gpx.exe";
#else
    return "gpx";
#endif
}

// ── GPX-Maschinen-Zuordnung ──────────────────────────────────────────────────
//
// Die hier verwendeten Kürzel sind 1:1 aus dem tatsächlichen GPX-Quellcode
// übernommen und gegen die offiziellen MakerBot-Desktop-"bot_type"-Werte
// abgeglichen (Quellen: github.com/markwal/GPX, src/shared/std_machines.h;
// sowie Library/MakerBot/default_configs/*.json aus der offiziellen
// MakerBot-Print-Installation):
//
//   bot_type (MakerBot)      Achsen/Tools (offiziell)        GPX -m
//   -----------------------  -------------------------------  ------
//   tomstepstrudersingle     1 Tool (Mk7), X106 Y120 Z106      t7   (t6 ist baugleich)
//   (TOM, 2 Tools)           2 Tools                            t7d
//   replicatorsingle         1 Tool (Mk8/A),  X225 Y145 Z150    r1
//   replicatordual           2 Tools (Mk8/A+B)                  r1d
//   replicator2              1 Tool (Mk8/A)                     r2
//   replicator2x             2 Tools (Mk8/A+B), X246 Y152 Z155  r2x
//
// Cupcake wurde von MakerBot Desktop/Print nie offiziell geführt (das Gerät
// ist älter als diese Software) - die c3/c4/cp4/cpp-Kürzel stammen direkt aus
// GPX selbst (Gen3/Gen4/Pololu-Elektronik-Varianten).
std::string GPXExport::gpx_machine_for_config(const PrintConfig& config)
{
    // Optional future profile key. It is intentionally read defensively so old
    // profiles still load if the key does not exist in PrintConfig yet.
    if (std::string explicit_machine = config_string_if_present(config, "gpx_machine_type"); !explicit_machine.empty())
        return explicit_machine;

    const std::string model = boost::algorithm::to_lower_copy(printer_model_string(config));
    const int extruders = extruder_count(config);

    // Replicator 2X (immer 2 Extruder, eigenes Kürzel unabhängig von extruders)
    if (contains(model, "2x"))
        return "r2x";

    // Replicator 2 (nicht-X). "hbp"/"heated" -> beheiztes Druckbett-Mod.
    if (contains(model, "replicator 2") || contains(model, "replicator2")) {
        if (contains(model, "hbp") || contains(model, "heated"))
            return "r2h";
        return "r2";
    }

    // Replicator 1 / "Original" (Single oder Dual je nach Extruderzahl)
    if (contains(model, "replicator")) {
        if (contains(model, "dual") || extruders >= 2)
            return "r1d";
        return "r1";
    }

    // Thing-O-Matic
    if (contains(model, "thing-o-matic") || contains(model, "thing o matic") || contains(model, "tom")) {
        if (extruders >= 2)
            return "t7d";
        if (contains(model, "mk6"))
            return "t6";
        return "t7"; // Mk7 - und mechanisch identisch zu Mk6, daher unkritischer Default
    }

    // Cupcake-Varianten (Elektronik/Extruder-Generation)
    if (contains(model, "cupcake")) {
        if (contains(model, "pololu") && (contains(model, "gen4") || contains(model, "mk5") || contains(model, "mk6")))
            return "cp4";
        if (contains(model, "pololu"))
            return "cpp";
        if (contains(model, "gen4") || contains(model, "g4"))
            return "c4";
        return "c3"; // Gen3 - verbreitetster/ältester Cupcake-Stand
    }

    // Kein Modellname erkannt: sicherer Fallback auf die in diesem Projekt
    // verbreitetste Dual-Legacy-Maschine.
    BOOST_LOG_TRIVIAL(warning) << "GPXExport: could not identify printer_model '" << model
        << "' for GPX machine mapping, falling back to r2x. Set 'gpx_machine_type' explicitly to override.";
    return "r2x";
}

bool GPXExport::export_to_x3g(
    const std::string& gcode_filepath,
    const std::string& output_filepath,
    const PrintConfig& config,
    std::string* error_message)
{
    auto fail = [&](const std::string& msg) {
        BOOST_LOG_TRIVIAL(error) << "GPXExport: " << msg;
        if (error_message) *error_message = msg;
        return false;
    };

    if (!fs::exists(gcode_filepath))
        return fail("Input G-code file does not exist: " + gcode_filepath);

    const std::string gpx_binary = find_gpx_binary();
    const std::string machine    = gpx_machine_for_config(config);

    fs::path out_path(output_filepath);
    if (!out_path.parent_path().empty()) {
        boost::system::error_code ec;
        fs::create_directories(out_path.parent_path(), ec);
    }

    std::ostringstream cmd;
    cmd << shell_quote(gpx_binary)
        << " -v -g -m " << shell_quote(machine);

    // Optional developer overrides. These keep GPX native to Orca while allowing
    // a tuned GPX .ini during development without exposing it as an Orca post script.
    const std::string ini_env = getenv_string("ORCA_GPX_INI");
    if (!ini_env.empty())
        cmd << " -c " << shell_quote(ini_env);

    cmd << " " << shell_quote(gcode_filepath)
        << " " << shell_quote(output_filepath);

    BOOST_LOG_TRIVIAL(info) << "GPXExport: running " << cmd.str();
    const int rc = std::system(cmd.str().c_str());
    if (rc != 0)
        return fail("GPX returned non-zero exit code (" + std::to_string(rc) +
                     "). Is gpx installed and on PATH? See ORCA_GPX_BIN to point at a specific binary.");

    if (!fs::exists(output_filepath))
        return fail("GPX completed but did not create output file: " + output_filepath);

    return true;
}

// ── Dispatch-friendly wrapper ────────────────────────────────────────────────

std::string GPXExport::get_archive_extension(GCodeFlavor flavor)
{
    return flavor == gcfMakerBotLegacy ? ".x3g" : ".gcode";
}

std::string GPXExport::pack_to_archive(const std::string& gcode_path, const PrintConfig& config)
{
    if (config.gcode_flavor != gcfMakerBotLegacy)
        return {}; // nicht unsere Flavor - nichts zu tun

    const fs::path gcode_p(gcode_path);

    // BUG FIX (2026-06-18): Since the export dialog (Plater.cpp) now names the
    // final path with .x3g directly, `gcode_path` here is frequently ALREADY
    // the desired final .x3g path - but its CONTENT is still plain G-code text
    // (finalize_gcode()'s copy_file() doesn't care about extensions). The old
    // code below derived archive_path by stripping and re-appending ".x3g",
    // which collapses to the SAME path as gcode_path in that case - gpx would
    // be asked to read and write the identical file, and the cleanup
    // fs::remove(gcode_path) afterwards then deleted the just-created result.
    // Confirmed via real export: "Exported successfully" toast shown, but no
    // file left on disk. Fix: if gcode_path already ends in .x3g, stage the
    // G-code content aside to a sibling temp file first, convert FROM there
    // TO the real target path, then remove the temp file - never collapse
    // input and output into the same path.
    std::string archive_path;
    std::string gcode_source = gcode_path;
    bool        used_temp_source = false;

    if (gcode_p.extension() == ".x3g") {
        archive_path = gcode_path;
        gcode_source = gcode_path + ".tmp_gcode_for_gpx";
        try {
            fs::rename(gcode_path, gcode_source);
            used_temp_source = true;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "GPXExport: could not stage temp G-code for " << gcode_path << ": " << e.what();
            return {};
        }
    } else {
        archive_path = (gcode_p.parent_path() / (gcode_p.stem().string() + ".x3g")).string();
    }

    std::string error;
    if (!export_to_x3g(gcode_source, archive_path, config, &error)) {
        BOOST_LOG_TRIVIAL(warning) << "GPXExport: failed for " << gcode_source << ": " << error;
        // Best effort: give the user back their G-code instead of leaving nothing.
        if (used_temp_source) {
            try { fs::rename(gcode_source, gcode_path); } catch (...) {}
        }
        return {};
    }

    try { fs::remove(gcode_source); } catch (...) {}

    BOOST_LOG_TRIVIAL(info) << "GPXExport: x3g archive created: " << archive_path;
    return archive_path;
}

} // namespace Slic3r
