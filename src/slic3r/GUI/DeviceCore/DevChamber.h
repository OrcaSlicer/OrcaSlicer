#pragma once
#include <nlohmann/json.hpp>
#include "slic3r/Utils/json_diff.hpp"

namespace Slic3r {

class MachineObject;

class DevChamber
{
public:
    static std::shared_ptr<DevChamber> Create(MachineObject *obj) { return std::shared_ptr<DevChamber>( new DevChamber(obj)); }

public: // getter
    bool HasChamber() const;
    bool SupportChamberTempDisplay() const;
    bool SupportChamberEdit() const;
    int  GetChamberTempEditMin() const;
    int  GetChamberTempEditMax() const;
    int  GetChamberTempSwitchHeat() const;

    float GetChamberTemp() const { return m_temp; };
    float GetChamberTempTarget() const { return m_temp_target; };

public:
    // setter
    void ParseChamber(const json &print_json);

    void ParseChamberV1_0(const json& print_json);
    void ParseChamberV2_0(const json& print_json);

    // control
    int CtrlSetChamberTemp(int temp);

protected:
    DevChamber(MachineObject *obj) : m_owner(obj) {}

private:
    float m_temp = 0.0f;
    float m_temp_target = 0.0f;
    MachineObject* m_owner = nullptr;
};

} // namespace Slic3r