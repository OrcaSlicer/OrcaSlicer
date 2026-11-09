#include "DevLamp.h"

#include <nlohmann/json.hpp>

#include <string>

static Slic3r::DevLamp::LIGHT_EFFECT _light_effect_parse(std::string effect_str)
{
    if (effect_str.compare("on") == 0)
        return Slic3r::DevLamp::LIGHT_EFFECT_ON;
    else if (effect_str.compare("off") == 0)
        return Slic3r::DevLamp::LIGHT_EFFECT_OFF;
    else if (effect_str.compare("flashing") == 0)
        return Slic3r::DevLamp::LIGHT_EFFECT_FLASHING;

    return Slic3r::DevLamp::LIGHT_EFFECT_UNKOWN;
}

void Slic3r::DevLamp::SetChamberLight(const std::string& status)
{
    m_chamber_light = _light_effect_parse(status);
}

void Slic3r::DevLamp::ParseLightsReport(const nlohmann::json& lights_report)
{
    if (!lights_report.is_array())
        return;

    bool has_chamber_light2 = false;
    for (const auto& light : lights_report) {
        const std::string node = light.value("node", "");
        if (node == "chamber_light" && light.contains("mode"))
            SetChamberLight(light["mode"].get<std::string>());
        else if (node == "chamber_light2")
            has_chamber_light2 = true;
    }
    // Sticky once seen, so a report that lists only some lights never drops a second light.
    m_has_chamber_light2 = m_has_chamber_light2.value_or(false) || has_chamber_light2;
}
