#include "CrealityCloud.hpp"

#include "Http.hpp"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <zlib.h>

namespace Slic3r { namespace CrealityCloud {

using nlohmann::json;

namespace {

std::string to_hex(const unsigned char* data, size_t size, bool upper)
{
    static const char* lower_digits = "0123456789abcdef";
    static const char* upper_digits = "0123456789ABCDEF";
    const char*        digits       = upper ? upper_digits : lower_digits;
    std::string        out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}

std::string md5_hex(const std::string& data, bool upper)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    EVP_Digest(data.data(), data.size(), digest, &length, EVP_md5(), nullptr);
    return to_hex(digest, length, upper);
}

std::string base64(const unsigned char* data, size_t size)
{
    std::string out(4 * ((size + 2) / 3), '\0');
    const int   written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
    out.resize(written > 0 ? size_t(written) : 0);
    return out;
}

// The API sends ids sometimes as strings and sometimes as numbers.
std::string json_string(const json& obj, const char* key)
{
    if (!obj.is_object())
        return {};
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_number_integer())
        return std::to_string(it->get<long long>());
    if (it->is_number())
        return std::to_string(it->get<double>());
    if (it->is_boolean())
        return it->get<bool>() ? "1" : "0";
    return {};
}

int json_int(const json& obj, const char* key, int fallback = 0)
{
    if (!obj.is_object())
        return fallback;
    auto it = obj.find(key);
    if (it == obj.end())
        return fallback;
    if (it->is_number())
        return it->get<int>();
    if (it->is_boolean())
        return it->get<bool>() ? 1 : 0;
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (...) {}
    }
    return fallback;
}

// Parses the common {"code":..,"msg":..,"result":..} envelope.
int parse_envelope(const std::string& body, json& result, std::string& error)
{
    const json reply = json::parse(body, nullptr, false);
    if (!reply.is_object() || !reply.contains("code")) {
        error = "unexpected reply from Creality Cloud";
        return CODE_PARSE;
    }
    const int code = json_int(reply, "code", CODE_PARSE);
    if (code != CODE_OK) {
        error = json_string(reply, "msg");
        if (error.empty())
            error = "Creality Cloud error " + std::to_string(code);
        return code;
    }
    result = reply.contains("result") ? reply["result"] : json::object();
    return CODE_OK;
}

std::string strip_scheme(std::string endpoint)
{
    for (const char* scheme : {"https://", "http://"})
        if (boost::istarts_with(endpoint, scheme))
            endpoint.erase(0, std::char_traits<char>::length(scheme));
    while (!endpoint.empty() && endpoint.back() == '/')
        endpoint.pop_back();
    return endpoint;
}

std::string url_encode_path(const std::string& value)
{
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
            out.push_back(char(c));
        else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string gcode_state(int state)
{
    switch (state) {
    case int(PrintState::Printing): return "RUNNING";
    case int(PrintState::Paused): return "PAUSE";
    case int(PrintState::Finished): return "FINISH";
    case int(PrintState::Failed):
    case int(PrintState::Stopped): return "FAILED";
    default: return "IDLE";
    }
}

float number(const json& values, const char* key, float fallback = 0.f)
{
    auto it = values.find(key);
    if (it == values.end())
        return fallback;
    if (it->is_number())
        return it->get<float>();
    if (it->is_string()) {
        try {
            return std::stof(it->get<std::string>());
        } catch (...) {}
    }
    return fallback;
}

std::mutex  s_session_mutex;
bool        s_session_loaded = false;
Session     s_session;
std::string s_device_uid;
std::string s_session_file;

std::string session_file_locked()
{
    if (s_session_file.empty())
        s_session_file = (boost::filesystem::path(data_dir()) / "creality_cloud.json").string();
    return s_session_file;
}

} // namespace

std::string region_to_string(Region region) { return region == Region::China ? "China" : "Global"; }

Region region_from_string(const std::string& region) { return boost::iequals(region, "China") ? Region::China : Region::Global; }

std::string api_base_url(Region region)
{
    return region == Region::China ? "https://api.crealitycloud.cn" : "https://api.crealitycloud.com";
}

std::string print_state_text(int state)
{
    switch (state) {
    case int(PrintState::Printing): return "Printing";
    case int(PrintState::Finished): return "Finished";
    case int(PrintState::Failed): return "Failed";
    case int(PrintState::Stopped): return "Stopped";
    case int(PrintState::Paused): return "Paused";
    default: return "Idle";
    }
}

// --- SessionStore -------------------------------------------------------------------------------

void SessionStore::load_locked()
{
    if (s_session_loaded)
        return;
    s_session_loaded = true;
    try {
        boost::nowide::ifstream in(session_file_locked());
        if (!in)
            return;
        const json j = json::parse(in, nullptr, false);
        if (!j.is_object())
            return;
        s_session.user_id  = json_string(j, "user_id");
        s_session.token    = json_string(j, "token");
        s_session.nickname = json_string(j, "nickname");
        s_session.avatar   = json_string(j, "avatar");
        s_session.region   = region_from_string(json_string(j, "region"));
        s_device_uid       = json_string(j, "device_uid");
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "CrealityCloud: could not read the saved session: " << e.what();
    }
}

void SessionStore::save_locked()
{
    json j;
    j["user_id"]    = s_session.user_id;
    j["token"]      = s_session.token;
    j["nickname"]   = s_session.nickname;
    j["avatar"]     = s_session.avatar;
    j["region"]     = region_to_string(s_session.region);
    j["device_uid"] = s_device_uid;
    try {
        boost::nowide::ofstream out(session_file_locked(), std::ios::out | std::ios::trunc);
        out << j.dump(4);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "CrealityCloud: could not save the session: " << e.what();
    }
}

Session SessionStore::get()
{
    std::lock_guard<std::mutex> lock(s_session_mutex);
    load_locked();
    return s_session;
}

void SessionStore::set(const Session& session)
{
    std::lock_guard<std::mutex> lock(s_session_mutex);
    load_locked();
    s_session = session;
    save_locked();
}

void SessionStore::clear()
{
    std::lock_guard<std::mutex> lock(s_session_mutex);
    load_locked();
    const Region region = s_session.region;
    s_session           = Session();
    s_session.region    = region;
    save_locked();
}

std::string SessionStore::device_uid()
{
    std::lock_guard<std::mutex> lock(s_session_mutex);
    load_locked();
    if (s_device_uid.empty()) {
        std::string uid = boost::uuids::to_string(boost::uuids::random_generator()());
        uid.erase(std::remove(uid.begin(), uid.end(), '-'), uid.end());
        s_device_uid = uid;
        save_locked();
    }
    return s_device_uid;
}

void SessionStore::set_file(const std::string& path)
{
    std::lock_guard<std::mutex> lock(s_session_mutex);
    s_session_file   = path;
    s_session_loaded = false;
    s_session        = Session();
    s_device_uid.clear();
}

// --- Pure helpers -------------------------------------------------------------------------------

std::map<std::string, std::string> base_headers(const Session& session, const std::string& device_uid)
{
    std::map<std::string, std::string> headers{
        {"Content-Type", "application/json"},
        {"__CXY_APP_ID_", "creality_model"},
        {"__CXY_PLATFORM_", "11"},
        {"__CXY_OS_LANG_", "0"},
        {"__CXY_DUID_", device_uid},
        {"__CXY_REQUESTID_", boost::uuids::to_string(boost::uuids::random_generator()())},
#if defined(_WIN32)
        {"__CXY_OS_VER_", "win"},
#elif defined(__APPLE__)
        {"__CXY_OS_VER_", "macos"},
#else
        {"__CXY_OS_VER_", "linux"},
#endif
    };
    if (session.valid()) {
        headers["__CXY_UID_"]   = session.user_id;
        headers["__CXY_TOKEN_"] = session.token;
    }
    return headers;
}

json login_body(int account_type, const std::string& account, const std::string& password)
{
    return json{{"type", account_type}, {"account", account}, {"password", password}};
}

int parse_login(const std::string& body, Session& session, std::string& error)
{
    json      result;
    const int code = parse_envelope(body, result, error);
    if (code != CODE_OK)
        return code;
    session.token   = json_string(result, "token");
    session.user_id = json_string(result, "userId");
    // A placeholder userId "0" means the account has not finished registration on the web.
    if (session.token.empty() || session.user_id.empty() || session.user_id == "0") {
        error = "Creality Cloud returned no session for this account";
        return CODE_PARSE;
    }
    if (result.contains("user_info") && result["user_info"].is_object()) {
        session.nickname = json_string(result["user_info"], "nickName");
        session.avatar   = json_string(result["user_info"], "avatar");
    }
    return CODE_OK;
}

int parse_user_info(const std::string& body, Session& session)
{
    json        result;
    std::string error;
    const int   code = parse_envelope(body, result, error);
    if (code != CODE_OK)
        return code;
    // user/getInfo nests the profile as userInfo.base; accept a flat object as well.
    const json* info = &result;
    if (result.contains("userInfo") && result["userInfo"].is_object()) {
        info = &result["userInfo"];
        if (info->contains("base") && (*info)["base"].is_object())
            info = &(*info)["base"];
    }
    const std::string nickname = json_string(*info, "nickName");
    const std::string avatar   = json_string(*info, "avatar");
    if (!nickname.empty())
        session.nickname = nickname;
    if (!avatar.empty())
        session.avatar = avatar;
    return CODE_OK;
}

int parse_devices(const std::string& body, std::vector<Device>& devices, std::string& error)
{
    json      result;
    const int code = parse_envelope(body, result, error);
    if (code != CODE_OK)
        return code;
    devices.clear();
    if (!result.contains("list") || !result["list"].is_array())
        return CODE_OK; // No printers bound to the account
    for (const json& item : result["list"]) {
        if (!item.is_object())
            continue;
        // Creality Print lists types 1 (older WiFi box) and 9 (current firmware) as cloud printers.
        const int type = json_int(item, "type", 9);
        if (type != 1 && type != 9)
            continue;
        Device d;
        d.dn          = json_string(item, "deviceName");
        d.tb_id       = json_string(item, "tbId");
        d.name        = json_string(item, "aliasName");
        d.model       = json_string(item, "model");
        d.mac         = json_string(item, "macAddress");
        d.state       = json_int(item, "deviceState", -1);
        d.idle_state  = json_int(item, "idleState");
        d.online      = d.state != -1;
        d.multi_color = json_int(item, "multicolor") == 1;
        if (boost::istarts_with(d.model, "CR-K1"))
            d.model.erase(0, 3);
        if (item.contains("printInfo") && item["printInfo"].is_object()) {
            const json& info = item["printInfo"];
            d.print_file     = json_string(info, "modelName");
            d.progress       = json_int(info, "printProgress");
            d.left_time_s    = json_int(info, "printLeftTime");
        }
        if (item.contains("iotTelemetry") && item["iotTelemetry"].is_array()) {
            for (const json& t : item["iotTelemetry"]) {
                const std::string key = json_string(t, "key");
                if (key == "printProgress")
                    d.progress = json_int(t, "value", d.progress);
                else if (key == "printLeftTime")
                    d.left_time_s = json_int(t, "value", d.left_time_s);
            }
        }
        if (d.name.empty())
            d.name = d.model.empty() ? d.dn : d.model;
        if (!d.dn.empty())
            devices.push_back(std::move(d));
    }
    return CODE_OK;
}

int parse_aliyun_info(const std::string& body, OssCredentials& creds, std::string& error)
{
    json      result;
    const int code = parse_envelope(body, result, error);
    if (code != CODE_OK)
        return code;
    if (!result.contains("aliyunInfo") || !result["aliyunInfo"].is_object()) {
        error = "Creality Cloud returned no upload credentials";
        return CODE_PARSE;
    }
    const json& info        = result["aliyunInfo"];
    creds.access_key_id     = json_string(info, "accessKeyId");
    creds.access_key_secret = json_string(info, "secretAccessKey");
    creds.security_token    = json_string(info, "sessionToken");
    if (creds.access_key_id.empty() || creds.access_key_secret.empty()) {
        error = "Creality Cloud returned incomplete upload credentials";
        return CODE_PARSE;
    }
    return CODE_OK;
}

int parse_oss_info(const std::string& body, OssCredentials& creds, std::string& error)
{
    json      result;
    const int code = parse_envelope(body, result, error);
    if (code != CODE_OK)
        return code;
    if (!result.contains("info") || !result["info"].is_object()) {
        error = "Creality Cloud returned no storage location";
        return CODE_PARSE;
    }
    const json& info = result["info"];
    creds.endpoint   = json_string(info, "endpoint");
    if (info.contains("file") && info["file"].is_object())
        creds.bucket = json_string(info["file"], "bucket");
    if (creds.endpoint.empty() || creds.bucket.empty()) {
        error = "Creality Cloud returned an incomplete storage location";
        return CODE_PARSE;
    }
    return CODE_OK;
}

int parse_uploaded_gcode(const std::string& body, UploadedGcode& gcode, std::string& error)
{
    json      result;
    const int code = parse_envelope(body, result, error);
    if (code != CODE_OK)
        return code;
    if (!result.contains("list") || !result["list"].is_array() || result["list"].empty()) {
        error = "Creality Cloud did not register the upload";
        return CODE_PARSE;
    }
    const json& item = result["list"][0];
    gcode.id         = json_string(item, "id");
    gcode.name       = json_string(item, "name");
    gcode.file_key   = json_string(item, "filekey");
    if (gcode.file_key.empty())
        gcode.file_key = json_string(item, "fileKey");
    // The caller falls back to the key it uploaded to when the record omits it.
    if (gcode.id.empty()) {
        error = "Creality Cloud returned an incomplete upload record";
        return CODE_PARSE;
    }
    return CODE_OK;
}

json parse_timeseries(const std::string& body)
{
    json       values = json::object();
    const json reply  = json::parse(body, nullptr, false);
    if (!reply.is_object())
        return values;
    // Either the bare telemetry map or wrapped in the usual envelope.
    const json& map = (reply.contains("result") && reply["result"].is_object()) ? reply["result"] : reply;
    for (const auto& item : map.items()) {
        const json& v = item.value();
        if (v.is_array() && !v.empty() && v[0].is_object() && v[0].contains("value"))
            values[item.key()] = v[0]["value"];
        else if (!v.is_array() && !v.is_object())
            values[item.key()] = v;
    }
    return values;
}

std::string gcode_object_key(const std::string& display_name, const std::string& file_md5)
{
    std::string name = display_name.empty() ? "unnamed" : display_name;
    if (boost::iends_with(name, ".gcode"))
        name.resize(name.size() - 6);
    return "model/slice/" + md5_hex(name + ":" + file_md5, true) + ".gcode.gz";
}

std::string extruder_slot_label(int extruder)
{
    const int index = std::max(extruder, 1) - 1;
    return "T" + std::to_string(index / 4 + 1) + char('A' + index % 4);
}

json add_task_body(const std::string&               dn,
                   const UploadedGcode&             gcode,
                   const std::string&               task_name,
                   const std::vector<FilamentSlot>& slots,
                   bool                             multi_color,
                   bool                             use_cfs)
{
    json body{{"deviceName", dn},       {"gcodeId", gcode.id},  {"taskName", task_name}, {"gcodeFilePath", gcode.cdn_url()},
              {"printCount", 1},        {"modelName", task_name}};
    if (!multi_color)
        return body;
    body["enableCfs"] = use_cfs ? 1 : 0;
    if (use_cfs) {
        json list = json::array();
        for (const FilamentSlot& slot : slots) {
            std::string color = slot.color;
            if (!color.empty() && color[0] == '#')
                color.erase(0, 1);
            std::transform(color.begin(), color.end(), color.begin(), ::toupper);
            list.push_back({{"cId", slot.slot_id},
                            {"filamentId", extruder_slot_label(slot.extruder)},
                            {"filamentType", slot.type},
                            {"filamentsColor", "#FF" + color}});
        }
        body["filamentsList"] = list;
    }
    return body;
}

json rpc_set(const json& params) { return json{{"method", "set"}, {"params", params}}; }

std::string oss_signature(const std::string&                        secret,
                          const std::string&                        verb,
                          const std::string&                        content_md5,
                          const std::string&                        content_type,
                          const std::string&                        date,
                          const std::map<std::string, std::string>& oss_headers,
                          const std::string&                        resource)
{
    // std::map keeps the x-oss-* headers sorted, as the canonical form requires; names must already be lower case.
    std::string to_sign = verb + "\n" + content_md5 + "\n" + content_type + "\n" + date + "\n";
    for (const auto& [name, value] : oss_headers)
        to_sign += name + ":" + value + "\n";
    to_sign += resource;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    HMAC(EVP_sha1(), secret.data(), static_cast<int>(secret.size()), reinterpret_cast<const unsigned char*>(to_sign.data()),
         to_sign.size(), digest, &length);
    return base64(digest, length);
}

std::string http_date_now()
{
    std::time_t now = std::time(nullptr);
    std::tm     gmt{};
#ifdef _WIN32
    gmtime_s(&gmt, &now);
#else
    gmtime_r(&now, &gmt);
#endif
    static const char* days[]   = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char               buf[64];
    std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT", days[gmt.tm_wday], gmt.tm_mday, months[gmt.tm_mon],
                  gmt.tm_year + 1900, gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
    return buf;
}

std::string file_md5_hex(const std::string& path)
{
    boost::nowide::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    EVP_DigestInit_ex(ctx.get(), EVP_md5(), nullptr);
    std::vector<char> buffer(1 << 16);
    while (in) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        if (in.gcount() > 0)
            EVP_DigestUpdate(ctx.get(), buffer.data(), size_t(in.gcount()));
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    EVP_DigestFinal_ex(ctx.get(), digest, &length);
    return to_hex(digest, length, false);
}

bool gzip_file(const std::string& source, const std::string& target, const std::function<bool()>& cancelled)
{
    boost::nowide::ifstream in(source, std::ios::binary);
    if (!in)
        return false;
    boost::nowide::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    z_stream zs{};
    // 15 + 16: zlib window with a gzip header and trailer.
    if (deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    std::vector<char> in_buf(1 << 16), out_buf(1 << 16);
    bool              ok = true;
    int               flush;
    do {
        if (cancelled && cancelled()) {
            ok = false;
            break;
        }
        in.read(in_buf.data(), std::streamsize(in_buf.size()));
        zs.avail_in = uInt(in.gcount());
        zs.next_in  = reinterpret_cast<Bytef*>(in_buf.data());
        flush       = in.eof() ? Z_FINISH : Z_NO_FLUSH;
        do {
            zs.avail_out = uInt(out_buf.size());
            zs.next_out  = reinterpret_cast<Bytef*>(out_buf.data());
            if (deflate(&zs, flush) == Z_STREAM_ERROR) {
                ok = false;
                break;
            }
            out.write(out_buf.data(), std::streamsize(out_buf.size() - zs.avail_out));
        } while (ok && zs.avail_out == 0);
    } while (ok && flush != Z_FINISH);
    deflateEnd(&zs);
    return ok && bool(out);
}

// --- Device translations ------------------------------------------------------------------------

json ssdp_payload(const Device& device)
{
    json payload;
    payload["dev_name"] = device.name;
    payload["dev_id"]   = device.dn;
    // No address: the printer is reached through the cloud. The dn keeps the field non-empty for code that requires it.
    payload["dev_ip"]       = device.dn;
    payload["dev_type"]     = device.model;
    payload["dev_signal"]   = "0";
    payload["connect_type"] = "cloud";
    payload["bind_state"]   = "occupied";
    payload["sec_link"]     = "secure";
    payload["ssdp_version"] = "v1";
    payload["online"]       = device.online;
    payload["status"]       = device.online ? print_state_text(device.state) : "Offline";
    payload["progress"]     = device.progress;
    return payload;
}

json status_payload(const Device& device, const json& telemetry)
{
    const int state = telemetry.contains("state") ? int(number(telemetry, "state")) : device.state;
    json      print;
    print["command"]             = "push_status";
    print["msg"]                 = 0;
    print["support_mqtt_alive"]  = true;
    print["gcode_state"]         = device.online ? gcode_state(state) : "OFFLINE";
    print["mc_print_stage"]      = state == int(PrintState::Printing) ? 1 : state == int(PrintState::Paused) ? 2 : 0;
    print["mc_print_error_code"] = 0;
    print["print_error"]         = 0;
    print["mc_percent"]          = telemetry.contains("printProgress") ? int(number(telemetry, "printProgress")) : device.progress;
    const int left_s             = telemetry.contains("printLeftTime") ? int(number(telemetry, "printLeftTime")) : device.left_time_s;
    print["mc_remaining_time"]   = left_s / 60;
    std::string file = device.print_file;
    if (auto it = telemetry.find("printFileName"); it != telemetry.end() && it->is_string())
        file = it->get<std::string>();
    print["subtask_name"] = file;
    print["gcode_file"]   = file;
    // The printer keeps its own storage; report it present so the send dialog does not block on it.
    print["sdcard"]             = true;
    print["home_flag"]          = DevStorage::HAS_SDCARD_NORMAL << 8;
    print["support_send_to_sd"] = true;
    print["nozzle_temp_range"]  = {0, 350};
    print["bed_temp_range"]     = {0, 120};
    print["nozzle_temper"]        = number(telemetry, "nozzleTemp");
    print["nozzle_target_temper"] = number(telemetry, "targetNozzleTemp");
    print["bed_temper"]           = number(telemetry, "bedTemp0");
    print["bed_target_temper"]    = number(telemetry, "targetBedTemp0");
    if (telemetry.contains("boxTemp"))
        print["chamber_temper"] = number(telemetry, "boxTemp");
    return json{{"print", print}};
}

std::vector<FilamentSlot> filament_slots(const std::string& ams_mapping, const std::vector<std::string>& types, const std::vector<std::string>& colors)
{
    std::vector<FilamentSlot> slots;
    const json                    mapping = json::parse(ams_mapping, nullptr, false);
    if (!mapping.is_array())
        return slots;
    for (size_t i = 0; i < mapping.size(); ++i) {
        if (!mapping[i].is_number_integer())
            continue;
        const int tray = mapping[i].get<int>();
        // -1 is unmapped and 254/255 the external spool; CFS trays are box * 4 + slot for up to four boxes.
        if (tray < 0 || tray >= 16)
            continue;
        FilamentSlot slot;
        slot.extruder = int(i) + 1;
        // Creality names a CFS slot T<box><slot letter>, boxes counted from 1: tray 0 is T1A, tray 5 is T2B.
        slot.slot_id = extruder_slot_label(tray + 1);
        slot.type    = i < types.size() ? types[i] : std::string();
        slot.color   = i < colors.size() ? colors[i] : std::string();
        slots.push_back(slot);
    }
    return slots;
}

// --- Client -------------------------------------------------------------------------------------

Client::Client(Session session) : m_session(std::move(session)) {}

namespace {

int perform(Http& http, std::string& reply, std::string& error)
{
    int code = CODE_NETWORK;
    http.timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned) {
            reply = std::move(body);
            code  = CODE_OK;
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            reply = std::move(body);
            error = err.empty() ? "HTTP " + std::to_string(status) : err;
            code  = CODE_NETWORK;
        })
        .perform_sync();
    return code;
}

void apply_headers(Http& http, const std::map<std::string, std::string>& headers)
{
    for (const auto& [name, value] : headers)
        http.header(name, value);
}

} // namespace

int Client::post_json(const std::string& path, const json& body, std::string& reply, std::string& error) const
{
    Http http = Http::post(api_base_url(m_session.region) + path);
    apply_headers(http, base_headers(m_session, SessionStore::device_uid()));
    http.set_post_body(body.dump());
    return perform(http, reply, error);
}

int Client::get(const std::string& path, std::string& reply, std::string& error) const
{
    Http http = Http::get(api_base_url(m_session.region) + path);
    apply_headers(http, base_headers(m_session, SessionStore::device_uid()));
    return perform(http, reply, error);
}

int Client::login(Region region, int account_type, const std::string& account, const std::string& password, Session& out, std::string& error)
{
    Session anonymous;
    anonymous.region = region;
    Client      client(anonymous);
    std::string reply;
    // Creality Print uses the cxy path; older clients used /api/account/loginV2, which the server still answers.
    int code = client.post_json("/api/cxy/account/v2/loginV2", login_body(account_type, account, password), reply, error);
    if (code != CODE_OK) {
        error.clear();
        code = client.post_json("/api/account/loginV2", login_body(account_type, account, password), reply, error);
    }
    if (code != CODE_OK)
        return code;
    out        = Session();
    out.region = region;
    code       = parse_login(reply, out, error);
    if (code == CODE_OK) {
        Client signed_in(out);
        signed_in.fetch_user_info(out);
    }
    return code;
}

int Client::fetch_user_info(Session& session)
{
    std::string reply, error;
    const int   code = post_json("/api/cxy/v2/user/getInfo", json::object(), reply, error);
    return code == CODE_OK ? parse_user_info(reply, session) : code;
}

int Client::list_devices(std::vector<Device>& devices, std::string& error)
{
    if (!m_session.valid())
        return CODE_NO_LOGIN;
    devices.clear();
    // Pages of 100 until a short page, capped so a misbehaving reply cannot loop forever.
    for (int page = 1; page <= 10; ++page) {
        std::string reply;
        int         code = post_json("/api/rest/print/cluster/devices/getDevices", json{{"page", page}, {"pageSize", 100}}, reply, error);
        if (code != CODE_OK)
            return code;
        std::vector<Device> chunk;
        code = parse_devices(reply, chunk, error);
        if (code != CODE_OK)
            return code;
        const size_t count = chunk.size();
        devices.insert(devices.end(), std::make_move_iterator(chunk.begin()), std::make_move_iterator(chunk.end()));
        if (count < 100)
            break;
    }
    return CODE_OK;
}

int Client::telemetry(const std::string& tb_id, json& values, std::string& error)
{
    if (!m_session.valid())
        return CODE_NO_LOGIN;
    std::string reply;
    const int   code = get("/api/rest/iotrouter/" + tb_id + "/values/timeseries?keys=&useStrictDataTypes=true", reply, error);
    if (code != CODE_OK)
        return code;
    values = parse_timeseries(reply);
    return CODE_OK;
}

int Client::rpc(const std::string& tb_id, const json& params, bool two_way, std::string& error)
{
    if (!m_session.valid())
        return CODE_NO_LOGIN;
    std::string reply;
    const int   code = post_json(std::string("/api/rest/iotrouter/rpc/") + (two_way ? "twoway/" : "oneway/") + tb_id, rpc_set(params), reply, error);
    if (code != CODE_OK)
        return code;
    // A two-way call answers with the printer's reply, which may lack the envelope; only an explicit error code fails it.
    const json j = json::parse(reply, nullptr, false);
    if (j.is_object() && j.contains("code") && json_int(j, "code") != CODE_OK) {
        error = json_string(j, "msg");
        return json_int(j, "code");
    }
    return CODE_OK;
}

int Client::put_oss_object(const OssCredentials& creds,
                           const std::string&    key,
                           const std::string&    local_path,
                           const std::string&    download_name,
                           const ProgressFn&     progress,
                           const CancelledFn&    cancelled,
                           std::string&          error) const
{
    const std::string host         = creds.bucket + "." + strip_scheme(creds.endpoint);
    const std::string content_type = "application/octet-stream";
    const std::string date         = http_date_now();
    std::map<std::string, std::string> oss_headers;
    if (!creds.security_token.empty())
        oss_headers["x-oss-security-token"] = creds.security_token;
    const std::string signature = oss_signature(creds.access_key_secret, "PUT", "", content_type, date, oss_headers,
                                                "/" + creds.bucket + "/" + key);

    Http http = Http::put("https://" + host + "/" + url_encode_path(key));
    http.header("Content-Type", content_type)
        .header("Date", date)
        .header("Authorization", "OSS " + creds.access_key_id + ":" + signature)
        .header("Content-Disposition", "attachment;filename=\"" + download_name + "\"")
        .header("Expect", "");
    for (const auto& [name, value] : oss_headers)
        http.header(name, value);
    http.set_put_body(boost::filesystem::path(local_path));

    int  code        = CODE_NETWORK;
    bool was_cancel  = false;
    http.timeout_connect(10)
        .on_progress([&](Http::Progress p, bool& cancel) {
            if (cancelled && cancelled()) {
                cancel     = true;
                was_cancel = true;
                return;
            }
            if (progress && p.ultotal > 0)
                progress(int(p.ulnow * 100 / p.ultotal));
        })
        .on_complete([&](std::string, unsigned) { code = CODE_OK; })
        .on_error([&](std::string body, std::string err, unsigned status) {
            error = err.empty() ? "upload rejected (HTTP " + std::to_string(status) + ")" : err;
            BOOST_LOG_TRIVIAL(error) << "CrealityCloud: OSS upload failed: " << error << " " << body;
        })
        .perform_sync();
    return was_cancel ? CODE_CANCELLED : code;
}

int Client::upload_gcode(const std::string& local_path,
                         const std::string& display_name,
                         const ProgressFn&  progress,
                         const CancelledFn& cancelled,
                         UploadedGcode&     uploaded,
                         std::string&       error)
{
    if (!m_session.valid())
        return CODE_NO_LOGIN;

    OssCredentials creds;
    std::string    reply;
    int            code = post_json("/api/cxy/account/v2/getAliyunInfo", json::object(), reply, error);
    if (code == CODE_OK)
        code = parse_aliyun_info(reply, creds, error);
    if (code != CODE_OK)
        return code;
    code = post_json("/api/cxy/v2/common/getOssInfo", json::object(), reply, error);
    if (code == CODE_OK)
        code = parse_oss_info(reply, creds, error);
    if (code != CODE_OK)
        return code;

    const std::string md5 = file_md5_hex(local_path);
    if (md5.empty()) {
        error = "cannot read " + local_path;
        return CODE_FILE;
    }
    const std::string key = gcode_object_key(display_name, md5);

    boost::system::error_code ec;
    const boost::filesystem::path gz = boost::filesystem::temp_directory_path(ec) /
                                       boost::filesystem::unique_path("creality-upload-%%%%-%%%%.gcode.gz", ec);
    if (ec || !gzip_file(local_path, gz.string(), cancelled)) {
        boost::filesystem::remove(gz, ec);
        if (cancelled && cancelled())
            return CODE_CANCELLED;
        error = "cannot compress the G-code";
        return CODE_FILE;
    }
    code = put_oss_object(creds, key, gz.string(), display_name + ".gcode.gz", progress, cancelled, error);
    boost::filesystem::remove(gz, ec);
    if (code != CODE_OK)
        return code;

    json body{{"list", json::array({json{{"name", display_name}, {"fileKey", key}, {"type", 1}}})}};
    code = post_json("/api/cxy/v2/gcode/uploadGcode", body, reply, error);
    if (code == CODE_OK)
        code = parse_uploaded_gcode(reply, uploaded, error);
    if (code == CODE_OK && uploaded.file_key.empty())
        uploaded.file_key = key;
    return code;
}

int Client::add_print_task(const std::string&               dn,
                           const UploadedGcode&             gcode,
                           const std::string&               task_name,
                           const std::vector<FilamentSlot>& slots,
                           bool                             multi_color,
                           bool                             use_cfs,
                           std::string&                     error)
{
    if (!m_session.valid())
        return CODE_NO_LOGIN;
    std::string reply;
    int         code = post_json("/api/rest/print/cluster/addSingleTask", add_task_body(dn, gcode, task_name, slots, multi_color, use_cfs), reply, error);
    if (code != CODE_OK)
        return code;
    json result;
    return parse_envelope(reply, result, error);
}

}} // namespace Slic3r::CrealityCloud
