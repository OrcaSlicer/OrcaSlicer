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
//
// Sends a meta-discovery query (_services._dns-sd._udp.local.) and listens
// for ~5 seconds for service announcements whose type-name contains the
// "Creality" / "creality" substring. K-series firmware announces each
// printer under a per-device-unique type _Creality-<MAC-derived-hex>._udp.local,
// so a fixed-name query does not work -- the meta-discovery is the only
// reliable way to find them.
//
// When probe_info is true, each discovered host is followed up with an HTTP
// GET http://<ip>/info call to fetch the printer's model code (F008/F012/F021)
// and MAC. The probe step adds ~2-4 seconds per host but yields enriched
// results that let the UI display "K2" / "K2 Plus" / "K2 Pro" instead of
// just an IP.
//
// Call from a background thread -- the function blocks for at least 5 seconds.
class CrealityHostDiscovery
{
public:
    static std::vector<CrealityHost> scan(bool probe_info = true);
};

} // namespace Slic3r

#endif
