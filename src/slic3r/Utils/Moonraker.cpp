#include "Moonraker.hpp"

#include <sstream>
#include <fstream>
#include <algorithm>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {

Moonraker::Moonraker(DynamicPrintConfig *config)
    : m_host(config->opt_string("print_host"))
    , m_apikey(config->opt_string("printhost_apikey"))
    , m_cafile(config->opt_string("printhost_cafile"))
    , m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* Moonraker::get_name() const { return "Moonraker"; }

wxString Moonraker::get_test_ok_msg() const
{
    return _(L("Connection to Moonraker is working correctly."));
}

wxString Moonraker::get_test_failed_msg(wxString &msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Moonraker"), msg);
}

std::string Moonraker::make_url(const std::string &path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/')
            return (boost::format("%1%%2%") % m_host % path).str();
        return (boost::format("%1%/%2%") % m_host % path).str();
    }
    return (boost::format("http://%1%/%2%") % m_host % path).str();
}

void Moonraker::set_auth(Http &http) const
{
    //ORCA: Moonraker accepts unauthenticated requests by default; X-Api-Key is the only auth header
    //      defined by the Moonraker spec. HTTP Basic / Digest do NOT belong here even if the user
    //      filled the user/password fields — those are PrusaLink/OctoPrint conventions.
    if (!m_apikey.empty())
        http.header("X-Api-Key", m_apikey);
    if (!m_cafile.empty())
        http.ca_file(m_cafile);
}

bool Moonraker::test(wxString &msg) const
{
    //ORCA: Moonraker's /server/info returns
    //          { "result": { "klippy_state": "ready|startup|shutdown|error|disconnected", ... } }
    //      We treat the connection as healthy as long as the envelope is valid and `klippy_state`
    //      is present — matching the OctoPrint/PrusaLink convention of "can I reach this host?".
    //      Klipper state (idle, error, etc.) is surfaced to the log but does not gate the test:
    //      buddy-fork firmwares legitimately report non-`ready` states at idle, and any real upload
    //      problem will surface a contextual error at upload() time anyway.
    const char *name = get_name();
    bool res = true;
    auto url = make_url("server/info");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get server info at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting server info: %2%, HTTP %3%, body: `%4%`")
            % name % error % status % body;
        res = false;
        msg = format_error(body, error, status);
    })
    .on_complete([&, this](std::string body, unsigned) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/info body: %2%") % name % body;
        try {
            std::stringstream ss(body);
            pt::ptree ptree;
            pt::read_json(ss, ptree);

            const auto klippy_state = ptree.get_optional<std::string>("result.klippy_state");
            if (!klippy_state) {
                //ORCA: response wasn't shaped like a Moonraker /server/info reply — likely an OctoPrint
                //      or PrusaLink host the user mis-selected as Moonraker, or a totally different
                //      service. Treat as a connection failure with a clear hint.
                res = false;
                msg = _L("The host responded but it doesn't look like Moonraker (missing result.klippy_state).");
                return;
            }
            BOOST_LOG_TRIVIAL(info) << boost::format("%1%: klippy_state = %2%") % name % (*klippy_state);
        } catch (const std::exception &ex) {
            res = false;
            msg = GUI::format_wxstr(_L("Could not parse Moonraker server response: %s"), ex.what());
        }
    })
#ifdef WIN32
    .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
    .perform_sync();

    return res;
}

bool Moonraker::get_storage(wxArrayString &storage_path, wxArrayString &storage_name) const
{
    //ORCA: GET /server/files/roots enumerates Moonraker's storage roots (default "gcodes" plus any
    //      configured extras like "config", "logs", "timelapse"). Only roots with permissions
    //      including "rw" or "rwd" can receive uploads; we filter to those so the UI dropdown only
    //      offers usable destinations. The base class returns false (no per-host storage); returning
    //      true here populates the storage picker in PrintHostDialogs's send-to-print dialog.
    //      Failures (404 — older Moonraker, or a buddy-fork that doesn't implement the endpoint)
    //      gracefully degrade to false so upload() falls back to the hardcoded "gcodes" default.
    const char *name = get_name();
    bool got_any = false;
    auto url = make_url("server/files/roots");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Enumerating storage roots at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        //ORCA: /server/files/roots is optional in the Moonraker spec and absent on older versions
        //      and slimmer shims (e.g. Prusa-Firmware-Buddy 0.8.x prusalink-shim returns 501). A
        //      missing endpoint here is benign — upload() silently falls back to the hardcoded
        //      "gcodes" root — so don't pollute the log at warning level for it. Other HTTP
        //      errors still warn.
        if (status == 404 || status == 501) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/files/roots not implemented (HTTP %2%); upload() will fall back to the \"gcodes\" root.")
                % name % status;
        } else {
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Could not enumerate roots: %2%, HTTP %3%, body: `%4%`")
                % name % error % status % body;
        }
    })
    .on_complete([&, this](std::string body, unsigned) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/files/roots body: %2%") % name % body;
        try {
            std::stringstream ss(body);
            pt::ptree ptree;
            pt::read_json(ss, ptree);
            const auto result_node = ptree.get_child_optional("result");
            if (!result_node)
                return;
            for (const auto &child : *result_node) {
                const std::string &root = child.second.get<std::string>("name", "");
                const std::string &perms = child.second.get<std::string>("permissions", "");
                if (root.empty() || perms.find('w') == std::string::npos)
                    continue;
                if (root != "gcodes")
                    continue;
                storage_path.Add(wxString::FromUTF8(root));
                storage_name.Add(wxString::FromUTF8(root));
                got_any = true;
            }
        } catch (const std::exception &ex) {
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Could not parse roots: %2%") % name % ex.what();
        }
    })
#ifdef WIN32
    .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
    .perform_sync();

    return got_any;
}

bool Moonraker::start_print(wxString &error_msg, const std::string &filename) const
{
    //ORCA: POST /printer/print/start with JSON body { "filename": "<name>.gcode" }.
    //      `filename` is what /server/files/upload returned as result.item.path (the storage-relative
    //      path inside `root`, no leading slash, with extension). Build the body via property_tree
    //      so that special characters in the filename (server-side collision-suffix could produce
    //      paths with quotes / backslashes on exotic file systems) are properly escaped.
    const char *name = get_name();
    bool res = true;
    auto url = make_url("printer/print/start");
    pt::ptree body_tree;
    body_tree.put("filename", filename);
    std::ostringstream body_ss;
    pt::write_json(body_ss, body_tree, /*pretty=*/false);
    std::string body = body_ss.str();

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Starting print of %2% at %3%") % name % filename % url;

    auto http = Http::post(std::move(url));
    set_auth(http);
    http.header("Content-Type", "application/json")
        .set_post_body(body)
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: print/start HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error starting print at %2%: %3%, HTTP %4%, body: `%5%`")
                % name % url % error % status % body;
            res = false;
            error_msg = format_error(body, error, status);
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    return res;
}

bool Moonraker::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    //ORCA: POST /server/files/upload as multipart/form-data with:
    //          file = <gcode file>
    //          root = <storage root>     (Moonraker default: "gcodes")
    //      Successful response shape:
    //          { "result": { "item": { "path": "<name>.gcode", "root": "<root>" }, "print_started": <bool> } }
    //      We always start the print explicitly via /printer/print/start regardless of `print_started`
    //      so the user can rely on a single call site for state.
    wxString test_msg;
    if (!test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    const char *name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    //ORCA: upload_data.storage is plumbed from the (future) per-printer storage dropdown. When unset,
    //      fall back to the Moonraker-standard "gcodes" root. Reading it through here means a UI
    //      addition later (storage picker) needs no change to this method.
    const std::string root = upload_data.storage.empty() ? std::string("gcodes") : upload_data.storage;

    //ORCA-CFS: If the send dialog resolved a filament->slot mapping, honour it by rewriting the bare
    //          tool-change lines (T0/T1/...) in the gcode so each project filament prints from the
    //          physical CFS slot the user chose. The Creality `box` Klipper module loads slot N when
    //          it receives Tn (it registers Tn natively; see `t_command`), so remapping tool indices
    //          is firmware-agnostic and needs no special macro. Identity/empty mapping -> no rewrite.
    boost::filesystem::path effective_source = upload_data.source_path;
    if (upload_data.extended("cfs_enabled") == "1") {
        wxString cfs_msg;
        if (!apply_cfs_tool_mapping(upload_data.extended("cfs_map"), upload_data.source_path, effective_source, cfs_msg)) {
            error_fn(std::move(cfs_msg));
            return false;
        }
    }

    std::string url = make_url("server/files/upload");
    bool result = true;
    std::string uploaded_path;

    //ORCA: gcode inside a .gcode.3mf is index-coded (Metadata/plate_<N>.gcode), so the upload names the
    //      plate via a 1-based `plateindex` (set only in the .3mf path, see Plater::send_gcode_legacy);
    //      servers that don't use it ignore the unknown form field.
    const std::string plateindex = upload_data.extended("plateindex");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% to %3% (root=%4%, filename=%5%, plateindex=%6%, start_print=%7%)")
        % name
        % upload_data.source_path
        % url
        % root
        % upload_filename.string()
        % (plateindex.empty() ? "-" : plateindex)
        % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false");

    auto http = Http::post(std::move(url));
    set_auth(http);
    http.form_add("root", root);
    if (!plateindex.empty())
        http.form_add("plateindex", plateindex);
    http.form_add_file("file", effective_source.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: upload HTTP %2%: %3%") % name % status % body;
            try {
                std::stringstream ss(body);
                pt::ptree ptree;
                pt::read_json(ss, ptree);

                //ORCA: Moonraker confirms the storage-relative path in result.item.path. We pass exactly
                //      that string to /printer/print/start so any server-side renaming (collision suffix,
                //      etc.) is respected.
                const auto stored_path = ptree.get_optional<std::string>("result.item.path");
                if (stored_path) {
                    uploaded_path = *stored_path;
                } else {
                    //ORCA: fallback if the server response omits result.item.path (older Moonraker, or
                    //      a buddy-fork that returns a slimmer envelope). Use the original filename.
                    uploaded_path = upload_filename.string();
                    BOOST_LOG_TRIVIAL(warning) << boost::format(
                        "%1%: upload response missing result.item.path, falling back to original filename `%2%`")
                        % name % uploaded_path;
                }
            } catch (const std::exception &ex) {
                BOOST_LOG_TRIVIAL(warning) << boost::format(
                    "%1%: could not parse upload response (%2%); falling back to original filename")
                    % name % ex.what();
                uploaded_path = upload_filename.string();
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading to %2%: %3%, HTTP %4%, body: `%5%`")
                % name % url % error % status % body;
            error_fn(format_error(body, error, status));
            result = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            progress_fn(std::move(progress), cancel);
            if (cancel) {
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                result = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    if (!result)
        return false;

    if (upload_data.post_action == PrintHostPostUploadAction::StartPrint && !uploaded_path.empty()) {
        wxString start_msg;
        if (!start_print(start_msg, uploaded_path)) {
            error_fn(std::move(start_msg));
            return false;
        }
    }
    return true;
}


// ============================ ORCA-CFS: Creality Filament System ============================

bool Moonraker::query_object(const std::string& object, std::string& body_out, wxString& msg) const
{
    //ORCA-CFS: GET /printer/objects/query?<object>  ->  {"result":{"status":{<object>:{...}}}}
    const char* name = get_name();
    bool res = true;
    auto url = make_url("printer/objects/query?" + object);
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_complete([&](std::string body, unsigned) { body_out = std::move(body); })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%-CFS: query %2% failed: %3% HTTP %4%")
                % name % object % error % status;
            res = false;
            msg = format_error(body, error, status);
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    return res;
}

bool Moonraker::detect_cfs_object(std::string& object_name_out, wxString& msg) const
{
    //ORCA-CFS: GET /printer/objects/list -> {"result":{"objects":["box","gcode_macro T0",...]}}.
    //          The CFS publishes a custom Klipper object; its exact name varies by firmware so we
    //          match against a candidate list (refined from cfs-moonraker-probe.sh output). The
    //          first match wins; absence simply means "no CFS" (not an error).
    //ORCA-CFS: On rooted Creality K1/K1C/K1 SE/K1 Max the CFS publishes a single Klipper object
    //          literally named "box" (confirmed via probe). Other names (cfs/filament_hub/...) are
    //          NOT registered objects — querying them just echoes an empty {} — so we match against
    //          /printer/objects/list, where only "box" appears. Extra candidates kept for forward
    //          compatibility with other firmwares.
    static const std::vector<std::string> kCandidates = {
        "box", "filament_hub", "cfs", "creality_cfs", "material_station"
    };
    const char* name = get_name();
    std::string body;
    bool res = true;
    auto url = make_url("printer/objects/list");
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_complete([&](std::string b, unsigned) { body = std::move(b); })
        .on_error([&](std::string b, std::string error, unsigned status) {
            res = false; msg = format_error(b, error, status);
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    if (!res)
        return false;

    try {
        std::stringstream ss(body);
        pt::ptree ptree;
        pt::read_json(ss, ptree);
        const auto objects = ptree.get_child_optional("result.objects");
        if (!objects) { object_name_out.clear(); return true; }
        std::vector<std::string> present;
        for (const auto& o : *objects)
            present.push_back(o.second.get_value<std::string>());
        for (const auto& cand : kCandidates)
            if (std::find(present.begin(), present.end(), cand) != present.end()) {
                object_name_out = cand;
                BOOST_LOG_TRIVIAL(info) << boost::format("%1%-CFS: detected CFS object \"%2%\"") % name % cand;
                return true;
            }
        object_name_out.clear(); // no CFS present
    } catch (const std::exception& ex) {
        res = false;
        msg = GUI::format_wxstr(_L("Could not parse Moonraker objects list: %s"), ex.what());
    }
    return res;
}

bool Moonraker::parse_cfs_slots(const std::string& object_name, const std::string& body, std::vector<CFSSlot>& slots) const
{
    //ORCA-CFS: Parse the Creality `box` object captured by cfs-moonraker-probe.sh. Shape:
    //   result.status.box = {
    //     "same_material": [ [ "<mat_code>", "<color>", ["T1A"], "<MaterialName>" ], ... ],
    //     "T1".."T4": { "state":"connect|None", "remain_len":["45","100",..],
    //                   "color_value":["0ffffff",..], "material_type":["006002",..] },
    //     "t_command":"T2", ... }
    //   `same_material` is the authoritative flat slot list for connected units: each entry carries
    //   the material *name* (PLA/PETG/...), the color, and a slot label "T<unit><A..D>". The label
    //   maps to a 0-based global tool index = (unit-1)*4 + ('A..D' index), matching the Tn tool
    //   commands the slicer emits at colour changes (the `box` module registers Tn natively;
    //   see `t_command`). Colors are 7-char "0RRGGBB" -> we keep the trailing 6 hex digits.
    slots.clear();

    auto norm_color = [](std::string c) -> std::string {
        if (c.empty() || c == "-1" || c == "None") return "";
        if (c.size() > 6) c = c.substr(c.size() - 6); // drop leading "0"
        std::transform(c.begin(), c.end(), c.begin(), ::tolower);
        return c;
    };
    auto label_to_global = [](const std::string& label, int& unit_out, int& global_out) -> bool {
        // "T1A".."T4D"
        if (label.size() < 3 || (label[0] != 'T' && label[0] != 't')) return false;
        const int unit = label[1] - '0';
        const int sidx = std::string("ABCD").find(static_cast<char>(::toupper(label[2])));
        if (unit < 1 || unit > 4 || sidx < 0) return false;
        unit_out   = unit;
        global_out = (unit - 1) * 4 + sidx;
        return true;
    };

    try {
        std::stringstream ss(body);
        pt::ptree ptree;
        pt::read_json(ss, ptree);
        const auto box = ptree.get_child_optional("result.status." + object_name);
        if (!box)
            return false;

        // --- Primary source: same_material (has material names) ---
        if (const auto same = box->get_child_optional("same_material")) {
            for (const auto& entry_kv : *same) {
                // entry is itself a JSON array: [code, color, [label], name]
                std::vector<const pt::ptree*> cols;
                for (const auto& c : entry_kv.second) cols.push_back(&c.second);
                if (cols.size() < 4) continue;
                const std::string code  = cols[0]->get_value<std::string>(""); // Creality filamentId
                const std::string color = norm_color(cols[1]->get_value<std::string>(""));
                std::string label;
                for (const auto& lbl : *cols[2]) { label = lbl.second.get_value<std::string>(""); break; }
                const std::string name = cols[3]->get_value<std::string>("");

                CFSSlot slot;
                slot.label          = label;
                slot.material_name  = name;
                slot.material_code  = code;
                slot.material_color = color;
                slot.has_filament   = true;
                int unit = 1, gid = 0;
                if (label_to_global(label, unit, gid)) { slot.unit_id = unit; slot.slot_id = gid; }
                else                                    slot.slot_id = static_cast<int>(slots.size());
                slots.push_back(slot);
            }
        }

        // --- Enrich with / fall back to per-unit arrays (remain %, empty slots) ---
        for (int unit = 1; unit <= 4; ++unit) {
            const std::string ukey = "T" + std::to_string(unit);
            const auto unode = box->get_child_optional(ukey);
            if (!unode) continue;
            const std::string ustate = unode->get<std::string>("state", "None");
            if (ustate == "None" || ustate.empty()) continue; // unit not present

            auto col_at = [&](const char* arr, int i) -> std::string {
                const auto a = unode->get_child_optional(arr);
                if (!a) return "";
                int k = 0; for (const auto& v : *a) { if (k++ == i) return v.second.get_value<std::string>(""); }
                return "";
            };
            for (int i = 0; i < 4; ++i) {
                const int gid = (unit - 1) * 4 + i;
                const std::string rl = col_at("remain_len", i);
                const int remain = (rl.empty() || rl == "-1") ? -1 : atoi(rl.c_str());
                auto it = std::find_if(slots.begin(), slots.end(),
                                       [&](const CFSSlot& s){ return s.slot_id == gid; });
                if (it != slots.end()) {
                    it->remain = remain;                 // enrich existing
                } else if (remain > 0) {
                    CFSSlot slot;                        // present but absent from same_material
                    slot.slot_id        = gid;
                    slot.unit_id        = unit;
                    slot.label          = ukey + static_cast<char>('A' + i);
                    slot.material_color = norm_color(col_at("color_value", i));
                    slot.remain         = remain;
                    slot.has_filament   = true;
                    slots.push_back(slot);
                }
            }
        }
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(warning) << boost::format("%1%-CFS: parse_cfs_slots failed: %2%") % get_name() % ex.what();
        return false;
    }

    std::sort(slots.begin(), slots.end(), [](const CFSSlot& a, const CFSSlot& b){ return a.slot_id < b.slot_id; });
    return !slots.empty();
}

bool Moonraker::fetch_cfs_slots(std::vector<CFSSlot>& slots, bool* supports_cfs, wxString& msg) const
{
    if (supports_cfs) *supports_cfs = false;
    slots.clear();

    std::string object_name;
    if (!detect_cfs_object(object_name, msg))
        return false;          // transport error
    if (object_name.empty())
        return true;           // reachable, but no CFS — UI hides mapping section

    std::string body;
    if (!query_object(object_name, body, msg))
        return false;
    if (!parse_cfs_slots(object_name, body, slots)) {
        msg = _L("A CFS unit was detected but its slots could not be read.");
        return false;
    }
    if (supports_cfs) *supports_cfs = true;
    BOOST_LOG_TRIVIAL(info) << boost::format("%1%-CFS: read %2% slots") % get_name() % slots.size();
    return true;
}

bool Moonraker::apply_cfs_tool_mapping(const std::string& mapping,
                                       const boost::filesystem::path& source,
                                       boost::filesystem::path& effective_out,
                                       wxString& msg) const
{
    //ORCA-CFS: `mapping` is "<tool>:<slot>,<tool>:<slot>,..." where <tool> is the 0-based tool index
    //          the slicer used for a project filament and <slot> is the 0-based global CFS slot the
    //          user chose (T1A=0..T1D=3, T2A=4, ...). We rewrite only standalone tool-change lines
    //          ("T0", "T3 ; comment", ...) so filament i loads from its mapped slot. If the mapping
    //          is empty or an identity, the original file is used unchanged (no copy, no overhead).
    effective_out = source;
    if (mapping.empty())
        return true;

    std::map<int,int> remap;
    bool non_identity = false;
    std::stringstream ms(mapping);
    std::string pair;
    while (std::getline(ms, pair, ',')) {
        const auto colon = pair.find(':');
        if (colon == std::string::npos) continue;
        try {
            const int tool = std::stoi(pair.substr(0, colon));
            const int slot = std::stoi(pair.substr(colon + 1));
            remap[tool] = slot;
            if (tool != slot) non_identity = true;
        } catch (...) { /* ignore malformed pair */ }
    }
    if (remap.empty() || !non_identity)
        return true; // nothing to change

    try {
        std::ifstream in(source.string(), std::ios::binary);
        if (!in) { msg = _L("CFS: could not open the sliced file to apply slot mapping."); return false; }

        boost::filesystem::path tmp = source;
        tmp += ".cfsmap.tmp";
        std::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        if (!out) { msg = _L("CFS: could not create a temporary file for slot mapping."); return false; }

        std::string line;
        while (std::getline(in, line)) {
            // Match a standalone tool-change line: optional spaces, 'T', digits, then end/space/';'.
            size_t i = 0; while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i < line.size() && (line[i] == 'T' || line[i] == 't')) {
                size_t d = i + 1, ds = d;
                while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) ++d;
                const bool has_digits = d > ds;
                const bool clean_tail = (d == line.size()) || line[d] == ' ' || line[d] == '\t'
                                        || line[d] == ';' || line[d] == '\r';
                if (has_digits && clean_tail) {
                    const int tool = std::stoi(line.substr(ds, d - ds));
                    const auto it = remap.find(tool);
                    if (it != remap.end() && it->second != tool) {
                        line = line.substr(0, ds) + std::to_string(it->second) + line.substr(d);
                    }
                }
            }
            out << line << '\n';
        }
        out.flush();
        if (!out) { msg = _L("CFS: failed while writing the slot-mapped file."); return false; }
        effective_out = tmp;
        BOOST_LOG_TRIVIAL(info) << boost::format("%1%-CFS: applied tool mapping `%2%` -> %3%")
            % get_name() % mapping % tmp.string();
    } catch (const std::exception& ex) {
        msg = GUI::format_wxstr(_L("CFS: error applying slot mapping: %s"), ex.what());
        return false;
    }
    return true;
}

}
