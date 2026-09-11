#include "DevChamber.h"

#include "DevConfig.h"
#include "DevUtil.h"

#include "slic3r/GUI/DeviceManager.hpp"

namespace Slic3r {

bool DevChamber::HasChamber() const { return m_owner->GetConfig()->HasChamber(); }

bool DevChamber::SupportChamberTempDisplay() const { return m_owner->GetConfig()->SupportChamberTempDisplay();}

bool DevChamber::SupportChamberEdit() const { return m_owner->GetConfig()->SupportChamberEdit(); }

int DevChamber::GetChamberTempEditMin() const { return m_owner->GetConfig()->GetChamberTempEditMin(); }

int DevChamber::GetChamberTempEditMax() const { return m_owner->GetConfig()->GetChamberTempEditMax(); }

int DevChamber::GetChamberTempSwitchHeat() const { return m_owner->GetConfig()->GetChamberTempSwitchHeat(); }

void DevChamber::ParseChamber(const json &print_json)
{
    ParseChamberV1_0(print_json);
    ParseChamberV2_0(print_json);
}

void DevChamber::ParseChamberV1_0(const json &print_json)
{
    DevJsonValParser::ParseVal(print_json, "chamber_temper", m_temp);
    DevJsonValParser::ParseVal(print_json, "ctt", m_temp_target);
}

void DevChamber::ParseChamberV2_0(const json &print_json)
{
    if (print_json.contains("device")) {
        const json &device_jj = print_json["device"];
        if (device_jj.contains("ctc")) {
            const json &ctc   = device_jj["ctc"];
            int         state = DevUtil::get_flag_bits(ctc["state"].get<int>(), 0, 4);
            if (ctc.contains("info")) {
                const json &info = ctc["info"];
                m_temp           = DevUtil::get_flag_bits(info["temp"].get<int>(), 0, 16);
                m_temp_target    = DevUtil::get_flag_bits(info["temp"].get<int>(), 16, 16);
            }
        }
    }
}

} // namespace Slic3r