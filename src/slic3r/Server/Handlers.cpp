#include "Handlers.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/filesystem/operations.hpp>

#include <nlohmann/json.hpp>

#include "SliceTypes.hpp"
#include "libslic3r/Config.hpp"   // DynamicPrintConfig, set_deserialize_strict
#include "RequestMapping.hpp"     // json_to_slice_request (extracted for testability)

namespace Slic3r {
namespace Server {

namespace http = boost::beast::http;
using json     = nlohmann::json;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Escape a string for embedding in a JSON string literal (minimal: handles
// quotes and backslashes; control chars in filenames are unlikely).
std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else                out += c;
    }
    return out;
}

// Build a plain JSON error response.
Response make_error(unsigned status, const std::string &msg, bool keep_alive = false)
{
    Response res{http::status{status}, 11};
    res.set(http::field::content_type, "application/json");
    res.keep_alive(keep_alive);
    res.body() = "{\"error\":\"" + json_escape(msg) + "\"}";
    res.prepare_payload();
    return res;
}

// Build a JSON response from a pre-serialised string.
Response make_json(unsigned status, const std::string &body, bool keep_alive = false)
{
    Response res{http::status{status}, 11};
    res.set(http::field::content_type, "application/json");
    res.keep_alive(keep_alive);
    res.body() = body;
    res.prepare_payload();
    return res;
}

// Build a JobState string for JSON.
const char *state_str(JobState s)
{
    switch (s) {
    case JobState::Queued:    return "queued";
    case JobState::Running:   return "running";
    case JobState::Done:      return "done";
    case JobState::Error:     return "error";
    case JobState::Cancelled: return "cancelled";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Multipart parser
//
// Parses the simplest form of multipart/form-data:
//   - boundary identified from the Content-Type header value
//   - each part has headers terminated by \r\n\r\n
//   - final boundary suffix is --
//
// Only two parts are expected:
//   "model"  — arbitrary binary (model file bytes)
//   "config" — UTF-8 JSON text
//
// This parser is intentionally minimal and well-commented.  It handles the
// common curl / browser POST encoding.  It does not support nested multipart,
// quoted-printable transfer encoding, or charset declarations.
// ---------------------------------------------------------------------------

struct MultipartPart {
    std::string name;
    std::string filename; // empty if this is a text field
    std::vector<uint8_t> data;
};

// Extract the value of a named parameter from a header value string.
// E.g.  headerval = "form-data; name=\"model\"; filename=\"mesh.3mf\""
//       param     = "name"   -> returns "model"
std::string extract_param(const std::string &headerval, const std::string &param)
{
    const std::string search = param + "=\"";
    auto pos = headerval.find(search);
    if (pos == std::string::npos)
        return {};
    pos += search.size();
    auto end = headerval.find('"', pos);
    if (end == std::string::npos)
        return {};
    return headerval.substr(pos, end - pos);
}

// Returns the boundary string from a Content-Type header value such as:
//   "multipart/form-data; boundary=----WebKitFormBoundary7MA4YWx"
std::string parse_boundary(const std::string &content_type)
{
    const std::string key = "boundary=";
    auto pos = content_type.find(key);
    if (pos == std::string::npos)
        return {};
    std::string bnd = content_type.substr(pos + key.size());
    // Strip leading/trailing whitespace and optional quotes.
    if (!bnd.empty() && bnd.front() == '"') bnd = bnd.substr(1);
    if (!bnd.empty() && bnd.back()  == '"') bnd.pop_back();
    auto sp = bnd.find(' ');
    if (sp != std::string::npos) bnd = bnd.substr(0, sp);
    return bnd;
}

// Parse multipart body.  Returns false if parsing fails.
bool parse_multipart(const std::string &body,
                     const std::string &boundary,
                     std::vector<MultipartPart> &parts)
{
    // Delimiter between parts is "\r\n--<boundary>" (or "--<boundary>" at the
    // very start of the body).
    const std::string delim = "--" + boundary;
    const std::string crlf  = "\r\n";

    // Find the first delimiter.
    auto pos = body.find(delim);
    if (pos == std::string::npos)
        return false;
    pos += delim.size();

    while (true) {
        // After delimiter: either "--" (final), "\r\n" (part header follows),
        // or "\n" (lenient — some clients omit CR).
        if (pos + 2 <= body.size() && body.substr(pos, 2) == "--")
            break; // End of multipart body.

        // Skip the CRLF (or LF) after the boundary.
        if (pos < body.size() && body[pos] == '\r') ++pos;
        if (pos < body.size() && body[pos] == '\n') ++pos;

        // Read part headers until \r\n\r\n.
        auto header_end = body.find("\r\n\r\n", pos);
        if (header_end == std::string::npos)
            return false;

        std::string raw_headers = body.substr(pos, header_end - pos);
        pos = header_end + 4; // skip \r\n\r\n

        // Parse Content-Disposition from headers.
        MultipartPart part;
        std::istringstream hs(raw_headers);
        std::string line;
        while (std::getline(hs, line)) {
            // Strip CR if present.
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // Case-insensitive prefix check for "content-disposition:".
            std::string lower_line = line;
            for (char &c : lower_line) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

            if (lower_line.substr(0, 20) == "content-disposition:") {
                // Note: use original `line` to preserve case in values.
                part.name     = extract_param(line, "name");
                part.filename = extract_param(line, "filename");
            }
        }

        if (part.name.empty())
            return false; // Malformed part — no name.

        // Find the next delimiter to locate end of part data.
        const std::string next_delim = crlf + delim;
        auto data_end = body.find(next_delim, pos);
        if (data_end == std::string::npos)
            return false;

        const char *data_ptr = body.data() + pos;
        size_t data_len      = data_end - pos;
        part.data.assign(data_ptr, data_ptr + data_len);

        parts.push_back(std::move(part));

        // Advance past the data and the \r\n--<boundary> we found.
        pos = data_end + next_delim.size();
    }

    return !parts.empty();
}

// ---------------------------------------------------------------------------
// Base64 decoder (RFC 4648, no padding enforcement beyond correct sizing)
// ---------------------------------------------------------------------------

int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+')              return 62;
    if (c == '/')              return 63;
    return -1; // padding or invalid
}

std::vector<uint8_t> base64_decode(const std::string &s)
{
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);

    int buf = 0, bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ')
            continue;
        int v = b64val(c);
        if (v < 0) continue; // skip unknown chars defensively
        buf  = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// json_to_slice_request: see RequestMapping.hpp / RequestMapping.cpp.
// The function is declared in the Slic3r::Server namespace and is therefore
// directly visible to all handler implementations below.

} // anonymous namespace

// ---------------------------------------------------------------------------
// Handler implementations
// ---------------------------------------------------------------------------

// GET /v1/profiles?type=printer|process|filament
//
// Enumerates preset NAMES from a PresetBundle loaded from ctx.datadir (served
// from a mutex-guarded cache). Returns 200 {"type":..,"names":[...]}.
//   400 — missing/invalid ?type=
//   500 — datadir missing/unreadable, or bundle load failed (loud, not silent)
Response handle_profiles(const Request &req, const ServerContext &ctx,
                          ProfileCache &cache, bool keep_alive)
{
    // Extract the ?type=... query parameter from the request target.
    const std::string target(req.target());
    std::string type;
    {
        const auto qpos = target.find('?');
        if (qpos != std::string::npos) {
            const std::string query = target.substr(qpos + 1);
            // Scan "k=v&k=v" pairs for the "type" key.
            std::size_t pos = 0;
            while (pos < query.size()) {
                const auto amp = query.find('&', pos);
                const std::string pair = query.substr(
                    pos, amp == std::string::npos ? std::string::npos : amp - pos);
                const auto eq = pair.find('=');
                if (eq != std::string::npos && pair.substr(0, eq) == "type") {
                    type = pair.substr(eq + 1);
                    break;
                }
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
        }
    }

    if (type.empty())
        return make_error(400,
            "Missing required query parameter 'type' "
            "(expected printer | process | filament)",
            keep_alive);

    // Load (or reuse cached) preset names for the configured datadir.
    ProfileCache::Names names;
    std::string err;
    if (!cache.get(ctx.datadir, names, err))
        return make_error(500, "Failed to load profiles: " + err, keep_alive);

    // Select the requested list. Accept a couple of friendly aliases.
    const std::vector<std::string> *selected = nullptr;
    if (type == "printer" || type == "printers")
        selected = &names.printers;
    else if (type == "process" || type == "processes" || type == "print")
        selected = &names.processes;
    else if (type == "filament" || type == "filaments")
        selected = &names.filaments;

    if (selected == nullptr)
        return make_error(400,
            "Invalid 'type' value: '" + type +
            "' (expected printer | process | filament)",
            keep_alive);

    json body = {
        {"type",  type},
        {"names", *selected}
    };
    return make_json(200, body.dump(), keep_alive);
}

// POST /v1/jobs
Response handle_job_submit(const Request &req, const ServerContext &ctx,
                            JobQueue &queue, bool keep_alive)
{
    SliceCore::SliceRequest slice_req;
    std::string             input_filename;

    // -----------------------------------------------------------------------
    // Body parsing: multipart/form-data  OR  application/json
    // -----------------------------------------------------------------------
    const std::string ct = std::string(req[http::field::content_type]);

    if (ct.find("multipart/form-data") != std::string::npos) {
        // --- Multipart path ---
        std::string boundary = parse_boundary(ct);
        if (boundary.empty())
            return make_error(400, "Missing multipart boundary in Content-Type", keep_alive);

        std::vector<MultipartPart> parts;
        if (!parse_multipart(req.body(), boundary, parts))
            return make_error(400, "Failed to parse multipart body", keep_alive);

        // Locate the "model" and "config" parts.
        const MultipartPart *model_part  = nullptr;
        const MultipartPart *config_part = nullptr;
        for (const auto &p : parts) {
            if (p.name == "model")  model_part  = &p;
            if (p.name == "config") config_part = &p;
        }

        if (!model_part)
            return make_error(422, "Missing 'model' part in multipart body", keep_alive);
        if (!config_part)
            return make_error(422, "Missing 'config' part in multipart body", keep_alive);

        // Parse the config JSON.
        json cfg;
        try {
            cfg = json::parse(std::string(config_part->data.begin(),
                                          config_part->data.end()));
        } catch (const json::exception &ex) {
            return make_error(422, std::string("Invalid JSON in 'config' part: ") + ex.what(),
                              keep_alive);
        }

        try {
            slice_req = json_to_slice_request(cfg);
        } catch (const std::exception &ex) {
            return make_error(422, std::string("Config mapping error: ") + ex.what(),
                              keep_alive);
        }

        input_filename = model_part->filename.empty() ? "model" : model_part->filename;
        slice_req.input_bytes    = std::vector<uint8_t>(model_part->data.begin(),
                                                         model_part->data.end());
        slice_req.input_filename = input_filename;

    } else if (ct.find("application/json") != std::string::npos) {
        // --- JSON path (base64-encoded model_b64 field) ---
        // Example body:
        //   {
        //     "model_b64": "<base64>",
        //     "model_filename": "part.3mf",
        //     "presets": {...},
        //     ...
        //   }
        json body;
        try {
            body = json::parse(req.body());
        } catch (const json::exception &ex) {
            return make_error(400, std::string("Invalid JSON body: ") + ex.what(),
                              keep_alive);
        }

        if (!body.contains("model_b64") || !body["model_b64"].is_string())
            return make_error(422,
                "JSON body must contain 'model_b64' (base64-encoded model bytes). "
                "Alternatively, POST multipart/form-data with 'model' and 'config' parts.",
                keep_alive);

        input_filename = body.value("model_filename", std::string("model"));

        try {
            slice_req = json_to_slice_request(body);
        } catch (const std::exception &ex) {
            return make_error(422, std::string("Config mapping error: ") + ex.what(),
                              keep_alive);
        }

        slice_req.input_bytes    = base64_decode(body["model_b64"].get<std::string>());
        slice_req.input_filename = input_filename;

    } else {
        return make_error(415,
            "Unsupported Media Type. Use multipart/form-data (model + config parts) "
            "or application/json (model_b64 field).",
            keep_alive);
    }

    // Default the datadir to the server's --datadir unless the request
    // explicitly supplied one.  SliceService/PresetResolver need this to locate
    // preset bundles when the request references presets by name.
    if (slice_req.datadir.empty())
        slice_req.datadir = ctx.datadir;

    // Enqueue.
    std::string job_id = queue.submit(std::move(slice_req));

    json resp = {{"job_id", job_id}};
    Response res{http::status::created, 11};
    res.set(http::field::content_type, "application/json");
    res.keep_alive(keep_alive);
    res.body() = resp.dump();
    res.prepare_payload();
    return res;
}

// GET /v1/jobs/{id}
Response handle_job_status(const Request & /*req*/, const std::string &job_id,
                             JobQueue &queue, bool keep_alive)
{
    auto info = queue.status(job_id);
    if (!info)
        return make_error(404, "Job not found: " + job_id, keep_alive);

    // Build plates array from result if done.
    json plates_arr = json::array();
    if (info->state == JobState::Done) {
        for (const auto &ps : info->result.plates) {
            // filament_volume_per_extruder: map<int,double> → JSON object
            json fvpe = json::object();
            for (const auto &kv : ps.filament_volume_per_extruder)
                fvpe[std::to_string(kv.first)] = kv.second;

            plates_arr.push_back({
                {"plate_id",                  ps.plate_id},
                {"sliced_ms",                 ps.sliced_ms},
                {"filament_used_mm",          ps.filament_used_mm},
                {"layer_count",               ps.layer_count},
                {"gcode_path",                ps.gcode_path},
                {"estimated_print_time_s",    ps.estimated_print_time_s},
                {"initial_layer_time_s",      ps.initial_layer_time_s},
                {"color_change_count",        ps.color_change_count},
                {"filament_volume_per_extruder", fvpe},
                {"thumbnail_available",       ps.thumbnail_generated}
            });
        }
    }

    // Warnings accumulated during placement/slicing (non-fatal diagnostics).
    json warnings_arr = json::array();
    if (info->state == JobState::Done) {
        for (const auto &w : info->result.warnings)
            warnings_arr.push_back(w);
    }

    json body = {
        {"job_id",   job_id},
        {"state",    state_str(info->state)},
        {"progress", info->progress},
        {"message",  info->message},
        {"plates",   plates_arr},
        {"warnings", warnings_arr}
    };

    // Dedicated, machine-readable failure reason. `message` carries progress
    // text and is reused across states, so it cannot unambiguously signal a
    // failure. Surface a distinct `error` field ONLY when the job failed; for
    // every other state it is explicit JSON null so clients can branch on it
    // without inferring from `state`.
    if (info->state == JobState::Error)
        body["error"] = info->message;
    else
        body["error"] = nullptr;

    return make_json(200, body.dump(), keep_alive);
}

// GET /v1/jobs/{id}/result[?plate=N]
//
// Streams the sliced gcode for a finished job. SliceService writes each plate's
// gcode to disk and records the path in SliceResult::plates[i].gcode_path; this
// handler reads that file and returns its bytes.
//
// Multi-plate handling:
//   - Single plate            → returns plates[0].
//   - Multiple plates, no ?plate → returns plate[0] (documented default).
//   - ?plate=N (1-based)      → returns that plate; 400 if out of range.
//   (Zip/concat of all plates is intentionally out of scope for now.)
//
// 404 — job unknown, OR the requested plate's gcode file is missing on disk
// 409 — job exists but is not Done, or produced no plates
// 400 — ?plate=N malformed / out of range
// 200 — gcode bytes with Content-Disposition: attachment
Response handle_job_result(const Request &req, const std::string &job_id,
                             JobQueue &queue, bool keep_alive)
{
    auto info = queue.status(job_id);
    if (!info)
        return make_error(404, "Job not found: " + job_id, keep_alive);

    if (info->state != JobState::Done)
        return make_error(409,
            "Job result not available: state is " + std::string(state_str(info->state)),
            keep_alive);

    const auto &plates = info->result.plates;
    if (plates.empty())
        return make_error(409, "Job produced no plates", keep_alive);

    // Parse optional ?plate=N (1-based) from the request target.
    int plate_sel = 0; // 0 → default to first plate
    {
        const std::string target(req.target());
        const auto qpos = target.find('?');
        if (qpos != std::string::npos) {
            const std::string query = target.substr(qpos + 1);
            std::size_t pos = 0;
            while (pos < query.size()) {
                const auto amp = query.find('&', pos);
                const std::string pair = query.substr(
                    pos, amp == std::string::npos ? std::string::npos : amp - pos);
                const auto eq = pair.find('=');
                if (eq != std::string::npos && pair.substr(0, eq) == "plate") {
                    const std::string val = pair.substr(eq + 1);
                    try {
                        plate_sel = std::stoi(val);
                    } catch (const std::exception &) {
                        return make_error(400,
                            "Invalid 'plate' query value: '" + val + "'", keep_alive);
                    }
                    break;
                }
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
        }
    }

    // Resolve the plate index (default first when plate_sel == 0).
    std::size_t idx = 0;
    if (plate_sel != 0) {
        if (plate_sel < 1 || static_cast<std::size_t>(plate_sel) > plates.size())
            return make_error(400,
                "Plate out of range: " + std::to_string(plate_sel) +
                " (job has " + std::to_string(plates.size()) + " plate(s))",
                keep_alive);
        idx = static_cast<std::size_t>(plate_sel - 1);
    }

    const std::string &gcode_path = plates[idx].gcode_path;
    if (gcode_path.empty())
        return make_error(409,
            "Plate " + std::to_string(idx + 1) + " has no gcode path", keep_alive);

    // Read the gcode file from disk.
    boost::system::error_code ec;
    if (!boost::filesystem::exists(gcode_path, ec))
        return make_error(404,
            "Gcode file no longer present on disk: " + gcode_path, keep_alive);

    std::ifstream ifs(gcode_path, std::ios::binary);
    if (!ifs)
        return make_error(500, "Failed to open gcode file: " + gcode_path, keep_alive);

    std::ostringstream ss;
    ss << ifs.rdbuf();
    if (ifs.bad())
        return make_error(500, "Failed to read gcode file: " + gcode_path, keep_alive);

    const std::string download_name =
        "job-" + job_id + "-plate-" + std::to_string(idx + 1) + ".gcode";

    Response res{http::status::ok, 11};
    res.set(http::field::content_type, "text/plain");   // gcode is plain text
    res.set(http::field::content_disposition,
            "attachment; filename=\"" + download_name + "\"");
    res.keep_alive(keep_alive);
    res.body() = ss.str();
    res.prepare_payload();
    return res;
}

// DELETE /v1/jobs/{id}
Response handle_job_cancel(const Request & /*req*/, const std::string &job_id,
                             JobQueue &queue, bool keep_alive)
{
    // Check existence first so we can distinguish 404 from 204.
    auto info = queue.status(job_id);
    if (!info)
        return make_error(404, "Job not found: " + job_id, keep_alive);

    queue.cancel(job_id);

    // 204 No Content — empty body.
    Response res{http::status::no_content, 11};
    res.keep_alive(keep_alive);
    res.prepare_payload();
    return res;
}

// GET /v1/jobs/{id}/preview[?plate=N]
//
// Serves the thumbnail PNG for a finished job plate.
//
// Multi-plate handling mirrors handle_job_result:
//   - No ?plate=N → returns the thumbnail for plates[0].
//   - ?plate=N (1-based) → returns that plate's thumbnail; 400 if out of range.
//
// 404 — job unknown, plate has no thumbnail_path, or PNG file missing on disk
// 409 — job exists but is not Done, or produced no plates, or thumbnail not
//        generated for the requested plate
// 400 — ?plate=N malformed / out of range
// 200 — PNG bytes with Content-Type: image/png
Response handle_job_preview(const Request &req, const std::string &job_id,
                             JobQueue &queue, bool keep_alive)
{
    auto info = queue.status(job_id);
    if (!info)
        return make_error(404, "Job not found: " + job_id, keep_alive);

    if (info->state != JobState::Done)
        return make_error(409,
            "Job preview not available: state is " + std::string(state_str(info->state)),
            keep_alive);

    const auto &plates = info->result.plates;
    if (plates.empty())
        return make_error(409, "Job produced no plates", keep_alive);

    // Parse optional ?plate=N (1-based) from the request target.
    int plate_sel = 0; // 0 → default to first plate
    {
        const std::string target(req.target());
        const auto qpos = target.find('?');
        if (qpos != std::string::npos) {
            const std::string query = target.substr(qpos + 1);
            std::size_t pos = 0;
            while (pos < query.size()) {
                const auto amp = query.find('&', pos);
                const std::string pair = query.substr(
                    pos, amp == std::string::npos ? std::string::npos : amp - pos);
                const auto eq = pair.find('=');
                if (eq != std::string::npos && pair.substr(0, eq) == "plate") {
                    const std::string val = pair.substr(eq + 1);
                    try {
                        plate_sel = std::stoi(val);
                    } catch (const std::exception &) {
                        return make_error(400,
                            "Invalid 'plate' query value: '" + val + "'", keep_alive);
                    }
                    break;
                }
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
        }
    }

    // Resolve the plate index (default first when plate_sel == 0).
    std::size_t idx = 0;
    if (plate_sel != 0) {
        if (plate_sel < 1 || static_cast<std::size_t>(plate_sel) > plates.size())
            return make_error(400,
                "Plate out of range: " + std::to_string(plate_sel) +
                " (job has " + std::to_string(plates.size()) + " plate(s))",
                keep_alive);
        idx = static_cast<std::size_t>(plate_sel - 1);
    }

    const auto &ps = plates[idx];
    if (!ps.thumbnail_generated)
        return make_error(409,
            "Thumbnail not available for plate " + std::to_string(idx + 1),
            keep_alive);

    if (ps.thumbnail_path.empty())
        return make_error(404,
            "Thumbnail path not set for plate " + std::to_string(idx + 1),
            keep_alive);

    // Verify the PNG exists on disk.
    boost::system::error_code ec;
    if (!boost::filesystem::exists(ps.thumbnail_path, ec))
        return make_error(404,
            "Thumbnail file not found on disk: " + ps.thumbnail_path, keep_alive);

    // Read PNG bytes.
    std::ifstream ifs(ps.thumbnail_path, std::ios::binary);
    if (!ifs)
        return make_error(500,
            "Failed to open thumbnail file: " + ps.thumbnail_path, keep_alive);

    std::ostringstream ss;
    ss << ifs.rdbuf();
    if (ifs.bad())
        return make_error(500,
            "Failed to read thumbnail file: " + ps.thumbnail_path, keep_alive);

    Response res{http::status::ok, 11};
    res.set(http::field::content_type, "image/png");
    res.keep_alive(keep_alive);
    res.body() = ss.str();
    res.prepare_payload();
    return res;
}

} // namespace Server
} // namespace Slic3r
