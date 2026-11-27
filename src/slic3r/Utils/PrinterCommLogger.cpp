#include "PrinterCommLogger.hpp"
#include "PrinterSequenceIds.hpp"

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <iomanip>
#include <ios>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

using json = nlohmann::json;

namespace {

// Values under these keys are replaced with a stable pseudonym. Secrets first: the LAN access
// code is also the FTPS password and the tunnel password, so it must never reach a shared log.
// The OAuth names cover Orca Cloud sign-in (PKCE code exchange, token refresh), and
// "credential" is the TURN password handed out for camera live view.
// Error and status codes are deliberately NOT in this list -- they are the protocol.
const std::array<const char*, 25> SECRET_KEYS = {
    "access_code", "user_access_code", "dev_access_code", "passwd", "password", "pwd",
    "ticket", "token", "access_token", "refresh_token", "auth_token", "authkey", "auth_key",
    "secret", "sec_link", "license", "auth_code", "code_verifier", "id_token", "provider_token",
    "provider_refresh_token", "client_secret", "api_key", "apikey", "credential"};

// Identity, pseudonymised rather than blanked so cross-message correlation still works.
const std::array<const char*, 8> IDENTITY_KEYS = {"dev_id", "sn", "uid", "user_id", "userid", "machine_id", "email", "phone"};

// Personal profile objects carried by sign-in responses (name, avatar, linked accounts). Nothing
// in them is protocol, so the whole value is dropped.
const std::array<const char*, 2> PROFILE_KEYS = {"user_metadata", "identity_data"};

// URLs that are known to embed credentials.
const std::array<const char*, 2> URL_KEYS = {"rtsp_url", "url"};

// Free-text fields whose value *begins* with a device serial: the printer_connected topic
// (optionally "tunnel/"-prefixed) and get_camera_url's packed "<dev_id>|<dev_ver>|<protocols>".
const std::array<const char*, 2> ID_PREFIXED_KEYS = {"topic", "request"};

// HTTP request headers are logged as a "Name: value" string array, so the key-name pass below
// cannot see them. These carry bearer tokens, API keys (OctoPrint and Moonraker hosts send
// X-Api-Key) and Orca Cloud's request signature.
const std::array<const char*, 9> SECRET_HEADERS = {"authorization", "apikey",     "api-key",   "cookie",    "set-cookie",
                                                   "x-auth-token",  "x-api-key", "x-token",   "x-orca-sig"};

std::string fingerprint(const std::string& value)
{
    // FNV-1a, purely to make equal values compare equal in the log. Not a security boundary --
    // the point is that the plaintext never lands on disk.
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : value) {
        h ^= c;
        h *= 1099511628211ull;
    }
    std::ostringstream ss;
    ss << std::hex << std::setw(8) << std::setfill('0') << (h & 0xffffffffull);
    return ss.str();
}

std::string lower_of(const std::string& v)
{
    std::string out = v;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool key_in(const std::string& key, const char* const* list, size_t n)
{
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    for (size_t i = 0; i < n; ++i)
        if (lower == list[i])
            return true;
    return false;
}

// Keeps the "tunnel/" marker and any trailing "|"-separated fields, which are protocol, and
// replaces only the serial with the same pseudonym the "dev" column uses.
std::string redact_leading_id(const std::string& value)
{
    std::string prefix, rest = value;
    if (rest.rfind("tunnel/", 0) == 0) {
        prefix = "tunnel/";
        rest   = rest.substr(7);
    }
    const auto sep  = rest.find('|');
    const auto id   = rest.substr(0, sep);
    const auto tail = sep == std::string::npos ? std::string() : rest.substr(sep);
    if (id.empty())
        return value;
    return prefix + "#" + fingerprint(id) + tail;
}

std::string redact_header(const std::string& header)
{
    const auto colon = header.find(':');
    if (colon == std::string::npos)
        return header;
    std::string name = lower_of(header.substr(0, colon));
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    for (const char* secret : SECRET_HEADERS)
        if (name == secret)
            return header.substr(0, colon + 1) + " <redacted:" + fingerprint(header.substr(colon + 1)) + ">";
    return header;
}

// Length of the longest prefix of `text` that is well-formed UTF-8 and ends on a character
// boundary, so cutting a body to size never leaves half a character behind.
size_t valid_utf8_prefix(const std::string& text)
{
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c   = static_cast<unsigned char>(text[i]);
        const size_t        len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > text.size())
            return i;
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80)
                return i;
        i += len;
    }
    return i;
}

bool is_name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; }

// Masks the value that follows a secret key name in text that is not, or no longer, valid JSON:
// form bodies and plugin log lines (`key=value`, `key: value`), and JSON cut short to fit the log
// (`"key": "value"`, where the closing quote may be gone). The word-boundary check keeps "token"
// from matching inside "refresh_token", which is masked under its own name.
std::string mask_secrets_in_text(const std::string& text)
{
    const std::string lower = lower_of(text);
    std::vector<std::pair<size_t, size_t>> spans; // [begin, end) of each value to mask

    const auto skip_spaces = [&](size_t i) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
            ++i;
        return i;
    };
    const auto end_of = [&](size_t from, const char* stops) {
        const size_t end = text.find_first_of(stops, from);
        return end == std::string::npos ? text.size() : end;
    };

    for (const char* key : SECRET_KEYS) {
        const size_t key_len = std::strlen(key);
        for (size_t pos = lower.find(key); pos != std::string::npos; pos = lower.find(key, pos + 1)) {
            const size_t after = pos + key_len;
            if ((pos > 0 && is_name_char(lower[pos - 1])) || (after < lower.size() && is_name_char(lower[after])))
                continue;

            size_t i = after;
            if (i < text.size() && (text[i] == '"' || text[i] == '\''))
                ++i;
            i = skip_spaces(i);
            if (i >= text.size() || (text[i] != '=' && text[i] != ':'))
                continue;
            i = skip_spaces(i + 1);

            size_t begin = i, end;
            if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
                const char quote = text[i];
                begin            = i + 1;
                end              = begin;
                while (end < text.size() && text[end] != quote)
                    end += text[end] == '\\' ? 2 : 1;
                end = std::min(end, text.size());
            } else {
                end = end_of(begin, "&\"',;}] \t\r\n");
            }
            if (end > begin)
                spans.emplace_back(begin, end);
        }
    }
    if (spans.empty())
        return text;

    std::sort(spans.begin(), spans.end());
    std::string out;
    size_t      copied = 0;
    for (const auto& [begin, end] : spans) {
        if (begin < copied)
            continue; // overlaps a span already masked
        out.append(text, copied, begin - copied);
        out += "<redacted:" + fingerprint(text.substr(begin, end - begin)) + ">";
        copied = end;
    }
    out.append(text, copied, std::string::npos);
    return out;
}

} // namespace

std::string PrinterCommLogger::redact_url(const std::string& url)
{
    std::string out = url;

    // scheme://user:password@host -> scheme://user:<redacted>@host
    auto at = out.find('@');
    if (at != std::string::npos) {
        auto scheme = out.find("://");
        auto start  = scheme == std::string::npos ? 0 : scheme + 3;
        auto colon  = out.find(':', start);
        if (colon != std::string::npos && colon < at)
            out.replace(colon + 1, at - colon - 1, "<redacted:" + fingerprint(out.substr(colon + 1, at - colon - 1)) + ">");
    }

    // query parameters that carry credentials
    for (const char* param : {"passwd=", "password=", "authkey=", "token=", "license=", "uid=", "device=", "cli_id="}) {
        auto pos = out.find(param);
        while (pos != std::string::npos) {
            auto value_start = pos + std::strlen(param);
            auto value_end   = out.find('&', value_start);
            if (value_end == std::string::npos)
                value_end = out.size();
            out.replace(value_start, value_end - value_start,
                        "<redacted:" + fingerprint(out.substr(value_start, value_end - value_start)) + ">");
            pos = out.find(param, value_start + 1);
        }
    }
    return out;
}

void PrinterCommLogger::redact(json& node)
{
    if (node.is_array()) {
        for (auto& child : node)
            redact(child);
        return;
    }
    if (!node.is_object())
        return;

    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string& key = it.key();
        json&              val = it.value();

        if (key_in(key, SECRET_KEYS.data(), SECRET_KEYS.size())) {
            val = "<redacted:" + fingerprint(val.is_string() ? val.get<std::string>() : val.dump()) + ">";
        } else if (key_in(key, PROFILE_KEYS.data(), PROFILE_KEYS.size())) {
            val = "<redacted>";
        } else if (key_in(key, IDENTITY_KEYS.data(), IDENTITY_KEYS.size())) {
            if (val.is_string() && !val.get<std::string>().empty())
                val = "#" + fingerprint(val.get<std::string>());
        } else if (val.is_string() && key_in(key, URL_KEYS.data(), URL_KEYS.size())) {
            val = redact_url(val.get<std::string>());
        } else if (val.is_string() && key_in(key, ID_PREFIXED_KEYS.data(), ID_PREFIXED_KEYS.size())) {
            val = redact_leading_id(val.get<std::string>());
        } else if (val.is_array() && lower_of(key) == "headers") {
            for (auto& h : val)
                if (h.is_string())
                    h = redact_header(h.get<std::string>());
        } else {
            redact(val);
        }
    }
}

json PrinterCommLogger::body_for_log(const std::string& body, std::size_t max_bytes)
{
    // Parsing is skipped for bodies far past anything worth recording (downloads); their kept
    // prefix is still masked as text.
    constexpr std::size_t MAX_PARSE_BYTES = 1024 * 1024;

    if (body.size() <= MAX_PARSE_BYTES) {
        json parsed = json::parse(body, nullptr, false);
        if (!parsed.is_discarded() && (parsed.is_object() || parsed.is_array())) {
            if (body.size() <= max_bytes)
                return parsed;
            // Too long to keep whole: redact while the structure is still known, then cut.
            redact(parsed);
            std::string text = parsed.dump();
            if (text.size() > max_bytes) {
                text.resize(max_bytes);
                text.resize(valid_utf8_prefix(text));
            }
            return text;
        }
    }

    std::string  kept     = body.substr(0, max_bytes);
    const size_t text_len = valid_utf8_prefix(kept);
    // Invalid bytes before the last few mean the body is not text at all (an image, an archive),
    // and only its size says anything useful; a few at the end are a character split by the cut.
    if (text_len + 4 < kept.size())
        return "<" + std::to_string(body.size()) + " bytes, not text>";
    kept.resize(text_len);
    return mask_secrets_in_text(kept);
}

PrinterCommLogger& PrinterCommLogger::instance()
{
    static PrinterCommLogger inst;
    return inst;
}

PrinterCommLogger::~PrinterCommLogger()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open())
        m_file.close();
}

void PrinterCommLogger::initialize(const std::string& log_dir)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_log_dir     = log_dir;
    m_initialized = true;
    m_start       = std::chrono::steady_clock::now();
    m_last_flush  = m_start;
    BOOST_LOG_TRIVIAL(info) << "PrinterCommLogger: capturing to " << log_dir;
}

bool PrinterCommLogger::is_enabled() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_initialized && !m_broken;
}

void PrinterCommLogger::flush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open())
        m_file.flush();
}

std::string PrinterCommLogger::timestamp()
{
    auto now  = std::chrono::system_clock::now();
    auto secs = std::chrono::system_clock::to_time_t(now);
    auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &secs);
#else
    localtime_r(&secs, &tm_now);
#endif

    std::ostringstream ss;
    ss << std::put_time(&tm_now, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count()
       << std::put_time(&tm_now, "%z");
    return ss.str();
}

std::string PrinterCommLogger::command_of(const json& payload)
{
    // The only top-level roots the protocol actually uses. "event" is the plugin's own
    // client.connected / client.disconnected notification; "files" is the Orca printers' file
    // browser.
    static const std::array<const char*, 9> roots = {"print", "pushing", "system", "info", "upgrade", "camera", "xcam", "event", "files"};

    if (!payload.is_object())
        return "unknown";

    for (const char* root : roots) {
        auto it = payload.find(root);
        if (it == payload.end())
            continue;
        if (it->is_object()) {
            auto cmd = it->find("command");
            if (cmd != it->end() && cmd->is_string())
                return std::string(root) + "." + cmd->get<std::string>();
        }
        return root;
    }
    return "unknown";
}

void PrinterCommLogger::ensure_file_open()
{
    // Caller holds m_mutex.
    if (m_file.is_open() && m_file_size < MAX_FILE_SIZE && m_file.good())
        return;

    if (m_file.is_open()) {
        m_file.close();
        m_file_size = 0;
        m_seen.clear(); // otherwise a rotated file inherits every suppression and stays empty
    }

    boost::filesystem::path dir = boost::filesystem::path(m_log_dir) / "printer_comm";
    boost::system::error_code ec;
    boost::filesystem::create_directories(dir, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "PrinterCommLogger: cannot create " << dir.string() << ": " << ec.message();
        m_broken = true;
        return;
    }

    auto    secs = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &secs);
#else
    localtime_r(&secs, &tm_now);
#endif

    std::ostringstream name;
    name << "printer_comm_" << std::put_time(&tm_now, "%Y%m%d_%H%M%S") << '_' << get_current_pid() << ".jsonl";

    m_file_path = (dir / name.str()).string();
    m_file.open(m_file_path, std::ios::out | std::ios::app | std::ios::binary);
    if (!m_file.is_open()) {
        BOOST_LOG_TRIVIAL(error) << "PrinterCommLogger: cannot open " << m_file_path;
        m_broken = true;
        return;
    }
    m_file_size = 0;
    BOOST_LOG_TRIVIAL(info) << "PrinterCommLogger: writing " << m_file_path;
}

bool PrinterCommLogger::should_suppress(const std::string& key, bool never_suppress, unsigned& skipped)
{
    // Caller holds m_mutex.
    skipped = 0;
    if (never_suppress)
        return false;

    auto  now   = std::chrono::steady_clock::now();
    auto& state = m_seen[key];

    const bool budget  = state.count < VERBATIM_COUNT;
    const bool overdue = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last).count() >= THROTTLE_MS;
    if (budget || overdue) {
        ++state.count;
        state.last    = now;
        skipped       = state.skipped;
        state.skipped = 0;
        return false;
    }
    ++state.skipped;
    return true;
}

void PrinterCommLogger::write_locked(json&& record)
{
    // Caller holds m_mutex. A diagnostic must never break what it diagnoses: any fault here
    // latches m_broken instead of unwinding into parse_json (whose catch(...) logs at trace)
    // or into one of the ~97 command_* callers on the send side.
    try {
        ensure_file_open();
        if (m_broken || !m_file.is_open())
            return;

        record["seq"]  = ++m_seq;
        record["ts"]   = timestamp();
        record["t_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count();

        // Payloads come off the network as-is; a stray invalid UTF-8 byte must not throw and
        // latch m_broken, which would end the capture for the rest of the session.
        std::string line = record.dump(-1, ' ', false, json::error_handler_t::replace);
        line += '\n';
        m_file.write(line.data(), static_cast<std::streamsize>(line.size()));
        m_file_size += line.size();

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_flush).count() >= FLUSH_MS) {
            m_file.flush();
            m_last_flush = now;
        }
    } catch (const std::exception& ex) {
        m_broken = true;
        BOOST_LOG_TRIVIAL(error) << "PrinterCommLogger: disabled after write fault: " << ex.what();
    } catch (...) {
        m_broken = true;
        BOOST_LOG_TRIVIAL(error) << "PrinterCommLogger: disabled after unknown write fault";
    }
}

void PrinterCommLogger::log_mqtt(
    const char* dir, const std::string& transport, const std::string& dev_id, const json& payload, int qos, int rc)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || m_broken)
        return;

    try {
        const std::string cmd = command_of(payload);

        // push_status is the printer's whole state-report protocol and arrives about once a
        // second: throttle it, but per device / transport / message kind so no shape is starved.
        std::string kind;
        auto        print = payload.find("print");
        if (print != payload.end() && print->is_object()) {
            auto msg = print->find("msg");
            if (msg != print->end() && msg->is_number())
                kind = msg->get<int>() == 0 ? "full" : "diff";
        }

        // Never throttle a reply to something we sent. PrinterSequenceIds' band is the app's own range.
        bool is_our_ack = false;
        if (payload.is_object()) {
            for (auto& [k, v] : payload.items()) {
                if (!v.is_object())
                    continue;
                auto sid = v.find("sequence_id");
                if (sid == v.end())
                    continue;
                try {
                    int id = sid->is_string() ? std::stoi(sid->get<std::string>()) : sid->get<int>();
                    if (PrinterSequenceIds::in_band(id))
                        is_our_ack = true;
                } catch (...) {}
            }
        }

        const std::string key = dev_id + '|' + transport + '|' + cmd + '|' + kind;
        unsigned          skipped = 0;
        if (should_suppress(key, is_our_ack, skipped))
            return;

        json record   = json::object();
        record["kind"] = "mqtt";
        record["dir"]  = dir;
        record["transport"] = transport;
        record["dev"]  = dev_id.empty() ? json(nullptr) : json("#" + fingerprint(dev_id));
        record["cmd"]  = cmd;
        if (!kind.empty())
            record["msg_kind"] = kind;
        if (qos >= 0)
            record["qos"] = qos;
        record["rc"]      = rc;
        if (skipped)
            record["rate_limited"] = skipped;

        json body = payload;
        redact(body);
        record["payload"] = std::move(body);

        write_locked(std::move(record));
    } catch (...) {
        m_broken = true;
        BOOST_LOG_TRIVIAL(error) << "PrinterCommLogger: disabled after fault in log_mqtt";
    }
}

void PrinterCommLogger::log_mqtt_raw(const std::string& tunnel, const std::string& dev_id, const std::string& payload, bool consumed)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || m_broken)
        return;

    try {
        json parsed = json::parse(payload, nullptr, false);

        json record         = json::object();
        record["kind"]      = "mqtt";
        record["dir"]       = "recv";
        record["transport"] = tunnel;
        record["dev"]       = dev_id.empty() ? json(nullptr) : json("#" + fingerprint(dev_id));
        record["bytes"]     = payload.size();
        record["consumed"]  = consumed; // true: handled by process_network_msg, never reaches parse_json

        if (parsed.is_discarded()) {
            // Malformed payloads are exactly the interesting case; keep the text, capped.
            record["cmd"]       = "unparsed";
            record["raw"]       = body_for_log(payload, 4096);
            record["truncated"] = payload.size() > 4096;
            write_locked(std::move(record));
            return;
        }

        const std::string cmd = command_of(parsed);
        record["cmd"]         = cmd;

        std::string kind;
        auto        print = parsed.find("print");
        if (print != parsed.end() && print->is_object()) {
            auto msg = print->find("msg");
            if (msg != print->end() && msg->is_number())
                kind = msg->get<int>() == 0 ? "full" : "diff";
        }
        if (!kind.empty())
            record["msg_kind"] = kind;

        const std::string key     = dev_id + '|' + tunnel + '|' + cmd + '|' + kind;
        unsigned          skipped = 0;
        if (should_suppress(key, false, skipped))
            return;
        if (skipped)
            record["rate_limited"] = skipped;

        redact(parsed);
        record["payload"] = std::move(parsed);
        write_locked(std::move(record));
    } catch (...) {
        m_broken = true;
        BOOST_LOG_TRIVIAL(error) << "PrinterCommLogger: disabled after fault in log_mqtt_raw";
    }
}

void PrinterCommLogger::log_event(const char* cat, const std::string& name, json fields)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || m_broken)
        return;

    try {
        redact(fields);

        json record    = json::object();
        record["kind"] = "event";
        record["cat"]  = cat;
        record["name"] = name;
        if (!fields.is_null())
            record["fields"] = std::move(fields);

        write_locked(std::move(record));
    } catch (...) {
        m_broken = true;
        BOOST_LOG_TRIVIAL(error) << "PrinterCommLogger: disabled after fault in log_event";
    }
}

} // namespace Slic3r
