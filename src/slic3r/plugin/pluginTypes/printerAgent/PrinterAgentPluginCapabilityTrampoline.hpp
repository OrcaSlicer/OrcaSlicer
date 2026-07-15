#ifndef slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_
#define slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_

#include "PrinterAgentPluginCapability.hpp"
#include "../../PyPluginTrampoline.hpp"

#include "IPrinterAgent.hpp"
#include <slic3r/plugin/PythonPluginInterface.hpp>

namespace Slic3r {
class PyPrinterAgentPluginCapabilityTrampoline : public PyPluginCommonTrampoline<PrinterAgentPluginCapability>
{
public:
    using PyPluginCommonTrampoline<PrinterAgentPluginCapability>::PyPluginCommonTrampoline;

    AgentInfo get_agent_info() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, AgentInfo, PrinterAgentPluginCapability,
            get_agent_info);
    }

    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, connect_printer, dev_id,
            dev_ip, username, password, use_ssl);
    }

    int disconnect_printer() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, disconnect_printer);
    }

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, send_message, dev_id,
            json_str, qos, flag);
    }

    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, send_message_to_printer,
            dev_id, json_str, qos, flag);
    }

    bool start_discovery(bool start, bool sending) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, bool, PrinterAgentPluginCapability, start_discovery, start,
            sending);
    }

    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, bind_detect, dev_ip,
            sec_link, detect);
    }

    std::string get_user_selected_machine() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, std::string, PrinterAgentPluginCapability,
            get_user_selected_machine);
    }

    int set_user_selected_machine(std::string dev_id) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability,
            set_user_selected_machine, dev_id);
    }

    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability,
            start_send_gcode_to_sdcard, params, update_fn, cancel_fn, wait_fn);
    }

    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, start_local_print,
            params, update_fn, cancel_fn);
    }

    FilamentSyncMode get_filament_sync_mode() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, FilamentSyncMode, PrinterAgentPluginCapability,
            get_filament_sync_mode);
    }

    bool fetch_filament_info(std::string dev_id) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, bool, PrinterAgentPluginCapability, fetch_filament_info, dev_id);
    }

    int check_cert() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, check_cert);
    }

    void install_device_cert(std::string dev_id, bool lan_only) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, void, PrinterAgentPluginCapability, install_device_cert, dev_id,
            lan_only);
    }

    int ping_bind(std::string ping_code) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, ping_bind, ping_code);
    }

    int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, bind, dev_ip, dev_id,
            sec_link, timezone, improved, update_fn);
    }

    int unbind(std::string dev_id) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, unbind, dev_id);
    }

    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, start_print, params,
            update_fn, cancel_fn, wait_fn);
    }

    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability,
            start_local_print_with_record, params, update_fn, cancel_fn, wait_fn);
    }

    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, start_sdcard_print, params,
            update_fn, cancel_fn);
    }

    int set_server_callback(OnServerErrFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_server_callback, fn);
    }

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_ssdp_msg_fn, fn);
    }

    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_printer_connected_fn,
            fn);
    }

    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_subscribe_failure_fn,
            fn);
    }

    int set_on_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_message_fn, fn);
    }

    int set_on_user_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_user_message_fn, fn);
    }

    int set_on_local_connect_fn(OnLocalConnectedFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_local_connect_fn, fn);
    }

    int set_on_local_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_local_message_fn, fn);
    }

    int set_queue_on_main_fn(QueueOnMainFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_queue_on_main_fn, fn);
    }

    // request_bind_ticket returns its ticket through a std::string* out-param, which pybind11
    // cannot marshal back through a plain override. We dispatch manually: the Python plugin
    // returns a (result, ticket) tuple, which we unpack into the int result and the out-param.
    int request_bind_ticket(std::string* ticket) override
    {
        ORCA_PY_AUDIT_SCOPE(::Slic3r::PluginAuditManager::AuditMode::Loading);
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
    }
};
} // namespace Slic3r

#endif /* slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_ */
