#ifndef slic3r_CrealityHostDiscovery_hpp_
#define slic3r_CrealityHostDiscovery_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// One discovered Creality K-series host on the LAN.
struct CrealityHost
{
    std::string ip;            // dotted-quad IPv4
    std::string service_name;  // raw mDNS service type, e.g. "_Creality-543324280CDB19._udp.local."
    std::string hostname;      // e.g. "K2-DB19" (derived from service-name suffix)
    std::string model_code;    // "F008" / "F012" / "F021" (empty if /info probe failed)
    std::string model_name;    // "K2 Plus" / "K2 Pro" / "K2" (empty if model not in our table)
    std::string mac;           // from /info if probed
    bool        cfs_capable = false;  // true when model_code is in the K2 family
};

// Synchronous LAN discovery for Creality K-series printers via DNS-SD mDNS.
// Uses meta-discovery (_services._dns-sd._udp.local.) since firmware uses per-device
// type names (_Creality-<MAC>._udp.local). When probe_info is true, fetches /info
// per host for model code (F008/F012/F021) and MAC (+2-4s per host).
// Call from a background thread - blocks at least 5 seconds.
class CrealityHostDiscovery
{
public:
    static std::vector<CrealityHost> scan(bool probe_info = true);
};

} // namespace Slic3r

#endif
