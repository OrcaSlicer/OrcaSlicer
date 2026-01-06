///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_TYPES_HPP
#define VGCODE_TYPES_HPP

#define VGCODE_ENABLE_COG_AND_TOOL_MARKERS 0

#include <array>
#include <vector>
#include <cstdint>

namespace libvgcode {

static constexpr float PI = 3.141592f;

//
// Predefined values for the radius, in mm, of the cylinders used to render the travel moves.
//
static constexpr float DEFAULT_TRAVELS_RADIUS_MM = 0.1f;
static constexpr float MIN_TRAVELS_RADIUS_MM = 0.05f;
static constexpr float MAX_TRAVELS_RADIUS_MM = 1.0f;

//
// Predefined values for the radius, in mm, of the cylinders used to render the wipe moves.
//
static constexpr float DEFAULT_WIPES_RADIUS_MM = 0.1f;
static constexpr float MIN_WIPES_RADIUS_MM = 0.05f;
static constexpr float MAX_WIPES_RADIUS_MM = 1.0f;

//
// Vector in 3 dimensions
// [0] -> x
// [1] -> y
// [2] -> z
// Used for positions, displacements and so on.
//
using Vec3 = std::array<float, 3>;

//
// 4x4 square matrix with elements in column-major order:
// | a[0] a[4] a[8]  a[12] |
// | a[1] a[5] a[9]  a[13] |
// | a[2] a[6] a[10] a[14] |
// | a[3] a[7] a[11] a[15] |
//
using Mat4x4 = std::array<float, 16>;

//
// RGB color
// [0] -> red
// [1] -> green
// [2] -> blue
//
using Color = std::array<uint8_t, 3>;

//
// Color palette
//
using Palette = std::vector<Color>;

//
// Axis aligned box in 3 dimensions
// [0] -> { min_x, min_y, min_z }
// [1] -> { max_x, max_y, max_z }
//
using AABox = std::array<Vec3, 2>;

//
// One dimensional natural numbers interval
// [0] -> min
// [1] -> max
//
using Interval = std::array<std::size_t, 2>;

//
// View types
//
enum class EViewType : uint8_t
{
    Summary, // ORCA
    FeatureType,
    ColorPrint,
    Speed,
    ActualSpeed,
    Height,
    Width,
    VolumetricFlowRate,
    ActualVolumetricFlowRate,
    LayerTimeLinear,
    LayerTimeLogarithmic,
    FanSpeed,
    Temperature,
    Tool,
    COUNT
};

static constexpr std::size_t VIEW_TYPES_COUNT = static_cast<std::size_t>(EViewType::COUNT);

//
// Move types
//
enum class EMoveType : uint8_t
{
    Noop,
    Retract,
    Unretract,
    Seam,
    ToolChange,
    ColorChange,
    PausePrint,
    CustomGCode,
    Travel,
    Wipe,
    Extrude,
    COUNT
};

static constexpr std::size_t MOVE_TYPES_COUNT = static_cast<std::size_t>(EMoveType::COUNT);

//
// Extrusion roles
//
enum class EGCodeExtrusionRole : uint8_t
{
      // This enum is used as in index into extrusion_roles_visibility.
      // Better only add things to the end.
	  None,
	  Perimeter,
	  ExternalPerimeter,
	  OverhangPerimeter,
	  InternalInfill,
	  SolidInfill,
	  TopSolidInfill,
	  Ironing,
	  BridgeInfill,
	  GapFill,
	  Skirt,
	  SupportMaterial,
	  SupportMaterialInterface,
	  WipeTower,
	  Custom,
      // ORCA
      BottomSurface,
      InternalBridgeInfill,
      Brim,
      SupportTransition,
      Mixed,
    COUNT
};

static constexpr std::size_t GCODE_EXTRUSION_ROLES_COUNT = static_cast<std::size_t>(EGCodeExtrusionRole::COUNT);

//
// Option types
//
enum class EOptionType : uint8_t
{
    // This enum is used as in index into options_visibility.
    // Better only add things to the end.
    Travels,
    Wipes,
    Retractions,
    Unretractions,
    Seams,
    ToolChanges,
    ColorChanges,
    PausePrints,
    CustomGCodes,
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    CenterOfGravity,
    ToolMarker,
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    COUNT
};

static constexpr std::size_t OPTION_TYPES_COUNT = static_cast<std::size_t>(EOptionType::COUNT);

//
// Time modes
//
enum class ETimeMode : uint8_t
{
    Normal,
    Stealth,
    COUNT
};

static constexpr std::size_t TIME_MODES_COUNT = static_cast<std::size_t>(ETimeMode::COUNT);

//
// Color range types
//
enum class EColorRangeType : uint8_t
{
    Linear,
    Logarithmic,
    COUNT
};

static constexpr std::size_t COLOR_RANGE_TYPES_COUNT = static_cast<std::size_t>(EColorRangeType::COUNT);

//
// Predefined colors
//
static const Color DUMMY_COLOR{ 64, 64, 64 };

//
// Mapping from EMoveType to EOptionType
// Returns EOptionType::COUNT if the given move type does not correspond
// to any option type.
//
extern EOptionType move_type_to_option(EMoveType type);

//
// Returns the linear interpolation between the two given colors
// at the given t.
// t is clamped in the range [0..1]
//
extern Color lerp(const Color& c1, const Color& c2, float t);

} // namespace libvgcode

#endif // VGCODE_TYPES_HPP
