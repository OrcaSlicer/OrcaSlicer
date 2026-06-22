#pragma once
// Phase 2.4.1 — orca::PrinterAgent — engine-side C++ interface for printer
// agents. A printer agent represents a single physical or networked printer
// instance. Agents are plugin-provided via the ORCA_SLOT_PRINTER_AGENT slot;
// the engine instantiates them via Session::create_printer_agent(agent_id).
//
// This interface is a deliberate ~10-method subset of the BBL-shaped
// Slic3r::IPrinterAgent in src/slic3r/Utils/. v1 covers what a Klipper,
// Moonraker, Octoprint, Mainsail, RepRapFirmware, or Marlin-USB agent
// realistically needs. Out-of-scope for v1: cloud-relay routing, MQTT
// subscription topology, multi-printer-binding workflows. Those can be added
// as additional slot kinds (or as additional methods if a real use case
// emerges).
//
// Error model: methods return orca::Result<...> using the existing engine
// Result.hpp convention (Result<void>{} on success; Result<void>{Error{...}}
// on failure, built via orca::err(ErrorCode, std::string)).

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "orca/Result.hpp"

namespace orca {

// Identifies a registered printer agent kind. Stable string the plugin
// declares in its slot vtable. e.g. "moonraker", "bambu", "klipper", "octoprint".
struct PrinterAgentInfo {
    std::string id;                ///< Unique stable id, used by create_printer_agent
    std::string name;              ///< Human-readable name
    std::string version;           ///< Semantic version string
    std::string description;       ///< One-line capability summary
};

// Connection target supplied by the host to PrinterAgent::connect.
struct PrinterConnection {
    std::string device_id;         ///< Stable per-printer id; opaque to the engine
    std::string host;              ///< Hostname or IP (LAN agents); empty if cloud-only
    int         port{0};           ///< TCP port (0 = agent default)
    std::string username;          ///< Optional auth username (empty if unused)
    std::string password;          ///< Optional auth password or API key
    bool        use_tls{false};    ///< Whether to use TLS for the connection
};

// Job submission parameters supplied to PrinterAgent::start_print.
struct PrintJob {
    std::string gcode_path;             ///< Absolute path to G-code file on disk
    std::string job_name;               ///< Display name (filename if blank)
    bool        start_immediately{true}; ///< If false, upload only — do not start
};

// Lifecycle / connection state.
enum class PrinterAgentState : std::uint32_t {
    Disconnected = 0,
    Connecting   = 1,
    Connected    = 2,
    Error        = 3,
};

// One status update delivered by the plugin via the host emit callback.
struct PrinterAgentStatus {
    PrinterAgentState state{PrinterAgentState::Disconnected};
    int               code{0};     ///< Plugin-defined detail code (0 = ok)
    std::string       message;     ///< Human-readable detail (may be empty)
};

// Abstract C++ interface satisfied by orca::PrinterAgentAdapter (in
// engine/src/) which wraps a plugin's C-vtable. The engine never subclasses
// this directly; callers receive instances via Session::create_printer_agent.
class PrinterAgent {
public:
    virtual ~PrinterAgent() = default;

    // Identity of the agent kind that produced this instance.
    virtual const PrinterAgentInfo& info() const = 0;

    // Establish a connection to the printer. Returns immediately; the actual
    // connection may complete asynchronously — register an on_status callback
    // to observe Connected / Error transitions. Non-OK result means the agent
    // refused the attempt (e.g. malformed connection params).
    virtual Result<void> connect(const PrinterConnection& target) = 0;

    // Tear down the connection. Idempotent if already disconnected.
    virtual Result<void> disconnect() = 0;

    // Current connection state — synchronous, observed by the agent.
    virtual PrinterAgentState state() const = 0;

    // Send a raw command/message to the printer. Encoding is agent-defined
    // (JSON for Moonraker/Klipper, gcode for Marlin-USB, etc.). The agent
    // may return immediately; the response (if any) arrives via on_message.
    virtual Result<void> send_command(std::string_view payload) = 0;

    // Start a print job. Returns OK if the upload + start kick-off succeeded.
    // Progress updates are delivered via on_status / on_message.
    virtual Result<void> start_print(const PrintJob& job) = 0;

    // Cancel the currently active print job (if any). Idempotent.
    virtual Result<void> cancel_print() = 0;

    // Register a status callback. Replaces any previously registered callback.
    // The callback fires on a thread owned by the plugin; receivers must
    // marshal to a UI thread themselves if they need that.
    using OnStatus = std::function<void(const PrinterAgentStatus&)>;
    virtual void on_status(OnStatus cb) = 0;

    // Register a message callback. Same threading semantics as on_status.
    using OnMessage = std::function<void(std::string_view payload)>;
    virtual void on_message(OnMessage cb) = 0;

    // Optional cancellation predicate the plugin may consult during long ops
    // (e.g. while uploading a large G-code file). Returning true tells the
    // plugin "abort whatever you're doing". The engine wires this to the
    // session's existing cancellation channel; plugins MAY ignore it.
    using IsCancelled = std::function<bool()>;
    virtual void on_is_cancelled(IsCancelled cb) = 0;
};

} // namespace orca
