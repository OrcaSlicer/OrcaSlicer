///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "../include/PathVertex.hpp"

namespace libvgcode {

const PathVertex PathVertex::DUMMY_PATH_VERTEX = PathVertex();

bool PathVertex::is_extrusion() const
{
    return type == EMoveType::Extrude;
}

bool PathVertex::is_travel() const
{
    return type == EMoveType::Travel;
}

bool PathVertex::is_wipe() const
{
    return type == EMoveType::Wipe;
}

bool PathVertex::is_option() const
{
    switch (type)
    {
    case EMoveType::Retract:
    case EMoveType::Unretract:
    case EMoveType::Seam:
    case EMoveType::ToolChange:
    case EMoveType::ColorChange:
    case EMoveType::PausePrint:
    case EMoveType::CustomGCode:
    {
        return true;
    }
    default:
    {
        return false;
    }
    }
}

bool PathVertex::is_custom_gcode() const
{
    return type == EMoveType::Extrude && role == EGCodeExtrusionRole::Custom;
}

} // namespace libvgcode
