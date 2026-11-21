#ifndef slic3r_KobraX_hpp_
#define slic3r_KobraX_hpp_

// Anycubic Kobra X LAN protocol, without any I/O: the credential answer, MQTT 3.1.1 framing, the
// request envelope, the report state and its translation to the Device tab's status message.
//
// The printer has no web interface. It answers HTTP on port 18910 (/info, /ctrl, /gcode_upload) and
// MQTT 3.1.1 over mutual TLS on port 9883. Facts come from Kobra Control
// (https://github.com/bloodf/kobracontrol, GPL-3.0-only, by the author of this code), which ported
// them from Kobralink and KX-Bridge (GPL-3.0); they were observed on firmware 2.0.2.8.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace KobraX {

constexpr int HTTP_PORT = 18910;
constexpr int MQTT_PORT = 9883;

struct Credentials
{
    std::string host;
    std::string username;
    std::string password;
    std::string device_id;
    std::string mode_id;
};

std::string md5_hex(const std::string& text);
// The /ctrl request's sign: md5(md5(token[0:16]) + ts + nonce)
std::string ctrl_sign(const std::string& token, const std::string& ts, const std::string& nonce);
bool        aes128_cbc_decrypt(const std::string& key, const std::string& iv, const std::string& data, std::string& plain);
// The /ctrl answer holds the MQTT login, AES-128-CBC encrypted with token[16:32]. Its own "ip" is
// ignored: the printer does not choose where we connect.
bool parse_ctrl_answer(const std::string& token, const std::string& body, const std::string& host, Credentials& creds, std::string& error);

namespace Mqtt {
enum Type : uint8_t { CONNECT = 1, CONNACK = 2, PUBLISH = 3, SUBSCRIBE = 8, SUBACK = 9, PINGREQ = 12, PINGRESP = 13, DISCONNECT = 14 };

struct Packet
{
    uint8_t     type  = 0;
    uint8_t     flags = 0;
    std::string body;
};

std::string connect(const std::string& client_id, const std::string& username, const std::string& password, uint16_t keepalive);
std::string subscribe(uint16_t packet_id, const std::string& topic);
std::string publish(const std::string& topic, const std::string& payload);
std::string pingreq();
std::string disconnect();
// Moves every complete packet out of `buffer`; false on a malformed or oversized packet.
bool split(std::string& buffer, std::vector<Packet>& packets);
bool parse_publish(const Packet& packet, std::string& topic, std::string& payload);
} // namespace Mqtt

// kind "slicer" (what Anycubic Slicer Next's slicer sends) or "web" (its Workbench page: temperatures, print/update)
std::string publish_topic(const Credentials& creds, const std::string& type, const std::string& kind = "slicer");
std::string subscribe_topic(const Credentials& creds);
// "info/report" for ".../<deviceId>/info/report"
std::string report_suffix(const std::string& topic);
nlohmann::json request(const std::string& type, const std::string& action, const nlohmann::json& data, const std::string& msgid, int64_t timestamp_ms);

// One filament source, numbered as the Device tab numbers trays (ams_id * 4 + slot). The printer reports its
// sources in multiColorBox/report: box id 0..3 are ACE units, box id -1 is the print head's own feeder, whose
// slots are the spools on the printer's spool holder. With an ACE connected the head's fourth inlet carries the
// ACE, so its first three slots are the holder positions: 3 + 4 sources with one ACE.
struct Slot
{
    int         tray      = 0;
    int         ams_index = 0; // the slot number print/start's ams_box_mapping takes
    bool        holder    = false; // a spool-holder position (box -1), not an ACE slot
    bool        loaded    = false;
    std::string type;
    int         r = 255, g = 255, b = 255;
    int         percent = -1;
};

// Reports folded into one picture of the printer. Not thread safe: the owner locks.
class State
{
public:
    void apply(const std::string& suffix, const nlohmann::json& payload);
    // {"print": {...}} as MachineObject parses a Bambu push. filament_id maps a material to a filament_id.
    nlohmann::json push_status(bool connected, const std::function<std::string(const std::string&)>& filament_id = {}) const;
    std::vector<Slot> slots() const;
    // "idle", "printing", "paused", "finished", "cancelled", "error" or "busy"
    std::string view_state() const;
    bool        preparing() const;

    std::string kobra_state; // raw printer state, "" before any report
    std::string firmware;
    std::string model;
    std::string filename;
    std::string task_id;
    std::string camera_url;
    std::string upload_url;
    double      progress = 0, layer = 0, total_layers = 0, remaining_s = 0;
    double      nozzle_temp = 0, nozzle_target = 0, bed_temp = 0, bed_target = 0, fan_pct = 0;
    int         speed_mode = 0;
    bool        light_on   = false;
    nlohmann::json features = nlohmann::json::object();
    nlohmann::json boxes    = nlohmann::json::array(); // multiColorBox/report multi_color_box

private:
    void set_state(const std::string& state, bool in_job);
    void print_fields(const nlohmann::json& data, const std::string& state);
    void on_print(const nlohmann::json& payload, const nlohmann::json& data);
    void on_info(const nlohmann::json& data);
    void on_temps(const nlohmann::json& temps);

    bool m_in_job    = false;
    bool m_error_end = false;
};

// print/start data for a file already on the printer. filepath is the folder: firmware 2.0.2.8 joins it
// with filename, and "/<name>" ends the task with code 10111.
nlohmann::json print_start_data(const std::string& filename, uint64_t size, const nlohmann::json& box_mapping, bool auto_leveling, bool flow_calibration, bool timelapse);
// One message to the printer: its topic kind ("slicer" or "web"), type, action and data.
struct Message
{
    std::string    kind = "slicer";
    std::string    type;
    std::string    action;
    nlohmann::json data;
};

enum class Control { Sent, NoOp, Unsupported, Refused };

// A Device tab "print" command other than pause, resume and stop, as the messages that carry it on a Kobra X:
// gcode_line (M104, M140, M106 P1, G28 / G28 Z, G91 + G1 X|Y|Z + G90 jogs, G90 alone), set_bed_temp,
// set_nozzle_temp, set_fan (fan_index 1) and print_speed 1-3. `state` is State::view_state(), `task_id` the job's.
// Refused when the state does not allow it: moves only while idle, the speed mode only during a print.
// Ported from Kobra Control's bambu.py parse_command and service.py.
Control device_control(const nlohmann::json& print, const std::string& state, const std::string& task_id, std::vector<Message>& out);

// OrcaSlicer's ams_mapping ("[tray, ...]", -1 = unused) as print/start's ams_box_mapping. Without any
// tray chosen, every loaded slot in order. False, with the reason, when a chosen tray cannot print.
bool box_mapping(const std::string& ams_mapping, const std::vector<Slot>& slots, nlohmann::json& mapping, std::string& error);

}} // namespace Slic3r::KobraX

#endif
