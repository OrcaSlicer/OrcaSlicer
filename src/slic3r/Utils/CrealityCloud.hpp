#pragma once

// Client for the Creality Cloud account and printing API (api.crealitycloud.com), the service
// Creality Print uses to send a print to a K1/K2/Hi/Ender-3 V3 printer bound to the user's account.
//
// A cloud print is three steps:
//   1. Upload: gzip the G-code and PUT it to Alibaba OSS with the temporary STS credentials the API hands
//      out (getAliyunInfo + getOssInfo), then register it (gcode/uploadGcode), which returns its id.
//   2. Start: print/cluster/addSingleTask with the printer's deviceName and the CDN URL of the upload.
//   3. Control: iotrouter/rpc/{oneway,twoway}/<tbId> with {"method":"set","params":{...}}.
//
// The pure helpers below (request bodies, response parsers, OSS request signing) carry no network
// state so they can be unit tested; Client wraps them with the HTTP calls.

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace CrealityCloud {

enum class Region { Global, China };

std::string region_to_string(Region region);
Region      region_from_string(const std::string& region);
std::string api_base_url(Region region);

// Result codes the API returns in its "code" field; 0 is success and 4 means the token is no longer valid.
constexpr int CODE_OK            = 0;
constexpr int CODE_TOKEN_INVALID = 4;
// Local failures, outside the server's range.
constexpr int CODE_NETWORK   = -1;
constexpr int CODE_PARSE     = -2;
constexpr int CODE_CANCELLED = -3;
constexpr int CODE_NO_LOGIN  = -4;
constexpr int CODE_FILE      = -5;

struct Session
{
    std::string user_id;
    std::string token;
    std::string nickname;
    std::string avatar;
    Region      region = Region::Global;

    bool valid() const { return !user_id.empty() && !token.empty(); }
};

// The signed-in account, shared by the account agent and every printer agent instance (the Device tab
// creates its own printer agent), and kept in <data_dir>/creality_cloud.json between runs.
class SessionStore
{
public:
    static Session     get();
    static void        set(const Session& session);
    static void        clear();
    // A random id the API asks every client to send (__CXY_DUID_), created once and kept with the session.
    static std::string device_uid();
    // Where the session is kept. Defaults to <data_dir>/creality_cloud.json; tests point it elsewhere.
    static void        set_file(const std::string& path);

private:
    static void load_locked();
    static void save_locked();
};

struct Device
{
    std::string dn;    // deviceName: the id addSingleTask and the device list use
    std::string tb_id; // IoT id: the id the rpc and telemetry endpoints use
    std::string name;  // User's alias for the printer
    std::string model;
    std::string mac;
    bool        online      = false;
    int         state       = 0; // deviceState: -1 offline
    int         idle_state  = 0;
    bool        multi_color = false; // CFS attached
    std::string print_file;
    int         progress    = 0;  // 0-100
    int         left_time_s = 0;
};

// Printer state as the K-series firmware reports it in telemetry key "state".
enum class PrintState { Idle = 0, Printing = 1, Finished = 2, Failed = 3, Stopped = 4, Paused = 5 };
std::string print_state_text(int state);

struct OssCredentials
{
    std::string access_key_id;
    std::string access_key_secret;
    std::string security_token;
    std::string endpoint; // e.g. oss-us-east-1.aliyuncs.com, possibly with a scheme
    std::string bucket;
};

struct UploadedGcode
{
    std::string id;
    std::string name;
    std::string file_key;
    std::string cdn_url() const { return "https://file-cdn.creality.com/" + file_key; }
};

// One extruder of the sliced file mapped to a CFS slot.
struct FilamentSlot
{
    int         extruder = 1; // 1-based extruder of the G-code
    std::string slot_id;      // CFS slot id as the printer reports it (cId)
    std::string type;         // PLA, PETG, ...
    std::string color;        // #RRGGBB
};

// --- Pure helpers ---------------------------------------------------------------------------------

std::map<std::string, std::string> base_headers(const Session& session, const std::string& device_uid);

nlohmann::json login_body(int account_type, const std::string& account, const std::string& password);
// Fills session from a login reply; returns the API code, CODE_PARSE when the body is not a login reply.
int            parse_login(const std::string& body, Session& session, std::string& error);
int            parse_user_info(const std::string& body, Session& session);
int            parse_devices(const std::string& body, std::vector<Device>& devices, std::string& error);
int            parse_aliyun_info(const std::string& body, OssCredentials& creds, std::string& error);
int            parse_oss_info(const std::string& body, OssCredentials& creds, std::string& error);
int            parse_uploaded_gcode(const std::string& body, UploadedGcode& gcode, std::string& error);
// Merges a telemetry reply ({"key":[{"ts":..,"value":..}]}) into a flat {key: value} object.
nlohmann::json parse_timeseries(const std::string& body);

// OSS object key Creality Print uses: model/slice/<MD5(name ":" md5(file))>.gcode.gz, upper-case hex.
std::string    gcode_object_key(const std::string& display_name, const std::string& file_md5_hex);
// "T1A" for extruder 1 ... "T1D" for 4, "T2A" for 5: the slot label addSingleTask expects.
std::string    extruder_slot_label(int extruder);
nlohmann::json add_task_body(const std::string&               dn,
                             const UploadedGcode&             gcode,
                             const std::string&               task_name,
                             const std::vector<FilamentSlot>& slots,
                             bool                             multi_color,
                             bool                             use_cfs);
nlohmann::json rpc_set(const nlohmann::json& params);

// The ssdp message announcing a printer: dev_id, dev_name, dev_type, connect_type plus online/status/progress.
nlohmann::json ssdp_payload(const Device& device);
// The Bambu-shaped push_status the device panels read, from the device record and its telemetry.
nlohmann::json status_payload(const Device& device, const nlohmann::json& telemetry);
// Maps the send dialog's ams_mapping ("[tray, tray, ...]" per filament, -1 for none) to CFS slots.
std::vector<FilamentSlot> filament_slots(const std::string& ams_mapping, const std::vector<std::string>& types, const std::vector<std::string>& colors);

// OSS V1 request signature: base64(HMAC-SHA1(secret, VERB \n MD5 \n TYPE \n DATE \n x-oss-headers resource)).
std::string oss_signature(const std::string&                        secret,
                          const std::string&                        verb,
                          const std::string&                        content_md5,
                          const std::string&                        content_type,
                          const std::string&                        date,
                          const std::map<std::string, std::string>& oss_headers,
                          const std::string&                        resource);
std::string http_date_now();
std::string file_md5_hex(const std::string& path);
bool        gzip_file(const std::string& source, const std::string& target, const std::function<bool()>& cancelled);

// --- Client -----------------------------------------------------------------------------------------

using ProgressFn  = std::function<void(int percent)>;
using CancelledFn = std::function<bool()>;

class Client
{
public:
    explicit Client(Session session);

    // account_type 2 is e-mail, 1 is a phone number with country code.
    static int login(Region region, int account_type, const std::string& account, const std::string& password, Session& out, std::string& error);

    int fetch_user_info(Session& session);
    int list_devices(std::vector<Device>& devices, std::string& error);
    int telemetry(const std::string& tb_id, nlohmann::json& values, std::string& error);
    int upload_gcode(const std::string& local_path,
                     const std::string& display_name,
                     const ProgressFn&  progress,
                     const CancelledFn& cancelled,
                     UploadedGcode&     uploaded,
                     std::string&       error);
    int add_print_task(const std::string& dn, const UploadedGcode& gcode, const std::string& task_name,
                       const std::vector<FilamentSlot>& slots, bool multi_color, bool use_cfs, std::string& error);
    int rpc(const std::string& tb_id, const nlohmann::json& params, bool two_way, std::string& error);

    int pause(const std::string& tb_id, std::string& error) { return rpc(tb_id, {{"pause", 1}}, true, error); }
    int resume(const std::string& tb_id, std::string& error) { return rpc(tb_id, {{"pause", 0}}, true, error); }
    int stop(const std::string& tb_id, std::string& error) { return rpc(tb_id, {{"stop", 1}}, true, error); }
    int set_self_test(const std::string& tb_id, bool enable, std::string& error)
    {
        return rpc(tb_id, {{"enableSelfTest", enable ? 1 : 0}}, true, error);
    }

private:
    int post_json(const std::string& path, const nlohmann::json& body, std::string& reply, std::string& error) const;
    int get(const std::string& path, std::string& reply, std::string& error) const;
    int put_oss_object(const OssCredentials& creds, const std::string& key, const std::string& local_path, const std::string& download_name,
                       const ProgressFn& progress, const CancelledFn& cancelled, std::string& error) const;

    Session m_session;
};

}} // namespace Slic3r::CrealityCloud
