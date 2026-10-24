#include "ActivePrinterSession.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <libslic3r/Preset.hpp>
#include <string>
#include <libslic3r/PrintConfig.hpp>
#include <cstddef>
#include <memory>

#include "DeviceCore/DevManager.h"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp" // MOONRAKER_PRINTER_AGENT_ID

namespace Slic3r { namespace GUI {

std::string ActivePrinterSession::Connection::access_code(const std::string& agent_id) const
{
    return api_key.empty() && agent_id != MOONRAKER_PRINTER_AGENT_ID ? std::string("88888888") : api_key;
}

const Preset& ActivePrinterSession::profile() const
{
    return wxGetApp().preset_bundle->printers.get_edited_preset();
}

std::string ActivePrinterSession::printer_type() const
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    return preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
}

ActivePrinterSession::Connection ActivePrinterSession::connection() const
{
    const DynamicPrintConfig& config = profile().config;

    Connection conn;
    conn.host    = config.opt_string("print_host");
    conn.api_key = config.opt_string("printhost_apikey");
    if (conn.host.empty())
        return conn;

    conn.dev_id  = MachineObject::dev_id_from_address(conn.host, config.opt_string("printhost_port"));
    conn.use_ssl = boost::istarts_with(conn.host, "https://");
    return conn;
}

MachineObject* ActivePrinterSession::live_machine() const
{
    DeviceManager* dev_mgr = wxGetApp().getDeviceManager();
    MachineObject* machine = dev_mgr ? dev_mgr->get_selected_machine() : nullptr;
    if (wxGetApp().getAgent() == nullptr || machine == nullptr)
        return nullptr;

    // The selection outlives a preset switch. Not is_same_printer_for_connected_and_selected:
    // its check_printer_initialized would turn a stale heartbeat into "offline".
    if (wxGetApp().is_blocking_printing(machine))
        return nullptr;

    // Otherwise every preset of the machine's model would read as live.
    const Connection conn = connection();
    if (!conn.configured())
        return nullptr;

    // The machine stores a bare ip; the profile's host may carry a scheme or port. A machine
    // with no ip (cloud-bound) is left to the model check.
    const std::string machine_ip = machine->get_dev_ip();
    if (!machine_ip.empty()) {
        std::string h = conn.host;
        if (size_t p = h.find("://"); p != std::string::npos)
            h = h.substr(p + 3);
        if (size_t p = h.find_first_of(":/"); p != std::string::npos)
            h = h.substr(0, p);
        if (h != machine_ip)
            return nullptr;
    }
    return machine;
}

bool ActivePrinterSession::live() const { return live_machine() != nullptr; }

bool ActivePrinterSession::bind_agent() const
{
    NetworkAgent* agent = wxGetApp().getAgent();
    if (agent == nullptr)
        return false;
    const Connection conn = connection();
    if (!conn.configured())
        return false;
    const std::shared_ptr<IPrinterAgent> printer_agent = agent->get_printer_agent();
    const std::string agent_id = printer_agent ? printer_agent->get_agent_info().id : std::string();
    return agent->bind_device_connection(conn.dev_id, conn.dev_id, conn.access_code(agent_id), conn.use_ssl);
}

NetworkAgent* ActivePrinterSession::sync_agent() const
{
    if (!live())
        return nullptr;
    bind_agent();
    return wxGetApp().getAgent();
}

const ActivePrinterSession& active_printer_session()
{
    static const ActivePrinterSession session;
    return session;
}

}} // namespace Slic3r::GUI
