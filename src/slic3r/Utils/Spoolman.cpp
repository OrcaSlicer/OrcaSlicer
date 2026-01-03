#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MainFrame.hpp>
#include <utility>
#include <slic3r/GUI/CreatePresetsDialog.hpp>
#include <boost/regex.hpp>
#include <boost/property_tree/json_parser.hpp>
#include "Spoolman.hpp"

namespace Slic3r {

namespace {
template<class Type> Type get_opt(pt::ptree& data, const string& path, Type default_val = {}) { return data.get_optional<Type>(path).value_or(default_val); }

void update_note(std::string& config_note, const std::string& type, const std::string& value)
{
    const auto header = ";spoolman_comment_" + type + "\n";
    if (size_t header_idx; !config_note.empty() && (header_idx = config_note.find(header)) != std::string::npos) {
        const size_t comment_begin_idx = header_idx + header.size();
        if (const size_t comment_end_idx = config_note.find("\n;spoolman_comment_", header_idx); comment_end_idx != std::string::npos) {
            const size_t comment_len = comment_end_idx - comment_begin_idx;
            config_note.replace(comment_begin_idx, comment_len, value + "\n");
            return;
        }
    }

    if (value.empty())
        return;

    if (const size_t end_comment_idx = config_note.find("\n;spoolman_comment_end"); end_comment_idx != std::string::npos) {
        config_note.insert(end_comment_idx, "\n" + header + value + "\n");
        return;
    }

    if (!config_note.empty())
        config_note.append("\n");
    config_note.append(header).append(value).append("\n\n;spoolman_comment_end");
}
} // namespace

// Max timout in seconds for Spoolman HTTP requests
static constexpr long MAX_TIMEOUT = 5;

//---------------------------------
// Spoolman
//---------------------------------

static std::string get_spoolman_api_url()
{
    std::string spoolman_host = wxGetApp().app_config->get("spoolman", "host");
    std::string spoolman_port = Spoolman::DEFAULT_PORT;

    // Remove http(s) designator from the string as it interferes with the next step
    spoolman_host = boost::regex_replace(spoolman_host, boost::regex("https?://"), "");

    // If the host contains a port, use that rather than the default
    if (spoolman_host.find_last_of(':') != string::npos) {
        static const boost::regex pattern(R"((?<host>[a-z0-9.\-_]+):(?<port>[0-9]+))", boost::regex_constants::icase);
        boost::smatch result;
        if (boost::regex_match(spoolman_host, result, pattern)) {
            spoolman_port = result["port"]; // get port value first since it is overwritten when setting the host value in the next line
            spoolman_host = result["host"];
        } else {
            BOOST_LOG_TRIVIAL(error) << "Failed to parse host string. Host: " << spoolman_host << ", Port: " << spoolman_port;
        }
    }

    return spoolman_host + ":" + spoolman_port + "/api/v1/";
}


Http Spoolman::get_http_instance(const HTTPAction action, const std::string& url)
{
    if (action == GET)
        return Http::get(url);
    if (action == PUT)
        return Http::put2(url);
    if (action == POST)
        return Http::post(url);
    if (action == PATCH)
        return Http::patch(url);
    throw RuntimeError("Invalid HTTP action");
}


pt::ptree Spoolman::spoolman_api_call(const HTTPAction http_action, const std::string& api_endpoint, const pt::ptree& data)
{
    const auto url  = get_spoolman_api_url() + api_endpoint;
    auto http = get_http_instance(http_action, url);

    bool        res;
    std::string res_body;

    http.header("Content-Type", "application/json")
        .on_error([&](const std::string& body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "Failed to put data to the Spoolman server. Make sure that the port is correct and the server is running." << boost::format(" HTTP Error: %1%, HTTP status code: %2%, Response body: %3%") % error % status % body;
            res = false;
        })
        .on_complete([&](std::string body, unsigned) {
            res_body = std::move(body);
            res      = true;
        })
        .timeout_max(MAX_TIMEOUT);

    if (!data.empty()) {
        stringstream ss;
        pt::write_json(ss, data);
        http.set_post_body(ss.str());
    }

    http.perform_sync();

    if (!res)
        return {};

    if (res_body.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Spoolman request returned an empty string";
        return {};
    }

    pt::ptree tree;
    try {
        stringstream ss = stringstream(res_body);
        pt::read_json(ss, tree);
    } catch (std::exception& exception) {
        BOOST_LOG_TRIVIAL(error) << "Failed to read json into property tree. Exception: " << exception.what();
        return {};
    }

    return tree;
}


bool Spoolman::pull_spoolman_spools()
{
    pt::ptree tree;

    this->clear();

    // Vendor
    tree = get_spoolman_json("vendor");
    if (tree.empty())
        return false;
    for (const auto& item : tree)
        m_vendors.emplace(item.second.get<int>("id"), make_shared<SpoolmanVendor>(SpoolmanVendor(item.second)));

    // Filament
    tree = get_spoolman_json("filament");
    if (tree.empty())
        return false;
    for (const auto& item : tree)
        m_filaments.emplace(item.second.get<int>("id"), make_shared<SpoolmanFilament>(SpoolmanFilament(item.second)));

    // Spool
    tree = get_spoolman_json("spool");
    if (tree.empty())
        return false;
    for (const auto& item : tree)
        m_spools.emplace(item.second.get<int>("id"), make_shared<SpoolmanSpool>(SpoolmanSpool(item.second)));

    return true;
}

bool Spoolman::use_spoolman_spool(const unsigned int& spool_id, const double& usage, const std::string& usage_type)
{
    pt::ptree tree;
    tree.put("use_" + usage_type, usage);

    std::string endpoint = (boost::format("spool/%1%/use") % spool_id).str();
    tree = put_spoolman_json(endpoint, tree);
    if (tree.empty())
        return false;

    get_spoolman_spool_by_id(spool_id)->update_from_json(tree);
    return true;
}

bool Spoolman::use_spoolman_spools(const std::map<unsigned int, double>& data, const std::string& usage_type)
{
    if (!(usage_type == "length" || usage_type == "weight"))
        return false;

    std::vector<unsigned int> spool_ids;

    for (auto& [spool_id, usage] : data) {
        if (!use_spoolman_spool(spool_id, usage, usage_type))
            return false;
        spool_ids.emplace_back(spool_id);
    }

    update_specific_spool_statistics(spool_ids);

    m_use_undo_buffer = data;
    m_last_usage_type = usage_type;
    return true;
}

bool Spoolman::undo_use_spoolman_spools()
{
    if (m_use_undo_buffer.empty() || m_last_usage_type.empty())
        return false;

    std::vector<unsigned int> spool_ids;

    for (auto& [spool_id, usage] : m_use_undo_buffer) {
        if (!use_spoolman_spool(spool_id, usage * -1, m_last_usage_type))
            return false;
        spool_ids.emplace_back(spool_id);
    }

    update_specific_spool_statistics(spool_ids);

    m_use_undo_buffer.clear();
    m_last_usage_type.clear();
    return true;
}

SpoolmanResult Spoolman::create_filament_preset_from_spool(const SpoolmanSpoolShrPtr& spool,
                                                           const Preset*              base_preset,
                                                           bool                       use_preset_data,
                                                           bool                       detach,
                                                           bool                       force)
{
    PresetCollection& filaments = wxGetApp().preset_bundle->filaments;
    SpoolmanResult    result;

    DynamicPrintConfig preset_data_config;
    std::map<std::string, std::string> additional_preset_data;
    if (use_preset_data) {
        // If the preset data is empty, use the base_preset
        if (spool->filament->preset_data.empty()) {
            use_preset_data = false;
        } else {
            if (spool->filament->get_config_from_preset_data(preset_data_config, &additional_preset_data)) {
                if (const auto& inherits = preset_data_config.opt_string("inherits"); inherits.empty()) {
                    // If inherits is empty, the profile data is detached. When use_preset_data,
                    // the base_preset is not important, so just provide the default preset
                    base_preset = &filaments.default_preset();
                } else {
                    if (const auto preset_data_base_preset = filaments.find_preset(inherits)) {
                        base_preset = preset_data_base_preset;
                    } else {
                        if (!force)
                            result.messages.emplace_back(_u8L("The provided preset data inherits from a non-existing preset."));
                        use_preset_data = false;
                    }
                }
            } else {
                if (!force)
                    result.messages.emplace_back(_u8L("There was an error parsing the preset data for the spool"));
                use_preset_data = false;
            }
        }
    }

    if (!base_preset)
        base_preset = &filaments.get_edited_preset();

    std::string filament_preset_name = spool->get_preset_name();

    // Add a printer/vendor identifier
    if (auto idx = base_preset->name.rfind(" @"); idx != std::string::npos && base_preset->name.substr(idx) != " @System") {
        filament_preset_name += base_preset->name.substr(idx);
    } else if (base_preset->vendor) {
        // Presets from the filament library are compatible with all printers by default
        // Skip adding an identifier
        if (!base_preset->m_from_orca_filament_lib)
            filament_preset_name += " @" + base_preset->vendor->name;
    } else if (use_preset_data && (idx = additional_preset_data[BBL_JSON_KEY_NAME].find(" @")) != std::string::npos) {
        filament_preset_name += additional_preset_data[BBL_JSON_KEY_NAME].substr(idx);
    } else {
        filament_preset_name += " @" + wxGetApp().preset_bundle->printers.get_selected_preset_name();
    }

    if (const auto idx = filament_preset_name.rfind(" - Copy"); idx != std::string::npos)
        filament_preset_name.erase(idx);

    Preset* preset = filaments.find_preset(filament_preset_name);

    if (preset && !preset->is_user())
        result.messages.emplace_back(_u8L("A system preset exists with the same name and cannot be overwritten"));
    if (!force) {
        // Check if a preset with the same name already exists
        if (preset && preset->is_user())
            result.messages.emplace_back(_u8L("Preset already exists with the same name"));

        // Check for presets with the same spool ID
        int compatible(0);
        for (const auto item : filaments.get_compatible()) { // count num of visible and invisible
            if (item->is_user() && item->config.opt_int("spoolman_spool_id", 0) == spool->id) {
                compatible++;
                if (compatible > 1)
                    break;
            }
        }
        // if there were any, build the message
        if (compatible) {
            if (compatible > 1)
                result.messages.emplace_back(_u8L("Multiple compatible presets share the same spool ID"));
            else
                result.messages.emplace_back(_u8L("A compatible preset shares the same spool ID"));
        }

        // Check if the material types match between the base preset and the spool
        if (!use_preset_data && base_preset->config.opt_string("filament_type", 0) != spool->filament->material) {
            result.messages.emplace_back(_u8L("The materials of the base preset and the Spoolman spool do not match"));
        }
    }

    if (result.has_failed())
        return result;

    // Begin building the new preset
    preset = new Preset(Preset::TYPE_FILAMENT, filament_preset_name);

    // Apply the base config
    if (use_preset_data) {
        preset->config.apply(preset_data_config);
    } else {
        preset->config.apply(base_preset->config);
    }

    // Get the first preset that is a system preset or base user preset in the inheritance hierarchy
    // preset data should already be based on a system preset
    if (!use_preset_data && !detach) {
        std::string inherits;
        if (const auto base = filaments.get_preset_base(*base_preset))
            inherits = base->name;
        else // fallback if the above operation fails
            inherits = base_preset->name;
        preset->config.set("inherits", inherits, true);
    }

    preset->config.set_key_value("filament_settings_id", new ConfigOptionStrings({filament_preset_name}));
    preset->filament_id = get_filament_id(filament_preset_name);

    // Apply settings from the spool on top of the base preset
    spool->apply_to_preset(preset);

    // Finalize and save
    if (use_preset_data) {
        if (auto version = Semver::parse(additional_preset_data[BBL_JSON_KEY_VERSION]))
            preset->version = *version;
    } else {
        preset->version = base_preset->version;
    }
    preset->loaded = true;

    static std::mutex save_mutex;
    // The save_current_preset function selects and modifies the current preset
    // The operation is not thread safe and will cause an exception if multiple
    // threads try to change the selected preset at the same time
    save_mutex.lock();
    filaments.save_current_preset(filament_preset_name, detach, false, preset);
    save_mutex.unlock();

    return result;
}

SpoolmanResult Spoolman::update_filament_preset_from_spool(Preset* filament_preset, bool update_from_server, bool only_update_statistics)
{
    DynamicConfig  config;
    SpoolmanResult result;
    if (filament_preset->type != Preset::TYPE_FILAMENT) {
        result.messages.emplace_back("Preset is not a filament preset");
        return result;
    }
    const int&     spool_id = filament_preset->config.opt_int("spoolman_spool_id", 0);
    if (spool_id < 1) {
        result.messages.emplace_back(
            "Preset provided does not have a valid Spoolman spool ID"); // IDs below 1 are not used by spoolman and should be ignored
        return result;
    }
    SpoolmanSpoolShrPtr spool = get_instance()->get_spoolman_spool_by_id(spool_id);
    if (!spool) {
        result.messages.emplace_back("The spool ID does not exist in the local spool cache");
        return result;
    }
    if (update_from_server)
        spool->update_from_server(!only_update_statistics);
    spool->apply_to_preset(filament_preset, only_update_statistics);
    return result;
}

SpoolmanResult Spoolman::save_preset_to_spoolman(const Preset* filament_preset)
{
    SpoolmanResult result;
    if (filament_preset->type != Preset::TYPE_FILAMENT) {
        result.messages.emplace_back("Preset is not a filament preset");
        return result;
    }
    const int&     spool_id = filament_preset->config.opt_int("spoolman_spool_id", 0);
    if (spool_id < 1) {
        result.messages.emplace_back(
            "Preset provided does not have a valid Spoolman spool ID"); // IDs below 1 are not used by spoolman and should be ignored
        return result;
    }
    SpoolmanSpoolShrPtr spool = get_instance()->get_spoolman_spool_by_id(spool_id);
    if (!spool) {
        result.messages.emplace_back("The spool ID does not exist in the local spool cache");
        return result;
    }
    if (filament_preset->is_dirty) {
        result.messages.emplace_back("Please save the current changes to the preset");
        return result;
    }

    auto& filaments = wxGetApp().preset_bundle->filaments;
    // get the first preset that is a system preset or base user preset in the inheritance hierarchy
    const DynamicPrintConfig* base_config = nullptr;
    if (!filament_preset->inherits().empty())
        if (auto base = filaments.get_preset_base(*filament_preset))
            base_config = &base->config;

    json j;
    filament_preset->save(base_config, &j);

    std::stringstream ss;
    ss << j;
    std::string preset_data = ss.str();

    // Spoolman extra fields are a string read as json
    // To save a string to an extra field, the data must be surrounded by double quotes
    // and literal quotes must be escaped twice
    std::string formated_preset_data = boost::replace_all_copy(preset_data, "\"", "\\\"");
    formated_preset_data = "\"" + formated_preset_data + "\"";

    pt::ptree pt;
    pt.add("extra.orcaslicer_preset_data", formated_preset_data);
    auto res = patch_spoolman_json("filament/" + std::to_string(spool->filament->id), pt);

    if (res.empty())
        result.messages.emplace_back("Failed to save the data");
    else
        spool->filament->preset_data = std::move(preset_data);
    return result;
}


void Spoolman::update_visible_spool_statistics(bool clear_cache)
{
    PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;
    PresetCollection& filaments    = preset_bundle->filaments;

    // Clear the cache so that it can be repopulated with the correct info
    if (clear_cache) get_instance()->clear();
    if (is_server_valid()) {
        for (const auto item : filaments.get_compatible()) {
            if (item->is_user() && item->spoolman_enabled()) {
                if (auto res = update_filament_preset_from_spool(item, true, true); res.has_failed())
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Failed to update spoolman statistics with the following error: "
                                             << res.build_single_line_message() << "Spool ID: " << item->config.opt_int("spoolman_spool_id", 0);
            }
        }
    }
}

void Spoolman::update_specific_spool_statistics(const std::vector<unsigned int>& spool_ids)
{
    PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;
    PresetCollection& filaments    = preset_bundle->filaments;

    std::set spool_ids_set(spool_ids.begin(), spool_ids.end());
    // make sure '0' is not a value
    spool_ids_set.erase(0);

    if (is_server_valid()) {
        for (const auto item : filaments.get_compatible()) {
            if (item->is_user() && spool_ids_set.count(item->config.opt_int("spoolman_spool_id", 0)) > 0) {
                if (auto res = update_filament_preset_from_spool(item, true, true); res.has_failed())
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Failed to update spoolman statistics with the following error: "
                                             << res.build_single_line_message() << "Spool ID: " << item->config.opt_int("spoolman_spool_id", 0);
            }
        }
    }
}


void Spoolman::on_server_changed()
{
    if (!is_server_valid())
        return;
    pt::ptree pt;
    pt.add("name", "OrcaSlicer Preset Data");
    pt.add("field_type", "text");
    post_spoolman_json("field/filament/orcaslicer_preset_data", pt);
}


bool Spoolman::is_server_valid(bool force_check)
{
    using namespace std::chrono;
    static time_point<steady_clock> last_validity_check;
    static bool                     last_res;

    bool res = false;
    if (!is_enabled())
        return res;

    if (!force_check) {
        if (duration_cast<seconds>(steady_clock::now() - last_validity_check).count() < 5)
            return last_res;
    }

    Http::get(get_spoolman_api_url() + "info").on_complete([&res](std::string, unsigned http_status) {
        if (http_status == 200)
            res = true;
    })
    .timeout_max(MAX_TIMEOUT)
    .perform_sync();

    last_validity_check = steady_clock::now();
    last_res = res;

    return res;
}

bool Spoolman::is_enabled() { return GUI::wxGetApp().app_config->get_bool("spoolman", "enabled"); }

//---------------------------------
// SpoolmanVendor
//---------------------------------

void SpoolmanVendor::update_from_server() { update_from_json(Spoolman::get_spoolman_json("vendor/" + std::to_string(id))); }

void SpoolmanVendor::update_from_json(pt::ptree json_data)
{
    id      = json_data.get<int>("id");
    name    = get_opt<string>(json_data, "name");
    comment = get_opt<string>(json_data, "comment");
}

void SpoolmanVendor::apply_to_config(Slic3r::DynamicConfig& config) const
{
    config.set_key_value("filament_vendor", new ConfigOptionStrings({name}));
    auto& config_note = config.opt<ConfigOptionStrings>("filament_notes", true)->get_at(0);
    update_note(config_note, "vendor", comment);
}

//---------------------------------
// SpoolmanFilament
//---------------------------------

void SpoolmanFilament::update_from_server(bool recursive)
{
    const boost::property_tree::ptree& json_data = Spoolman::get_spoolman_json("filament/" + std::to_string(id));
    update_from_json(json_data);
    if (recursive && vendor)
        vendor->update_from_json(json_data.get_child("vendor"));
}

void SpoolmanFilament::update_from_json(pt::ptree json_data)
{
    auto vendor_id = json_data.get_optional<int>("vendor.id");
    if (vendor && !vendor_id.has_value()) {
        vendor = nullptr;
    } else if (vendor_id.has_value() && (!vendor || vendor->id != vendor_id.get())) {
        auto val = vendor_id.get();
        if (!m_spoolman->m_vendors.count(val))
            m_spoolman->m_vendors.emplace(val, make_shared<SpoolmanVendor>(SpoolmanVendor(json_data.get_child("vendor"))));
        vendor = m_spoolman->m_vendors[val];
    }
    id             = json_data.get<int>("id");
    name           = get_opt<string>(json_data, "name");
    material       = get_opt<string>(json_data, "material");
    price          = get_opt<double>(json_data, "price");
    density        = get_opt<double>(json_data, "density");
    diameter       = get_opt<double>(json_data, "diameter");
    weight         = get_opt<double>(json_data, "weight");
    article_number = get_opt<string>(json_data, "article_number");
    extruder_temp  = get_opt<int>(json_data, "settings_extruder_temp");
    bed_temp       = get_opt<int>(json_data, "settings_bed_temp");
    color          = "#" + get_opt<string>(json_data, "color_hex");
    preset_data    = get_opt<string>(json_data, "extra.orcaslicer_preset_data");
    comment        = get_opt<string>(json_data, "comment");
    if (!preset_data.empty()) {
        boost::trim_if(preset_data, [](char c) { return c == '"'; });
        boost::replace_all(preset_data, "\\\"", "\"");
    }
}

void SpoolmanFilament::apply_to_config(Slic3r::DynamicConfig& config) const
{
    config.set_key_value("filament_type", new ConfigOptionStrings({material}));
    config.set_key_value("filament_cost", new ConfigOptionFloats({price}));
    config.set_key_value("filament_density", new ConfigOptionFloats({density}));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({diameter}));
    if (extruder_temp > 0) {
        config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts({extruder_temp + 5}));
        config.set_key_value("nozzle_temperature", new ConfigOptionInts({extruder_temp}));
    }
    if (bed_temp > 0) {
        config.set_key_value("hot_plate_temp_initial_layer", new ConfigOptionInts({bed_temp + 5}));
        config.set_key_value("hot_plate_temp", new ConfigOptionInts({bed_temp}));
    }
    config.set_key_value("default_filament_colour", new ConfigOptionStrings{color});
    auto& config_note = config.opt<ConfigOptionStrings>("filament_notes", true)->get_at(0);
    update_note(config_note, "filament", comment);
    if (vendor)
        vendor->apply_to_config(config);
}

bool SpoolmanFilament::get_config_from_preset_data(DynamicPrintConfig& config, std::map<std::string, std::string>* additional_values) const
{
    if (preset_data.empty())
        return false;
    json        j = json::parse(preset_data);
    ConfigSubstitutionContext context(Enable);
    std::map<std::string, std::string> key_values;
    std::string reason;
    DynamicPrintConfig new_config;
    new_config.load_from_json(j, context, true, key_values, reason);
    if (!reason.empty())
        return false;
    auto& presets = wxGetApp().preset_bundle->filaments;
    if (!presets.load_full_config(new_config))
        return false;
    const auto invalid_keys = Preset::remove_invalid_keys(new_config, presets.default_preset().config);
    if (!invalid_keys.empty())
        return false;

    // Checks passed - apply to preset
    config = std::move(new_config);
    if (additional_values)
        *additional_values = std::move(key_values);
    return true;
}


//---------------------------------
// SpoolmanSpool
//---------------------------------

void SpoolmanSpool::update_from_server(bool recursive)
{
    const boost::property_tree::ptree& json_data = Spoolman::get_spoolman_json("spool/" + std::to_string(id));
    update_from_json(json_data);
    if (recursive) {
        filament->update_from_json(json_data.get_child("filament"));
        if (get_vendor())
            get_vendor()->update_from_json(json_data.get_child("filament.vendor"));
    }
}

std::string SpoolmanSpool::get_preset_name()
{
    string name;
    if (get_vendor())
        name += get_vendor()->name;
    if (!filament->name.empty())
        name += " " + filament->name;
    if (!filament->material.empty())
        name += " " + filament->material;
    boost::trim(name);

    return remove_special_key(name);
}

void SpoolmanSpool::apply_to_config(Slic3r::DynamicConfig& config) const
{
    config.set_key_value("spoolman_spool_id", new ConfigOptionInts({id}));
    auto& config_note = config.opt<ConfigOptionStrings>("filament_notes", true)->get_at(0);
    update_note(config_note, "spool", comment);
    filament->apply_to_config(config);
}

void SpoolmanSpool::apply_to_preset(Preset* preset, bool only_update_statistics) const
{
    auto spoolman_stats = preset->spoolman_statistics;
    spoolman_stats->remaining_weight = remaining_weight;
    spoolman_stats->used_weight = used_weight;
    spoolman_stats->remaining_length = remaining_length;
    spoolman_stats->used_length = used_length;
    spoolman_stats->archived = archived;
    if (only_update_statistics)
        return;
    this->apply_to_config(preset->config);
}

void SpoolmanSpool::update_from_json(pt::ptree json_data)
{
    if (int filament_id = json_data.get<int>("filament.id"); filament && filament->id != filament_id) {
        if (!m_spoolman->m_filaments.count(filament_id))
            m_spoolman->m_filaments.emplace(filament_id, make_shared<SpoolmanFilament>(SpoolmanFilament(json_data.get_child("filament"))));
        filament = m_spoolman->m_filaments.at(filament_id);
    }
    id               = json_data.get<int>("id");
    comment          = get_opt<string>(json_data, "comment");
    remaining_weight = get_opt<double>(json_data, "remaining_weight");
    used_weight      = get_opt<double>(json_data, "used_weight");
    remaining_length = get_opt<double>(json_data, "remaining_length");
    used_length      = get_opt<double>(json_data, "used_length");
    archived         = get_opt<bool>(json_data, "archived");
}

} // namespace Slic3r