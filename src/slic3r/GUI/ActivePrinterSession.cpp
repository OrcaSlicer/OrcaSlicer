#include "ActivePrinterSession.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include "DeviceCore/DevManager.h" // full DeviceManager definition -- DeviceManager.hpp only forward-declares it
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

namespace Slic3r { namespace GUI {

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
    conn.port    = config.opt_string("printhost_port");
    conn.api_key = config.opt_string("printhost_apikey");
    if (conn.host.empty())
        return conn;

    conn.dev_id  = MachineObject::dev_id_from_address(conn.host, conn.port);
    conn.use_ssl = boost::istarts_with(conn.host, "https://");
    return conn;
}

MachineObject* ActivePrinterSession::live_machine() const
{
    DeviceManager* dev_mgr = wxGetApp().getDeviceManager();
    MachineObject* machine = dev_mgr ? dev_mgr->get_selected_machine() : nullptr;
    if (wxGetApp().getAgent() == nullptr || machine == nullptr)
        return nullptr;

    // DeviceManager::selected_machine outlives a printer-preset switch, so a non-null selected
    // machine doesn't by itself mean the edited profile still corresponds to it. Reuse
    // GUI_App::is_blocking_printing's profile<->machine correspondence check (also used to gate
    // AMS sync and the extruder-separator icon) rather than a second rule that could disagree with
    // it. Deliberately not Plater::is_same_printer_for_connected_and_selected: that additionally
    // requires check_printer_initialized, which must keep reporting "connected, sync unavailable"
    // for a stale heartbeat rather than falling back to "offline".
    if (wxGetApp().is_blocking_printing(machine))
        return nullptr;

    // Model correspondence alone is not enough: every preset of the connected machine's model
    // (including a factory default with no connection settings at all) would read as live.
    const Connection conn = connection();
    if (!conn.configured())
        return nullptr;

    // When both sides expose an address, they must be the same machine -- two same-model profiles
    // pointing at different printers must not read each other's connection as their own. Compare
    // just the host part (the profile's host may carry a scheme or port; the machine stores a bare
    // ip). A machine with no ip (e.g. a cloud-bound connection) is left to the model check above.
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
    return agent->bind_device_connection(conn.dev_id, conn.dev_id, conn.access_code(), conn.use_ssl);
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
