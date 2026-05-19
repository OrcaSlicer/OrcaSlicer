#include "CrealityHostDiscovery.hpp"
#include "mdns/cxmdns.h"
#include "Http.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace Slic3r {

namespace {

struct ModelEntry { const char* code; const char* name; };
constexpr ModelEntry kCfsCapableModels[] = {
    {"F008", "K2 Plus"},
    {"F012", "K2 Pro"},
    {"F021", "K2"},
};

bool is_cfs_capable(const std::string& code)
{
    for (const auto& m : kCfsCapableModels)
        if (code == m.code) return true;
    return false;
}

std::string model_name_for(const std::string& code)
{
    for (const auto& m : kCfsCapableModels)
        if (code == m.code) return m.name;
    return {};
}

// Extract the device suffix from a service name like
// "_Creality-543324280CDB19._udp.local." and synthesise a hostname-ish label
// (e.g. "K2-DB19" using the last 4 hex of the MAC-derived suffix).
std::string hostname_from_service(const std::string& service_name)
{
    auto dash = service_name.find_last_of('-');
    if (dash == std::string::npos) return {};
    auto dot = service_name.find('.', dash);
    if (dot == std::string::npos) return {};
    std::string suffix = service_name.substr(dash + 1, dot - dash - 1);
    if (suffix.size() >= 4) {
        return "K2-" + suffix.substr(suffix.size() - 4);
    }
    return suffix.empty() ? std::string{} : "K2-" + suffix;
}

// Synchronously probe http://<ip>/info for {model, mac}. Short timeout --
// we don't want one slow host to drag down discovery.
void probe_info(CrealityHost& host)
{
    const std::string url = "http://" + host.ip + "/info";
    auto http = Http::get(url);
    http.timeout_connect(2)
        .timeout_max(4)
        .on_complete([&host](std::string body, unsigned /*status*/) {
            try {
                auto j = nlohmann::json::parse(body);
                if (j.contains("model") && j["model"].is_string())
                    host.model_code = j["model"].get<std::string>();
                if (j.contains("mac") && j["mac"].is_string())
                    host.mac = j["mac"].get<std::string>();
                if (is_cfs_capable(host.model_code)) {
                    host.cfs_capable = true;
                    host.model_name  = model_name_for(host.model_code);
                }
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning)
                    << "CrealityHostDiscovery: /info parse failed for "
                    << host.ip << ": " << e.what();
            }
        })
        .on_error([&host](std::string /*body*/, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(info)
                << "CrealityHostDiscovery: /info GET failed for "
                << host.ip << ": " << error << " (HTTP " << status << ")";
        })
        .perform_sync();
}

} // namespace

std::vector<CrealityHost> CrealityHostDiscovery::scan(bool probe)
{
    const std::vector<std::string> prefixes{ "Creality", "creality" };

    BOOST_LOG_TRIVIAL(info)
        << "CrealityHostDiscovery: starting DNS-SD discovery (prefixes: Creality, creality)";

    auto raw = cxnet::syncDiscoveryService(prefixes);

    BOOST_LOG_TRIVIAL(info)
        << "CrealityHostDiscovery: mDNS returned " << raw.size() << " match(es)";

    std::vector<CrealityHost> hosts;
    hosts.reserve(raw.size());

    // Dedupe by IP -- one printer may announce twice if multi-homed or if
    // we capture both IPv4/IPv6 replies.
    std::vector<std::string> seen_ips;
    for (const auto& m : raw) {
        if (m.machineIp.empty()) continue;
        if (std::find(seen_ips.begin(), seen_ips.end(), m.machineIp) != seen_ips.end())
            continue;
        seen_ips.push_back(m.machineIp);

        CrealityHost h;
        h.ip           = m.machineIp;
        h.service_name = m.answer;
        h.hostname     = hostname_from_service(m.answer);

        if (probe) {
            probe_info(h);
        }

        hosts.push_back(std::move(h));
    }

    BOOST_LOG_TRIVIAL(info)
        << "CrealityHostDiscovery: " << hosts.size() << " unique host(s) after dedup";

    return hosts;
}

} // namespace Slic3r
