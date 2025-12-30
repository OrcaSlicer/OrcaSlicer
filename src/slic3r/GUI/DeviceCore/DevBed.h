#pragma once
#include <nlohmann/json.hpp>
#include "slic3r/Utils/json_diff.hpp"

namespace Slic3r {

class MachineObject;

class DevBed
{
public:
    DevBed(MachineObject *obj) : m_owner(obj), bed_temp(0.0f), bed_temp_target(0.0f) {}

    float GetBedTemp() { return bed_temp; };
    float GetBedTempTarget() { return bed_temp_target; };

public:

    static void ParseV1_0(const json &print_json, DevBed *system);
    static void ParseV2_0(const json &print_json, DevBed *system);

private:

    float bed_temp;
    float bed_temp_target;

    MachineObject* m_owner = nullptr;
};

} // namespace Slic3r