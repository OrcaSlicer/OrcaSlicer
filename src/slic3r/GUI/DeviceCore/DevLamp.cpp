#include "DevLamp.h"

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
