#ifndef slic3r_GUI_ActivePrinterSession_hpp_
#define slic3r_GUI_ActivePrinterSession_hpp_

#include <string>

#include "libslic3r/Preset.hpp"

namespace Slic3r {

class MachineObject;
class NetworkAgent;

namespace GUI {

// The one accessor for "which printer is the user working with, and how do we reach it".
//
// INVARIANT: this class holds no state of its own. The printer PROFILE is the sole source of
// truth, and this is the only place that reads it for connection purposes. Every accessor below
// derives its answer from the live preset bundle on the call, so there is nothing here that can
// go stale, nothing to invalidate when the user switches or edits a preset, and no second copy of
// the truth for anything to disagree with. Adding a data member that stores a host, a key or a
// printer identity would break that guarantee and turn this into a second owner.
//
// The two pieces of connection state that DO persist elsewhere are subordinated to the profile,
// never read as truth:
//   - the printer agent's device_info is a cache of connection(), written only through
//     bind_agent() (see IPrinterAgent::bind_device_connection);
//   - DeviceManager's machine selection is only ever COMPARED against the profile
//     (live_machine()), never used to answer "which printer is this": it outlives a preset
//     switch, so it cannot be trusted as identity.
class ActivePrinterSession
{
public:
    // How to reach the active profile's printer, read from that profile ALONE. Never from a
    // MachineObject: a MachineObject's dev_ip can be a value restored from a previous session's
    // AppConfig entry, and it is never refreshed against the preset afterwards, so it can name a
    // real but wrong address. The preset is the value the user actually edited and can verify.
    struct Connection
    {
        std::string host;    ///< print_host verbatim (may carry a scheme and/or a port)
        std::string port;    ///< printhost_port
        std::string api_key; ///< printhost_apikey ("" when unset)
        std::string dev_id;  ///< host+port as a device id (MachineObject::dev_id_from_address)
        bool        use_ssl = false;

        /// A profile only addresses a printer once it carries its own connection settings.
        /// print_host is a per-preset option and one preset per physical machine is how
        /// connections are organized throughout, so no host means "no printer to talk to",
        /// not "the printer someone connected to earlier".
        bool configured() const { return !host.empty(); }

        /// The access code Orca's device layer expects, which must be non-empty even when the
        /// printer needs no key. Kept here so the substitution can't differ between call sites.
        std::string access_code() const { return api_key.empty() ? std::string("88888888") : api_key; }
    };

    // THE resolution of "the current printer profile", deliberately the EDITED preset rather than
    // the selected one. An unsaved printer edit must reach every surface at once: slice-time
    // mapping resolves tool counts against the edited preset, so a dialog resolving the selected
    // preset instead could show a different tool count than the mapper that actually slices, and
    // an inventory is only ever padded (never truncated), so that mismatch would persist on disk.
    // Nothing else in this feature repeats this choice -- it is made here, once.
    const Preset& profile() const;

    // profile()'s printer type (the model id the device layer identifies a machine by). Exists
    // because Preset::get_printer_type() needs a mutable Preset and the bundle it belongs to,
    // which profile()'s const reference deliberately doesn't hand out.
    std::string printer_type() const;

    Connection connection() const;

    // Whether a connected machine currently corresponds to profile(): an agent exists, a machine
    // is selected, that machine matches the profile by model AND by address, and the profile is
    // configured at all. An unconfigured profile is never live.
    //
    // Deliberately NOT "is this printer reachable right now" or "does it support filament sync" --
    // either can be false for a printer that still corresponds to the profile (a momentarily stale
    // heartbeat, a model that can't report loaded filaments), and those keep reading as live so a
    // failure surfaces as a failed sync rather than as "offline".
    bool live() const;

    // The selected machine, but only once it is confirmed to correspond to profile() -- otherwise
    // nullptr. For machine-side DATA only (its filament system, its heartbeat); identity and
    // addressing come from profile()/connection(), never from what this returns.
    MachineObject* live_machine() const;

    // The agent to run printer I/O through, with its connection state already pointed at
    // connection(). nullptr when there is nothing live to talk to. This is the only supported way
    // for this feature to reach the agent for a fetch/push.
    NetworkAgent* sync_agent() const;

    // Pushes connection() into the printer agent, whose device_info is a CACHE of it -- so its
    // REST calls dial the address the active profile names. The single place preset connection
    // settings reach an agent; agents do not read presets themselves. No-op when the profile is
    // unconfigured or no agent exists.
    bool bind_agent() const;
};

// The app-wide session. Stateless by the invariant above, so this is just the one name every call
// site shares -- not an object that owns anything.
const ActivePrinterSession& active_printer_session();

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ActivePrinterSession_hpp_
