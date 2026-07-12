#include "CrealityPrintAgent.hpp"
#include "CrealityCfs.hpp"

#include <boost/log/trivial.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {
constexpr const char* CrealityPrintAgent_VERSION = "0.1.0";
}

CrealityPrintAgent::CrealityPrintAgent(std::string log_dir)
    : MoonrakerPrinterAgent(std::move(log_dir))
{
}

AgentInfo CrealityPrintAgent::get_agent_info_static()
{
    return AgentInfo{
        "crealityprint",
        "CrealityPrint",
        CrealityPrintAgent_VERSION,
        "Creality K-series printer agent (CFS-aware filament sync)"
    };
}

bool CrealityPrintAgent::fetch_filament_info(std::string dev_id)
{
    // Try each Creality product-line CFS provider in order; the first whose
    // transport finds loaded slots wins. The providers are decoupled in
    // CrealityCfs.* so the K1 (Moonraker `box`) and K2 (port-9999 boxsInfo) paths
    // never tangle and can evolve independently.
    std::vector<std::unique_ptr<ICrealityCfsProvider>> providers;
    providers.push_back(std::make_unique<CrealityCfsK1Provider>(
        device_info.dev_ip, device_info.api_key, device_info.base_url));
    providers.push_back(std::make_unique<CrealityCfsK2Provider>(
        device_info.dev_ip, device_info.api_key));

    for (auto& provider : providers) {
        std::vector<CfsTray> cfs_trays;
        int                  box_count = 0;
        if (!provider->fetch(cfs_trays, box_count) || box_count <= 0)
            continue;

        // Convert the neutral CfsTray list into the base agent's AmsTrayData and
        // publish. build_ams_payload fills the unlisted slots as placeholders.
        std::vector<AmsTrayData> trays;
        trays.reserve(cfs_trays.size());
        for (const auto& t : cfs_trays) {
            AmsTrayData a;
            a.slot_index    = t.slot_index;
            a.has_filament  = t.has_filament;
            a.tray_type     = t.tray_type;
            a.tray_color    = t.tray_color;
            a.tray_info_idx = t.tray_info_idx;
            a.remain        = t.remain;
            trays.push_back(std::move(a));
        }

        const int max_lane_index = box_count * 4 - 1;
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: CFS via " << provider->name() << " — "
            << cfs_trays.size() << " loaded slot(s) across " << box_count << " box(es)";
        build_ams_payload(box_count, max_lane_index, trays);
        return true;
    }

    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: no CFS detected, deferring to base Moonraker agent";
    return MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id));
}

} // namespace Slic3r
