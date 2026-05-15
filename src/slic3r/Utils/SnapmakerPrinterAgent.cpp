#include "SnapmakerPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <cctype>
#include <limits>
#include <boost/log/trivial.hpp>

namespace Slic3r {

namespace {

constexpr const char* SNAPMAKER_AGENT_VERSION = "0.0.1";

// Safely access a parallel array by index, returning a fallback if out of bounds.
template<typename T>
T safe_at(const std::vector<T>& vec, int index, const T& fallback)
{
    return (index >= 0 && index < static_cast<int>(vec.size())) ? vec[index] : fallback;
}

} // anonymous namespace

SnapmakerPrinterAgent::SnapmakerPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

AgentInfo SnapmakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{"snapmaker", "Snapmaker", SNAPMAKER_AGENT_VERSION, "Snapmaker printer agent"};
}

std::string SnapmakerPrinterAgent::combine_filament_type(const std::string& type, const std::string& sub_type)
{
    const std::string base = trim_and_upper(type);
    const std::string sub  = trim_and_upper(sub_type);

    if (base.empty())
        return "PLA";

    if (sub.empty() || sub == "NONE")
        return base;

    if (sub == "CF")
        return base + "-CF";
    if (sub == "GF")
        return base + "-GF";
    if (sub == "SNAPSPEED" || sub == "HS")
        return base + " HIGH SPEED";
    if (sub == "SILK")
        return base + " SILK";
    if (sub == "WOOD")
        return base + " WOOD";
    if (sub == "MATTE")
        return base + " MATTE";
    if (sub == "MARBLE")
        return base + " MARBLE";

    // Preserve unknown sub-type as a hint for central vendor/type matching.
    return base + " " + sub;
}

std::string SnapmakerPrinterAgent::resolve_tray_info_idx(const std::string& tray_vendor, const std::string& tray_type) const
{
    auto* bundle = GUI::wxGetApp().preset_bundle;
    if (!bundle)
        return {};

    const std::string target_vendor = trim_and_upper(tray_vendor);
    const std::string target_type   = trim_and_upper(tray_type);
    if (target_type.empty())
        return {};

    const auto tokenize_upper_words = [this](const std::string& input) {
        std::vector<std::string> tokens;
        std::string current;
        const std::string text = trim_and_upper(input);
        for (unsigned char ch : text) {
            if (std::isalnum(ch)) {
                current.push_back(static_cast<char>(ch));
            } else if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        }
        if (!current.empty())
            tokens.push_back(current);
        return tokens;
    };
    const auto contains_token = [](const std::vector<std::string>& tokens, const std::string& needle) {
        for (const auto& token : tokens)
            if (token == needle)
                return true;
        return false;
    };
    const auto has_support_token = [&](const std::vector<std::string>& tokens) {
        return contains_token(tokens, "SUPPORT") || contains_token(tokens, "BREAKAWAY") ||
               contains_token(tokens, "PVA") || contains_token(tokens, "BVOH") || contains_token(tokens, "HIPS");
    };

    const std::vector<std::string> target_type_tokens = tokenize_upper_words(target_type);
    if (target_type_tokens.empty())
        return {};
    const std::vector<std::string> target_vendor_tokens = tokenize_upper_words(target_vendor);

    const std::string target_base_token = target_type_tokens.front();
    std::vector<std::string> target_subtype_tokens;
    for (size_t i = 1; i < target_type_tokens.size(); ++i) {
        if (target_type_tokens[i] != "NONE")
            target_subtype_tokens.push_back(target_type_tokens[i]);
    }
    const bool target_support = has_support_token(target_type_tokens);

    int         best_score = std::numeric_limits<int>::min();
    int         best_subtype_hits = 0;
    bool        best_type_exact = false;
    bool        best_vendor_match = false;
    std::string best_id;

    for (const auto& preset : bundle->filaments) {
        if (!preset.is_compatible || bundle->filaments.get_preset_base(preset) != &preset)
            continue;

        const std::string preset_type = trim_and_upper(preset.config.opt_string("filament_type", 0u));
        if (preset_type.empty())
            continue;
        const std::string preset_name = trim_and_upper(preset.name);
        const std::vector<std::string> preset_type_tokens = tokenize_upper_words(preset_type);
        if (preset_type_tokens.empty())
            continue;
        const std::string preset_base_token = preset_type_tokens.front();

        const bool type_exact = preset_type == target_type;
        const bool type_base  = preset_base_token == target_base_token;
        if (!type_exact && !type_base)
            continue;

        std::string preset_vendor_upper;
        if (auto* vendor_opt = dynamic_cast<const ConfigOptionStrings*>(preset.config.option("filament_vendor"));
            vendor_opt && !vendor_opt->values.empty()) {
            preset_vendor_upper = trim_and_upper(vendor_opt->values.front());
        }
        const std::vector<std::string> preset_match_tokens = tokenize_upper_words(preset_type + " " + preset_name + " " + preset_vendor_upper);
        int vendor_token_hits = 0;
        for (const auto& token : target_vendor_tokens)
            vendor_token_hits += contains_token(preset_match_tokens, token) ? 1 : 0;
        const bool vendor_match_exact = !target_vendor.empty() && preset_vendor_upper == target_vendor;
        const bool vendor_match_partial = !target_vendor_tokens.empty() && vendor_token_hits > 0;

        int subtype_hits = 0;
        bool all_subtype_tokens_match = !target_subtype_tokens.empty();
        for (const auto& token : target_subtype_tokens) {
            const bool hit = contains_token(preset_match_tokens, token);
            subtype_hits += hit ? 1 : 0;
            all_subtype_tokens_match = all_subtype_tokens_match && hit;
        }

        const bool preset_support = preset.config.opt_bool("filament_is_support", 0u) || has_support_token(preset_match_tokens);

        int score = 0;
        score += type_exact ? 10000 : 6000;
        if (vendor_match_exact)
            score += 2200;
        else if (vendor_match_partial)
            score += 1200 + vendor_token_hits * 150;
        if (all_subtype_tokens_match)
            score += 1800;
        else
            score += subtype_hits * 400;
        if (!target_subtype_tokens.empty() && subtype_hits == 0)
            score -= 700;
        score += (target_support == preset_support) ? 250 : -1200;

        // Penalize specialty variants unless the target hints at them.
        for (const char* specialty : {"CF", "GF", "ESD", "RCF", "SILK"}) {
            const std::string token = specialty;
            if (contains_token(preset_match_tokens, token) && !contains_token(target_type_tokens, token))
                score -= 250;
        }

        if (score > best_score) {
            best_score = score;
            best_subtype_hits = subtype_hits;
            best_type_exact = type_exact;
            best_vendor_match = vendor_match_exact || vendor_match_partial;
            best_id    = preset.filament_id;
        }
    }

    // If the tray declares a subtype (e.g. PolyLite / PolySonic), do not force a weak "best match"
    // that only matches on base type. Let caller fall back to a safe generic profile.
    if (!best_id.empty() && !target_subtype_tokens.empty() && !best_type_exact && best_subtype_hits == 0)
        return {};
    if (!best_id.empty() && !target_vendor_tokens.empty() && !best_vendor_match && !best_type_exact)
        return {};

    return best_id;
}

bool SnapmakerPrinterAgent::fetch_filament_info(std::string dev_id)
{
    std::string base_url = device_info.base_url;
    if (base_url.empty() && !dev_id.empty()) {
        if (auto* app_cfg = GUI::wxGetApp().app_config) {
            base_url = app_cfg->get("ip_address", dev_id);
        }
    }
    base_url = normalize_base_url(base_url, "");
    if (base_url.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: cannot resolve base_url"
                                   << ", device_info.base_url='" << device_info.base_url
                                   << "', dev_id='" << dev_id << "'";
        return false;
    }

    std::string url = join_url(base_url, "/printer/objects/query?print_task_config&filament_detect");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!device_info.api_key.empty()) {
        http.header("X-Api-Key", device_info.api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: HTTP request failed: " << http_error;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: Invalid JSON response";
        return false;
    }

    // Navigate to result.status.print_task_config
    if (!json.contains("result") || !json["result"].contains("status") ||
        !json["result"]["status"].contains("print_task_config")) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: Missing print_task_config in response";
        return false;
    }

    auto& ptc = json["result"]["status"]["print_task_config"];

    // Read parallel arrays from print_task_config
    auto filament_exist    = ptc.value("filament_exist", std::vector<bool>{});
    auto filament_vendor   = ptc.value("filament_vendor", std::vector<std::string>{});
    auto filament_type     = ptc.value("filament_type", std::vector<std::string>{});
    auto filament_sub_type = ptc.value("filament_sub_type", std::vector<std::string>{});
    auto filament_color    = ptc.value("filament_color_rgba", std::vector<std::string>{});

    const int slot_count = static_cast<int>(filament_exist.size());
    if (slot_count == 0) {
        BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent::fetch_filament_info: No filament slots reported";
        return false;
    }

    // Read NFC filament_detect data for temperature info (optional)
    nlohmann::json nfc_info;
    if (json["result"]["status"].contains("filament_detect") &&
        json["result"]["status"]["filament_detect"].contains("info")) {
        nfc_info = json["result"]["status"]["filament_detect"]["info"];
    }

    static const std::string empty_str;
    static const std::string default_color = "FFFFFFFF";

    std::vector<AmsTrayData> trays;
    trays.reserve(slot_count);

    for (int i = 0; i < slot_count; ++i) {
        AmsTrayData tray;
        tray.slot_index   = i;
        tray.has_filament = filament_exist[i];

        if (tray.has_filament) {
            tray.tray_type     = combine_filament_type(safe_at(filament_type, i, empty_str),
                                                       safe_at(filament_sub_type, i, empty_str));
            tray.tray_vendor   = safe_at(filament_vendor, i, empty_str);
            tray.tray_info_idx = resolve_tray_info_idx(tray.tray_vendor, tray.tray_type);
            if (tray.tray_info_idx.empty()) {
                std::string base_type = tray.tray_type;
                if (auto sep = base_type.find(' '); sep != std::string::npos)
                    base_type = base_type.substr(0, sep);

                // Prefer a stable generic fallback over a weak vendor/profile guess.
                const std::string generic_id = map_filament_type_to_generic_id(base_type);
                if (!generic_id.empty() && generic_id != UNKNOWN_FILAMENT_ID) {
                    tray.tray_info_idx = generic_id;
                } else {
                    auto* bundle = GUI::wxGetApp().preset_bundle;
                    tray.tray_info_idx = bundle
                        ? bundle->filaments.filament_id_by_type(base_type)
                        : map_filament_type_to_generic_id(base_type);
                }
            }
            tray.tray_color    = safe_at(filament_color, i, default_color);

            // Extract NFC temperature data if available
            if (nfc_info.is_array() && i < static_cast<int>(nfc_info.size()) && nfc_info[i].is_object()) {
                auto& nfc_slot = nfc_info[i];
                std::string nfc_vendor = nfc_slot.value("VENDOR", "NONE");
                if (nfc_vendor != "NONE" && !nfc_vendor.empty()) {
                    tray.bed_temp    = nfc_slot.value("BED_TEMP", 0);
                    tray.nozzle_temp = nfc_slot.value("FIRST_LAYER_TEMP", 0);
                }
            }
        }

        trays.emplace_back(std::move(tray));
    }

    build_ams_payload(1, slot_count - 1, trays);
    return true;
}

} // namespace Slic3r
