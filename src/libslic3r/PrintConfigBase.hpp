#ifndef slic3r_PrintConfigBase_hpp_
#define slic3r_PrintConfigBase_hpp_

// The print configuration without the generated static config classes: the option enums,
// PrintConfigDef, DynamicPrintConfig, the StaticPrintConfig base, ModelConfig and helpers that take
// a config by reference. PrintConfig.hpp adds FullPrintConfig and the classes it is built from;
// include that only where those classes are used, since they are costly to compile.

#include "Point.hpp"
#include "libslic3r.h"
#include "CommonDefs.hpp"
#include "Config.hpp"
#include "Polygon.hpp"
#include <boost/container_hash/hash.hpp>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <utility>
#include <cstddef>
#include <cassert>
#include <cstdint>
#include <cereal/access.hpp>
#include <cmath>
#include <cereal/specialize.hpp>
#include <stdexcept>

namespace Slic3r {

class DynamicPrintConfig;

enum GCodeFlavor : unsigned char {
    gcfMarlinLegacy, 
    gcfKlipper, 
    gcfRepRapFirmware, 
    gcfRepetier, 
    gcfMarlinFirmware, 
    gcfRepRapSprinter, 
    gcfTeacup, 
    gcfMakerWare, 
    gcfSailfish, 
    gcfMach3, 
    gcfMachinekit,
    gcfSmoothie, 
    gcfNoExtrusion
};

// How a filament is used across the model. Part of the multi-nozzle grouping data; not yet
// read by the shipping slicer — the nozzle-centric FilamentGroup engine consumes it.
enum FilamentUsageType {
    SupportOnly,
    ModelOnly,
    Hybrid
};


enum class FuzzySkinType {
    None,
    External,
    Hole,
    All,
    AllWalls,
    Disabled_fuzzy,
};

enum class FuzzySkinMode {
    Displacement,
    Extrusion,
    Combined,
};

// ORCA: direction in which top_surface_expansion grows the top surfaces.
enum class TopSurfaceExpansionDirection {
    InwardAndOutward,
    Inward,
    Outward,
};

enum class CenterOfSurfacePattern {
    Each_Surface,
    Each_Model,
    Each_Assembly,
};

enum class NoiseType {
    Classic,
    Perlin,
    Billow,
    RidgedMulti,
    Voronoi,
    Ripple,
};

enum class WipeTowerType {
    Type1,
    Type2,
};

enum PrintHostType {
    htPrusaLink, htPrusaConnect, htOctoPrint, htDuet, htUltiMaker, htFlashAir, htAstroBox, htRepetier, htMKS, htESP3D, htCrealityPrint, htObico, htFlashforge, htSimplyPrint, htElegooLink, ht3DPrinterOS, htMoonraker
};

enum AuthorizationType {
    atKeyPassword, atUserPassword
};

enum InfillPattern : int {
    ipMonotonic, ipMonotonicLine,
    ipRectilinear, ipAlignedRectilinear, ipZigZag, ipCrossZag, ipLockedZag,
    ipLine, ipGrid,
    ipTriangles, ipStars,
    ipCubic, ipAdaptiveCubic, ipQuarterCubic, ipSupportCubic, ipLightning,
    ipHoneycomb, ip3DHoneycomb, ipLateralHoneycomb, ipLateralLattice,
    ipCrossHatch, ipTpmsD, ipTpmsFK, ipGyroid,
    ipConcentric, ipSpiralInset, ipHilbertCurve, ipArchimedeanChords, ipOctagramSpiral,
    ipSupportBase, ipConcentricInternal,
    ipCount,
};

// Orca: Infill patterns that the "separated_infills" option can center on each connected body.
inline bool is_separable_infill_pattern(InfillPattern pattern)
{
    switch (pattern) {
    case ipMonotonic:
    case ipMonotonicLine:
    case ipRectilinear:
    case ipAlignedRectilinear:
    case ipZigZag:
    case ipCrossZag:
    case ipLockedZag:
    case ipLine:
    case ipGrid:
    case ipTriangles:
    case ipStars:          // tri-hexagon
    case ipCubic:
    case ipQuarterCubic:
    case ipHoneycomb:
    case ip3DHoneycomb:
    case ipLateralHoneycomb:
    case ipLateralLattice:
    case ipCrossHatch:
    case ipTpmsD:
    case ipTpmsFK:
    case ipGyroid:
    case ipHilbertCurve:
    case ipArchimedeanChords:
    case ipOctagramSpiral:
        return true;
    default:
        return false;
    }
}

// Orca: Infill patterns laid out by an octree, which each connected body always gets of its own.
inline bool is_octree_infill_pattern(InfillPattern pattern) { return pattern == ipAdaptiveCubic || pattern == ipSupportCubic; }
// Orca: Infill patterns graded by the "adaptive_infill_mode" option.
inline bool is_adaptive_infill_pattern(InfillPattern pattern) { return pattern == ipGyroid || pattern == ipTpmsD || pattern == ipTpmsFK; }

// Orca: Infill patterns that round their corners by the "sparse_infill_smooth_factor" option.
// Grid, Triangles and Tri-hexagon only do so in their trapezoidal form, which is generated with more
// than one line per infill wall; a single line makes them plain crossing lines with nothing to round.
inline bool is_smoothable_infill_pattern(InfillPattern pattern, int multiline = 1)
{
    switch (pattern) {
    case ipHilbertCurve:
    case ipOctagramSpiral:
    case ipLightning:
    case ipHoneycomb:
    case ip3DHoneycomb:
    case ipConcentric:
    case ipCrossHatch:
        return true;
    case ipGrid:
    case ipTriangles:
    case ipStars:
    case ipCubic:
    case ipQuarterCubic:
        return multiline > 1;
    default:
        return false;
    }
}

enum class IroningType {
    NoIroning,
    TopSurfaces,
    TopmostOnly,
    AllSolid,
    Count,
};

// Smallest usable ironing line spacing. Anything tighter yields an unprintable number of lines,
// and zero stops the fillers from making progress.
constexpr double IRONING_SPACING_MIN = 0.05;

//BBS
enum class WallInfillOrder {
    InnerOuterInfill,
    OuterInnerInfill,
    InfillInnerOuter,
    InfillOuterInner,
    InnerOuterInnerInfill,
    Count,
};

enum class BedTempFormula {
    btfFirstFilament,
    btfHighestTemp,
    count,
};

// Orca
enum class PowerLossRecoveryMode {
    PrinterConfiguration,
    Enable,
    Disable,
};

// Orca
enum class ArcFittingMode {
    Disabled,
    Enabled,
    SparseInfill,
};

// BBS
enum class WallSequence {
    InnerOuter,
    OuterInner,
    InnerOuterInner,
    Count,
};

// Orca
enum class WallDirection
{
    CounterClockwise,
    Clockwise,
    Count,
};

// IMEX: physical bed corner where tool T0 sits. Determines how tool indices
// map to bed zones (front = lower Y near operator, rear = higher Y).
enum class ImexToolLayout {
    FrontLeft,
    FrontRight,
    RearLeft,
    RearRight,
    Count,
};

// IMEX: color theme for the bed-zone visualization. Colorblind-friendly variants
// are provided for users with color-vision differences.
enum class ImexVizTheme {
    Standard,
    Deuteranopia,
    Tritanopia,
    HighContrast,
    Count,
};

// Orca: print order of surface fill loops/fragments for center-based fill patterns
// (Concentric, Archimedean Chords, Octagram Spiral).
enum class SurfaceFillOrder {
    Default,
    Outward,
    Inward,
    Count,
};

// Orca: what the adaptive infill density follows: the 3D shape of the object, or its 2D sections normal to an axis.
enum class AdaptiveInfillMode {
    Disabled,
    DistanceWarp,
    SmoothBlend,
    SteppedShells,
    Lobes,
    NormalZ,
    NormalY,
    NormalX,
    Count,
};

// Orca: how the adaptive infill density changes from the object surface to its deepest point.
enum class AdaptiveInfillGradient {
    Linear,
    Quadratic,
    Exponential,
};

//BBS
enum class PrintSequence {
    ByLayer,
    ByObject,
    ByDefault,
    Count,
};

enum class PrintOrder
{
    Default,
    AsObjectList,
    BestOfStrategies,    // run all custom strategies, pick the shortest total path
    Snake,               // snake-like row traversal (back-and-forth) + 2-opt
    Count,
};

enum class SlicingMode
{
    // Regular, applying pftNonZero rule when creating ExPolygons.
    Regular,
    // Compatible with 3DLabPrint models, applying pftEvenOdd rule when creating ExPolygons.
    EvenOdd,
    // Orienting all contours CCW, thus closing all holes.
    CloseHoles,
};

// Axis around which the mesh is rotated before slicing, when
// `belt_slice_rotation` is set.  None disables the rotation stage.  This is the
// single "belt tilt" axis: it drives both the pre-slice mesh rotation and the
// post-slice machine-frame transform (shear + scale derived from the tilt angle).
enum class BeltRotationAxis
{
    None = 0,
    X    = 1,
    Y    = 2,
    Z    = 3,
};

enum class RemapAxis
{
    PosX = 0, PosY = 1, PosZ = 2,
    NegX = 3, NegY = 4, NegZ = 5,
    RevX = 6, RevY = 7, RevZ = 8,  // Reversed: max - pos
};

enum SupportMaterialPattern {
    smpDefault,
    smpRectilinear, smpRectilinearGrid, smpHoneycomb,
    smpLightning,
    smpNone,
};

enum SupportMaterialStyle {
    smsDefault, smsGrid, smsSnug, smsTreeOrganic, smsTreeSlim, smsTreeStrong, smsTreeHybrid,
};

enum LongRectrationLevel
{
    Disabled=0,
    EnableMachine,
    EnableFilament
};

enum SupportMaterialInterfacePattern {
    smipAuto, smipRectilinear, smipConcentric, smipSpiralInset, smipRectilinearInterlaced, smipGrid
};

// BBS
enum SupportType {
    stNormalAuto, stTreeAuto, stNormal, stTree
};
inline bool is_tree(SupportType stype)
{
    return std::set<SupportType>{stTreeAuto, stTree}.count(stype) != 0;
};
inline bool is_tree_slim(SupportType type, SupportMaterialStyle style)
{
    return is_tree(type) && style==smsTreeSlim;
};
inline bool is_auto(SupportType stype)
{
    return std::set<SupportType>{stNormalAuto, stTreeAuto}.count(stype) != 0;
};

enum SeamPosition {
    spNearest, spAligned, spAlignedBack, spRear, spRandom
};

// Orca
enum class SeamScarfType {
    None,
    External,
    All,
};

// Orca
enum EnsureVerticalShellThickness {
    evstNone,
    evstCriticalOnly,
    evstModerate,
    evstAll,
};

//Orca
enum InternalBridgeFilter {
    ibfDisabled, ibfLimited, ibfNofilter
};

//Orca
enum EnableExtraBridgeLayer {
    eblDisabled, eblExternalBridgeOnly, eblInternalBridgeOnly, eblApplyToAll
};

//Orca
enum GapFillTarget {
     gftEverywhere, gftTopBottom, gftNowhere
 };


enum LiftType {
    NormalLift,
    SpiralLift,
    SlopeLift
};

enum SLAMaterial {
    slamTough,
    slamFlex,
    slamCasting,
    slamDental,
    slamHeatResistant,
};

enum SLADisplayOrientation {
    sladoLandscape,
    sladoPortrait
};

enum SLAPillarConnectionMode {
    slapcmZigZag,
    slapcmCross,
    slapcmDynamic
};

enum BrimType {
    btAutoBrim,  // BBS
    btEar, // Orca
    btPainted,  // BBS
    btOuterOnly,
    btInnerOnly,
    btOuterAndInner,
    btNoBrim,
    // Belt printers: brim only where the part first touches the belt, nothing after
    // that.  Appended last so no existing value shifts.  On a non-belt printer this
    // has no meaning and behaves as btOuterOnly.
    btLeadingEdgeOnly,
};

enum TimelapseType : int {
    tlTraditional = 0,
    tlSmooth
};

enum SkirtType {
    stCombined, stPerObject
};

enum DraftShield {
    dsDisabled, dsEnabled
};

enum class PerimeterGeneratorType
{
    // Classic perimeter generator using Clipper offsets with constant extrusion width.
    Classic,
    // Perimeter generator with variable extrusion width based on the paper
    // "A framework for adaptive width control of dense contour-parallel toolpaths in fused deposition modeling" ported from Cura.
    Arachne
};

enum class ToolChangeOrderingType
{
    Default,
    Cyclic,
};

// BBS
enum OverhangFanThreshold {
    Overhang_threshold_none = 0,
    Overhang_threshold_1_4,
    Overhang_threshold_2_4,
    Overhang_threshold_3_4,
    Overhang_threshold_4_4,
    Overhang_threshold_bridge
};

// BBS
enum BedType {
    btDefault = 0,
    btPC,
    btEP,
    btPEI,
    btPTE,
    btPCT,
    btSuperTack,
    btCount
};

enum class ExtruderOnlyAreaType:unsigned char {
    btNoArea= 0,
    Engilish,
    Chinese,
    btAreaCount
};

// BBS
enum LayerSeq {
    flsAuto,
    flsCustomize
};

enum FanDirection {
    fdUndefine = 0,
    fdLeft,
    fdRight,
    fdBoth
};

static std::unordered_map<NozzleType, std::string>NozzleTypeEumnToStr = {
    {NozzleType::ntUndefine,        "undefine"},
    {NozzleType::ntHardenedSteel,   "hardened_steel"},
    {NozzleType::ntStainlessSteel,  "stainless_steel"},
    {NozzleType::ntTungstenCarbide, "tungsten_carbide"},
    {NozzleType::ntBrass,           "brass"},
    {NozzleType::ntE3D,             "E3D"}
};

static std::unordered_map<std::string, NozzleType>NozzleTypeStrToEumn = {
    {"undefine", NozzleType::ntUndefine},
    {"hardened_steel", NozzleType::ntHardenedSteel},
    {"stainless_steel", NozzleType::ntStainlessSteel},
    {"tungsten_carbide", NozzleType::ntTungstenCarbide},
    {"brass", NozzleType::ntBrass},
    {"E3D", NozzleType::ntE3D}
};

// BBS
enum PrinterStructure {
    psUndefine=0,
    psCoreXY,
    psI3,
    psHbot,
    psDelta
};

enum class InputShaperType : unsigned char {
    Default = 0,
    MZV,
    ZV,
    ZVD,
    ZVDD,
    ZVDDD,
    EI,
    EI2,
    TwoHumpEI,
    EI3,
    ThreeHumpEI,
    DAA,
    Disable
};

// BBS
enum ZHopType {
    zhtAuto = 0,
    zhtNormal,
    zhtSlope,
    zhtSpiral,
    zhtCount
};

enum RetractLiftEnforceType {
    rletAllSurfaces = 0,
    rletTopOnly,
    rletBottomOnly,
    rletTopAndBottom
};

enum class GCodeThumbnailsFormat {
    PNG, JPG, QOI, BTT_TFT, ColPic
};

enum CounterboreHoleBridgingOption {
    chbNone, chbBridges, chbFilled
};

 enum WipeTowerWallType {
     wtwRectangle = 0,
     wtwCone,
     wtwRib
 };

// BBS
enum ExtruderType {
    etDirectDrive = 0,
    etBowden,
    etMaxExtruderType = etBowden
};

enum NozzleVolumeType {
    nvtStandard = 0,
    nvtHighFlow,
    nvtHybrid,       // extruder holds a mix of Standard and High Flow sub-nozzles; selectable only for extruders
                     // with more than one sub-nozzle (extruder_max_nozzle_count > 1); matched as Standard for
                     // preset lookup and never emitted in profile variant strings
    nvtTPUHighFlow,  // physical variant, used on H2D/H2DP 0.4 nozzles only
    // 4 is reserved: E3D High Flow is 5 in BambuStudio's slice_info and device numbering.
    nvtE3DHighFlow = 5, // physical variant, E3D high-flow hotend on 0.4/0.6 nozzles
    nvtExtraHighFlow = 6, // Orca: physical variant with no BambuStudio or device counterpart; only profiles name it
    // Integer values are serialized as raw ints in 3mf plate metadata and device MQTT, so they MUST stay stable.
    nvtMaxNozzleVolumeType = nvtExtraHighFlow
};

enum FilamentMapMode {
    fmmAutoForFlush,
    fmmAutoForMatch,
    fmmManual,
    fmmNozzleManual, // Fully-manual filament->physical-nozzle mapping (filament_nozzle_map). Kept ordered right after fmmManual so every `< fmmManual` "is-auto" check stays correct.
    fmmDefault
};

// Defines how bed exclusion volumes are resolved for printers with multiple
// independently positioned nozzles/toolheads.
enum class BedExcludeVolumeMode {
    Shared = 0,
    ToolheadOffset,
    PerExtruder
};

// All auto modes are ordered before fmmManual (see the enum ordering note above).
inline bool is_auto_filament_map_mode(FilamentMapMode mode) {
    return mode < fmmManual;
}

// Dual-extruder purge control. Default reproduces the current
// per-extruder flush_multiplier + filament_prime_volume behaviour, so absent/default is inert.
//   Saving -> reduce prime volume to 15 mm3;  Fast -> use flush_multiplier_fast + filament_flush_temp_fast.
enum PrimeVolumeMode {
    pvmDefault = 0,
    pvmSaving,
    pvmFast
};

extern std::string get_extruder_variant_string(ExtruderType extruder_type, NozzleVolumeType nozzle_volume_type);

// The variant index a value is taken from: in a variant list paired with its 1-based extruder or
// filament ids, the variant with the same variant string and id, else that id's first variant, else -1.
// variant_id_1based < 0 or empty variant_ids_1based match any id. A list without variant strings has
// one variant per id, and one with neither variant strings nor ids has a single variant.
extern int find_variant_index(const std::string& variant, int variant_id_1based, const std::vector<std::string>& variant_list, const std::vector<int>& variant_ids_1based);
// find_variant_index for every variant of a list paired with its ids, into from_variants/from_ids.
// A variant past the end of a shorter id list has no id and gets -1.
extern std::vector<int> map_variant_indices(const std::vector<std::string>& variants, const std::vector<int>& ids,
                                            const std::vector<std::string>& from_variants, const std::vector<int>& from_ids);

// Variant index lookup: scans a variant list (paired with its 1-based extruder/filament ids) for the
// entry matching the given extruder/volume type and id, as find_variant_index. Returns 0 when the id
// has no variant.
extern int get_config_index_base(NozzleVolumeType volume_type, ExtruderType extruder_type, int variant_id_1based, const std::vector<std::string>& variant_list, const std::vector<int>& variant_ids_1based);

static std::set<NozzleVolumeType> get_valid_nozzle_volume_type() {
    std::set<NozzleVolumeType> type;
    for (int i = 0; i <= nvtMaxNozzleVolumeType; ++i) {
        auto t = static_cast<NozzleVolumeType>(i);
        // Hybrid is not a physical nozzle variant: presets never define it, so it must not
        // produce a variant string.
        if (t == nvtHybrid) continue;
        // Skip the reserved gap between nvtTPUHighFlow (3) and nvtE3DHighFlow (5).
        if (i > nvtTPUHighFlow && i < nvtE3DHighFlow) continue;
        type.insert(t);
    }
    return type;
}

// The nozzle volume types the given extruder physically provides, as declared by the printer
// profile's extruder_variant_list. An empty set means the profile could not be read and must be
// treated as "unknown", not as "none". nvtHybrid is never reported: it describes an extruder
// holding a mix of nozzles, not a nozzle the profile can offer.
extern std::set<NozzleVolumeType> get_extruder_supported_nozzle_volume_types(const DynamicPrintConfig &printer_config, int extruder_id);

std::string get_nozzle_volume_type_string(NozzleVolumeType nozzle_volume_type);

static std::string bed_type_to_gcode_string(const BedType type)
{
    std::string type_str;

    switch (type) {
    case btSuperTack:
        type_str = "supertack_plate";
        break;
    case btPC:
        type_str = "cool_plate";
        break;
    case btPCT:
        type_str = "textured_cool_plate";
        break;
    case btEP:
        type_str = "eng_plate";
        break;
    case btPEI:
        type_str = "hot_plate";
        break;
    case btPTE:
        type_str = "textured_plate";
        break;
    default:
        type_str = "unknown";
        break;
    }

    return type_str;
}

static std::string get_bed_temp_key(const BedType type)
{
    if (type == btSuperTack)
        return "supertack_plate_temp";

    if (type == btPC)
        return "cool_plate_temp";

    if (type == btPCT)
        return "textured_cool_plate_temp";

    if (type == btEP)
        return "eng_plate_temp";

    if (type == btPEI)
        return "hot_plate_temp";

    if (type == btPTE)
        return "textured_plate_temp";

    return "";
}

static std::string get_bed_temp_1st_layer_key(const BedType type)
{
    if (type == btSuperTack)
        return "supertack_plate_temp_initial_layer";

    if (type == btPC)
        return "cool_plate_temp_initial_layer";

    if (type == btPCT)
        return "textured_cool_plate_temp_initial_layer";

    if (type == btEP)
        return "eng_plate_temp_initial_layer";

    if (type == btPEI)
        return "hot_plate_temp_initial_layer";

    if (type == btPTE)
        return "textured_plate_temp_initial_layer";

    return "";
}

extern const std::vector<std::string> filament_extruder_override_keys;
// Full override-key check incl. filament_retract_length_nc (defined outside the generator list).
extern bool is_filament_extruder_override_key(const std::string &opt_key);

// for parse extruder_ams_count
extern std::vector<std::map<int, int>> get_extruder_ams_count(const std::vector<std::string> &strs);
extern std::vector<std::string> save_extruder_ams_count_to_string(const std::vector<std::map<int, int>> &extruder_ams_count);

// maps a full extruder variant string (e.g. "Direct Drive High Flow") to its NozzleVolumeType; nvtHybrid if unparsable
extern NozzleVolumeType convert_to_nvt_type(const std::string& variant_str);

// for parse extruder_nozzle_stats (per-extruder physical nozzle inventory by volume type)
extern std::vector<std::map<NozzleVolumeType, int>> get_extruder_nozzle_stats(const std::vector<std::string> &strs);
extern std::vector<std::string> save_extruder_nozzle_stats_to_string(const std::vector<std::map<NozzleVolumeType, int>> &extruder_nozzle_stats);

#define CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(NAME) \
    template<> const t_config_enum_names& ConfigOptionEnum<NAME>::get_enum_names(); \
    template<> const t_config_enum_values& ConfigOptionEnum<NAME>::get_enum_values();

CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(PrinterTechnology)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(GCodeFlavor)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(FuzzySkinType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(FuzzySkinMode)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(TopSurfaceExpansionDirection)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(WipeTowerType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(NoiseType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(InfillPattern)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(IroningType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SlicingMode)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(BeltRotationAxis)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(RemapAxis)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SupportMaterialPattern)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SupportMaterialStyle)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SupportMaterialInterfacePattern)
// BBS
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SupportType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SeamPosition)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SeamScarfType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SLADisplayOrientation)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SLAPillarConnectionMode)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(BrimType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(TimelapseType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(BedType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SkirtType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(InputShaperType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(DraftShield)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(ForwardCompatibilitySubstitutionRule)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(GCodeThumbnailsFormat)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(CounterboreHoleBridgingOption)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(PrintHostType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(AuthorizationType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(WipeTowerWallType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(PerimeterGeneratorType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(ToolChangeOrderingType)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(PowerLossRecoveryMode)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(ArcFittingMode)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SurfaceFillOrder)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(AdaptiveInfillMode)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(AdaptiveInfillGradient)

#undef CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS

// Defines each and every configuration option of Slic3r, including the properties of the GUI dialogs.
// Does not store the actual values, but defines default values.
class PrintConfigDef : public ConfigDef
{
public:
    PrintConfigDef();

    static void handle_legacy(t_config_option_key &opt_key, std::string &value);
    static void handle_legacy_composite(DynamicPrintConfig &config);

    // Array options growing with the number of extruders
    const std::vector<std::string>& extruder_option_keys() const { return m_extruder_option_keys; }
    // Options defining the extruder retract properties. These keys are sorted lexicographically.
    // The extruder retract keys could be overidden by the same values defined at the Filament level
    // (then the key is further prefixed with the "filament_" prefix).
    const std::vector<std::string>& extruder_retract_keys() const { return m_extruder_retract_keys; }

    // BBS
    const std::vector<std::string>& filament_option_keys() const { return m_filament_option_keys; }
    const std::vector<std::string>& filament_retract_keys() const { return m_filament_retract_keys; }

private:
    void init_common_params();
    void init_fff_params();
    void init_extruder_option_keys();
    void init_sla_params();

    std::vector<std::string>    m_extruder_option_keys;
    std::vector<std::string>    m_extruder_retract_keys;

    // BBS
    void init_filament_option_keys();

    std::vector<std::string>    m_filament_option_keys;
    std::vector<std::string>    m_filament_retract_keys;
};

// The one and only global definition of SLic3r configuration options.
// This definition is constant.
extern const PrintConfigDef print_config_def;

class StaticPrintConfig;

// Minimum object distance for arrangement, based on printer technology.
double min_object_distance(const ConfigBase &cfg);

// Whether any value is set, a nil value included.
template<bool NULLABLE> bool any_enabled(const ConfigOptionBoolsTempl<NULLABLE> &option)
{
    return std::any_of(option.values.begin(), option.values.end(), [](unsigned char enabled) { return enabled != 0; });
}

// One (extruder type x nozzle volume type) parameter variant a filament prints through, plus a
// representative physical extruder observed using it. Ordering (and set-dedup identity) covers
// the variant pair only, so the same variant reached through two extruders keeps one config slot.
struct FilamentVariantUse
{
    ExtruderType     extruder_type{etDirectDrive};
    NozzleVolumeType nozzle_volume_type{nvtStandard};
    int              extruder_id{0}; // 0-based, first extruder seen using this variant

    bool operator<(const FilamentVariantUse &other) const
    {
        if (extruder_type != other.extruder_type)
            return extruder_type < other.extruder_type;
        return nozzle_volume_type < other.nozzle_volume_type;
    }
};

// Slic3r dynamic configuration, used to override the configuration
// per object, per modification volume or per printing material.
// The dynamic configuration is also used to store user modifications of the print global parameters,
// so the modified configuration values may be diffed against the active configuration
// to invalidate the proper slicing resp. g-code generation processing steps.
class DynamicPrintConfig : public DynamicConfig
{
public:
    DynamicPrintConfig() {}
    DynamicPrintConfig(const DynamicPrintConfig &rhs) : DynamicConfig(rhs) {}
    DynamicPrintConfig(DynamicPrintConfig &&rhs) noexcept : DynamicConfig(std::move(rhs)) {}
    explicit DynamicPrintConfig(const StaticPrintConfig &rhs);
    explicit DynamicPrintConfig(const ConfigBase &rhs) : DynamicConfig(rhs) {}

    DynamicPrintConfig& operator=(const DynamicPrintConfig &rhs) { DynamicConfig::operator=(rhs); return *this; }
    DynamicPrintConfig& operator=(DynamicPrintConfig &&rhs) noexcept { DynamicConfig::operator=(std::move(rhs)); return *this; }

    static DynamicPrintConfig  full_print_config();
    static DynamicPrintConfig* new_from_defaults_keys(const std::vector<std::string> &keys);

    // Overrides ConfigBase::def(). Static configuration definition. Any value stored into this ConfigBase shall have its definition here.
    const ConfigDef*    def() const override { return &print_config_def; }

    void                normalize_fdm(int used_filaments = 0);
    void                normalize_fdm_1();
    //return the changed param set
    t_config_option_keys normalize_fdm_2(int num_objects, int used_filaments = 0);

    size_t              get_parameter_size(const std::string& param_name, size_t extruder_nums);
    void                set_num_extruders(unsigned int num_extruders);

    // BBS
    void                set_num_filaments(unsigned int num_filaments);

    //BBS
    // Validate the PrintConfig. Returns an empty string on success, otherwise an error message is returned.
    std::map<std::string, std::string>         validate(bool under_cli = false);

    // Verify whether the opt_key has not been obsoleted or renamed.
    // Both opt_key and value may be modified by handle_legacy().
    // If the opt_key is no more valid in this version of Slic3r, opt_key is cleared by handle_legacy().
    // handle_legacy() is called internally by set_deserialize().
    void                handle_legacy(t_config_option_key &opt_key, std::string &value) const override
        { PrintConfigDef::handle_legacy(opt_key, value); }

    // Called after a config is loaded as a whole.
    // Perform composite conversions, for example merging multiple keys into one key.
    // For conversion of single options, the handle_legacy() method above is called.
    void                handle_legacy_composite() override
        { PrintConfigDef::handle_legacy_composite(*this); }

    //BBS special case Support G/ Support W
    std::string get_filament_type(std::string &displayed_filament_type, int id = 0);

    //BBS
    bool is_using_different_extruders();
    bool support_different_extruders(int& extruder_count) const;
    // Whether any filament defines more than one variant (filament_extruder_variant longer than
    // filament_diameter). Its variants then have to be resolved even on a printer with a single
    // extruder variant, which picks the filament's variant of the same variant string.
    bool has_multi_variant_filament() const;
    // Counts the config slots of a printer: one per (extruder x nozzle volume type) as described by
    // extruder_nozzle_stats, or simply one per extruder when the stats are absent/mismatched.
    // Fills nozzle_volume_types with each extruder's volume types in ascending enum order.
    int get_extruder_nozzle_volume_count(int extruder_count, std::vector<std::vector<NozzleVolumeType>>& nozzle_volume_types) const;
    int get_index_for_extruder(int extruder_or_filament_id, std::string id_name, ExtruderType extruder_type, NozzleVolumeType nozzle_volume_type, std::string variant_name, unsigned int stride = 1) const;
    std::vector<int> update_values_to_printer_extruders(DynamicPrintConfig& printer_config, int extruder_count, int extruder_nozzle_volume_count, std::vector<std::vector<NozzleVolumeType>>& nv_types,
        std::set<std::string>& key_set, std::string id_name, std::string variant_name, unsigned int stride = 1, unsigned int extruder_id = 0, NozzleVolumeType filament_nvt = nvtStandard);
    void update_values_to_printer_extruders_for_multiple_filaments(DynamicPrintConfig& printer_config, int extruder_count, int extruder_nozzle_volume_count, std::set<std::string>& key_set, std::string id_name, std::string variant_name);
    // Rebuilds the per-slot filament arrays from a per-layer grouping outcome: a filament that
    // prints through several (extruder x nozzle volume type) variants keeps one slot per variant
    // (unlike the single-slot rebuild above), so layer-aware consumers can resolve the slot the
    // current layer actually prints with. Filaments absent from filament_variant_uses keep a
    // single slot resolved from filament_map / filament_volume_map. When slot_machine_indices is
    // non-null it receives one machine-variant slot index per output slot (the nil-value fallback
    // keying for the extruder retract overrides; a per-filament map cannot index expanded arrays).
    void update_filament_config_values_for_multiple_extruders(DynamicPrintConfig& printer_config,
        const std::unordered_map<int, std::vector<FilamentVariantUse>>& filament_variant_uses,
        int extruder_count, int extruder_nozzle_volume_count,
        std::set<std::string>& key_set, std::string id_name, std::string variant_name,
        std::vector<int>* slot_machine_indices = nullptr);

    void update_non_diff_values_to_base_config(DynamicPrintConfig& new_config, const t_config_option_keys& keys, const std::set<std::string>& different_keys, std::string extruder_id_name, std::string extruder_variant_name,
        std::set<std::string>& key_set1, std::set<std::string>& key_set2);
    void update_diff_values_to_child_config(DynamicPrintConfig& new_config, std::string extruder_id_name, std::string extruder_variant_name, std::set<std::string>& key_set1, std::set<std::string>& key_set2);

    int update_values_from_single_to_multi(DynamicPrintConfig& multi_config, std::set<std::string>& key_set, std::string id_name, std::string variant_name);
    int update_values_from_multi_to_multi(DynamicPrintConfig& new_config, std::set<std::string>& key_set, std::string id_name, std::string variant_name, std::vector<std::string>& extruder_variants);

    //int update_values_from_single_to_multi_2(DynamicPrintConfig& multi_config, std::set<std::string>& key_set);
    //int update_values_from_multi_to_single_2(std::set<std::string>& key_set);

    int update_values_from_multi_to_multi_2(const std::vector<std::string>& src_extruder_variants, const std::vector<std::string>& dst_extruder_variants, const DynamicPrintConfig& dst_config, const std::set<std::string>& key_sets);

public:
    // query filament
    std::string get_filament_vendor() const;
    std::string get_filament_type() const;
};
extern std::set<std::string> printer_extruder_options;
extern std::set<std::string> print_options_with_variant;
extern std::set<std::string> filament_options_with_variant;
extern std::set<std::string> printer_options_with_variant_1;
extern std::set<std::string> printer_options_with_variant_2;
extern std::set<std::string> empty_options;

void set_variant_override(ConfigOptionVectorBase &target, const ConfigOptionVectorBase &source,
                          const std::vector<int> &variant_index, int stride = 1);

// Orca: lays every filament_options_with_variant option out one value per filament variant, as
// filament_self_index maps the variants to filaments. An option holding one value per filament, or a
// single value, gives every variant of a filament that filament's value; other lengths are left alone.
void normalize_filament_values_to_variants(DynamicPrintConfig &config);

extern std::set<std::string> filament_dev_options;

// Orca: a filament_dev_options option holds several values per filament, and how many is up to the
// filament preset, so one filament's values cannot be replaced in place. This rebuilds each option from
// filament_configs, one config per filament in slot order, as the filaments' values one after another.
void set_filament_dev_options(DynamicPrintConfig &config, const std::vector<const DynamicPrintConfig *> &filament_configs);

// Orca: sizes the per-slot mixed-colour metadata options to new_slot_count, keeping the first
// old_slot_count values; an option the config lacks is created.
void resize_mixed_filament_metadata(DynamicPrintConfig &config, size_t old_slot_count, size_t new_slot_count);

extern void update_static_print_config_from_dynamic(ConfigBase& config, const DynamicPrintConfig& dest_config, std::vector<int> variant_index, std::set<std::string>& key_set1, int stride = 1);
extern void compute_filament_override_value(const std::string& opt_key, const ConfigOption *opt_old_machine, const ConfigOption *opt_new_machine, const ConfigOption *opt_new_filament, const DynamicPrintConfig& new_full_config,
    t_config_option_keys& diff_keys, DynamicPrintConfig& filament_overrides, std::vector<int>& f_map_indices);

void handle_legacy_sla(DynamicPrintConfig &config);

class StaticPrintConfig : public StaticConfig
{
public:
    StaticPrintConfig() {}

    // Overrides ConfigBase::def(). Static configuration definition. Any value stored into this ConfigBase shall have its definition here.
    const ConfigDef*    def() const override { return &print_config_def; }
    // Reference to the cached list of keys.
    virtual const t_config_option_keys& keys_ref() const = 0;

protected:
    // Verify whether the opt_key has not been obsoleted or renamed.
    // Both opt_key and value may be modified by handle_legacy().
    // If the opt_key is no more valid in this version of Slic3r, opt_key is cleared by handle_legacy().
    // handle_legacy() is called internally by set_deserialize().
    void                handle_legacy(t_config_option_key &opt_key, std::string &value) const override
        { PrintConfigDef::handle_legacy(opt_key, value); }

    // Internal class for keeping a dynamic map to static options.
    class StaticCacheBase
    {
    public:
        // To be called during the StaticCache setup.
        // Add one ConfigOption into m_map_name_to_offset.
        template<typename T>
        void                opt_add(const std::string &name, const char *base_ptr, const T &opt)
        {
            assert(m_map_name_to_offset.find(name) == m_map_name_to_offset.end());
            m_map_name_to_offset[name] = (const char*)&opt - base_ptr;
        }

    protected:
        std::map<std::string, ptrdiff_t>    m_map_name_to_offset;
    };

    // Parametrized by the type of the topmost class owning the options.
    template<typename T>
    class StaticCache : public StaticCacheBase
    {
    public:
        // Calling the constructor of m_defaults with 0 forces m_defaults to not run the initialization.
        StaticCache() : m_defaults(nullptr) {}
        ~StaticCache() { delete m_defaults; m_defaults = nullptr; }

        bool                initialized() const { return ! m_keys.empty(); }

        ConfigOption*       optptr(const std::string &name, T *owner) const
        {
            const auto it = m_map_name_to_offset.find(name);
            return (it == m_map_name_to_offset.end()) ? nullptr : reinterpret_cast<ConfigOption*>((char*)owner + it->second);
        }

        const ConfigOption* optptr(const std::string &name, const T *owner) const
        {
            const auto it = m_map_name_to_offset.find(name);
            return (it == m_map_name_to_offset.end()) ? nullptr : reinterpret_cast<const ConfigOption*>((const char*)owner + it->second);
        }

        const std::vector<std::string>& keys()      const { return m_keys; }
        const T&                        defaults()  const { return *m_defaults; }

        // To be called during the StaticCache setup.
        // Collect option keys from m_map_name_to_offset,
        // assign default values to m_defaults.
        void                finalize(T *defaults, const ConfigDef *defs)
        {
            assert(defs != nullptr);
            m_defaults = defaults;
            m_keys.clear();
            m_keys.reserve(m_map_name_to_offset.size());
            for (const auto &kvp : defs->options) {
                // Find the option given the option name kvp.first by an offset from (char*)m_defaults.
                ConfigOption *opt = this->optptr(kvp.first, m_defaults);
                if (opt == nullptr)
                    // This option is not defined by the ConfigBase of type T.
                    continue;
                m_keys.emplace_back(kvp.first);
                const ConfigOptionDef *def = defs->get(kvp.first);
                assert(def != nullptr);
                if (def->default_value) {
                    if (def->default_value->type() == coEnums) {
                        // Enum list defaults are built without a keys map; set() adopts the one create_default_option() adds.
                        std::unique_ptr<ConfigOption> default_option(def->create_default_option());
                        opt->set(default_option.get());
                    } else
                        opt->set(def->default_value.get());
                }
            }
        }

    private:
        T                                  *m_defaults;
        std::vector<std::string>            m_keys;
    };
};

// Defined in PrintConfig.hpp.
class PrintConfig;
class GCodeConfig;
class SLAPrinterConfig;

class CLIActionsConfigDef : public ConfigDef
{
public:
    CLIActionsConfigDef();
};

class CLITransformConfigDef : public ConfigDef
{
public:
    CLITransformConfigDef();
};

class CLIMiscConfigDef : public ConfigDef
{
public:
    CLIMiscConfigDef();
};

typedef std::string t_custom_gcode_key;
// This map containes list of specific placeholders for each custom G-code, if any exist
const std::map<t_custom_gcode_key, t_config_option_keys>& custom_gcode_specific_placeholders();

// Next classes define placeholders used by GUI::EditGCodeDialog.

class ReadOnlySlicingStatesConfigDef : public ConfigDef
{
public:
    ReadOnlySlicingStatesConfigDef();
};

class ReadWriteSlicingStatesConfigDef : public ConfigDef
{
public:
    ReadWriteSlicingStatesConfigDef();
};

class OtherSlicingStatesConfigDef : public ConfigDef
{
public:
    OtherSlicingStatesConfigDef();
};

class PrintStatisticsConfigDef : public ConfigDef
{
public:
    PrintStatisticsConfigDef();
};

class ObjectsInfoConfigDef : public ConfigDef
{
public:
    ObjectsInfoConfigDef();
};

class DimensionsConfigDef : public ConfigDef
{
public:
    DimensionsConfigDef();
};

class TemperaturesConfigDef : public ConfigDef
{
public:
    TemperaturesConfigDef();
};

class TimestampsConfigDef : public ConfigDef
{
public:
    TimestampsConfigDef();
};

class OtherPresetsConfigDef : public ConfigDef
{
public:
    OtherPresetsConfigDef();
};

// This classes defines all custom G-code specific placeholders.
class CustomGcodeSpecificConfigDef : public ConfigDef
{
public:
    CustomGcodeSpecificConfigDef();
};
extern const CustomGcodeSpecificConfigDef    custom_gcode_specific_config_def;

// This class defines the command line options representing actions.
extern const CLIActionsConfigDef    cli_actions_config_def;

// This class defines the command line options representing transforms.
extern const CLITransformConfigDef  cli_transform_config_def;

// This class defines all command line options that are not actions or transforms.
extern const CLIMiscConfigDef       cli_misc_config_def;

class DynamicPrintAndCLIConfig : public DynamicPrintConfig
{
public:
    DynamicPrintAndCLIConfig() {}
    DynamicPrintAndCLIConfig(const DynamicPrintAndCLIConfig &other) : DynamicPrintConfig(other) {}

    // Overrides ConfigBase::def(). Static configuration definition. Any value stored into this ConfigBase shall have its definition here.
    const ConfigDef*        def() const override { return &s_def; }

    // Verify whether the opt_key has not been obsoleted or renamed.
    // Both opt_key and value may be modified by handle_legacy().
    // If the opt_key is no more valid in this version of Slic3r, opt_key is cleared by handle_legacy().
    // handle_legacy() is called internally by set_deserialize().
    void                    handle_legacy(t_config_option_key &opt_key, std::string &value) const override;

private:
    class PrintAndCLIConfigDef : public ConfigDef
    {
    public:
        PrintAndCLIConfigDef() {
            this->options.insert(print_config_def.options.begin(), print_config_def.options.end());
            this->options.insert(cli_actions_config_def.options.begin(), cli_actions_config_def.options.end());
            this->options.insert(cli_transform_config_def.options.begin(), cli_transform_config_def.options.end());
            this->options.insert(cli_misc_config_def.options.begin(), cli_misc_config_def.options.end());
            for (const auto &kvp : this->options)
                this->by_serialization_key_ordinal[kvp.second.serialization_key_ordinal] = &kvp.second;
        }
        // Do not release the default values, they are handled by print_config_def & cli_actions_config_def / cli_transform_config_def / cli_misc_config_def.
        ~PrintAndCLIConfigDef() { this->options.clear(); }
    };
    static PrintAndCLIConfigDef s_def;
};

bool is_XL_printer(const DynamicPrintConfig &cfg);
bool is_XL_printer(const PrintConfig &cfg);

// The area every extruder can reach: the whole bed when no extruder has an area of its own, and empty
// when the extruder areas do not overlap.
Polygon get_shared_poly(const std::vector<Pointfs>& extruder_polys, const Pointfs& bed);
// The bed to place objects on. With use_share it is the shared area, or the whole bed when there is none.
Points get_bed_shape(const DynamicPrintConfig &cfg, bool use_share = true);
Points get_bed_shape(const PrintConfig &cfg, bool use_share = false);
Points get_bed_shape(const SLAPrinterConfig &cfg);
struct BedExcludeRegion {
    enum class Purpose {
        MaterialKeepout,
        CollisionVolume
    };

    Polygon polygon;
    double  z_min { 0.0 };
    double  z_max { 0.0 };
    Purpose purpose { Purpose::MaterialKeepout };
    bool    has_z_range { false };

    bool is_collision_volume() const { return purpose == Purpose::CollisionVolume; }
};
std::vector<BedExcludeRegion> get_bed_excluded_regions(const DynamicPrintConfig& cfg);
std::vector<BedExcludeRegion> get_bed_excluded_regions(const PrintConfig& cfg);
std::vector<BedExcludeRegion> get_bed_excluded_regions(const DynamicPrintConfig &cfg, size_t extruder_id);
std::vector<BedExcludeRegion> get_bed_excluded_regions(const PrintConfig &cfg, size_t extruder_id);
std::vector<std::vector<BedExcludeRegion>> get_bed_excluded_regions_by_extruder(const DynamicPrintConfig &cfg);
std::vector<std::vector<BedExcludeRegion>> get_bed_excluded_regions_by_extruder(const PrintConfig &cfg);
// Resolve the nozzle whose exclusion regions apply to a filament. Filament
// and return ids are 0-based. Returns -1 while Bambu automatic grouping has
// no concrete assignment.
int bed_exclusion_extruder_for_filament(size_t filament_id, const std::vector<int> &configured_map,
                                        FilamentMapMode map_mode, bool is_bambu, bool automatic_map_resolved,
                                        size_t extruder_count);
bool is_bed_exclusion_volume_syntax(const std::string &value);
// Bed exclusion areas must be simple polygons with non-zero area.
bool is_valid_bed_exclude_area(const Pointfs &points);
bool is_valid_bed_exclude_volumes_string(const std::string &value, double printable_height);
// Add shared definitions without replacing existing volumes or changing the
// configured multi-nozzle mode. In individual mode the new definitions are
// applied to every physical extruder.
void append_bed_exclude_volumes(DynamicPrintConfig &config, const std::string &definition);
bool has_bed_exclude_volumes(const DynamicPrintConfig &cfg);
bool has_bed_exclude_volumes(const PrintConfig &cfg);
BedExcludeVolumeMode active_bed_exclude_volume_mode(const DynamicPrintConfig &cfg);
BedExcludeVolumeMode active_bed_exclude_volume_mode(const PrintConfig &cfg);
Slic3r::Polygons get_bed_excluded_area(const PrintConfig& cfg);
Slic3r::Polygon get_bed_shape_with_excluded_area(const PrintConfig& cfg, bool use_share = false);
bool has_skirt(const DynamicPrintConfig& cfg);
float get_real_skirt_dist(const DynamicPrintConfig& cfg);

// ModelConfig is a wrapper around DynamicPrintConfig with an addition of a timestamp.
// Each change of ModelConfig is tracked by assigning a new timestamp from a global counter.
// The counter is used for faster synchronization of the background slicing thread
// with the front end by skipping synchronization of equal config dictionaries.
// The global counter is also used for avoiding unnecessary serialization of config
// dictionaries when taking an Undo snapshot.
//
// The global counter is NOT thread safe, therefore it is recommended to use ModelConfig from
// the main thread only.
//
// As there is a global counter and it is being increased with each change to any ModelConfig,
// if two ModelConfig dictionaries differ, they should differ with their timestamp as well.
// Therefore copying the ModelConfig including its timestamp is safe as there is no harm
// in having multiple ModelConfig with equal timestamps as long as their dictionaries are equal.
//
// The timestamp is used by the Undo/Redo stack. As zero timestamp means invalid timestamp
// to the Undo/Redo stack (zero timestamp means the Undo/Redo stack needs to serialize and
// compare serialized data for differences), zero timestamp shall never be used.
// Timestamp==1 shall only be used for empty dictionaries.
class ModelConfig
{
public:
    // Following method clears the config and increases its timestamp, so the deleted
    // state is considered changed from perspective of the undo/redo stack.
    void         reset() { m_data.clear(); touch(); }

    void         assign_config(const ModelConfig &rhs) {
        if (m_timestamp != rhs.m_timestamp) {
            m_data      = rhs.m_data;
            m_timestamp = rhs.m_timestamp;
        }
    }
    void         assign_config(ModelConfig &&rhs) {
        if (m_timestamp != rhs.m_timestamp) {
            m_data      = std::move(rhs.m_data);
            m_timestamp = rhs.m_timestamp;
            rhs.reset();
        }
    }

    // Modification of the ModelConfig is not thread safe due to the global timestamp counter!
    // Don't call modification methods from the back-end!
    // Assign methods don't assign if src==dst to not having to bump the timestamp in case they are equal.
    void         assign_config(const DynamicPrintConfig &rhs)  { if (m_data != rhs) { m_data = rhs; this->touch(); } }
    void         assign_config(DynamicPrintConfig &&rhs)       { if (m_data != rhs) { m_data = std::move(rhs); this->touch(); } }
    void         apply(const ModelConfig &other, bool ignore_nonexistent = false) { this->apply(other.get(), ignore_nonexistent); }
    void         apply(const ConfigBase &other, bool ignore_nonexistent = false) { m_data.apply_only(other, other.keys(), ignore_nonexistent); this->touch(); }
    void         apply_only(const ModelConfig &other, const t_config_option_keys &keys, bool ignore_nonexistent = false) { this->apply_only(other.get(), keys, ignore_nonexistent); }
    void         apply_only(const ConfigBase &other, const t_config_option_keys &keys, bool ignore_nonexistent = false) { m_data.apply_only(other, keys, ignore_nonexistent); this->touch(); }
    bool         set_key_value(const std::string &opt_key, ConfigOption *opt) { bool out = m_data.set_key_value(opt_key, opt); this->touch(); return out; }
    template<typename T>
    void         set(const std::string &opt_key, T value) { m_data.set(opt_key, value, true); this->touch(); }
    void         set_deserialize(const t_config_option_key &opt_key, const std::string &str, ConfigSubstitutionContext &substitution_context, bool append = false)
        { m_data.set_deserialize(opt_key, str, substitution_context, append); this->touch(); }
    bool         erase(const t_config_option_key &opt_key) { bool out = m_data.erase(opt_key); if (out) this->touch(); return out; }

    // Getters are thread safe.
    // The following implicit conversion breaks the Cereal serialization.
//    operator const DynamicPrintConfig&() const throw() { return this->get(); }
    const DynamicPrintConfig&   get() const throw() { return m_data; }
    bool                        empty() const throw() { return m_data.empty(); }
    size_t                      size() const throw() { return m_data.size(); }
    auto                        cbegin() const { return m_data.cbegin(); }
    auto                        cend() const { return m_data.cend(); }
    t_config_option_keys        keys() const { return m_data.keys(); }
    bool                        has(const t_config_option_key &opt_key) const { return m_data.has(opt_key); }
    const ConfigOption*         option(const t_config_option_key &opt_key) const { return m_data.option(opt_key); }
    int                         opt_int(const t_config_option_key &opt_key) const { return m_data.opt_int(opt_key); }
    int                         extruder() const { return opt_int("extruder"); }
    double opt_float(const t_config_option_key &opt_key) const {
      return m_data.opt_float(opt_key);
    }
    double get_abs_value(const t_config_option_key &opt_key) const {
      return m_data.get_abs_value(opt_key);
    }
    std::string                 opt_serialize(const t_config_option_key &opt_key) const { return m_data.opt_serialize(opt_key); }

    // Return an optional timestamp of this object.
    // If the timestamp returned is non-zero, then the serialization framework will
    // only save this object on the Undo/Redo stack if the timestamp is different
    // from the timestmap of the object at the top of the Undo / Redo stack.
    virtual uint64_t    timestamp() const throw() { return m_timestamp; }
    bool                timestamp_matches(const ModelConfig &rhs) const throw() { return m_timestamp == rhs.m_timestamp; }
    // Not thread safe! Should not be called from other than the main thread!
    void                touch() { m_timestamp = ++ s_last_timestamp; }

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive& ar) { ar(m_timestamp); ar(m_data); }

    uint64_t                    m_timestamp { 1 };
    DynamicPrintConfig          m_data;

    static uint64_t             s_last_timestamp;
};

// const std::vector<double> &fv_matrix:  origin matrix from json
// size_t extruder_id: -1 means single-nozzle for old file, 0 means the 1st extruder, 1 means the 2nd extruder
template<class T>
static std::vector<T> get_flush_volumes_matrix(const std::vector<T> &fv_matrix, size_t extruder_id = -1, size_t nozzle_nums = 1)
{
    if (extruder_id != -1 && nozzle_nums != 1) {
        return std::vector<T>(fv_matrix.begin() + size_t(fv_matrix.size() / nozzle_nums * extruder_id + EPSILON),
                                   fv_matrix.begin() + size_t(fv_matrix.size() / nozzle_nums * (extruder_id + 1) + EPSILON));
    }
    return fv_matrix;
}

// std::vector<double> &out_matrix:
// const std::vector<double> &fv_matrix: the matrix of one nozzle
// size_t extruder_id: -1 means single-nozzle for old file, 0 means the 1st extruder, 1 means the 2nd extruder
template<class T>
static void set_flush_volumes_matrix(std::vector<T> &out_matrix, const std::vector<T> &fv_matrix, size_t extruder_id = -1, size_t nozzle_nums = 1)
{
    bool is_multi_extruder = false;
    if (extruder_id != -1 && nozzle_nums != 1) {
        std::copy(fv_matrix.begin(), fv_matrix.end(), out_matrix.begin() + size_t(out_matrix.size() / nozzle_nums * extruder_id + EPSILON));
    }
    else {
        out_matrix = std::vector<T>(fv_matrix.begin(), fv_matrix.end());
    }
}

template<class T>
static bool has_zero_flush_volume_for_used_filaments(const std::vector<T> &fv_matrix,
                                                      const std::vector<T> &flush_multipliers,
                                                      const std::vector<int> &used_filaments)
{
    if (used_filaments.size() < 2 || flush_multipliers.empty())
        return false;

    if (fv_matrix.size() % flush_multipliers.size() != 0)
        return false;

    const size_t matrix_len = fv_matrix.size() / flush_multipliers.size();
    const size_t row_len    = size_t(std::sqrt(double(matrix_len)));
    if (row_len < 2 || row_len * row_len != matrix_len)
        return false;

    std::vector<int> filtered_filaments;
    filtered_filaments.reserve(used_filaments.size());
    for (int filament_id : used_filaments) {
        if (filament_id <= 0 || filament_id > int(row_len))
            continue;
        if (std::find(filtered_filaments.begin(), filtered_filaments.end(), filament_id) == filtered_filaments.end())
            filtered_filaments.push_back(filament_id);
    }
    if (filtered_filaments.size() < 2)
        return false;

    for (T multiplier : flush_multipliers) {
        if (multiplier == 0)
            return true;
    }

    for (size_t nozzle_idx = 0; nozzle_idx < flush_multipliers.size(); nozzle_idx++) {
        const size_t block_offset = nozzle_idx * matrix_len;
        for (int from_id : filtered_filaments) {
            for (int to_id : filtered_filaments) {
                if (from_id == to_id)
                    continue;

                const size_t matrix_idx = block_offset + size_t(from_id - 1) * row_len + size_t(to_id - 1);
                if (matrix_idx < fv_matrix.size() && fv_matrix[matrix_idx] == 0)
                    return true;
            }
        }
    }

    return false;
}

size_t get_extruder_index(const GCodeConfig& config, unsigned int filament_id);

double nozzle_diameter_for_filament(const PrintConfig& config, int filament_id, bool is_bbl_printer);

} // namespace Slic3r

// Serialization through the Cereal library
namespace cereal {
    // Let cereal know that there are load / save non-member functions declared for DynamicPrintConfig, ignore serialize / load / save from parent class DynamicConfig.
    template <class Archive> struct specialize<Archive, Slic3r::DynamicPrintConfig, cereal::specialization::non_member_load_save> {};

    template<class Archive> void load(Archive& archive, Slic3r::DynamicPrintConfig &config)
    {
        size_t cnt;
        archive(cnt);
        config.clear();
        for (size_t i = 0; i < cnt; ++ i) {
            size_t serialization_key_ordinal;
            archive(serialization_key_ordinal);
            assert(serialization_key_ordinal > 0);
            auto it = Slic3r::print_config_def.by_serialization_key_ordinal.find(serialization_key_ordinal);
            if (it == Slic3r::print_config_def.by_serialization_key_ordinal.end())
                throw std::runtime_error("VendorCache: unknown serialization_key_ordinal " + std::to_string(serialization_key_ordinal) + " - cache is stale");
            config.set_key_value(it->second->opt_key, it->second->load_option_from_archive(archive));
        }
    }

    template<class Archive> void save(Archive& archive, const Slic3r::DynamicPrintConfig &config)
    {
        size_t cnt = config.size();
        archive(cnt);
        for (auto it = config.cbegin(); it != config.cend(); ++it) {
            const Slic3r::ConfigOptionDef* optdef = Slic3r::print_config_def.get(it->first);
            assert(optdef != nullptr);
            assert(optdef->serialization_key_ordinal > 0);
            archive(optdef->serialization_key_ordinal);
            optdef->save_option_to_archive(archive, it->second.get());
        }
    }
}


#endif // slic3r_PrintConfigBase_hpp_
