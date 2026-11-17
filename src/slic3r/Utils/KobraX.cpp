#include "KobraX.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>

#include <openssl/evp.h>

using json = nlohmann::json;

namespace Slic3r { namespace KobraX {

namespace {

// Printer packets are a few KB; refuse more before buffering it
constexpr size_t MAX_PACKET = 4 * 1024 * 1024;

// Job phases (print/report action "start"). They belong to a running print, and the numbers they report are
// their own, often 0, so progress and layers keep their last values meanwhile.
const std::set<std::string> PRE_PRINT = {"preheating", "auto_leveling", "checking", "updated", "init", "hotbed_heating",
                                         "nozzle_heating", "nozzle_cooling", "nozzle_cooling_off", "hotbed_cooling_off",
                                         "nozzle_purging", "nozzle_wiping", "frequency_sweeping", "z_offset_calibrating",
                                         "extfilbox_filament_feeding", "ace_filament_feeding", "ace_filament_unfeeding"};
const std::set<std::string> RUNNING  = {"printing", "resuming", "resumed", "stopping"};
const std::set<std::string> PAUSED   = {"pausing", "paused", "pause"};
const std::set<std::string> TERMINAL = {"finished", "stoped", "canceled"};
// print/report actions that describe the job; others (getSliceParam answers another client) do not
const std::set<std::string> JOB_ACTIONS = {"start", "pause", "resume", "stop", "finish", "query", ""};
// Job phase -> Bambu stage id, which the Device tab shows as text. Phases without a matching text stay 0.
const std::map<std::string, int> STAGES = {{"auto_leveling", 1},  {"hotbed_heating", 2}, {"frequency_sweeping", 3},
                                           {"nozzle_heating", 7}, {"nozzle_purging", 14}, {"nozzle_wiping", 14},
                                           {"ace_filament_unfeeding", 22}, {"ace_filament_feeding", 24}};
constexpr int READY_STATUS = 5; // slot status: filament loaded

bool contains(const std::set<std::string>& set, const std::string& value) { return set.count(value) != 0; }

std::string normalise(const std::string& state)
{
    if (state.empty() || state == "free")
        return "idle";
    if (contains(PAUSED, state))
        return "paused";
    if (state == "finished")
        return "finished";
    if (state == "stoped" || state == "canceled")
        return "cancelled";
    if (state == "failed")
        return "error";
    return contains(PRE_PRINT, state) || contains(RUNNING, state) ? "printing" : "busy";
}

// A finite number from a JSON number or numeric string, else `fallback`
double num(const json& value, double fallback = 0)
{
    double v = fallback;
    if (value.is_number() && !value.is_boolean())
        v = value.get<double>();
    else if (value.is_string()) {
        try {
            size_t used = 0;
            const std::string& text = value.get_ref<const std::string&>();
            v = std::stod(text, &used);
            if (used != text.size())
                v = fallback;
        } catch (...) {
            v = fallback;
        }
    }
    return std::isfinite(v) ? v : fallback;
}

std::string str(const json& obj, const char* key)
{
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

const json& object(const json& obj, const char* key)
{
    static const json empty = json::object();
    auto it = obj.find(key);
    return it != obj.end() && it->is_object() ? *it : empty;
}

std::string mqtt_string(const std::string& text) { return std::string{char(text.size() >> 8), char(text.size() & 0xFF)} + text; }

std::string mqtt_packet(uint8_t head, const std::string& body)
{
    std::string out(1, char(head));
    size_t      n = body.size();
    do {
        uint8_t digit = n % 128;
        n /= 128;
        out += char(digit | (n ? 0x80 : 0));
    } while (n);
    return out + body;
}

std::string hex(const unsigned char* data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 0x0F];
    }
    return out;
}

bool base64_decode(const std::string& text, std::string& out)
{
    if (text.empty() || text.size() % 4 != 0)
        return false;
    std::string buffer(text.size() / 4 * 3, '\0');
    const int   n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(&buffer[0]), reinterpret_cast<const unsigned char*>(text.data()),
                                    int(text.size()));
    if (n < 0)
        return false;
    // EVP_DecodeBlock keeps the bytes the padding stands for
    const size_t padding = std::count(text.end() - 2, text.end(), '=');
    buffer.resize(size_t(n) - padding);
    out = std::move(buffer);
    return true;
}

json rgba(const Slot& slot) { return json::array({slot.r, slot.g, slot.b, 255}); }

json mapping_entry(int paint_index, const Slot& slot, const json& paint_color)
{
    return {{"paint_index", paint_index},
            {"ams_index", slot.ams_index},
            {"paint_color", paint_color},
            {"ams_color", rgba(slot)},
            {"material_type", slot.type.empty() ? "PLA" : slot.type}};
}

} // namespace

std::string md5_hex(const std::string& text)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  size = 0;
    EVP_Digest(text.data(), text.size(), digest, &size, EVP_md5(), nullptr);
    return hex(digest, size);
}

std::string ctrl_sign(const std::string& token, const std::string& ts, const std::string& nonce)
{
    return md5_hex(md5_hex(token.substr(0, 16)) + ts + nonce);
}

bool aes128_cbc_decrypt(const std::string& key, const std::string& iv, const std::string& data, std::string& plain)
{
    if (key.size() != 16 || iv.size() != 16 || data.empty() || data.size() % 16 != 0)
        return false;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    std::string out(data.size() + 16, '\0');
    int         n = 0, last = 0;
    auto*       dst = reinterpret_cast<unsigned char*>(&out[0]);
    if (!ctx ||
        EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(iv.data())) != 1 ||
        EVP_DecryptUpdate(ctx.get(), dst, &n, reinterpret_cast<const unsigned char*>(data.data()), int(data.size())) != 1 ||
        EVP_DecryptFinal_ex(ctx.get(), dst + n, &last) != 1)
        return false;
    out.resize(size_t(n + last));
    plain = std::move(out);
    return true;
}

bool parse_ctrl_answer(const std::string& token, const std::string& body, const std::string& host, Credentials& creds, std::string& error)
{
    const json  answer = json::parse(body, nullptr, false);
    const json  data   = answer.is_object() ? object(answer, "data") : json::object();
    if (token.size() < 32 || !data.contains("info") || !data["info"].is_string() || !data.contains("token") ||
        !data["token"].is_string()) {
        error = "unexpected /ctrl answer from the printer";
        return false;
    }
    std::string cipher, plain;
    if (!base64_decode(data["info"].get<std::string>(), cipher) ||
        !aes128_cbc_decrypt(token.substr(16, 16), data["token"].get<std::string>(), cipher, plain)) {
        error = "could not decrypt the printer's LAN credentials";
        return false;
    }
    const json login = json::parse(plain, nullptr, false);
    if (!login.is_object()) {
        error = "could not decrypt the printer's LAN credentials";
        return false;
    }
    if (!str(login, "error").empty()) {
        error = "the printer refused LAN access: " + str(login, "error");
        return false;
    }
    creds           = Credentials{};
    creds.host      = host;
    creds.username  = str(login, "username");
    creds.password  = str(login, "password");
    creds.device_id = str(login, "deviceId");
    creds.mode_id   = str(login, "modeId").empty() ? "20030" : str(login, "modeId");
    if (creds.device_id.empty()) {
        error = "the printer's LAN credentials name no device";
        return false;
    }
    return true;
}

namespace Mqtt {

std::string connect(const std::string& client_id, const std::string& username, const std::string& password, uint16_t keepalive)
{
    // Clean session, user name and password
    std::string body = mqtt_string("MQTT") + char(4) + char(0x02 | 0x80 | 0x40) + char(keepalive >> 8) + char(keepalive & 0xFF);
    return mqtt_packet(CONNECT << 4, body + mqtt_string(client_id) + mqtt_string(username) + mqtt_string(password));
}

std::string subscribe(uint16_t packet_id, const std::string& topic)
{
    return mqtt_packet(SUBSCRIBE << 4 | 2, std::string{char(packet_id >> 8), char(packet_id & 0xFF)} + mqtt_string(topic) + char(0));
}

std::string publish(const std::string& topic, const std::string& payload) { return mqtt_packet(PUBLISH << 4, mqtt_string(topic) + payload); }
std::string pingreq() { return mqtt_packet(PINGREQ << 4, {}); }
std::string disconnect() { return mqtt_packet(DISCONNECT << 4, {}); }

bool split(std::string& buffer, std::vector<Packet>& packets)
{
    size_t pos = 0;
    while (buffer.size() - pos >= 2) {
        size_t length = 0, multiplier = 1, i = pos + 1;
        bool   complete = false;
        for (int digits = 0; digits < 4 && i < buffer.size(); ++digits, ++i) {
            const auto byte = uint8_t(buffer[i]);
            length += (byte & 0x7F) * multiplier;
            multiplier *= 128;
            if (!(byte & 0x80)) {
                complete = true;
                ++i;
                break;
            }
            if (digits == 3)
                return false; // a fifth length byte
        }
        if (!complete)
            break;
        if (length > MAX_PACKET)
            return false;
        if (buffer.size() - i < length)
            break;
        packets.push_back({uint8_t(uint8_t(buffer[pos]) >> 4), uint8_t(buffer[pos] & 0x0F), buffer.substr(i, length)});
        pos = i + length;
    }
    buffer.erase(0, pos);
    return true;
}

bool parse_publish(const Packet& packet, std::string& topic, std::string& payload)
{
    if (packet.type != PUBLISH || packet.body.size() < 2)
        return false;
    const size_t n     = size_t(uint8_t(packet.body[0])) << 8 | uint8_t(packet.body[1]);
    const size_t start = 2 + n + (((packet.flags >> 1) & 3) ? 2 : 0); // QoS > 0 carries a packet id
    if (packet.body.size() < start)
        return false;
    topic   = packet.body.substr(2, n);
    payload = packet.body.substr(start);
    return true;
}

} // namespace Mqtt

std::string publish_topic(const Credentials& creds, const std::string& type)
{
    return "anycubic/anycubicCloud/v1/slicer/printer/" + creds.mode_id + "/" + creds.device_id + "/" + type;
}

std::string subscribe_topic(const Credentials& creds)
{
    return "anycubic/anycubicCloud/v1/printer/public/" + creds.mode_id + "/" + creds.device_id + "/#";
}

std::string report_suffix(const std::string& topic)
{
    const size_t last = topic.rfind('/');
    if (last == std::string::npos || last == 0)
        return topic;
    const size_t before = topic.rfind('/', last - 1);
    return before == std::string::npos ? topic : topic.substr(before + 1);
}

json request(const std::string& type, const std::string& action, const json& data, const std::string& msgid, int64_t timestamp_ms)
{
    return {{"type", type}, {"action", action}, {"msgid", msgid}, {"timestamp", timestamp_ms}, {"data", data}};
}

// State -------------------------------------------------------------------------------------------------

std::string State::view_state() const
{
    std::string state = kobra_state.empty() ? "busy" : normalise(kobra_state);
    if (state == "busy" && m_in_job)
        return "printing";
    if (state == "cancelled" && m_error_end)
        return "error";
    return state;
}

bool State::preparing() const
{
    return view_state() == "printing" && (contains(PRE_PRINT, kobra_state) || (m_in_job && normalise(kobra_state) == "busy"));
}

void State::set_state(const std::string& state, bool in_job)
{
    kobra_state     = state;
    const bool was  = m_in_job;
    m_in_job        = in_job && !contains(TERMINAL, state) && state != "free";
    if (m_in_job && !was) // a job begins: nothing of the last one carries over
        progress = layer = total_layers = remaining_s = 0;
    else if (state == "free")
        progress = 0;
    if (state != "stoped")
        m_error_end = false;
}

void State::print_fields(const json& data, const std::string& state)
{
    const bool in_phase = contains(PRE_PRINT, state) || (m_in_job && normalise(state) == "busy");
    if (!in_phase) {
        // Never back within a job: after the last layer the printer reports progress 0
        if (data.contains("progress") && (contains(TERMINAL, state) || num(data["progress"]) >= progress))
            progress = num(data["progress"]);
        if (data.contains("remain_time"))
            remaining_s = num(data["remain_time"]) * 60;
        if (data.contains("curr_layer"))
            layer = num(data["curr_layer"]);
        if (data.contains("total_layers"))
            total_layers = num(data["total_layers"]);
    }
    if (data.contains("taskid"))
        task_id = data["taskid"].is_string() ? data["taskid"].get<std::string>() : data["taskid"].dump();
}

void State::on_print(const json& payload, const json& data)
{
    const json& settings = object(data, "settings");
    if (settings.contains("print_speed_mode"))
        speed_mode = int(num(settings["print_speed_mode"]));
    const std::string action = str(payload, "action");
    const std::string state  = str(payload, "state");
    if (!contains(JOB_ACTIONS, action) || state == "failed") // another client's answer, or a refused command
        return;
    if (state == "checking") // a new job starts
        progress = layer = total_layers = 0;
    if (!state.empty()) {
        set_state(state, action == "start" || m_in_job);
        const json code = payload.value("code", json());
        if (state == "stoped" && !code.is_null() && num(code) != 0 && num(code) != 200)
            m_error_end = true; // the printer ended the task itself (10111: abnormal end of task)
    }
    json fields = data;
    if (contains(TERMINAL, state)) {
        filename.clear();
        progress = remaining_s = 0;
        if (state != "finished") // a finished job keeps its layers until the next one starts
            layer = total_layers = 0;
        fields = json::object();
        for (const char* key : {"progress", "taskid"})
            if (data.contains(key))
                fields[key] = data[key];
    } else if (data.contains("filename") && data["filename"].is_string()) {
        const std::string name = data["filename"].get<std::string>(); // "/<name>"
        filename               = name.substr(name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
    }
    print_fields(fields, state);
}

void State::on_temps(const json& temps)
{
    if (temps.empty())
        return;
    nozzle_temp   = num(temps.value("curr_nozzle_temp", json()));
    nozzle_target = num(temps.value("target_nozzle_temp", json()));
    bed_temp      = num(temps.value("curr_hotbed_temp", json()));
    bed_target    = num(temps.value("target_hotbed_temp", json()));
}

void State::on_info(const json& data)
{
    if (!str(data, "version").empty())
        firmware = str(data, "version");
    for (const char* key : {"modelName", "model"})
        if (!str(data, key).empty())
            model = str(data, key);
    json              project = object(data, "project");
    const std::string state   = str(project, "state").empty() ? str(data, "state") : str(project, "state");
    if (!state.empty())
        set_state(state, !project.empty());
    if (contains(TERMINAL, state)) {
        if (state != "finished")
            layer = total_layers = 0;
        for (const char* key : {"filename", "curr_layer", "total_layers"})
            project.erase(key);
    } else if (project.contains("filename") && project["filename"].is_string()) {
        filename = project["filename"].get<std::string>();
    }
    print_fields(project, state);
    on_temps(object(data, "temp"));
    const json& urls = object(data, "urls");
    if (!str(urls, "fileUploadurl").empty())
        upload_url = str(urls, "fileUploadurl");
    if (!str(urls, "rtspUrl").empty()) // in fact HTTP-FLV, http://<printer>:18088/live/<token>
        camera_url = str(urls, "rtspUrl");
    if (data.contains("features") && data["features"].is_object())
        features = data["features"];
    if (data.contains("fan_speed_pct"))
        fan_pct = num(data["fan_speed_pct"]);
    if (data.contains("print_speed_mode"))
        speed_mode = int(num(data["print_speed_mode"]));
}

void State::apply(const std::string& suffix, const json& payload)
{
    if (!payload.is_object())
        return;
    const json& data = object(payload, "data");
    if (suffix == "print/report")
        on_print(payload, data);
    else if (suffix == "info/report")
        on_info(data);
    else if (suffix == "tempature/report") // spelled so by the printer
        on_temps(data);
    else if (suffix == "multiColorBox/report") {
        // A refused setInfo answers "failed" with data as a list: keep the slots
        if (str(payload, "state") != "failed" && data.contains("multi_color_box") && data["multi_color_box"].is_array())
            boxes = data["multi_color_box"];
    } else if (suffix == "light/report")
        light_on = num(data.value("status", json())) != 0;
    else if (suffix == "fan/report" && data.contains("fan_speed_pct"))
        fan_pct = num(data["fan_speed_pct"]);
    else if (suffix == "status/report") {
        // The device is busy (moving, homing) or free; during a job the job's own state says more
        const std::string state = str(payload, "state");
        if (str(payload, "action") == "workReport" && (state == "busy" || state == "free") && !m_in_job)
            set_state(state, false);
    }
}

std::vector<Slot> State::slots() const
{
    const json* head = nullptr;
    std::vector<const json*> aces;
    for (const json& box : boxes) {
        if (!box.is_object() || !box.contains("id") || !box["id"].is_number_integer())
            continue;
        if (box["id"].get<int>() == -1)
            head = &box;
        else if (box["id"].get<int>() >= 0)
            aces.push_back(&box);
    }
    std::sort(aces.begin(), aces.end(), [](const json* a, const json* b) { return (*a)["id"].get<int>() < (*b)["id"].get<int>(); });
    // ACE units with the print head ("hub"), ACE units alone, or the print head alone
    const bool hub = head && !aces.empty();

    std::vector<Slot> out;
    auto add = [&out](const json& box, size_t max_slots, int box_pos, int ams_base) {
        const json& slots = box.contains("slots") && box["slots"].is_array() ? box["slots"] : json::array();
        for (size_t i = 0; i < slots.size() && i < max_slots; ++i) {
            const json& raw = slots[i].is_object() ? slots[i] : json::object();
            Slot        slot;
            slot.tray      = box_pos * 4 + int(i);
            slot.ams_index = ams_base + int(i);
            slot.loaded    = int(num(raw.value("status", json()))) == READY_STATUS;
            slot.type      = str(raw, "type");
            const json& c  = raw.value("color", json());
            if (c.is_array() && c.size() >= 3) {
                slot.r = std::clamp(int(num(c[0])), 0, 255);
                slot.g = std::clamp(int(num(c[1])), 0, 255);
                slot.b = std::clamp(int(num(c[2])), 0, 255);
            }
            if (raw.contains("consumables_percent"))
                slot.percent = std::clamp(int(num(raw["consumables_percent"], -1)), -1, 100);
            out.push_back(slot);
        }
    };
    if (aces.empty()) {
        if (head)
            add(*head, 4, 0, 0);
        return out;
    }
    if (hub)
        add(*head, 3, 0, 0);
    for (const json* ace : aces) {
        const int id = (*ace)["id"].get<int>();
        // In hub mode the head takes ams_id 0 and print slots 0..2, the first ACE 4..7
        add(*ace, 4, hub ? id + 1 : id, hub ? 4 + id * 4 : id * 4);
    }
    return out;
}

json State::push_status(bool connected, const std::function<std::string(const std::string&)>& filament_id) const
{
    static const std::map<std::string, std::string> gcode_states = {{"idle", "IDLE"},    {"printing", "RUNNING"},
                                                                     {"busy", "RUNNING"}, {"paused", "PAUSE"},
                                                                     {"finished", "FINISH"}, {"cancelled", "FAILED"},
                                                                     {"error", "FAILED"}};
    static const std::map<std::string, int>         print_stages = {{"printing", 1}, {"paused", 2}, {"finished", 3}, {"error", 4}};

    const std::string state = connected ? view_state() : "idle";
    json              p     = json::object();
    p["command"]            = "push_status";
    p["msg"]                = 0;
    p["support_mqtt_alive"] = true;
    // Every print message reaches DevFan's command handler, which logs one without a sequence_id
    p["sequence_id"]        = "0";
    // A print in its preparation phase is PREPARE, which still counts as printing, so no new print starts
    const auto gcode_state  = gcode_states.find(state);
    p["gcode_state"]        = connected && preparing() && layer == 0 ? "PREPARE" : gcode_state != gcode_states.end() ? gcode_state->second : "RUNNING";
    const auto print_stage  = print_stages.find(state);
    p["mc_print_stage"]     = print_stage != print_stages.end() ? print_stage->second : 0;
    if (state == "printing" || state == "paused") {
        const auto stage = STAGES.find(kobra_state);
        p["stg"]         = json::array({stage != STAGES.end() ? stage->second : 0});
        p["stg_cur"]     = stage != STAGES.end() ? stage->second : 0;
    } else {
        p["stg"]     = json::array();
        p["stg_cur"] = 0;
    }
    p["mc_print_error_code"] = 0;
    p["print_error"]         = 0;
    // The print dialog refuses a LAN print without storage, read from "sdcard" and then from home_flag bits 8-9
    // (1 = present). Bit 16 offers flow calibration in the print dialog.
    auto feature = [this](const char* name) { return features.is_object() && features.contains(name) ? features[name] : json(); };
    const bool flow    = feature("flow_calibration_support") != false;
    p["sdcard"]        = connected;
    p["home_flag"]     = (connected ? 1 << 8 : 0) | (flow ? 1 << 16 : 0);
    p["support_send_to_sd"]   = true;
    p["support_timelapse"]    = feature("camera_timelapse_support") == true;
    p["support_bed_leveling"] = feature("auto_leveling_support") == false ? 0 : 1;
    p["nozzle_temp_range"]    = json::array({0, 300});
    p["bed_temp_range"]       = json::array({0, 120});
    p["nozzle_temper"]        = nozzle_temp;
    p["nozzle_target_temper"] = nozzle_target;
    p["bed_temper"]           = bed_temp;
    p["bed_target_temper"]    = bed_target;
    p["fan_gear"]             = int(std::clamp(fan_pct, 0.0, 100.0) * 255 / 100 + 0.5);
    p["subtask_name"]         = filename;
    p["gcode_file"]           = filename;
    p["mc_percent"]           = int(std::lround(std::clamp(progress, 0.0, 100.0)));
    p["mc_remaining_time"]    = int(std::max(0.0, remaining_s) / 60);
    p["layer_num"]            = int(layer);
    p["total_layer_num"]      = int(total_layers);
    p["spd_lvl"]              = speed_mode >= 1 && speed_mode <= 3 ? speed_mode : 2;
    p["lights_report"]        = json::array({{{"node", "chamber_light"}, {"mode", light_on ? "on" : "off"}}});
    p["ipcam"]                = {{"ipcam_dev", camera_url.empty() ? "0" : "1"}};

    // The ACE units as AMS Lite units (info 0002), trays numbered ams_id * 4 + slot
    std::map<int, json> units;
    unsigned long       ams_bits = 0, tray_bits = 0;
    bool                remain   = false;
    for (const Slot& slot : slots()) {
        const int ams_id = slot.tray / 4, index = slot.tray % 4;
        ams_bits |= 1ul << ams_id;
        json tray = {{"id", std::to_string(index)}, {"tag_uid", "0000000000000000"}};
        if (slot.loaded) {
            tray_bits |= 1ul << slot.tray;
            char color[9];
            std::snprintf(color, sizeof(color), "%02X%02X%02XFF", slot.r, slot.g, slot.b);
            tray["tray_type"]     = slot.type;
            tray["tray_color"]    = color;
            tray["cols"]          = json::array({color});
            tray["ctype"]         = 1;
            tray["tray_info_idx"] = filament_id && !slot.type.empty() ? filament_id(slot.type) : std::string();
            tray["remain"]        = slot.percent;
            remain                = remain || slot.percent >= 0;
        } else {
            tray["tray_info_idx"]         = "";
            tray["tray_type"]             = "";
            tray["tray_color"]            = "00000000";
            tray["tray_slot_placeholder"] = "1";
        }
        json& unit = units[ams_id];
        if (unit.is_null())
            unit = {{"id", std::to_string(ams_id)}, {"info", "0002"}, {"tray", json::array()}};
        unit["tray"].push_back(tray);
    }
    json ams = json::array();
    for (auto& [id, unit] : units)
        ams.push_back(unit);
    char ams_hex[17], tray_hex[17];
    std::snprintf(ams_hex, sizeof(ams_hex), "%lX", ams_bits);
    std::snprintf(tray_hex, sizeof(tray_hex), "%lX", tray_bits);
    // The Device tab shows a spool's "remain" only with calibrate_remain_flag
    p["ams"] = {{"ams", ams}, {"ams_exist_bits", ams_hex}, {"tray_exist_bits", tray_hex}, {"calibrate_remain_flag", remain}};

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    return {{"print", p}, {"t_utc", int64_t(now.count())}};
}

json print_start_data(const std::string& filename, uint64_t size, const json& box_mapping, bool auto_leveling, bool flow_calibration, bool timelapse)
{
    return {{"taskid", "-1"},
            {"url", ""},
            {"filename", filename},
            {"md5", ""},
            {"filepath", "/"},
            {"filetype", 1},
            {"project_type", 1},
            {"filesize", size},
            {"ams_settings", {{"use_ams", !box_mapping.empty()}, {"ams_box_mapping", box_mapping}}},
            {"task_settings",
             {{"auto_leveling", auto_leveling ? 1 : 0},
              {"vibration_compensation", 0},
              {"flow_calibration", flow_calibration ? 1 : 0},
              {"dry_mode", 0},
              {"ai_settings", {{"status", 0}, {"count", 0}, {"type", 1}}},
              {"timelapse", {{"status", timelapse ? 1 : 0}, {"count", 0}, {"type", 64}}},
              {"drying_settings", {{"status", 0}, {"target_temp", 0}, {"duration", 0}, {"remain_time", 0}}},
              {"model_objects_skip_parts", json::array()}}}};
}

bool box_mapping(const std::string& ams_mapping, const std::vector<Slot>& slots, json& mapping, std::string& error)
{
    mapping = json::array();
    std::vector<int> trays;
    const json chosen = json::parse(ams_mapping, nullptr, false);
    if (chosen.is_array())
        for (const json& tray : chosen)
            trays.push_back(tray.is_number_integer() ? tray.get<int>() : -1);

    if (std::none_of(trays.begin(), trays.end(), [](int tray) { return tray >= 0; })) {
        // No tray chosen: filament N prints from loaded slot N, a gap from the last loaded slot, never from
        // an empty one (the printer rejects that)
        std::vector<const Slot*> loaded;
        for (const Slot& slot : slots)
            if (slot.loaded)
                loaded.push_back(&slot);
        if (loaded.empty()) {
            error = "no filament is loaded in the printer";
            return false;
        }
        const Slot* fallback = loaded.back();
        for (size_t i = 0, n = slots.size(); i < n; ++i) {
            if (&slots[i] > fallback)
                break;
            mapping.push_back(mapping_entry(int(i), slots[i].loaded ? slots[i] : *fallback, json::array({255, 255, 255, 255})));
        }
        return true;
    }

    for (size_t i = 0; i < trays.size(); ++i) {
        if (trays[i] < 0)
            continue;
        const auto slot = std::find_if(slots.begin(), slots.end(), [&](const Slot& s) { return s.tray == trays[i]; });
        const std::string label = "filament " + std::to_string(i + 1);
        if (slot == slots.end())
            error += (error.empty() ? "" : "; ") + label + ": the printer has no slot " + std::to_string(trays[i]);
        else if (!slot->loaded)
            error += (error.empty() ? "" : "; ") + label + ": slot " + std::to_string(trays[i]) + " is empty";
        else
            mapping.push_back(mapping_entry(int(i), *slot, rgba(*slot)));
    }
    return error.empty();
}

}} // namespace Slic3r::KobraX
