#ifndef slic3r_PrinterCommLogger_hpp_
#define slic3r_PrinterCommLogger_hpp_

// Developer tool: a capture of every channel between OrcaSlicer and a printer or printer cloud
// (Bambu Lab through the network plugin, Orca printers and Orca Cloud through their own
// transports), for protocol analysis. It is built only with the CMake option
// ORCA_PRINTER_COMM_LOG. Call sites put their capture code in these macros, so a normal build
// compiles exactly the code it would without the capture:
//
//   PRINTER_COMM_LOG(statements...)        the statements, only in a capture build.
//   PRINTER_COMM_RESULT(expr, statements)  the value of expr; a capture build first runs the
//                                          statements, which see that value as `rc`.
#ifndef ORCA_PRINTER_COMM_LOG

#define PRINTER_COMM_LOG(...)
#define PRINTER_COMM_RESULT(expr, ...) (expr)

#else

#define PRINTER_COMM_LOG(...) __VA_ARGS__
#define PRINTER_COMM_RESULT(expr, ...) ([&] { auto rc = (expr); __VA_ARGS__; return rc; }())

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r {

// Writes one compact JSON object per line (JSONL) to
// <data_dir>/log/printer_comm/printer_comm_<stamp>_<pid>.jsonl. The subdirectory is
// deliberate: GUI_App's log retention sweep only removes regular files directly under
// <data_dir>/log, so captures there would otherwise be deleted and would evict debug_*.log
// from the same 30-file budget.
//
// Payloads are redacted by key name before they are written -- these files are meant to be
// shareable, and the raw traffic carries the printer's LAN access code, which doubles as the
// FTPS and tunnel password.
class PrinterCommLogger
{
public:
    static PrinterCommLogger& instance();

    // log_dir is <data_dir>/log; the "printer_comm" subdirectory is created underneath it.
    // Nothing is recorded before this is called.
    void initialize(const std::string& log_dir);
    bool is_enabled() const;
    void flush();

    // MQTT payloads that the app itself formatted or parsed.
    // transport is "lan-mqtt" | "tunnel-mqtt" | "cloud-mqtt" | "unknown"; qos < 0 means n/a.
    void log_mqtt(const char*           dir,
                  const std::string&    transport,
                  const std::string&    dev_id,
                  const nlohmann::json& payload,
                  int                   qos = -1,
                  int                   rc  = 0);

    // Inbound payload captured at the message callback, i.e. before process_network_msg may
    // consume it and before parse_json's early returns. Records the byte length and whether the
    // message was consumed; the body is reserialized after redaction, so key order is not preserved.
    void log_mqtt_raw(const std::string& tunnel, const std::string& dev_id, const std::string& payload, bool consumed);

    // Every channel that is not an MQTT payload: connection lifecycle, SSDP, print jobs,
    // the file-transfer tunnels, live video, cloud REST and native HTTP.
    void log_event(const char* cat, const std::string& name, nlohmann::json fields);

    // Redact a URL that may embed credentials (rtsps://user:pass@host, bambu:///...&passwd=).
    static std::string redact_url(const std::string& url);

    // A text body (HTTP request or response, a JSON string handed over by a plugin, a plugin log
    // line) as a value for a log_event or log_mqtt field. A plain string field is never searched
    // for secrets, so a body must pass through here. JSON that fits in max_bytes is returned
    // parsed, and the logger then redacts it by key with the rest of the record; other text
    // comes back cut to max_bytes with the values of secret keys masked, and a body that is not
    // text at all as a note of its size.
    static nlohmann::json body_for_log(const std::string& body, std::size_t max_bytes);

private:
    PrinterCommLogger() = default;
    ~PrinterCommLogger();

    PrinterCommLogger(const PrinterCommLogger&)            = delete;
    PrinterCommLogger& operator=(const PrinterCommLogger&) = delete;

    void write_locked(nlohmann::json&& record);
    void ensure_file_open();
    // Returns true if this frame should be dropped. On false, `skipped` receives the number
    // of frames dropped for this key since the last one that was written.
    bool should_suppress(const std::string& key, bool never_suppress, unsigned& skipped);

    static void        redact(nlohmann::json& node);
    static std::string command_of(const nlohmann::json& payload);
    static std::string timestamp();

    mutable std::mutex m_mutex;
    std::string        m_log_dir;
    std::string        m_file_path;
    std::ofstream      m_file;
    bool               m_initialized = false;
    bool               m_broken      = false; // latched after a write fault; never retried
    std::uint64_t      m_seq         = 0;
    std::uintmax_t     m_file_size   = 0;

    std::chrono::steady_clock::time_point m_start;
    std::chrono::steady_clock::time_point m_last_flush;

    // Rate limiting for repetitive frames (push_status and friends), keyed on
    // device + transport + command + message kind so no device or shape is ever starved.
    struct SeenState
    {
        unsigned                              count     = 0; // frames written for this key
        unsigned                              skipped   = 0; // frames dropped since the last write
        std::chrono::steady_clock::time_point last;
    };
    std::map<std::string, SeenState> m_seen;

    static constexpr std::uintmax_t MAX_FILE_SIZE   = 200 * 1024 * 1024;
    static constexpr unsigned       VERBATIM_COUNT  = 20;   // log this many per key, then throttle
    static constexpr int            THROTTLE_MS     = 30000;
    static constexpr int            FLUSH_MS        = 1000;
};

} // namespace Slic3r

#endif // ORCA_PRINTER_COMM_LOG

#endif // slic3r_PrinterCommLogger_hpp_
