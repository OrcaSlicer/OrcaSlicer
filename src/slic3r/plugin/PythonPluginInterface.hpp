#ifndef slic3r_PythonPluginInterface_hpp_
#define slic3r_PythonPluginInterface_hpp_

#include <atomic>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include <pybind11/embed.h>

namespace Slic3r {

enum class PluginCapabilityType { PrinterConnection = 0, Automation, Analysis, Importer, Exporter, Visualization, Script, SlicingPipeline, Unknown };

inline std::string plugin_capability_type_to_string(PluginCapabilityType type)
{
    switch (type) {
    case PluginCapabilityType::PrinterConnection: return "printer-connection";
    case PluginCapabilityType::Automation: return "automation";
    case PluginCapabilityType::Analysis: return "analysis";
    case PluginCapabilityType::Importer: return "importer";
    case PluginCapabilityType::Exporter: return "exporter";
    case PluginCapabilityType::Visualization: return "visualization";
    case PluginCapabilityType::Script: return "script";
    case PluginCapabilityType::SlicingPipeline: return "slicing-pipeline";
    default: return "unknown";
    }
}

inline std::string plugin_capability_type_display_name(PluginCapabilityType type)
{
    switch (type) {
    case PluginCapabilityType::PrinterConnection: return "Printer connection";
    case PluginCapabilityType::Automation: return "Automation";
    case PluginCapabilityType::Analysis: return "Analysis";
    case PluginCapabilityType::Importer: return "Importer";
    case PluginCapabilityType::Exporter: return "Exporter";
    case PluginCapabilityType::Visualization: return "Visualization";
    case PluginCapabilityType::Script: return "Script";
    case PluginCapabilityType::SlicingPipeline: return "Slicing Pipeline";
    default: return "Unknown";
    }
}

inline PluginCapabilityType plugin_capability_type_from_string(std::string_view value)
{
    auto to_lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
    std::string lowered;
    lowered.reserve(value.size());
    for (unsigned char ch : value) {
        lowered.push_back(to_lower(ch));
    }

    if (lowered == "printer-connection")
        return PluginCapabilityType::PrinterConnection;
    if (lowered == "automation")
        return PluginCapabilityType::Automation;
    if (lowered == "analysis")
        return PluginCapabilityType::Analysis;
    if (lowered == "importer")
        return PluginCapabilityType::Importer;
    if (lowered == "exporter")
        return PluginCapabilityType::Exporter;
    if (lowered == "visualization")
        return PluginCapabilityType::Visualization;
    if (lowered == "script")
        return PluginCapabilityType::Script;
    if (lowered == "slicing-pipeline")
        return PluginCapabilityType::SlicingPipeline;
    return PluginCapabilityType::Unknown;
}

struct PluginContext
{ std::string orca_version; };

enum class PluginResult { Success, Skipped, RecoverableError, FatalError };

struct ExecutionResult
{
    PluginResult status = PluginResult::Success;
    std::string message;
    std::string data;

    static ExecutionResult success(std::string message = {}, std::string data = {})
    { return {PluginResult::Success, std::move(message), std::move(data)}; }

    static ExecutionResult skipped(std::string message = {})
    {
        return {PluginResult::Skipped, std::move(message), {}};
    }

    static ExecutionResult failure(PluginResult status, std::string message, std::string data = {})
    { return {status, std::move(message), std::move(data)}; }
};

class PluginCapabilityInterface
{
public:
    class RefCounter
    {
    public:
        explicit RefCounter(const PluginCapabilityInterface& iface) : m_iface(&iface) { m_iface->increment(); }

        ~RefCounter() { m_iface->decrement(); }

        RefCounter(const RefCounter&)            = delete;
        RefCounter& operator=(const RefCounter&) = delete;

    private:
        const PluginCapabilityInterface* m_iface;
    };

    virtual ~PluginCapabilityInterface() = default;

    // DO NOT CALL THESE OUTSIDE THE LOADER'S MATERIALIZATION BLOCK. get_name() is pure virtual and
    // always implemented in Python: the trampoline routes it through PYBIND11_OVERRIDE_PURE, so every
    // call acquires the GIL, dispatches into the plugin, and opens a filesystem-enforcement scope.
    // Capability lookup is by name and runs under the plugin registry lock, so calling this on a
    // lookup would hold that lock while taking the GIL — inverting the lock order against plugin
    // dispatch on the slicing threads — and it is undefined after the interpreter is finalized.
    // get_type() is virtual with a C++ default that the typed bases override in C++, but the untyped
    // trampoline still routes it to Python, so it is only GIL-free by accident of the base chosen.
    //
    // The loader resolves both exactly once, at materialization, under the GIL it already holds, and
    // caches them below. Everything else reads name()/type().
    virtual std::string get_name() const = 0;                                               // required — overridden in Python
    virtual PluginCapabilityType get_type() const { return PluginCapabilityType::Unknown; } // optional — typed bases override

    virtual void on_load() {}
    virtual void on_unload() {}
    virtual void on_cancelled() {}

    // ── C++-only host state, never exposed to Python. Set by the loader at materialization. ──
    //
    // The capability owns its own identity and enable flag: they are read once under the GIL, live
    // exactly as long as the capability does, and are discarded with it on unload. Nothing about a
    // capability outlives the capability — the durable record is the .install_state.json sidecar.

    // Cached identity. Plain C++ reads, safe under any lock and after the interpreter is gone.
    const std::string&   name() const { return m_name; }
    PluginCapabilityType type() const { return m_type; }
    void                 set_resolved_identity(std::string name, PluginCapabilityType type)
    {
        m_name = std::move(name);
        m_type = type;
    }

    // Logical enable/disable. A disabled capability stays loaded but is skipped by consumers.
    // Atomic because dispatch reads it off a shared_ptr handed out by the manager, i.e. without the
    // registry lock held. Seeded from the sidecar at load; PluginManager writes it through on change.
    bool is_enabled() const { return m_enabled.load(std::memory_order_acquire); }
    void set_enabled(bool enabled) { m_enabled.store(enabled, std::memory_order_release); }

    // The owning package (PluginDescriptor::plugin_key), the canonical runtime id. Also scopes
    // filesystem enforcement for trampoline calls.
    void set_audit_plugin_key(std::string key) { m_audit_plugin_key = std::move(key); }
    const std::string& audit_plugin_key() const { return m_audit_plugin_key; }

    void increment() const { m_refs.fetch_add(1, std::memory_order_acq_rel); }
    void decrement() const { m_refs.fetch_sub(1, std::memory_order_acq_rel); }
    int ref_count() const { return m_refs.load(std::memory_order_acquire); }

private:
    std::string m_name;
    PluginCapabilityType m_type = PluginCapabilityType::Unknown;
    std::atomic<bool> m_enabled{true};
    std::string m_audit_plugin_key;

    mutable std::atomic<int> m_refs{0};
};

} // namespace Slic3r

#endif /* slic3r_PythonPluginInterface_hpp_ */
