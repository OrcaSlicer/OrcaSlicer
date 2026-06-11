// RequestMapping.cpp — JSON -> SliceCore::SliceRequest parsing.
//
// Implementation of json_to_slice_request(), moved from the anonymous namespace
// in Handlers.cpp to make it independently unit-testable.  The signature and
// semantics are identical to the original; only the following new fields are
// parsed:
//   GAP 1 (Critical): presets.overrides  -> req.presets.overrides
//   GAP 4:            transforms.rotate_x / rotate_y / ensure_on_bed /
//                     convert_unit / assemble

#include "RequestMapping.hpp"

#include "libslic3r/Config.hpp"   // DynamicPrintConfig, ConfigSubstitutionContext,
                                   // ForwardCompatibilitySubstitutionRule

namespace Slic3r {
namespace Server {

using json = nlohmann::json;

SliceCore::SliceRequest json_to_slice_request(const json &j)
{
    SliceCore::SliceRequest req;

    // presets
    if (j.contains("presets")) {
        const auto &p = j["presets"];
        if (p.contains("printer"))
            req.presets.printer_name = p["printer"].get<std::string>();
        if (p.contains("process"))
            req.presets.process_name = p["process"].get<std::string>();
        if (p.contains("filaments") && p["filaments"].is_array())
            req.presets.filament_names = p["filaments"].get<std::vector<std::string>>();
        if (p.contains("load_settings") && p["load_settings"].is_array())
            req.presets.load_settings = p["load_settings"].get<std::vector<std::string>>();
        if (p.contains("load_filaments") && p["load_filaments"].is_array())
            req.presets.load_filaments = p["load_filaments"].get<std::vector<std::string>>();

        // GAP 1: config overrides — lenient (unknown keys are silently skipped).
        if (p.contains("overrides") && p["overrides"].is_object()) {
            ConfigSubstitutionContext subs(ForwardCompatibilitySubstitutionRule::Enable);
            for (auto it = p["overrides"].begin(); it != p["overrides"].end(); ++it) {
                std::string val = it.value().is_string()
                                    ? it.value().get<std::string>()
                                    : it.value().dump();
                req.presets.overrides.set_deserialize(it.key(), val, subs);
            }
        }
    }

    // datadir (optional override; the submit handler defaults this to the
    // server's --datadir when the request omits it).
    if (j.contains("datadir"))
        req.datadir = j["datadir"].get<std::string>();

    // plate
    if (j.contains("plate"))
        req.plate = j["plate"].get<int>();

    // transforms
    if (j.contains("transforms")) {
        const auto &t = j["transforms"];
        if (t.contains("arrange"))     req.transforms.arrange     = t["arrange"].get<int>();
        if (t.contains("orient"))      req.transforms.orient      = t["orient"].get<int>();
        if (t.contains("rotate"))      req.transforms.rotate      = t["rotate"].get<double>();
        if (t.contains("scale"))       req.transforms.scale       = t["scale"].get<double>();
        if (t.contains("repetitions")) req.transforms.repetitions = t["repetitions"].get<int>();
        if (t.contains("skip_objects") && t["skip_objects"].is_array())
            req.transforms.skip_objects = t["skip_objects"].get<std::vector<int>>();

        // GAP 4: additional transform fields present in the Transforms struct
        // but previously not parsed from JSON.
        if (t.contains("rotate_x"))      req.transforms.rotate_x     = t["rotate_x"].get<double>();
        if (t.contains("rotate_y"))      req.transforms.rotate_y     = t["rotate_y"].get<double>();
        if (t.contains("ensure_on_bed")) req.transforms.ensure_on_bed = t["ensure_on_bed"].get<bool>();
        if (t.contains("convert_unit"))  req.transforms.convert_unit  = t["convert_unit"].get<bool>();
        if (t.contains("assemble"))      req.transforms.assemble      = t["assemble"].get<bool>();
    }

    // output
    if (j.contains("output")) {
        const auto &o = j["output"];
        if (o.contains("outputdir"))
            req.output.outputdir = o["outputdir"].get<std::string>();
        if (o.contains("start_print"))
            req.output.start_print = o["start_print"].get<bool>();
        if (o.contains("mode")) {
            const std::string m = o["mode"].get<std::string>();
            if (m == "stdout")    req.output.mode = SliceCore::OutputMode::Stdout;
            // accept both "host" and "printhost" as the print-host mode alias.
            else if (m == "host" || m == "printhost")
                req.output.mode = SliceCore::OutputMode::PrintHost;
            // else leave as File (default)
        }

        // host_config: only meaningful in PrintHost mode.  The deliver() worker
        // builds a PrintHost from these DynamicPrintConfig keys (verified
        // against OctoPrint.cpp:129-130 / PrintHost.cpp:54):
        //   print_host        (string)  <- output.host.url
        //   host_type         (enum)    <- output.host.type  (e.g. "octoprint",
        //                                   "moonraker", "prusalink", …)
        //   printhost_apikey  (string)  <- output.host.apikey
        // set_deserialize_strict() is used for all three so the enum host_type
        // is parsed from its string form exactly as the CLI/GUI does, and the
        // keys are created in the config from the global PrintConfigDef.
        if (o.contains("host") && o["host"].is_object()) {
            const auto &h = o["host"];
            if (h.contains("url"))
                req.output.host_config.set_deserialize_strict(
                    "print_host", h["url"].get<std::string>());
            if (h.contains("type"))
                req.output.host_config.set_deserialize_strict(
                    "host_type", h["type"].get<std::string>());
            if (h.contains("apikey"))
                req.output.host_config.set_deserialize_strict(
                    "printhost_apikey", h["apikey"].get<std::string>());
        }
    }

    // export kind
    if (j.contains("export") && j["export"].contains("kind")) {
        const std::string k = j["export"]["kind"].get<std::string>();
        if (k == "3mf")       req.export_kind = SliceCore::ExportKind::ThreeMF;
        else if (k == "stl")  req.export_kind = SliceCore::ExportKind::Stl;
        // else Gcode (default)
    }

    return req;
}

} // namespace Server
} // namespace Slic3r
