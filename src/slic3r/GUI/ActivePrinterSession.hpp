#ifndef slic3r_GUI_ActivePrinterSession_hpp_
#define slic3r_GUI_ActivePrinterSession_hpp_

#include <string>

#include "libslic3r/Preset.hpp"

namespace Slic3r {

class MachineObject;
class NetworkAgent;

namespace GUI {

// Which printer the user is working with and how to reach it, derived from the printer profile
// on every call. Holds no state: storing a host, key or identity here would make a second owner.
// The agent's device_info is a cache written only by bind_agent(); DeviceManager's selection
// outlives a preset switch, so it is only compared against the profile, never trusted as identity.
class ActivePrinterSession
{
public:
    // Read from the profile only: a MachineObject's dev_ip may be restored from a previous
    // session and never refreshed, so it can name a wrong address.
    struct Connection
    {
        std::string host;    ///< print_host verbatim (may carry a scheme and/or a port)
        std::string api_key; ///< printhost_apikey ("" when unset)
        std::string dev_id;  ///< host+port as a device id (MachineObject::dev_id_from_address)
        bool        use_ssl = false;

        bool configured() const { return !host.empty(); }

        /// An empty key becomes the "88888888" placeholder, except for Moonraker, which treats an
        /// empty key as a trusted client and answers the placeholder with HTTP 401.
        std::string access_code(const std::string& agent_id) const;
    };

    // The edited preset, not the selected one: slice-time mapping resolves tool counts against
    // the edited preset, and a mismatch would persist since inventories are only ever padded.
    const Preset& profile() const;

    // Preset::get_printer_type() needs a mutable Preset, which profile() doesn't hand out.
    std::string printer_type() const;

    Connection connection() const;

    // The selected machine matches the configured profile by model and address. Not
    // reachability: a stale heartbeat still reads as live, so failures surface as a failed sync.
    bool live() const;

    // nullptr unless live(). For machine-side data only; addressing comes from connection().
    MachineObject* live_machine() const;

    // The agent bound to connection(), or nullptr when not live().
    NetworkAgent* sync_agent() const;

    // The only path from preset connection settings to an agent; agents don't read presets.
    bool bind_agent() const;
};

const ActivePrinterSession& active_printer_session();

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ActivePrinterSession_hpp_
