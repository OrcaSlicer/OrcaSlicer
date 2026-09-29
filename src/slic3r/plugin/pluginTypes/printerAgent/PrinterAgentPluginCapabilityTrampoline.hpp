#ifndef slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_
#define slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_

#include "PrinterAgentPluginCapability.hpp"
#include "../../PyPluginTrampoline.hpp"

#include "IPrinterAgent.hpp"
#include <slic3r/plugin/PythonPluginInterface.hpp>

#include <type_traits>

// IPrinterAgent reports failure through its return values and its callers do not catch, so nothing
// the plugin does may leave the trampoline as an exception: a Python raise, a missing override or a
// wrongly typed return is logged and answered with what NetworkAgent returns when no agent is set.
#define ORCA_PY_AGENT_CATCH(name) \
    catch (const std::exception& ex) { this->log_failure(#name, ex.what()); } \
    catch (...) { this->log_failure(#name, "unknown error"); }

#define ORCA_PY_AGENT_OVERRIDE(ret, name, ...) \
    try { \
        ORCA_PY_OVERRIDE_AUDITED([] {}, PYBIND11_OVERRIDE_PURE, ret, PrinterAgentPluginCapability, name, ##__VA_ARGS__); \
    } ORCA_PY_AGENT_CATCH(name) \
    return printer_agent_failure<ret>()

namespace Slic3r {
// NetworkAgent's no-agent answer: -1 for a status code, the empty value (false, "", none) otherwise.
template<typename T> T printer_agent_failure()
{
    if constexpr (std::is_same_v<T, int>)
        return -1;
    else if constexpr (!std::is_void_v<T>)
        return T{};
}

class PyPrinterAgentPluginCapabilityTrampoline : public PyPluginCommonTrampoline<PrinterAgentPluginCapability>
{
public:
    using PyPluginCommonTrampoline<PrinterAgentPluginCapability>::PyPluginCommonTrampoline;

    AgentInfo get_agent_info() override
    {
        ORCA_PY_AGENT_OVERRIDE(AgentInfo, get_agent_info);
    }

    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, connect_printer, dev_id, dev_ip, username, password, use_ssl);
    }

    int disconnect_printer() override
    {
        ORCA_PY_AGENT_OVERRIDE(int, disconnect_printer);
    }

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, send_message, dev_id, json_str, qos, flag);
    }

    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, send_message_to_printer, dev_id, json_str, qos, flag);
    }

    bool start_discovery(bool start, bool sending) override
    {
        ORCA_PY_AGENT_OVERRIDE(bool, start_discovery, start, sending);
    }

    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override
    {
        // Passed as a pointer: pybind11 copies a reference argument, so the plugin's writes would be lost.
        ORCA_PY_AGENT_OVERRIDE(int, bind_detect, dev_ip, sec_link, &detect);
    }

    std::string get_user_selected_machine() override
    {
        ORCA_PY_AGENT_OVERRIDE(std::string, get_user_selected_machine);
    }

    int set_user_selected_machine(std::string dev_id) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_user_selected_machine, dev_id);
    }

    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, start_send_gcode_to_sdcard, params, update_fn, cancel_fn, wait_fn);
    }

    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, start_local_print, params, update_fn, cancel_fn);
    }

    FilamentSyncMode get_filament_sync_mode() const override
    {
        ORCA_PY_AGENT_OVERRIDE(FilamentSyncMode, get_filament_sync_mode);
    }

    bool fetch_filament_info(std::string dev_id) override
    {
        ORCA_PY_AGENT_OVERRIDE(bool, fetch_filament_info, dev_id);
    }

    int check_cert() override
    {
        ORCA_PY_AGENT_OVERRIDE(int, check_cert);
    }

    void install_device_cert(std::string dev_id, bool lan_only) override
    {
        ORCA_PY_AGENT_OVERRIDE(void, install_device_cert, dev_id, lan_only);
    }

    int ping_bind(std::string ping_code) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, ping_bind, ping_code);
    }

    int bind(std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, bind, dev_ip, dev_id, dev_model, sec_link, timezone, improved, update_fn);
    }

    int unbind(std::string dev_id) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, unbind, dev_id);
    }

    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, start_print, params, update_fn, cancel_fn, wait_fn);
    }

    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, start_local_print_with_record, params, update_fn, cancel_fn, wait_fn);
    }

    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, start_sdcard_print, params, update_fn, cancel_fn);
    }

    int get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, get_hms_snapshot, dev_id, file_name, callback);
    }

    int set_server_callback(OnServerErrFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_server_callback, fn);
    }

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_ssdp_msg_fn, fn);
    }

    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_printer_connected_fn, fn);
    }

    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_subscribe_failure_fn, fn);
    }

    int set_on_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_message_fn, fn);
    }

    int set_on_user_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_user_message_fn, fn);
    }

    int set_on_local_connect_fn(OnLocalConnectedFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_local_connect_fn, fn);
    }

    int set_on_local_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_local_message_fn, fn);
    }

    int set_queue_on_main_fn(QueueOnMainFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_queue_on_main_fn, fn);
    }

    // request_bind_ticket returns its ticket through a std::string* out-param, which pybind11
    // cannot marshal back through a plain override. We dispatch manually: the Python plugin
    // returns a (result, ticket) tuple, which we unpack into the int result and the out-param.
    int request_bind_ticket(std::string* ticket) override
    {
        try {
            ORCA_PY_AUDIT_SCOPE();
            ::Slic3r::PluginCapabilityInterface::RefCounter _orca_ref_counter(*this);
            ::Slic3r::PythonGILState gil;
            if (!gil)
                throw std::runtime_error("Python interpreter is shutting down");
            pybind11::function override =
                pybind11::get_override(static_cast<const PrinterAgentPluginCapability*>(this), "request_bind_ticket");
            if (!override)
                pybind11::pybind11_fail("Tried to call pure virtual function \"PrinterAgentPluginCapability::request_bind_ticket\"");
            try {
                pybind11::tuple result = override().cast<pybind11::tuple>();
                if (ticket)
                    *ticket = result[1].cast<std::string>();
                return result[0].cast<int>();
            } catch (pybind11::error_already_set& err) {
                ::Slic3r::log_python_exception_keep(err);
                throw;
            }
        } ORCA_PY_AGENT_CATCH(request_bind_ticket)
        return printer_agent_failure<int>();
    }

private:
    void log_failure(const char* operation, const char* error) const
    {
        BOOST_LOG_TRIVIAL(error) << "Printer agent plugin '" << this->audit_plugin_key() << "': " << operation << " failed: " << error;
    }
};
} // namespace Slic3r

#endif /* slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_ */
