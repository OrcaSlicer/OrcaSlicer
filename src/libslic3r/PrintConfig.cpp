#include "PrintConfig.hpp"
#include "PrintConfigConstants.hpp"
#include "ClipperUtils.hpp"
#include "Config.hpp"
#include "MaterialType.hpp"
#include "I18N.hpp"
#include "format.hpp"

#include "GCode/Thumbnails.hpp"
#include <set>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/thread.hpp>
#include <float.h>

namespace {
std::set<std::string> SplitStringAndRemoveDuplicateElement(const std::string &str, const std::string &separator)
{
    std::set<std::string> result;
    if (str.empty()) return result;

    std::string strs = str + separator;
    size_t      pos;
    size_t      size = strs.size();

    for (int i = 0; i < size; ++i) {
        pos = strs.find(separator, i);
        if (pos < size) {
            std::string sub_str = strs.substr(i, pos - i);
            result.insert(sub_str);
            i = pos + separator.size() - 1;
        }
    }

    return result;
}

void ReplaceString(std::string &resource_str, const std::string &old_str, const std::string &new_str)
{
    std::string::size_type pos = 0;
    while ((pos = resource_str.find(old_str, pos)) != std::string::npos) {
        resource_str.replace(pos, old_str.length(), new_str);
        pos += new_str.length(); //advance position to continue after replacement
    }
}
}

namespace Slic3r {

//! macro used to mark string used at localization,
//! return same string
#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

// Filament types are defined in MaterialType.


const std::vector<std::string> filament_extruder_override_keys = {
    // floats
    "filament_retraction_length",
    "filament_z_hop",
    "filament_z_hop_types",
    "filament_retract_lift_above",  //not in filament_options_with_variant, not used?
    "filament_retract_lift_below",  //not in filament_options_with_variant, not used?
    "filament_retract_lift_enforce",
    "filament_retraction_speed",
    "filament_deretraction_speed",
    "filament_retract_restart_extra",  //not in filament_options_with_variant, added on 20250816
    "filament_retraction_minimum_travel",
    // BBS: floats
    "filament_wipe_distance",
    // bools
    "filament_retract_when_changing_layer",
    "filament_wipe",
    // percents
    "filament_retract_before_wipe",
    // Orca
    "filament_retract_after_wipe",
    // BBS
    "filament_long_retractions_when_cut",
    "filament_retraction_distances_when_cut"
};

// Some filament override parameters are generated from filament_extruder_override_keys,
// while filament_retract_length_nc is defined separately. Keep the generator list
// unchanged and use this helper for behavior checks that need the full override set.
bool is_filament_extruder_override_key(const std::string &opt_key)
{
    return std::find(filament_extruder_override_keys.begin(), filament_extruder_override_keys.end(), opt_key) != filament_extruder_override_keys.end() ||
           opt_key == "filament_retract_length_nc";
}

size_t get_extruder_index(const GCodeConfig& config, unsigned int filament_id)
{
    if (filament_id < config.filament_map.size()) {
        return config.filament_map.get_at(filament_id)-1;
    }
    return 0;
}


// Orca: input shaping values types by flavor
std::vector<std::string> get_shaper_type_values_for_flavor(GCodeFlavor flavor)
{
    switch (flavor) {
    case GCodeFlavor::gcfKlipper:
        return {"Default", "MZV", "ZV", "ZVD", "EI", "2HUMP_EI", "3HUMP_EI"};
    case GCodeFlavor::gcfRepRapFirmware:
        return {"Default", "MZV", "ZV", "ZVD", "ZVDD", "ZVDDD", "EI2", "EI3", "DAA"};
    case GCodeFlavor::gcfMarlinFirmware:
        return {"ZV"};
    case GCodeFlavor::gcfMarlinLegacy:
        return {};
    default:
        break;
    }
    return {"Default"};
}

static t_config_enum_names enum_names_from_keys_map(const t_config_enum_values &enum_keys_map)
{
    t_config_enum_names names;
    int cnt = 0;
    for (const auto& kvp : enum_keys_map)
        cnt = std::max(cnt, kvp.second);
    cnt += 1;
    names.assign(cnt, "");
    for (const auto& kvp : enum_keys_map)
        names[kvp.second] = kvp.first;
    return names;
}

#define CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(NAME)     static t_config_enum_names s_keys_names_##NAME = enum_names_from_keys_map(s_keys_map_##NAME);     template<> const t_config_enum_values& ConfigOptionEnum<NAME>::get_enum_values() { return s_keys_map_##NAME; }     template<> const t_config_enum_names& ConfigOptionEnum<NAME>::get_enum_names() { return s_keys_names_##NAME; }

static t_config_enum_values s_keys_map_PrinterTechnology {
    { "FFF",            ptFFF },
    { "SLA",            ptSLA }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PrinterTechnology)

static t_config_enum_values s_keys_map_PrintHostType {
    { "prusalink",      htPrusaLink },
    { "prusaconnect",   htPrusaConnect },
    { "octoprint",      htOctoPrint },
    { "crealityprint",  htCrealityPrint },
    { "duet",           htDuet },
    { "flashair",       htFlashAir },
    { "astrobox",       htAstroBox },
    { "repetier",       htRepetier },
    { "mks",            htMKS },
    { "esp3d",          htESP3D },
    { "obico",          htObico },
    { "flashforge",     htFlashforge },
    { "simplyprint",    htSimplyPrint },
    { "elegoolink",     htElegooLink },
    { "3dprinteros",    ht3DPrinterOS },
    { "moonraker",      htMoonraker }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PrintHostType)

static t_config_enum_values s_keys_map_AuthorizationType {
    { "key",            atKeyPassword },
    { "user",           atUserPassword }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(AuthorizationType)

static t_config_enum_values s_keys_map_GCodeFlavor {
    { "marlin",         gcfMarlinLegacy },
    { "klipper",        gcfKlipper },
    { "reprapfirmware", gcfRepRapFirmware },
    { "repetier",       gcfRepetier },
    { "marlin2",        gcfMarlinFirmware },
    { "reprap",         gcfRepRapSprinter },
    { "teacup",         gcfTeacup },
    { "makerware",      gcfMakerWare },
    { "sailfish",       gcfSailfish },
    { "smoothie",       gcfSmoothie },
    { "mach3",          gcfMach3 },
    { "machinekit",     gcfMachinekit },
    { "no-extrusion",   gcfNoExtrusion }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(GCodeFlavor)

static t_config_enum_values s_keys_map_BedTempFormula {
    { "by_first_filament",int(BedTempFormula::btfFirstFilament) },
    { "by_highest_temp", int(BedTempFormula::btfHighestTemp)}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(BedTempFormula)

// Orca
static t_config_enum_values s_keys_map_PowerLossRecoveryMode {
    { "printer_configuration", int(PowerLossRecoveryMode::PrinterConfiguration) },
    { "enable",                 int(PowerLossRecoveryMode::Enable) },
    { "disable",                int(PowerLossRecoveryMode::Disable) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PowerLossRecoveryMode)

static t_config_enum_values s_keys_map_CenterOfSurfacePattern{
    {"each_surface", int(CenterOfSurfacePattern::Each_Surface)},
    {"each_model", int(CenterOfSurfacePattern::Each_Model)},
    {"each_assembly", int(CenterOfSurfacePattern::Each_Assembly)}};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(CenterOfSurfacePattern)

static t_config_enum_values s_keys_map_FuzzySkinType {
    { "none",           int(FuzzySkinType::None) },
    { "external",       int(FuzzySkinType::External) },
    { "hole",           int(FuzzySkinType::Hole) },
    { "all",            int(FuzzySkinType::All) },
    { "allwalls",       int(FuzzySkinType::AllWalls)},
    { "disabled_fuzzy", int(FuzzySkinType::Disabled_fuzzy)}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(FuzzySkinType)

static t_config_enum_values s_keys_map_NoiseType {
    { "classic",        int(NoiseType::Classic) },
    { "perlin",         int(NoiseType::Perlin) },
    { "billow",         int(NoiseType::Billow) },
    { "ridgedmulti",    int(NoiseType::RidgedMulti) },
    { "voronoi",        int(NoiseType::Voronoi) },
    { "ripple",         int(NoiseType::Ripple) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(NoiseType)

static t_config_enum_values s_keys_map_WipeTowerType {
    { "type1",          int(WipeTowerType::Type1) },
    { "type2",          int(WipeTowerType::Type2) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(WipeTowerType)

static t_config_enum_values s_keys_map_FuzzySkinMode {
    { "displacement",   int(FuzzySkinMode::Displacement) },
    { "extrusion",      int(FuzzySkinMode::Extrusion) },
    { "combined",       int(FuzzySkinMode::Combined)}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(FuzzySkinMode)

static t_config_enum_values s_keys_map_TopSurfaceExpansionDirection {
    { "inward_and_outward", int(TopSurfaceExpansionDirection::InwardAndOutward) },
    { "inward",             int(TopSurfaceExpansionDirection::Inward) },
    { "outward",            int(TopSurfaceExpansionDirection::Outward) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(TopSurfaceExpansionDirection)

static t_config_enum_values s_keys_map_InfillPattern {
    { "monotonic", ipMonotonic },
    { "monotonicline", ipMonotonicLine },
    { "rectilinear", ipRectilinear },
    { "alignedrectilinear", ipAlignedRectilinear },
    { "zigzag", ipZigZag },
    { "crosszag", ipCrossZag },
    { "lockedzag", ipLockedZag },
    { "line", ipLine },
    { "grid", ipGrid },
    { "triangles", ipTriangles },
    { "tri-hexagon", ipStars },
    { "cubic", ipCubic },
    { "adaptivecubic", ipAdaptiveCubic },
    { "quartercubic", ipQuarterCubic },
    { "supportcubic", ipSupportCubic },
    { "lightning", ipLightning },
    { "honeycomb", ipHoneycomb },
    { "3dhoneycomb", ip3DHoneycomb },
    { "lateral-honeycomb", ipLateralHoneycomb },
    { "lateral-lattice", ipLateralLattice },
    { "crosshatch", ipCrossHatch },
    { "tpmsd", ipTpmsD },
    { "tpmsfk", ipTpmsFK },
    { "gyroid", ipGyroid },
    { "concentric", ipConcentric },
    { "hilbertcurve", ipHilbertCurve },
    { "archimedeanchords", ipArchimedeanChords },
    { "octagramspiral", ipOctagramSpiral }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(InfillPattern)

static t_config_enum_values s_keys_map_IroningType {
    { "no ironing",     int(IroningType::NoIroning) },
    { "top",            int(IroningType::TopSurfaces) },
    { "topmost",        int(IroningType::TopmostOnly) },
    { "solid",          int(IroningType::AllSolid) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(IroningType)

//BBS
static t_config_enum_values s_keys_map_WallInfillOrder {
    { "inner wall/outer wall/infill",     int(WallInfillOrder::InnerOuterInfill) },
    { "outer wall/inner wall/infill",     int(WallInfillOrder::OuterInnerInfill) },
    { "inner-outer-inner wall/infill",     int(WallInfillOrder::InnerOuterInnerInfill) },
    { "infill/inner wall/outer wall",     int(WallInfillOrder::InfillInnerOuter) },
    { "infill/outer wall/inner wall",     int(WallInfillOrder::InfillOuterInner) },
    { "inner-outer-inner wall/infill",     int(WallInfillOrder::InnerOuterInnerInfill)}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(WallInfillOrder)

//BBS
static t_config_enum_values s_keys_map_WallSequence {
    { "inner wall/outer wall",     int(WallSequence::InnerOuter) },
    { "outer wall/inner wall",     int(WallSequence::OuterInner) },
    { "inner-outer-inner wall",    int(WallSequence::InnerOuterInner)}

};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(WallSequence)

//Orca
static t_config_enum_values s_keys_map_WallDirection{
    { "ccw",  int(WallDirection::CounterClockwise) },
    { "cw",   int(WallDirection::Clockwise)},
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(WallDirection)

//Orca
static t_config_enum_values s_keys_map_SurfaceFillOrder{
    { "default",  int(SurfaceFillOrder::Default) },
    { "outward",  int(SurfaceFillOrder::Outward) },
    { "inward",   int(SurfaceFillOrder::Inward) },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SurfaceFillOrder)

//BBS
static t_config_enum_values s_keys_map_PrintSequence {
    { "by layer",     int(PrintSequence::ByLayer) },
    { "by object",    int(PrintSequence::ByObject) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PrintSequence)

static t_config_enum_values s_keys_map_PrintOrder{
    { "default",     int(PrintOrder::Default) },
    { "as_obj_list", int(PrintOrder::AsObjectList)},
    { "best_of",       int(PrintOrder::BestOfStrategies)},
    { "snake", int(PrintOrder::Snake)},
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PrintOrder)

static t_config_enum_values s_keys_map_SlicingMode {
    { "regular",        int(SlicingMode::Regular) },
    { "even_odd",       int(SlicingMode::EvenOdd) },
    { "close_holes",    int(SlicingMode::CloseHoles) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SlicingMode)

static t_config_enum_values s_keys_map_SupportMaterialPattern {
    { "rectilinear",        smpRectilinear },
    { "rectilinear-grid",   smpRectilinearGrid },
    { "honeycomb",          smpHoneycomb },
    { "lightning",          smpLightning },
    { "default",            smpDefault},
    { "hollow",               smpNone},
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SupportMaterialPattern)

static t_config_enum_values s_keys_map_SupportMaterialStyle {
    { "default",        smsDefault },
    { "grid",           smsGrid },
    { "snug",           smsSnug },
    { "organic",        smsTreeOrganic },
    { "tree_slim",      smsTreeSlim },
    { "tree_strong",    smsTreeStrong },
    { "tree_hybrid",    smsTreeHybrid }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SupportMaterialStyle)

static t_config_enum_values s_keys_map_SupportMaterialInterfacePattern {
    { "auto",           smipAuto },
    { "rectilinear",    smipRectilinear },
    { "concentric",     smipConcentric },
    { "rectilinear_interlaced", smipRectilinearInterlaced},
    { "grid",           smipGrid }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SupportMaterialInterfacePattern)

static t_config_enum_values s_keys_map_SupportType{
    { "normal(auto)",   stNormalAuto },
    { "tree(auto)", stTreeAuto },
    { "normal(manual)", stNormal },
    { "tree(manual)", stTree }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SupportType)

static t_config_enum_values s_keys_map_SeamPosition {
    { "nearest",        spNearest },
    { "aligned",        spAligned },
    { "aligned_back",   spAlignedBack },
    { "back",           spRear },
    { "random",         spRandom }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SeamPosition)

// Orca
static t_config_enum_values s_keys_map_SeamScarfType{
    { "none",           int(SeamScarfType::None) },
    { "external",       int(SeamScarfType::External) },
    { "all",            int(SeamScarfType::All) },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SeamScarfType)

// Orca
static t_config_enum_values s_keys_map_EnsureVerticalShellThickness{
    { "none",           int(EnsureVerticalShellThickness::evstNone) },
    { "ensure_critical_only",         int(EnsureVerticalShellThickness::evstCriticalOnly) },
    { "ensure_moderate",            int(EnsureVerticalShellThickness::evstModerate) },
    { "ensure_all",         int(EnsureVerticalShellThickness::evstAll) },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(EnsureVerticalShellThickness)

// Orca
static t_config_enum_values s_keys_map_InternalBridgeFilter {
    { "disabled",        ibfDisabled },
    { "limited",        ibfLimited },
    { "nofilter",           ibfNofilter },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(InternalBridgeFilter)

static t_config_enum_values s_keys_map_EnableExtraBridgeLayer {
    { "disabled",        eblDisabled },
    { "external_bridge_only",        eblExternalBridgeOnly },
    { "internal_bridge_only",        eblInternalBridgeOnly },
    { "apply_to_all",           eblApplyToAll },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(EnableExtraBridgeLayer)

// Orca
static t_config_enum_values s_keys_map_GapFillTarget {
    { "everywhere",        gftEverywhere },
    { "topbottom",        gftTopBottom },
    { "nowhere",           gftNowhere },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(GapFillTarget)

static const t_config_enum_values s_keys_map_SLADisplayOrientation = {
    { "landscape",      sladoLandscape},
    { "portrait",       sladoPortrait}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SLADisplayOrientation)

static const t_config_enum_values s_keys_map_SLAPillarConnectionMode = {
    {"zigzag",          slapcmZigZag},
    {"cross",           slapcmCross},
    {"dynamic",         slapcmDynamic}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SLAPillarConnectionMode)

static const t_config_enum_values s_keys_map_SLAMaterialSpeed = {
    {"slow", slamsSlow},
    {"fast", slamsFast}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SLAMaterialSpeed);

static const t_config_enum_values s_keys_map_BrimType = {
    {"no_brim",         btNoBrim},
    {"outer_only",      btOuterOnly},
    {"inner_only",      btInnerOnly},
    {"outer_and_inner", btOuterAndInner},
    {"auto_brim", btAutoBrim},  // BBS
    {"brim_ears", btEar},     // Orca
    {"painted", btPainted},  // BBS
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(BrimType)

// using 0,1 to compatible with old files
static const t_config_enum_values s_keys_map_TimelapseType = {
    {"0",       tlTraditional},
    {"1",       tlSmooth}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(TimelapseType)

static const t_config_enum_values s_keys_map_SkirtType = {
    { "combined", stCombined },
    { "perobject", stPerObject }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SkirtType)

static const t_config_enum_values s_keys_map_DraftShield = {
    { "disabled", dsDisabled },
    { "enabled",  dsEnabled  }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(DraftShield)

static const t_config_enum_values s_keys_map_ForwardCompatibilitySubstitutionRule = {
    { "disable",        ForwardCompatibilitySubstitutionRule::Disable },
    { "enable",         ForwardCompatibilitySubstitutionRule::Enable },
    { "enable_silent",  ForwardCompatibilitySubstitutionRule::EnableSilent }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(ForwardCompatibilitySubstitutionRule)

static const t_config_enum_values s_keys_map_OverhangFanThreshold = {
    { "0%",         Overhang_threshold_none },
    { "10%",        Overhang_threshold_1_4  },
    { "25%",        Overhang_threshold_2_4  },
    { "50%",        Overhang_threshold_3_4  },
    { "75%",        Overhang_threshold_4_4  },
    { "95%",        Overhang_threshold_bridge  }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(OverhangFanThreshold)

// BBS
static const t_config_enum_values s_keys_map_BedType = {
    { "Default Plate",      btDefault },
    { "Supertack Plate",    btSuperTack },
    { "Cool Plate",         btPC },
    { "Engineering Plate",  btEP  },
    { "High Temp Plate",    btPEI  },
    { "Textured PEI Plate", btPTE },
    { "Textured Cool Plate", btPCT }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(BedType)

// BBS
static const t_config_enum_values s_keys_map_LayerSeq = {
    { "Auto",              flsAuto },
    { "Customize",         flsCustomize },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(LayerSeq)

static t_config_enum_values s_keys_map_NozzleType {
    { "undefine",       int(NozzleType::ntUndefine) },
    { "hardened_steel", int(NozzleType::ntHardenedSteel) },
    { "stainless_steel", int(NozzleType::ntStainlessSteel)},
    { "tungsten_carbide", int(NozzleType::ntTungstenCarbide)},
    { "brass",          int(NozzleType::ntBrass) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(NozzleType)

static t_config_enum_values s_keys_map_FanDirection {
    { "undefine",       int(FanDirection::fdUndefine) },
    { "left",           int(FanDirection::fdLeft) },
    { "right",          int(FanDirection::fdRight) },
    { "both",           int(FanDirection::fdBoth) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(FanDirection)

static t_config_enum_values s_keys_map_PrinterStructure {
    {"undefine",        int(PrinterStructure::psUndefine)},
    {"corexy",          int(PrinterStructure::psCoreXY)},
    {"i3",              int(PrinterStructure::psI3)},
    {"hbot",            int(PrinterStructure::psHbot)},
    {"delta",           int(PrinterStructure::psDelta)}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PrinterStructure)

static t_config_enum_values s_keys_map_InputShaperType {
    {"Default", int(InputShaperType::Default)},
    {"MZV",     int(InputShaperType::MZV)},
    {"ZV",      int(InputShaperType::ZV)},
    {"ZVD",     int(InputShaperType::ZVD)},
    {"ZVDD",    int(InputShaperType::ZVDD)},
    {"ZVDDD",   int(InputShaperType::ZVDDD)},
    {"EI",     int(InputShaperType::EI)},
    {"EI2",     int(InputShaperType::EI2)},
    {"2HUMP_EI",int(InputShaperType::TwoHumpEI)},
    {"EI3",     int(InputShaperType::EI3)},
    {"3HUMP_EI",int(InputShaperType::ThreeHumpEI)},
    {"DAA",     int(InputShaperType::DAA)},
    {"Disable", int(InputShaperType::Disable)}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(InputShaperType)

static t_config_enum_values s_keys_map_PerimeterGeneratorType{
    { "classic", int(PerimeterGeneratorType::Classic) },
    { "arachne", int(PerimeterGeneratorType::Arachne) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PerimeterGeneratorType)

static t_config_enum_values s_keys_map_ToolChangeOrderingType {
    { "default", int(ToolChangeOrderingType::Default) },
    { "cyclic",  int(ToolChangeOrderingType::Cyclic) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(ToolChangeOrderingType)

static const t_config_enum_values s_keys_map_ZHopType = {
    { "Auto Lift",          zhtAuto },
    { "Normal Lift",        zhtNormal },
    { "Slope Lift",         zhtSlope },
    { "Spiral Lift",        zhtSpiral }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(ZHopType)

static const t_config_enum_values s_keys_map_RetractLiftEnforceType = {
    {"All Surfaces",        rletAllSurfaces},
    {"Top Only",         rletTopOnly},
    {"Bottom Only",      rletBottomOnly},
    {"Top and Bottom",      rletTopAndBottom}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(RetractLiftEnforceType)

static const t_config_enum_values  s_keys_map_GCodeThumbnailsFormat = {
    { "PNG", int(GCodeThumbnailsFormat::PNG) },
    { "JPG", int(GCodeThumbnailsFormat::JPG) },
    { "QOI", int(GCodeThumbnailsFormat::QOI) },
    { "BTT_TFT", int(GCodeThumbnailsFormat::BTT_TFT) },
    { "COLPIC", int(GCodeThumbnailsFormat::ColPic) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(GCodeThumbnailsFormat)

static const t_config_enum_values s_keys_map_CounterboreHoleBridgingOption{
    { "none", chbNone },
    { "partiallybridge", chbBridges },
    { "sacrificiallayer", chbFilled },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(CounterboreHoleBridgingOption)

static const t_config_enum_values s_keys_map_WipeTowerWallType{
    {"rectangle", wtwRectangle},
    {"cone", wtwCone},
    {"rib", wtwRib},
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(WipeTowerWallType)

static const t_config_enum_values s_keys_map_ExtruderType = {
    { "Direct Drive",   etDirectDrive },
    { "Bowden",        etBowden }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(ExtruderType)

static const t_config_enum_values s_keys_map_NozzleVolumeType = {
    { "Standard",  nvtStandard },
    { "High Flow", nvtHighFlow },
    { "TPU High Flow", nvtTPUHighFlow },
    { "Hybrid", nvtHybrid }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(NozzleVolumeType)

static const t_config_enum_values s_keys_map_FilamentMapMode = {
    { "Auto For Flush", fmmAutoForFlush },
    { "Auto For Match", fmmAutoForMatch },
    { "Manual", fmmManual },
    { "Nozzle Manual", fmmNozzleManual }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(FilamentMapMode)

// PrimeVolumeMode. Serialized string keys must stay stable; they round-trip through .3mf.
static const t_config_enum_values s_keys_map_PrimeVolumeMode = {
    { "Default", pvmDefault },
    { "Saving",  pvmSaving },
    { "Fast",    pvmFast }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PrimeVolumeMode)


//BBS
std::string get_extruder_variant_string(ExtruderType extruder_type, NozzleVolumeType nozzle_volume_type)
{
    std::string variant_string;

    if (extruder_type > etMaxExtruderType) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", unsupported ExtruderType=%1%")%extruder_type;
        //extruder_type = etDirectDrive;
        return variant_string;
    }
    auto nozzle_volume_types = get_valid_nozzle_volume_type();
    if (nozzle_volume_types.count(nozzle_volume_type) == 0) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", unsupported NozzleVolumeType=%1%")%nozzle_volume_type;
        //extruder_type = etDirectDrive;
        return variant_string;
    }
    variant_string = s_keys_names_ExtruderType[extruder_type];
    variant_string+= " ";
    variant_string+= s_keys_names_NozzleVolumeType[nozzle_volume_type];
    return variant_string;
}

int get_config_index_base(NozzleVolumeType volume_type, ExtruderType extruder_type, int variant_id_1based, const std::vector<std::string>& variant_list, const std::vector<int>& variant_ids_1based)
{
    assert(variant_list.size() == variant_ids_1based.size());
    std::string extruder_variant = get_extruder_variant_string(extruder_type, volume_type);
    for (int index = 0; index < int(variant_list.size()); ++index) {
        if (extruder_variant == variant_list[index] && variant_ids_1based[index] == variant_id_1based) { return index; }
    }
    return 0;
}

std::string get_nozzle_volume_type_string(NozzleVolumeType nozzle_volume_type)
{
    if (nozzle_volume_type > nvtMaxNozzleVolumeType) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", unsupported NozzleVolumeType=%1%") % nozzle_volume_type;
        return "";
    }
    return s_keys_names_NozzleVolumeType[nozzle_volume_type];
}

std::vector<std::map<int, int>> get_extruder_ams_count(const std::vector<std::string>& strs)
{
    std::vector<std::map<int, int>> extruder_ams_counts;
    for (const std::string& str : strs) {
        std::map<int, int> ams_count_info;
        if (str.empty()) {
            extruder_ams_counts.emplace_back(ams_count_info);
            continue;
        }
        std::vector<std::string> ams_infos;
        boost::algorithm::split(ams_infos, str, boost::algorithm::is_any_of("|"));
        for (const std::string& ams_info : ams_infos) {
            std::vector<std::string> numbers;
            boost::algorithm::split(numbers, ams_info, boost::algorithm::is_any_of("#"));
            assert(numbers.size() == 2);
            ams_count_info.insert(std::make_pair(stoi(numbers[0]), stoi(numbers[1])));
        }
        extruder_ams_counts.emplace_back(ams_count_info);
    }
    return extruder_ams_counts;
}

std::vector<std::string> save_extruder_ams_count_to_string(const std::vector<std::map<int, int>> &extruder_ams_count)
{
    std::vector<std::string> extruder_ams_count_str;
    for (size_t i = 0; i < extruder_ams_count.size(); ++i) {
        std::ostringstream oss;
        const auto &item = extruder_ams_count[i];
        for (auto it = item.begin(); it != item.end(); ++it) {
            oss << it->first << "#" << it->second;
            if (std::next(it) != item.end()) {
                oss << "|";
            }
        }
        extruder_ams_count_str.push_back(oss.str());
    }
    return extruder_ams_count_str;
}

// Maps a full extruder variant string such as "Direct Drive High Flow" to its
// NozzleVolumeType: the extruder-type prefix is stripped, the remainder is trimmed
// and looked up as a volume-type name. Returns nvtHybrid when the string cannot
// be parsed.
NozzleVolumeType convert_to_nvt_type(const std::string &variant_str) {
    const auto &ext_types = ConfigOptionEnum<ExtruderType>::get_enum_names();

    auto trim = [](std::string &s) {
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        s.erase(0, s.find_first_not_of(" \t\r\n"));
    };

    for (auto ext_type : ext_types) {
        size_t pos = variant_str.find(ext_type);
        if (pos == std::string::npos)
            continue;

        std::string result = variant_str;
        result.erase(pos, ext_type.size());
        trim(result);

        auto iter = s_keys_map_NozzleVolumeType.find(result);
        if (iter != s_keys_map_NozzleVolumeType.end())
            return NozzleVolumeType(iter->second);
    }

    return nvtHybrid;
}

// Parses the extruder_nozzle_stats option: one "|"-separated token list per
// extruder, each token
// "<volume_type_name>#<count>". Empty string => no per-type stats for that extruder.
std::vector<std::map<NozzleVolumeType, int>> get_extruder_nozzle_stats(const std::vector<std::string> &strs)
{
    std::vector<std::map<NozzleVolumeType, int>> extruder_nozzle_counts;
    for (const std::string &str : strs) {
        std::map<NozzleVolumeType, int> nozzle_count_map;
        if (str.empty()) {
            extruder_nozzle_counts.emplace_back(nozzle_count_map);
            continue;
        }
        std::vector<std::string> nozzle_infos;
        boost::algorithm::split(nozzle_infos, str, boost::algorithm::is_any_of("|"));
        for (auto &nozzle_info : nozzle_infos) {
            std::vector<std::string> attr;
            boost::algorithm::split(attr, nozzle_info, boost::algorithm::is_any_of("#"));
            // Guard against malformed device/project data: skip tokens without a "<type>#<count>"
            // shape or with an unknown volume-type key instead of throwing (.at) or reading attr[1]
            // out of bounds. Corrupt tokens are dropped and logged; the rest still parse.
            if (attr.size() < 2) {
                BOOST_LOG_TRIVIAL(warning) << "get_extruder_nozzle_stats: skipping malformed token '" << nozzle_info << "'";
                continue;
            }
            auto it = s_keys_map_NozzleVolumeType.find(attr[0]);
            if (it == s_keys_map_NozzleVolumeType.end()) {
                BOOST_LOG_TRIVIAL(warning) << "get_extruder_nozzle_stats: skipping unknown nozzle volume type '" << attr[0] << "'";
                continue;
            }
            NozzleVolumeType volume_type = NozzleVolumeType(it->second);
            int              nozzle_count = std::atoi(attr[1].c_str());
            nozzle_count_map[volume_type] = nozzle_count;
        }
        extruder_nozzle_counts.emplace_back(nozzle_count_map);
    }
    return extruder_nozzle_counts;
}

std::vector<std::string> save_extruder_nozzle_stats_to_string(const std::vector<std::map<NozzleVolumeType, int>> &extruder_nozzle_stats)
{
    std::vector<std::string> extruder_nozzle_count_str;
    for (size_t idx = 0; idx < extruder_nozzle_stats.size(); ++idx) {
        std::ostringstream oss;
        const auto        &item = extruder_nozzle_stats[idx];
        for (auto it = item.begin(); it != item.end(); ++it) {
            oss << get_nozzle_volume_type_string(it->first) << "#" << it->second;
            if (std::next(it) != item.end())
                oss << "|";
        }
        extruder_nozzle_count_str.emplace_back(oss.str());
    }
    return extruder_nozzle_count_str;
}

static void assign_printer_technology_to_unknown(t_optiondef_map &options, PrinterTechnology printer_technology)
{
    for (std::pair<const t_config_option_key, ConfigOptionDef> &kvp : options)
        if (kvp.second.printer_technology == ptUnknown)
            kvp.second.printer_technology = printer_technology;
}

PrintConfigDef::PrintConfigDef()
{
    this->init_common_params();
    assign_printer_technology_to_unknown(this->options, ptAny);
    this->init_fff_params();
    this->init_extruder_option_keys();
    assign_printer_technology_to_unknown(this->options, ptFFF);
    this->init_sla_params();
    assign_printer_technology_to_unknown(this->options, ptSLA);
}

void PrintConfigDef::init_common_params()
{
    ConfigOptionDef* def;

    def = this->add("printer_technology", coEnum);
    def->label = L("Printer technology");
    //def->tooltip = L("Printer technology.");
    def->enum_keys_map = &ConfigOptionEnum<PrinterTechnology>::get_enum_values();
    def->enum_values.push_back("FFF");
    def->enum_values.push_back("SLA");
    def->set_default_value(new ConfigOptionEnum<PrinterTechnology>(ptFFF));

    def = this->add("printable_area", coPoints);
    def->label = L("Printable area");
    //BBS
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionPoints{ Vec2d(0, 0), Vec2d(200, 0), Vec2d(200, 200), Vec2d(0, 200) });

    def = this->add("extruder_printable_area", coPointsGroups);
    def->label = L("Extruder printable area");
    def->mode = comAdvanced;
    def->gui_type = ConfigOptionDef::GUIType::one_string;
    def->set_default_value(new ConfigOptionPointsGroups{});

    def           = this->add("support_parallel_printheads", coBool);
    def->label    = L("Support parallel printheads");
    def->tooltip  = L("Enable printer settings for machines that can use multiple printheads in parallel.");
    def->mode     = comAdvanced;
    def->set_default_value(new ConfigOptionBool{false});

    def           = this->add("parallel_printheads_count", coInt);
    def->label    = L("Parallel printheads count");
    def->tooltip  = L("Set the number of parallel printheads for machines like OrangeStorm Giga printer.");
    def->mode     = comAdvanced;
    def->min      = 1;
    def->max      = 4;
    def->set_default_value(new ConfigOptionInt{1});

    def           = this->add("parallel_printheads_bed_exclude_areas", coStrings);
    def->label    = L("Parallel printheads bed exclude areas");
    def->tooltip  = L("Ordered list of bed exclude areas by parallel printhead count. Item 1 applies to one printhead, item 2 to two printheads, and so on. Leave an item empty for no excluded area.");
    def->mode     = comAdvanced;
    def->set_default_value(new ConfigOptionStrings());

    //BBS: add "bed_exclude_area"
    def = this->add("bed_exclude_area", coPoints);
    def->label = L("Excluded bed area");
    def->tooltip = L("Unprintable area in XY plane. For example, X1 Series printers use the front left corner to cut filament during filament change. "
        "The area is expressed as polygon by points in following format: \"XxY, XxY, ...\"");
    def->mode = comAdvanced;
    def->gui_type = ConfigOptionDef::GUIType::one_string;
    def->set_default_value(new ConfigOptionPoints{ Vec2d(0, 0) });

    def = this->add("bed_custom_texture", coString);
    def->label = L("Bed custom texture");
    def->mode = comAdvanced;
    def->gui_type = ConfigOptionDef::GUIType::one_string;
    def->set_default_value(new ConfigOptionString());

    def = this->add("bed_custom_model", coString);
    def->label = L("Bed custom model");
    def->mode = comAdvanced;
    def->gui_type = ConfigOptionDef::GUIType::one_string;
    def->set_default_value(new ConfigOptionString());

    def = this->add("elefant_foot_compensation", coFloat);
    def->label = L("Elephant foot compensation");
    def->category = L("Quality");
    def->tooltip = L("This shrinks the first layer on the build plate to compensate for elephant foot effect.");
    def->sidetext = L("mm");	// millimeters, CIS languages need translation
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.));

    def           = this->add("elefant_foot_compensation_layers", coInt);
    def->label    = L("Elephant foot compensation layers");
    def->category = L("Quality");
    def->tooltip  = L("The number of layers on which the elephant foot compensation will be active. "
                       "The first layer will be shrunk by the elephant foot compensation value, then "
                       "the next layers will be linearly shrunk less, up to the layer indicated by this value.");
    def->sidetext = L("layers");
    def->min      = 1;
    def->mode     = comAdvanced;
    def->set_default_value(new ConfigOptionInt(1));	

    def           = this->add("elefant_foot_layers_density", coPercent);
    def->label    = L("Elephant foot layers density");
    def->category = L("Quality");
    def->tooltip  = L("Density of internal solid infill for Elephant foot layers compensation.\n"
                      "The initial value for the second layer is set.\n"
                      "Subsequent layers become linearly denser by the height specified in elefant_foot_compensation_layers.");
    def->sidetext = "%";
    def->min      = 50;
    def->max      = 100;
    def->mode     = comExpert;
    def->set_default_value(new ConfigOptionPercent(100));

    def = this->add("layer_height", coFloat);
    def->label = L("Layer height");
    def->category = L("Quality");
    def->tooltip = L("This is the height for each layer. Smaller layer heights give greater accuracy but longer printing time.");
    def->sidetext = L("mm");	// millimeters, CIS languages need translation
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(INITIAL_LAYER_HEIGHT));

    def = this->add("printable_height", coFloat);
    def->label = L("Printable height");
    def->tooltip = L("This is the maximum printable height which is limited by the height of the build area.");
    def->sidetext = L("mm");	// millimeters, CIS languages need translation
    def->min = 0;
    def->max = 214700;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloat(100.0));

    def           = this->add("extruder_printable_height", coFloats);
    def->label    = L("Extruder printable height");
    def->tooltip  = L("Maximum printable height of this extruder which is limited by mechanism of printer.");
    def->sidetext = L("mm");	// millimeters, CIS languages need translation
    def->min      = 0;
    def->max      = 1000;
    def->mode     = comAdvanced;
    def->set_default_value(new ConfigOptionFloatsNullable{0});

    def = this->add("preferred_orientation", coFloat);
    def->label = L("Preferred orientation");
    def->tooltip = L("Automatically orient STL files on the Z axis upon initial import.");
    def->sidetext = u8"°";	// degrees, don't need translation
    def->max = 360;
    def->min = -360;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.0));

    // Options used by physical printers

    def = this->add("preset_names", coStrings);
    def->label = L("Printer preset names");
    //def->tooltip = L("Names of presets related to the physical printer.");
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionStrings());

    def = this->add("bbl_use_printhost", coBool);
    def->label = L("Use 3rd-party print host");
    def->tooltip = L("Allow controlling BambuLab's printer through 3rd party print hosts.");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("use_3mf", coBool);
    def->label = L("Use 3MF instead of G-code");
    def->tooltip = L("Enable this if the printer accepts a 3MF file as the print job. When enabled, Orca Slicer "
                     "sends the sliced file as a .gcode.3mf, instead of a plain .gcode file.");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("printer_agent", coString);
    def->label = L("Printer Agent");
    def->tooltip = L("Select the network agent implementation for printer communication.");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    // Plugin-backed (see ConfigOptionDef::is_plugin_backed), but edited through the Choice widget above
    // rather than a plugin_picker field.
    def->plugin_type = "printer-connection";
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("print_host", coString);
    def->label = L("Hostname, IP or URL");
    def->tooltip = L("Orca Slicer can upload G-code files to a printer host. This field should contain "
        "the hostname, IP address or URL of the printer host instance. "
        "Print host behind HAProxy with basic auth enabled can be accessed by putting the user name and password into the URL "
        "in the following format: https://username:password@your-octopi-address/");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString());

    def = this->add("print_host_webui", coString);
    def->label = L("Device UI");
    def->tooltip = L("Specify the URL of your device user interface if it's not same as print_host.");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString());

    def = this->add("printhost_apikey", coString);
    def->label = L("API Key / Password");
    def->tooltip = L("Orca Slicer can upload G-code files to a printer host. This field should contain "
        "the API Key or the password required for authentication.");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString());

    def = this->add("flashforge_serial_number", coString);
    def->label = L("Serial Number");
    def->tooltip = L("Flashforge local API requires the printer serial number.");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString());

    def = this->add("printhost_port", coString);
    def->label = L("Printer");
    def->tooltip = L("Name of the printer.");
    def->gui_type = ConfigOptionDef::GUIType::select_open;
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString());

    def = this->add("printhost_cafile", coString);
    def->label = L("HTTPS CA File");
    def->tooltip = L("Custom CA certificate file can be specified for HTTPS OctoPrint connections, in crt/pem format. "
        "If left blank, the default OS CA certificate repository is used.");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString());

    // Options used by physical printers

    def = this->add("printhost_user", coString);
    def->label = L("User");
    //def->tooltip = L("");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString());

    def = this->add("printhost_password", coString);
    def->label = L("Password");
    //def->tooltip = L("");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString());

    // Only available on Windows.
    def = this->add("printhost_ssl_ignore_revoke", coBool);
    def->label = L("Ignore HTTPS certificate revocation checks");
    def->tooltip = L("Ignore HTTPS certificate revocation checks in the case of missing or offline distribution points. One may want to enable this option for self signed certificates if connection fails.");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("preset_names", coStrings);
    def->label = L("Printer preset names");
    def->tooltip = L("Names of presets related to the physical printer.");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionStrings());

    def = this->add("printhost_authorization_type", coEnum);
    def->label = L("Authorization Type");
    //def->tooltip = L("");
    def->enum_keys_map = &ConfigOptionEnum<AuthorizationType>::get_enum_values();
    def->enum_values.push_back("key");
    def->enum_values.push_back("user");
    def->enum_labels.push_back(L("API key"));
    def->enum_labels.push_back(L("HTTP digest"));
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionEnum<AuthorizationType>(atKeyPassword));
    
    // temporary workaround for compatibility with older Slicer
    {
        def = this->add("preset_name", coString);
        def->set_default_value(new ConfigOptionString());
    }

    // One key per preset type (Preset::plugin_overrides_key), so the print, printer and filament
    // overrides don't clobber each other when the presets merge into one full config. No handle_legacy
    // migration from the shared "plugin_config_overrides" they replace: it only ever shipped in
    // nightlies. Never a text field — GUIType::plugin_config renders a button opening PluginsConfigDialog.
    for (const char* key : {"print_plugin_config_overrides", "printer_plugin_config_overrides", "filament_plugin_config_overrides"}) {
        def = this->add(key, coString);
        def->label = L("Capabilities");
        def->tooltip = L("Configuration for the plugin capabilities this preset uses, overriding the global "
                         "Capabilities configuration. Stored as a raw JSON array and edited through the dialog "
                         "behind the button, never typed in directly.");
        def->gui_type = ConfigOptionDef::GUIType::plugin_config;
        def->mode = comAdvanced;
        def->cli = ConfigOptionDef::nocli;
        def->set_default_value(new ConfigOptionString(""));
    }
}

void PrintConfigDef::init_fff_params()
{
    ConfigOptionDef* def;
#include "../slic3r/GUI/generated/PrintConfigDef_generated.cpp"

}

#include "../slic3r/GUI/generated/OptionKeys_generated.cpp"

void PrintConfigDef::init_extruder_option_keys()
{
    m_extruder_option_keys = s_extruder_option_keys;
    // extruder_printable_height is defined in init_common_params(), not in proto files
    m_extruder_option_keys.push_back("extruder_printable_height");

    m_extruder_retract_keys = {
        "deretraction_speed",
        "long_retractions_when_cut",
        "retract_after_wipe",
        "retract_before_wipe",
        "retract_lift_above",
        "retract_lift_below",
        "retract_lift_enforce",
        "retract_restart_extra",
        "retract_when_changing_layer",
        "retraction_distances_when_cut",
        "retraction_length",
        "retraction_minimum_travel",
        "retraction_speed",
        "travel_slope",
        "wipe",
        "wipe_distance",
        "z_hop",
        "z_hop_types"
    };
    assert(std::is_sorted(m_extruder_retract_keys.begin(), m_extruder_retract_keys.end()));
}

void PrintConfigDef::init_filament_option_keys()
{
    m_filament_option_keys = s_filament_option_keys;

    m_filament_retract_keys = {
        "deretraction_speed",
        "filament_retract_length_nc",
        "long_retractions_when_cut",
        "retract_after_wipe",
        "retract_before_wipe",
        "retract_lift_above",
        "retract_lift_below",
        "retract_lift_enforce",
        "retract_restart_extra",
        "retract_when_changing_layer",
        "retraction_distances_when_cut",
        "retraction_length",
        "retraction_minimum_travel",
        "retraction_speed",
        "wipe",
        "wipe_distance",
        "z_hop",
        "z_hop_types"
    };
    assert(std::is_sorted(m_filament_retract_keys.begin(), m_filament_retract_keys.end()));
}

void PrintConfigDef::init_sla_params()
{
    ConfigOptionDef* def;

    // SLA Printer settings

    def = this->add("display_width", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    def->min = 1;
    def->set_default_value(new ConfigOptionFloat(120.));

    def = this->add("display_height", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    def->min = 1;
    def->set_default_value(new ConfigOptionFloat(68.));

    def = this->add("display_pixels_x", coInt);
    //def->full_label = L("");
    def->label = ("X");
    //def->tooltip = L("");
    def->min = 100;
    def->set_default_value(new ConfigOptionInt(2560));

    def = this->add("display_pixels_y", coInt);
    //def->full_label = L("");
    def->label = ("Y");
    //def->tooltip = L("");
    def->min = 100;
    def->set_default_value(new ConfigOptionInt(1440));

    def = this->add("display_mirror_x", coBool);
    //def->full_label = L("");
    //def->label = L("");
    //def->tooltip = L("");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("display_mirror_y", coBool);
    //def->full_label = L("");
    //def->label = L("");
    //def->tooltip = L("");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("display_orientation", coEnum);
    //def->label = L("");
    //def->tooltip = L("");
    def->enum_keys_map = &ConfigOptionEnum<SLADisplayOrientation>::get_enum_values();
    def->enum_values.push_back("landscape");
    def->enum_values.push_back("portrait");
    def->enum_labels.push_back(" ");
    def->enum_labels.push_back(" ");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionEnum<SLADisplayOrientation>(sladoPortrait));

    def = this->add("fast_tilt_time", coFloat);
    //def->label = L("");
    //def->full_label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(5.));

    def = this->add("slow_tilt_time", coFloat);
    //def->label = L("");
    //def->full_label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(8.));

    def = this->add("area_fill", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(50.));

    def = this->add("relative_correction", coFloats);
    //def->label = L("");
    //def->full_label = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloats( { 1., 1.} ));

    def = this->add("relative_correction_x", coFloat);
    //def->label = L("");
    //def->full_label = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("relative_correction_y", coFloat);
    //def->label = L("");
    //def->full_label = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("relative_correction_z", coFloat);
    //def->label = L("");
    //def->full_label = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("absolute_correction", coFloat);
    //def->label = L("");
    //def->full_label = L("");
    //def->tooltip = L("");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.0));

    def = this->add("elefant_foot_min_width", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.2));

    def = this->add("gamma_correction", coFloat);
    //def->label = L("");
    //def->full_label = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->max = 1;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.0));


    // SLA Material settings.

    def = this->add("material_colour", coString);
    //def->label = L("");
    //def->tooltip = L("");
    def->gui_type = ConfigOptionDef::GUIType::color;
    def->set_default_value(new ConfigOptionString("#29B2B2"));

    def = this->add("material_type", coString);
    //def->label = L("");
    //def->tooltip = L("");
    def->gui_type = ConfigOptionDef::GUIType::f_enum_open;   // TODO: ???
    def->gui_flags = "show_value";
    def->enum_values.push_back("Tough");
    def->enum_values.push_back("Flexible");
    def->enum_values.push_back("Casting");
    def->enum_values.push_back("Dental");
    def->enum_values.push_back("Heat-resistant");
    def->set_default_value(new ConfigOptionString("Tough"));

    def = this->add("initial_layer_height", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(0.3));

    def = this->add("bottle_volume", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 50;
    def->set_default_value(new ConfigOptionFloat(1000.0));

    def = this->add("bottle_weight", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(1.0));

    def = this->add("material_density", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(1.0));

    def = this->add("bottle_cost", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(0.0));

    def = this->add("faded_layers", coInt);
    //def->label = L("");
    //def->tooltip = L("");
    def->min = 3;
    def->max = 20;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionInt(10));

    def = this->add("min_exposure_time", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("max_exposure_time", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(100));

    def = this->add("exposure_time", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(10));

    def = this->add("min_initial_exposure_time", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("max_initial_exposure_time", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(150));

    def = this->add("initial_exposure_time", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(15));

    def = this->add("material_correction", coFloats);
    //def->full_label = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloats( { 1., 1., 1. } ));

    def = this->add("material_correction_x", coFloat);
    //def->full_label = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("material_correction_y", coFloat);
    //def->full_label = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("material_correction_z", coFloat);
    //def->full_label = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("material_vendor", coString);
    //def->label = L("");
    //def->tooltip = L("");
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("default_sla_material_profile", coString);
    //def->label = L("");
    //def->tooltip = L("");
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("sla_material_settings_id", coString);
    //def->label = L("");
    //def->tooltip = L("");
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("default_sla_print_profile", coString);
    //def->label = L("");
    //def->tooltip = L("");
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("sla_print_settings_id", coString);
    //def->label = L("");
    //def->tooltip = L("");
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("supports_enable", coBool);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("support_head_front_diameter", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.4));

    def = this->add("support_head_penetration", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->mode = comAdvanced;
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(0.2));

    def = this->add("support_head_width", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->max = 20;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.0));

    def = this->add("support_pillar_diameter", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->max = 15;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloat(1.0));

    def = this->add("support_small_pillar_diameter_percent", coPercent);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 1;
    def->max = 100;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionPercent(50));

    def = this->add("support_max_bridges_on_pillar", coInt);
    //def->label = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->max = 50;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionInt(3));

    def = this->add("support_pillar_connection_mode", coEnum);
    //def->label = L("");
    //def->tooltip = L("");
    def->enum_keys_map = &ConfigOptionEnum<SLAPillarConnectionMode>::get_enum_values();
    def->enum_values.push_back("zigzag");
    def->enum_values.push_back("cross");
    def->enum_values.push_back("dynamic");
    def->enum_labels.push_back(" ");
    def->enum_labels.push_back(" ");
    def->enum_labels.push_back(" ");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionEnum<SLAPillarConnectionMode>(slapcmDynamic));

    def = this->add("support_buildplate_only", coBool);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("support_pillar_widening_factor", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->max = 1;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.0));

    def = this->add("support_base_diameter", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->max = 30;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(4.0));

    def = this->add("support_base_height", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.0));

    def = this->add("support_base_safety_distance", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->max = 10;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1));

    def = this->add("support_critical_angle", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->max = 90;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(45));

    def = this->add("support_max_bridge_length", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(15.0));

    def = this->add("support_max_pillar_link_distance", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;   // 0 means no linking
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(10.0));

    def = this->add("support_object_elevation", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->max = 150; // This is the max height of print on SL1
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(5.0));

    def = this->add("support_points_density_relative", coInt);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->set_default_value(new ConfigOptionInt(100));

    def = this->add("support_points_minimal_distance", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    def->sidetext = L("mm");	// millimeters, CIS languages need translation
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("pad_enable", coBool);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("pad_wall_thickness", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->max = 30;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloat(2.0));

    def = this->add("pad_wall_height", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->category = L("");
    //def->sidetext = "";
    def->min = 0;
    def->max = 30;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = this->add("pad_brim_size", coFloat);
    //def->label = L("");
    //def->tooltip = L("");
    //def->category = L("");
    //def->sidetext = "";
    def->min = 0;
    def->max = 30;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.6));

    def = this->add("pad_max_merge_distance", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(50.0));

    def = this->add("pad_wall_slope", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 45;
    def->max = 90;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(90.0));

    def = this->add("pad_around_object", coBool);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("pad_around_object_everywhere", coBool);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("pad_object_gap", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->max = 10;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1));

    def = this->add("pad_object_connector_stride", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(10));

    def = this->add("pad_object_connector_width", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.5));

    def = this->add("pad_object_connector_penetration", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.3));

    def = this->add("hollowing_enable", coBool);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("hollowing_min_thickness", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    //def->sidetext = "";
    def->min = 1;
    def->max = 10;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloat(3.));

    def = this->add("hollowing_quality", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    def->min = 0;
    def->max = 1;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.5));

    def = this->add("hollowing_closing_distance", coFloat);
    //def->label = L("");
    //def->category = L("");
    //def->tooltip = L("");
    def->sidetext = L("mm");	// millimeters, CIS languages need translation
    def->min = 0;
    def->max = 10;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(2.0));

    def = this->add("material_print_speed", coEnum);
    //def->label = L("");
    //def->tooltip = L("");
    def->enum_keys_map = &ConfigOptionEnum<SLAMaterialSpeed>::get_enum_values();
    def->enum_values.push_back("slow");
    def->enum_values.push_back("fast");
    def->enum_labels.push_back(" ");
    def->enum_labels.push_back(" ");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionEnum<SLAMaterialSpeed>(slamsFast));
}

void PrintConfigDef::handle_legacy(t_config_option_key &opt_key, std::string &value)
{
    //BBS: handle legacy options
    if (opt_key == "curr_bed_type" && value == "SuperTack Plate") {
        value = "Supertack Plate";
    } else if (opt_key == "enable_wipe_tower") {
        opt_key = "enable_prime_tower";
    } else if (opt_key == "wipe_tower_width") {
        opt_key = "prime_tower_width";
    } else if (opt_key == "wiping_volume") {
        opt_key = "prime_volume";
    } else if (opt_key == "wipe_tower_brim_width") {
        opt_key = "prime_tower_brim_width";
    } else if (opt_key == "tool_change_gcode") {
        opt_key = "change_filament_gcode";
    } else if (opt_key == "bridge_fan_speed") {
        opt_key = "overhang_fan_speed";
    } else if (opt_key == "infill_extruder" || opt_key == "sparse_infill_filament") {
        // ORCA: legacy feature-filament selector. Pre-2.4.0-dev these keys were 1-based and the
        // default value "1" meant "the first/active filament". The current scheme uses a dedicated
        // key where 0 = "Default" (inherit the object/part filament) and 1..N = explicit filament.
        // Renaming to the new *_id key here means every config (process presets, 3mf project
        // settings, imported gcode) is translated uniformly on load - not just the version-gated
        // 3mf paths the old bespoke migration covered - and a brand-new key can never be misread as
        // a legacy default. Map the legacy default "1" to "0" (inherit); keep explicit values >1.
        opt_key = "sparse_infill_filament_id";
        if (value == "1") value = "0";
    } else if (opt_key == "solid_infill_extruder" || opt_key == "solid_infill_filament") {
        opt_key = "internal_solid_filament_id";
        if (value == "1") value = "0";
    } else if (opt_key == "top_solid_infill_filament") {
        opt_key = "top_surface_filament_id";
        if (value == "1") value = "0";
    } else if (opt_key == "bottom_solid_infill_filament") {
        opt_key = "bottom_surface_filament_id";
        if (value == "1") value = "0";
    } else if (opt_key == "perimeter_extruder" || opt_key == "wall_filament" || opt_key == "wall_filament_id") {
        opt_key = "outer_wall_filament_id";
        if (value == "1") value = "0";
    } else if (opt_key == "inner_wall_filament") {
        opt_key = "inner_wall_filament_id";
        if (value == "1") value = "0";
    } else if (opt_key == "outer_wall_filament") {
        opt_key = "outer_wall_filament_id";
        if (value == "1") value = "0";
    }else if(opt_key == "wipe_tower_extruder") {
        opt_key = "wipe_tower_filament";
    }else if (opt_key == "support_material_extruder") {
        opt_key = "support_filament";
    } else if (opt_key == "support_material_interface_extruder") {
        opt_key = "support_interface_filament";
    } else if (opt_key == "support_material_angle") {
        opt_key = "support_angle";
    } else if (opt_key == "support_material_enforce_layers") {
        opt_key = "enforce_support_layers";
    } else if ((opt_key == "initial_layer_print_height"   ||
                opt_key == "initial_layer_speed"          ||
                opt_key == "internal_solid_infill_speed"  ||
                opt_key == "top_surface_speed"            ||
                opt_key == "support_interface_speed"      ||
                opt_key == "outer_wall_speed"             ||
                opt_key == "support_object_xy_distance")     && value.find("%") != std::string::npos) {
        //BBS: this is old profile in which value is expressed as percentage.
        //But now these key-value must be absolute value.
        //Reset to default value by erasing these key to avoid parsing error.
        opt_key = "";
    } else if (opt_key == "inherits_cummulative") {
        opt_key = "inherits_group";
    } else if (opt_key == "compatible_printers_condition_cummulative") {
        opt_key = "compatible_machine_expression_group";
    } else if (opt_key == "compatible_prints_condition_cummulative") {
        opt_key = "compatible_process_expression_group";
    } else if (opt_key == "cooling") {
        opt_key = "slow_down_for_layer_cooling";
    } else if (opt_key == "timelapse_no_toolhead") {
        opt_key = "timelapse_type";
    } else if (opt_key == "timelapse_type" && value == "2") {
        // old file "0" is None, "2" is Traditional
        // new file "0" is Traditional, erase "2"
        value = "0";
    } else if (opt_key == "support_type" && value == "normal") {
        value = "normal(manual)";
    } else if (opt_key == "support_type" && value == "tree") {
        value = "tree(manual)";
    } else if (opt_key == "support_type" && value == "hybrid(auto)") {
        value = "tree(auto)";
    } else if (opt_key == "support_base_pattern" && value == "none") {
        value = "hollow";
    } else if (opt_key == "different_settings_to_system") {
        std::string copy_value = value;
        copy_value.erase(std::remove(copy_value.begin(), copy_value.end(), '\"'), copy_value.end()); // remove '"' in string
        std::set<std::string> split_keys = SplitStringAndRemoveDuplicateElement(copy_value, ";");
        for (std::string split_key : split_keys) {
            std::string copy_key = split_key, copy_value = "";
            handle_legacy(copy_key, copy_value);
            if (copy_key != split_key) {
                ReplaceString(value, split_key, copy_key);
            }
        }
    } else if (opt_key == "overhang_fan_threshold" && value == "5%") {
        value = "10%";
    } else if( opt_key == "wall_infill_order" ) {
        if (value == "inner wall/outer wall/infill" || value == "infill/inner wall/outer wall") {
            opt_key = "wall_sequence";
            value = "inner wall/outer wall";
        } else if (value == "outer wall/inner wall/infill" || value == "infill/outer wall/inner wall") {
            opt_key = "wall_sequence";
            value = "outer wall/inner wall";
        } else if (value == "inner-outer-inner wall/infill") {
            opt_key = "wall_sequence";
            value = "inner-outer-inner wall";
        } else {
            opt_key = "wall_sequence";
        }
    } else if (opt_key == "nozzle_volume_type"
        || opt_key == "default_nozzle_volume_type"
        || opt_key == "printer_extruder_variant"
        || opt_key == "print_extruder_variant"
        || opt_key == "filament_extruder_variant"
        || opt_key == "extruder_variant_list") {
        ReplaceString(value, "Normal", "Standard");
        ReplaceString(value, "Big Traffic", "High Flow");
    }
    else if (opt_key == "extruder_type") {
        ReplaceString(value, "DirectDrive", "Direct Drive");
    }
    else if (opt_key == "enable_power_loss_recovery") {
        if (value == "1" || boost::iequals(value, "true")) {
            value = "enable";
        } else if (value == "0" || boost::iequals(value, "false")) {
            value = "disable";
        }
    }
    else if(opt_key == "ensure_vertical_shell_thickness") {
        if(value == "1") {
            value = "ensure_all";
        }
        else if (value == "0"){
            value = "ensure_moderate";
        }
    } else if (opt_key == "rotate_solid_infill_direction") {
        opt_key = "solid_infill_rotate_template";
        if (value == "1") {
            value = "0,90";
        } else if (value == "0") {
            value = "0";
        }
    } else if (opt_key == "sparse_infill_anchor") {
        opt_key = "infill_anchor";
    } else if (opt_key == "sparse_infill_anchor_max") {
        opt_key = "infill_anchor_max";
    } else if (opt_key == "chamber_temperatures") {
        opt_key = "chamber_temperature";
    } else if (opt_key == "thumbnail_size") {
        opt_key = "thumbnails";
    } else if (opt_key == "top_one_wall_type" && value != "none") {
        opt_key = "only_one_wall_top";
        value = "1";
    } else if (opt_key == "initial_layer_flow_ratio") {
        opt_key = "bottom_solid_infill_flow_ratio";
    } else if (opt_key == "ironing_direction") {
        opt_key = "ironing_angle";
    } else if (opt_key == "ironing_angle" && boost::starts_with(value, "-")) {
        value = "0";
    } else if (opt_key == "counterbole_hole_bridging") {
        opt_key = "counterbore_hole_bridging";
    } else if (opt_key == "draft_shield" && value == "limited") {
        value = "disabled";
    } else if ((opt_key == "sparse_infill_pattern"         ||
                opt_key == "top_surface_pattern"           ||
                opt_key == "bottom_surface_pattern"        ||
                opt_key == "internal_solid_infill_pattern" ||
                opt_key == "ironing_pattern"               ||
                opt_key == "support_ironing_pattern") && value == "zig-zag") {
        value = "rectilinear";
    } else if (opt_key == "filament_map_mode") {
        if (value == "Auto") value = "Auto For Flush";
    }
    else if (opt_key == "filament_type"){
        std::vector<std::string> type_list;
        std::stringstream ss(value);
        std::string token;
        bool rebuild_value = false;
        while (std::getline(ss, token, ';')) {
            if (token.size() >= 2 && token.front() == '"' && token.back() == '"')
                token = token.substr(1, token.size() - 2);
            if (token == "ASA-Aero") {
                token = "ASA-AERO";
                rebuild_value = true;
            }
            type_list.emplace_back(token);
        }
        if (rebuild_value) {
            value.clear();
            for (size_t idx = 0; idx < type_list.size(); ++idx) {
                if (idx != 0)
                    value += ';';
                value += "\"" + type_list[idx] + "\"";
            }
        }
    }
    // Orca: Rename wipe tower ribs related options
    else if (opt_key == "prime_tower_rib_wall") {
        if (value == "1") {
            opt_key = "wipe_tower_wall_type";
            value   = "rib";
        } else {
            opt_key = "";
        }
    } else if (opt_key == "prime_tower_extra_rib_length") {
        opt_key = "wipe_tower_extra_rib_length";
    } else if (opt_key == "prime_tower_rib_width") {
        opt_key = "wipe_tower_rib_width";
    } else if (opt_key == "prime_tower_fillet_wall") {
        opt_key = "wipe_tower_fillet_wall";
    } else if (opt_key == "extruder_clearance_max_radius") {
        opt_key = "extruder_clearance_radius";
    } else if (opt_key == "machine_switch_extruder_time") {
        opt_key = "machine_tool_change_time";
    }
    else if (opt_key == "wall_direction" && value == "auto") {
        value = "ccw";
    }

    // Ignore the following obsolete configuration keys:
    static std::set<std::string> ignore = {
        "acceleration", "scale", "rotate", "duplicate", "duplicate_grid",
        "bed_size",
        "print_center", "g0", "wipe_tower_per_color_wipe", 
        "support_sharp_tails","support_remove_small_overhangs", "support_with_sheath",
        "tree_support_collision_resolution", "tree_support_with_infill",
        "max_volumetric_speed", "max_print_speed",
        "support_closing_radius",
        "remove_freq_sweep", "remove_bed_leveling", "remove_extrusion_calibration",
        "support_transition_line_width", "support_transition_speed", "bed_temperature", "bed_temperature_initial_layer",
        "can_switch_nozzle_type", "can_add_auxiliary_fan", "extra_flush_volume", "spaghetti_detector", "adaptive_layer_height",
        "z_hop_type", "z_lift_type", "bed_temperature_difference","long_retraction_when_cut",
        "retraction_distance_when_cut",
        "internal_bridge_support_thickness", "top_area_threshold", "reduce_wall_solid_infill","filament_load_time","filament_unload_time",
        "smooth_coefficient", "overhang_totally_speed", "silent_mode",
        "overhang_speed_classic",
        "anisotropic_surfaces", // superseded by top_surface_fill_order / bottom_surface_fill_order
    };

    if (ignore.find(opt_key) != ignore.end()) {
        opt_key = "";
        return;
    }

    if (! print_config_def.has(opt_key)) {
        opt_key = "";
        return;
    }
}

// Called after a config is loaded as a whole.
// Perform composite conversions, for example merging multiple keys into one key.
// Don't convert single options here, implement such conversion in PrintConfigDef::handle_legacy() instead.
void PrintConfigDef::handle_legacy_composite(DynamicPrintConfig &config)
{
    if (config.has("thumbnails")) {
        std::string extention;
        if (config.has("thumbnails_format")) {
            if (const ConfigOptionDef* opt = config.def()->get("thumbnails_format")) {
                extention = opt->enum_values.at(config.option("thumbnails_format")->getInt());
            }
        }

        std::string thumbnails_str = config.opt_string("thumbnails");
        auto [thumbnails_list, errors] = GCodeThumbnails::make_and_check_thumbnail_list(thumbnails_str, extention);

        if (errors != enum_bitmask<ThumbnailError>()) {
            std::string error_str = "\n" + Slic3r::format("Invalid value provided for parameter %1%: %2%", "thumbnails", thumbnails_str);
            error_str += GCodeThumbnails::get_error_string(errors);
            throw BadOptionValueException(error_str);
        }

        if (!thumbnails_list.empty()) {
            const auto& extentions = ConfigOptionEnum<GCodeThumbnailsFormat>::get_enum_names();
            thumbnails_str.clear();
            for (const auto& [ext, size] : thumbnails_list)
                thumbnails_str += Slic3r::format("%1%x%2%/%3%, ", size.x(), size.y(), extentions[int(ext)]);
            thumbnails_str.resize(thumbnails_str.length() - 2);

            config.set_key_value("thumbnails", new ConfigOptionString(thumbnails_str));
        }
    }

    if (config.has("wiping_volumes_matrix") && !config.has("wiping_volumes_use_custom_matrix")) {
        // This is apparently some pre-2.7.3 config, where the wiping_volumes_matrix was always used.
        // The 2.7.3 introduced an option to use defaults derived from config. In case the matrix
        // contains only default values, switch it to default behaviour. The default values
        // were zeros on the diagonal and 140 otherwise.
        std::vector<double> matrix = config.opt<ConfigOptionFloats>("wiping_volumes_matrix")->values;
        int num_of_extruders = int(std::sqrt(matrix.size()) + 0.5);
        int i = -1;
        bool custom = false;
        for (int j = 0; j < int(matrix.size()); ++j) {
            if (j % num_of_extruders == 0)
                ++i;
            if (i != j % num_of_extruders && !is_approx(matrix[j], 140.)) {
                custom = true;
                break;
            }
        }
        config.set_key_value("wiping_volumes_use_custom_matrix", new ConfigOptionBool(custom));
    }
}

const PrintConfigDef print_config_def;

//todo
std::set<std::string> print_options_with_variant = {
    "initial_layer_speed",
    "initial_layer_infill_speed",
    "outer_wall_speed",
    "inner_wall_speed",
    "small_perimeter_speed",  //coFloatsOrPercents
    "small_perimeter_threshold",
    "sparse_infill_speed",
    "internal_solid_infill_speed",
    "top_surface_speed",
    "enable_overhang_speed", //coBools
    "overhang_1_4_speed",
    "overhang_2_4_speed",
    "overhang_3_4_speed",
    "overhang_4_4_speed",
    "slowdown_for_curled_perimeters",
    "bridge_speed",
    "internal_bridge_speed",
    "gap_infill_speed",
    "support_speed",
    "support_interface_speed",
    "travel_speed",
    "travel_speed_z",
    "initial_layer_travel_speed",
    "default_acceleration",
    "bridge_acceleration",
    "travel_acceleration",
    "initial_layer_travel_acceleration",
    "initial_layer_acceleration",
    "outer_wall_acceleration",
    "inner_wall_acceleration",
    "sparse_infill_acceleration", //coFloatsOrPercents
    "internal_solid_infill_acceleration", //coFloatsOrPercents
    "top_surface_acceleration",
    "default_jerk",
    "outer_wall_jerk",
    "inner_wall_jerk",
    "infill_jerk",
    "top_surface_jerk",
    "initial_layer_jerk",
    "travel_jerk",
    "initial_layer_travel_jerk",
    "default_junction_deviation",
    "print_extruder_id", //coInts
    "print_extruder_variant" //coStrings
};

std::set<std::string> filament_options_with_variant = {
    "filament_flow_ratio",
    "filament_max_volumetric_speed",
    // Per-variant ramming / pre-cooling / nozzle-change filament overrides
    "filament_ramming_volumetric_speed",
    "filament_pre_cooling_temperature",
    "filament_ramming_travel_time",
    "filament_ramming_volumetric_speed_nc",
    "filament_pre_cooling_temperature_nc",
    "filament_ramming_travel_time_nc",
    "filament_retract_length_nc",
    "filament_preheat_temperature_delta",
    //"filament_extruder_id",
    "filament_extruder_variant",
    "filament_retraction_length",
    "filament_z_hop",
    "filament_z_hop_types",
    "filament_retract_lift_above",
    "filament_retract_lift_below",
    "filament_retract_lift_enforce",
    "filament_retract_restart_extra",
    "filament_retraction_speed",
    "filament_deretraction_speed",
    "filament_retraction_minimum_travel",
    "filament_retract_when_changing_layer",
    "filament_wipe",
    //BBS
    "filament_wipe_distance",
    "filament_retract_before_wipe",
    // Orca
    "filament_retract_after_wipe",
    //BBS
    "filament_long_retractions_when_cut",
    "filament_retraction_distances_when_cut",
    "long_retractions_when_ec",
    "retraction_distances_when_ec",
    "nozzle_temperature_initial_layer",
    "nozzle_temperature",
    "filament_flush_volumetric_speed",
    "filament_flush_temp",
    "filament_cooling_before_tower",
    "volumetric_speed_coefficients",
    "filament_adaptive_volumetric_speed",
    "filament_ironing_flow",
    "filament_ironing_spacing",
    "filament_ironing_inset",
    "filament_ironing_speed",
    "activate_air_filtration",
    "activate_air_filtration_during_print",
    "activate_air_filtration_on_completion",
    "during_print_exhaust_fan_speed",
    "complete_print_exhaust_fan_speed"
};

// Parameters that are the same as the number of extruders
std::set<std::string> printer_extruder_options = {
    "extruder_type",
    "nozzle_diameter",
    "default_nozzle_volume_type",
    "extruder_printable_area",
    "extruder_printable_height",
    "min_layer_height",
    "max_layer_height",
    "extruder_max_nozzle_count"     // Per-extruder max (sub-)nozzle count
};

std::set<std::string> printer_options_with_variant_1 = {
    "nozzle_volume",
    "retraction_length",
    "z_hop",
    "travel_slope",
    "retract_lift_above",
    "retract_lift_below",
    "retract_lift_enforce",
    "z_hop_types",
    "retraction_speed",
    "deretraction_speed",
    "retraction_minimum_travel",
    "retract_when_changing_layer",
    "wipe",
    "wipe_distance",
    "retract_before_wipe",
    // Orca:
    "retract_after_wipe",
    "retract_length_toolchange",
    "retract_restart_extra",
    "retract_restart_extra_toolchange",
    "long_retractions_when_cut",
    "retraction_distances_when_cut",
    "nozzle_volume",
    "nozzle_type",
    "printer_extruder_id",
    "printer_extruder_variant",
    // Per-variant hotend heat-up / cool-down rates (pre-heat time model)
    "hotend_cooling_rate",
    "hotend_heating_rate",
    "nozzle_flush_dataset"
};

//options with silient mode
std::set<std::string> printer_options_with_variant_2 = {
    "machine_max_acceleration_x",
    "machine_max_acceleration_y",
    "machine_max_acceleration_z",
    "machine_max_acceleration_e",
    "machine_max_acceleration_extruding",
    "machine_max_acceleration_retracting",
    "machine_max_acceleration_travel",
    "machine_max_speed_x",
    "machine_max_speed_y",
    "machine_max_speed_z",
    "machine_max_speed_e",
    "machine_max_jerk_x",
    "machine_max_jerk_y",
    "machine_max_jerk_z",
    "machine_max_jerk_e",
    "machine_max_junction_deviation",
};

std::set<std::string> empty_options;

std::set<std::string> filament_dev_options = {
    "filament_dev_ams_drying_ams_limitations",
    "filament_dev_ams_drying_temperature",
    "filament_dev_ams_drying_time",
    "filament_dev_ams_drying_heat_distortion_temperature",
    "filament_dev_chamber_drying_bed_temperature",
    "filament_dev_chamber_drying_time",
    "filament_dev_drying_softening_temperature",
    "filament_dev_drying_cooling_temperature"
};

DynamicPrintConfig DynamicPrintConfig::full_print_config()
{
	return DynamicPrintConfig((const PrintRegionConfig&)FullPrintConfig::defaults());
}

DynamicPrintConfig::DynamicPrintConfig(const StaticPrintConfig& rhs) : DynamicConfig(rhs, rhs.keys_ref())
{
}

DynamicPrintConfig* DynamicPrintConfig::new_from_defaults_keys(const std::vector<std::string> &keys)
{
    auto *out = new DynamicPrintConfig();
    out->apply_only(FullPrintConfig::defaults(), keys);
    return out;
}

double min_object_distance(const ConfigBase &cfg)
{
    const ConfigOptionEnum<PrinterTechnology> *opt_printer_technology = cfg.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
    auto printer_technology = opt_printer_technology ? opt_printer_technology->value : ptUnknown;

    double ret = 0.;

    if (printer_technology == ptSLA)
        ret = 6.;
    else {
        //BBS: duplicate_distance seam to be useless
        constexpr double duplicate_distance = 6.;
        auto ecr_opt = cfg.option<ConfigOptionFloat>("extruder_clearance_radius");
        auto co_opt  = cfg.option<ConfigOptionEnum<PrintSequence>>("print_sequence");

        if (!ecr_opt || !co_opt)
            ret = 0.;
        else {
            // min object distance is max(duplicate_distance, clearance_radius)
            ret = ((co_opt->value == PrintSequence::ByObject) && ecr_opt->value > duplicate_distance) ?
                      ecr_opt->value : duplicate_distance;
        }
    }

    return ret;
}

void DynamicPrintConfig::normalize_fdm(int used_filaments)
{
    if (this->has("extruder")) {
        int extruder = this->option("extruder")->getInt();
        this->erase("extruder");
        if (extruder != 0) {
            if (!this->has("sparse_infill_filament_id") || this->option("sparse_infill_filament_id")->getInt() == 0)
                this->option("sparse_infill_filament_id", true)->setInt(extruder);
            if (!this->has("outer_wall_filament_id") || this->option("outer_wall_filament_id")->getInt() == 0)
                this->option("outer_wall_filament_id", true)->setInt(extruder);
            if (!this->has("inner_wall_filament_id") || this->option("inner_wall_filament_id")->getInt() == 0)
                this->option("inner_wall_filament_id", true)->setInt(extruder);
            if (!this->has("internal_solid_filament_id") || this->option("internal_solid_filament_id")->getInt() == 0)
                this->option("internal_solid_filament_id", true)->setInt(extruder);
            if (!this->has("top_surface_filament_id") || this->option("top_surface_filament_id")->getInt() == 0)
                this->option("top_surface_filament_id", true)->setInt(extruder);
            if (!this->has("bottom_surface_filament_id") || this->option("bottom_surface_filament_id")->getInt() == 0)
                this->option("bottom_surface_filament_id", true)->setInt(extruder);
            // Don't propagate the current extruder to support.
            // For non-soluble supports, the default "0" extruder means to use the active extruder,
            // for soluble supports one certainly does not want to set the extruder to non-soluble.
            // if (!this->has("support_filament"))
            //     this->option("support_filament", true)->setInt(extruder);
            // if (!this->has("support_interface_filament"))
            //     this->option("support_interface_filament", true)->setInt(extruder);
        }
    }

    if (this->has("sparse_infill_filament_id")) {
        int sparse_infill_filament_id = this->option("sparse_infill_filament_id")->getInt();
        if (sparse_infill_filament_id > 0 && (!this->has("internal_solid_filament_id") || this->option("internal_solid_filament_id")->getInt() == 0))
            this->option("internal_solid_filament_id", true)->setInt(sparse_infill_filament_id);
    }

    const int internal_solid = this->has("internal_solid_filament_id") ? this->option("internal_solid_filament_id")->getInt() : 0;
    const int top_surface    = this->has("top_surface_filament_id") ? this->option("top_surface_filament_id")->getInt() : 0;
    const int bottom_surface = this->has("bottom_surface_filament_id") ? this->option("bottom_surface_filament_id")->getInt() : 0;

    if (internal_solid == 0 && top_surface > 0)
        this->option("internal_solid_filament_id", true)->setInt(top_surface);
    if (internal_solid == 0 && bottom_surface > 0)
        this->option("internal_solid_filament_id", true)->setInt(bottom_surface);
    if (top_surface == 0 && internal_solid > 0)
        this->option("top_surface_filament_id", true)->setInt(internal_solid);
    if (bottom_surface == 0 && internal_solid > 0)
        this->option("bottom_surface_filament_id", true)->setInt(internal_solid);

    if (this->has("spiral_mode") && this->opt<ConfigOptionBool>("spiral_mode", true)->value) {
        {
            // this should be actually done only on the spiral layers instead of all
            auto* opt = this->opt<ConfigOptionBools>("retract_when_changing_layer", true);
            opt->values.assign(opt->values.size(), false);  // set all values to false
            // Disable retract on layer change also for filament overrides.
            auto* opt_n = this->opt<ConfigOptionBoolsNullable>("filament_retract_when_changing_layer", true);
            opt_n->values.assign(opt_n->values.size(), false);  // Set all values to false.
        }
        {
            this->opt<ConfigOptionInt>("wall_loops", true)->value       = 1;
            this->opt<ConfigOptionBool>("alternate_extra_wall", true)->value = false;
            this->opt<ConfigOptionInt>("top_shell_layers", true)->value = 0;
            this->opt<ConfigOptionPercent>("sparse_infill_density", true)->value = 0;
        }
    }

    if (auto *opt_gcode_resolution = this->opt<ConfigOptionFloat>("resolution", false); opt_gcode_resolution)
        // Resolution will be above 1um.
        opt_gcode_resolution->value = std::max(opt_gcode_resolution->value, 0.001);

    // BBS
    ConfigOptionBool* ept_opt = this->option<ConfigOptionBool>("enable_prime_tower");
    if (used_filaments > 0 && ept_opt != nullptr) {
        ConfigOptionBool* islh_opt = this->option<ConfigOptionBool>("independent_support_layer_height", true);
        //ConfigOptionBool* alh_opt = this->option<ConfigOptionBool>("adaptive_layer_height");
        ConfigOptionEnum<PrintSequence>* ps_opt = this->option<ConfigOptionEnum<PrintSequence>>("print_sequence");

        ConfigOptionEnum<TimelapseType>* timelapse_opt = this->option<ConfigOptionEnum<TimelapseType>>("timelapse_type");
        bool is_smooth_timelapse = timelapse_opt != nullptr && timelapse_opt->value == TimelapseType::tlSmooth;
        if (!is_smooth_timelapse && (used_filaments == 1 || ps_opt->value == PrintSequence::ByObject)) {
            ept_opt->value = false;
        }

        if (ept_opt->value) {
            if (islh_opt)
                islh_opt->value = false;
            //if (alh_opt)
            //    alh_opt->value = false;
        }
        /* BBS: MusangKing - not sure if this is still valid, just comment it out cause "Independent support layer height" is re-opened.
        else {
            if (islh_opt)
                islh_opt->value = true;
        }
        */
    }
}

//BBS:divide normalize_fdm to 2 steps and call them one by one in Print::Apply
void DynamicPrintConfig::normalize_fdm_1()
{
    if (this->has("extruder")) {
        int extruder = this->option("extruder")->getInt();
        this->erase("extruder");
        if (extruder != 0) {
            if (!this->has("sparse_infill_filament_id") || this->option("sparse_infill_filament_id")->getInt() == 0)
                this->option("sparse_infill_filament_id", true)->setInt(extruder);
            if (!this->has("outer_wall_filament_id") || this->option("outer_wall_filament_id")->getInt() == 0)
                this->option("outer_wall_filament_id", true)->setInt(extruder);
            if (!this->has("inner_wall_filament_id") || this->option("inner_wall_filament_id")->getInt() == 0)
                this->option("inner_wall_filament_id", true)->setInt(extruder);
            if (!this->has("internal_solid_filament_id") || this->option("internal_solid_filament_id")->getInt() == 0)
                this->option("internal_solid_filament_id", true)->setInt(extruder);
            if (!this->has("top_surface_filament_id") || this->option("top_surface_filament_id")->getInt() == 0)
                this->option("top_surface_filament_id", true)->setInt(extruder);
            if (!this->has("bottom_surface_filament_id") || this->option("bottom_surface_filament_id")->getInt() == 0)
                this->option("bottom_surface_filament_id", true)->setInt(extruder);
            // Don't propagate the current extruder to support.
            // For non-soluble supports, the default "0" extruder means to use the active extruder,
            // for soluble supports one certainly does not want to set the extruder to non-soluble.
            // if (!this->has("support_filament"))
            //     this->option("support_filament", true)->setInt(extruder);
            // if (!this->has("support_interface_filament"))
            //     this->option("support_interface_filament", true)->setInt(extruder);
        }
    }

    if (this->has("sparse_infill_filament_id")) {
        int sparse_infill_filament_id = this->option("sparse_infill_filament_id")->getInt();
        if (sparse_infill_filament_id > 0 && (!this->has("internal_solid_filament_id") || this->option("internal_solid_filament_id")->getInt() == 0))
            this->option("internal_solid_filament_id", true)->setInt(sparse_infill_filament_id);
    }

    const int internal_solid = this->has("internal_solid_filament_id") ? this->option("internal_solid_filament_id")->getInt() : 0;
    const int top_surface    = this->has("top_surface_filament_id") ? this->option("top_surface_filament_id")->getInt() : 0;
    const int bottom_surface = this->has("bottom_surface_filament_id") ? this->option("bottom_surface_filament_id")->getInt() : 0;

    if (internal_solid == 0 && top_surface > 0)
        this->option("internal_solid_filament_id", true)->setInt(top_surface);
    if (internal_solid == 0 && bottom_surface > 0)
        this->option("internal_solid_filament_id", true)->setInt(bottom_surface);
    if (top_surface == 0 && internal_solid > 0)
        this->option("top_surface_filament_id", true)->setInt(internal_solid);
    if (bottom_surface == 0 && internal_solid > 0)
        this->option("bottom_surface_filament_id", true)->setInt(internal_solid);

    if (this->has("spiral_mode") && this->opt<ConfigOptionBool>("spiral_mode", true)->value) {
        {
            // this should be actually done only on the spiral layers instead of all
            auto* opt = this->opt<ConfigOptionBools>("retract_when_changing_layer", true);
            opt->values.assign(opt->values.size(), false);  // set all values to false
            // Disable retract on layer change also for filament overrides.
            auto* opt_n = this->opt<ConfigOptionBoolsNullable>("filament_retract_when_changing_layer", true);
            opt_n->values.assign(opt_n->values.size(), false);  // Set all values to false.
        }
        {
            this->opt<ConfigOptionInt>("wall_loops", true)->value       = 1;
            this->opt<ConfigOptionBool>("alternate_extra_wall", true)->value = false;
            this->opt<ConfigOptionInt>("top_shell_layers", true)->value = 0;
            this->opt<ConfigOptionPercent>("sparse_infill_density", true)->value = 0;
        }
    }

    if (auto *opt_gcode_resolution = this->opt<ConfigOptionFloat>("resolution", false); opt_gcode_resolution)
        // Resolution will be above 1um.
        opt_gcode_resolution->value = std::max(opt_gcode_resolution->value, 0.001);

    return;
}

t_config_option_keys DynamicPrintConfig::normalize_fdm_2(int num_objects, int used_filaments)
{
    t_config_option_keys changed_keys;
    ConfigOptionBool* ept_opt = this->option<ConfigOptionBool>("enable_prime_tower");
    if (used_filaments > 0 && ept_opt != nullptr) {
        ConfigOptionBool* islh_opt = this->option<ConfigOptionBool>("independent_support_layer_height", true);
        //ConfigOptionBool* alh_opt = this->option<ConfigOptionBool>("adaptive_layer_height");
        ConfigOptionEnum<PrintSequence>* ps_opt = this->option<ConfigOptionEnum<PrintSequence>>("print_sequence");

        ConfigOptionEnum<TimelapseType>* timelapse_opt = this->option<ConfigOptionEnum<TimelapseType>>("timelapse_type");
        bool is_smooth_timelapse = timelapse_opt != nullptr && timelapse_opt->value == TimelapseType::tlSmooth;

        ConfigOptionBool *enable_wrapping_opt = this->option<ConfigOptionBool>("enable_wrapping_detection");
        bool enable_wrapping = enable_wrapping_opt != nullptr && enable_wrapping_opt->value;

        if (!is_smooth_timelapse && !enable_wrapping && (used_filaments == 1 || (ps_opt->value == PrintSequence::ByObject && num_objects > 1))) {
            if (ept_opt->value) {
                ept_opt->value = false;
                changed_keys.push_back("enable_prime_tower");
            }
            //ept_opt->value = false;
        }

        if (ept_opt->value) {
            if (islh_opt) {
                if (islh_opt->value) {
                    islh_opt->value = false;
                    changed_keys.push_back("independent_support_layer_height");
                }
                //islh_opt->value = false;
            }
            //if (alh_opt) {
            //    if (alh_opt->value) {
            //        alh_opt->value = false;
            //        changed_keys.push_back("adaptive_layer_height");
            //    }
            //    //alh_opt->value = false;
            //}
        }
        /* BBS：MusangKing - use "global->support->Independent support layer height" widget to replace previous assignment
        else {
            if (islh_opt) {
                if (!islh_opt->value) {
                    islh_opt->value = true;
                    changed_keys.push_back("independent_support_layer_height");
                }
                //islh_opt->value = true;
            }
        }
        */
    }

    return changed_keys;
}

void  handle_legacy_sla(DynamicPrintConfig &config)
{
    for (std::string corr : {"relative_correction", "material_correction"}) {
        if (config.has(corr)) {
            if (std::string corr_x = corr + "_x"; !config.has(corr_x)) {
                auto* opt = config.opt<ConfigOptionFloat>(corr_x, true);
                opt->value = config.opt<ConfigOptionFloats>(corr)->values[0];
            }

            if (std::string corr_y = corr + "_y"; !config.has(corr_y)) {
                auto* opt = config.opt<ConfigOptionFloat>(corr_y, true);
                opt->value = config.opt<ConfigOptionFloats>(corr)->values[0];
            }

            if (std::string corr_z = corr + "_z"; !config.has(corr_z)) {
                auto* opt = config.opt<ConfigOptionFloat>(corr_z, true);
                opt->value = config.opt<ConfigOptionFloats>(corr)->values[1];
            }
        }
    }
}

size_t DynamicPrintConfig::get_parameter_size(const std::string& param_name, size_t extruder_nums)
{
    constexpr size_t default_param_length = 1;
    size_t filament_variant_length = default_param_length;
    size_t process_variant_length = default_param_length;
    size_t machine_variant_length = default_param_length;

    if (this->has("filament_extruder_variant"))
        filament_variant_length = this->option<ConfigOptionStrings>("filament_extruder_variant")->size();
    if (this->has("print_extruder_variant"))
        process_variant_length = this->option<ConfigOptionStrings>("print_extruder_variant")->size();
    if (this->has("printer_extruder_variant"))
        machine_variant_length = this->option<ConfigOptionStrings>("printer_extruder_variant")->size();

    if (printer_options_with_variant_1.count(param_name) > 0) {
        return machine_variant_length;
    }
    else if (printer_options_with_variant_2.count(param_name) > 0) {
        return machine_variant_length * 2;
    }
    else if (filament_options_with_variant.count(param_name) > 0) {
        return filament_variant_length;
    }
    else if (print_options_with_variant.count(param_name) > 0) {
        return process_variant_length;
    }
    return extruder_nums;
}

// Orca: Special handling for extruder variants
// BBL printers have extruder variants pre-defined in system profiles, however for customized multi-extruder profile,
// we need to set up these parameters automatically, otherwise per-extruder options won't work properly.
static void extend_extruder_variant(DynamicPrintConfig& config, const unsigned int num_extruders)
{
    // 1. Make sure the `extruder_variant_list` is the same length as extruder cnt
    if (!config.has("extruder_variant_list")) {
        config.set_key_value("extruder_variant_list",
                             new ConfigOptionStrings(std::vector<std::string>(num_extruders, "Direct Drive Standard")));
    }
    auto extruder_variant_opt = dynamic_cast<ConfigOptionStrings*>(config.option("extruder_variant_list"));
    assert(extruder_variant_opt != nullptr);
    extruder_variant_opt->resize(num_extruders, extruder_variant_opt); // Use the first option as the default value, so all extruders have the same variant

    // 2. Update `printer_extruder_variant` and `printer_extruder_id` based on `extruder_variant_list`
    auto printer_extruder_id_opt = dynamic_cast<ConfigOptionInts*>(config.option("printer_extruder_id"));
    assert(printer_extruder_id_opt != nullptr);
    printer_extruder_id_opt->values.clear();
    auto printer_extruder_variant_opt = dynamic_cast<ConfigOptionStrings*>(config.option("printer_extruder_variant"));
    assert(printer_extruder_variant_opt != nullptr);
    printer_extruder_variant_opt->values.clear();
    for (int i = 0; i < num_extruders; i++) {
        // `extruder_variant_list` specifies supported variant of each nozzle/extruder,
        // each item is a comma separated list of variants (extruder type + nozzle flow type) this extruder supported
        std::string variant = extruder_variant_opt->get_at(i);
        std::vector<std::string> variants_list;
        boost::split(variants_list, variant, boost::is_any_of(","), boost::token_compress_on);

        if (!variants_list.empty()) {
            printer_extruder_id_opt->values.insert(printer_extruder_id_opt->values.end(), variants_list.size(), i + 1);
            printer_extruder_variant_opt->values.insert(printer_extruder_variant_opt->values.end(), variants_list.begin(), variants_list.end());
        }
    }
}

void DynamicPrintConfig::set_num_extruders(unsigned int num_extruders)
{
    extend_extruder_variant(*this, num_extruders);

    const auto &defaults = FullPrintConfig::defaults();
    for (const std::string &key : print_config_def.extruder_option_keys()) {
        if (key == "default_filament_profile")
            // Don't resize this field, as it is presented to the user at the "Dependencies" page of the Printer profile and we don't want to present
            // empty fields there, if not defined by the system profile.
            continue;
        auto *opt = this->option(key, false);
        assert(opt != nullptr);
        assert(opt->is_vector());
        if (opt != nullptr && opt->is_vector()) {
            static_cast<ConfigOptionVectorBase*>(opt)->resize(get_parameter_size(key, num_extruders), defaults.option(key));
        }
    }
}

// BBS
void DynamicPrintConfig::set_num_filaments(unsigned int num_filaments)
{
    const auto& defaults = FullPrintConfig::defaults();
    for (const std::string& key : print_config_def.filament_option_keys()) {
        if (key == "default_filament_profile")
            // Don't resize this field, as it is presented to the user at the "Dependencies" page of the Printer profile and we don't want to present
            // empty fields there, if not defined by the system profile.
            continue;
        auto* opt = this->option(key, false);
        assert(opt != nullptr);
        assert(opt->is_vector());
        if (opt != nullptr && opt->is_vector())
            static_cast<ConfigOptionVectorBase*>(opt)->resize(num_filaments, defaults.option(key));
    }
}

//BBS: pass map to recording all invalid valies
std::map<std::string, std::string> DynamicPrintConfig::validate(bool under_cli)
{
    // Full print config is initialized from the defaults.
    const ConfigOption *opt = this->option("printer_technology", false);
    auto printer_technology = (opt == nullptr) ? ptFFF : static_cast<PrinterTechnology>(dynamic_cast<const ConfigOptionEnumGeneric*>(opt)->value);
    switch (printer_technology) {
    case ptFFF:
    {
        FullPrintConfig fpc;
        fpc.apply(*this, true);
        // Verify this print options through the FullPrintConfig.
        return Slic3r::validate(fpc, under_cli);
    }
    default:
        //FIXME no validation on SLA data?
        return std::map<std::string, std::string>();
    }
}

std::string DynamicPrintConfig::get_filament_type(std::string &displayed_filament_type, int id)
{
    auto* filament_id = dynamic_cast<const ConfigOptionStrings*>(this->option("filament_id"));
    auto* filament_type = dynamic_cast<const ConfigOptionStrings*>(this->option("filament_type"));
    auto* filament_is_support = dynamic_cast<const ConfigOptionBools*>(this->option("filament_is_support"));

    if (!filament_type)
        return "";

    if (!filament_is_support) {
        if (filament_type) {
            displayed_filament_type = filament_type->get_at(id);
            return filament_type->get_at(id);
        }
        else {
            displayed_filament_type = "";
            return "";
        }
    }
    else {
        bool is_support = filament_is_support ? filament_is_support->get_at(id) : false;
        if (is_support) {
            if (filament_id) {
                if (filament_id->get_at(id) == "GFS00") {
                    displayed_filament_type = "Sup.PLA";
                    return "PLA-S";
                }
                else if (filament_id->get_at(id) == "GFS01") {
                    displayed_filament_type = "Sup.PA";
                    return "PA-S";
                }
                else {
                    if (filament_type->get_at(id) == "PLA") {
                        displayed_filament_type = "Sup.PLA";
                        return "PLA-S";
                    }
                    else if (filament_type->get_at(id) == "PA") {
                        displayed_filament_type = "Sup.PA";
                        return "PA-S";
                    }
                    else {
                        displayed_filament_type = filament_type->get_at(id);
                        return filament_type->get_at(id);
                    }
                }
            }
            else {
                if (filament_type->get_at(id) == "PLA") {
                    displayed_filament_type = "Sup.PLA";
                    return "PLA-S";
                } else if (filament_type->get_at(id) == "PA") {
                    displayed_filament_type = "Sup.PA";
                    return "PA-S";
                } else {
                    displayed_filament_type = filament_type->get_at(id);
                    return filament_type->get_at(id);
                }
            }
        }
        else {
            displayed_filament_type = filament_type->get_at(id);
            return filament_type->get_at(id);
        }
    }
    return "PLA";
}

bool DynamicPrintConfig::is_using_different_extruders()
{
    bool ret = false;

    auto nozzle_diameters_opt = dynamic_cast<const ConfigOptionFloats*>(this->option("nozzle_diameter"));
    if (nozzle_diameters_opt != nullptr) {
        int size = nozzle_diameters_opt->size();
        if (size > 1) {
            auto extruder_type_opt = dynamic_cast<const ConfigOptionEnumsGeneric*>(this->option("extruder_type"));
            auto nozzle_volume_type_opt = dynamic_cast<const ConfigOptionEnumsGeneric*>(this->option("nozzle_volume_type"));
            if (extruder_type_opt && nozzle_volume_type_opt) {
                ExtruderType extruder_type = (ExtruderType)(extruder_type_opt->get_at(0));
                NozzleVolumeType nozzle_volume_type = (NozzleVolumeType)(nozzle_volume_type_opt->get_at(0));
                for (int index = 1; index < size; index++)
                {
                    ExtruderType extruder_type_1 = (ExtruderType)(extruder_type_opt->get_at(index));
                    NozzleVolumeType nozzle_volume_type_1 = (NozzleVolumeType)(nozzle_volume_type_opt->get_at(index));
                    if ((extruder_type_1 != extruder_type) || (nozzle_volume_type_1 != nozzle_volume_type)) {
                        ret = true;
                        break;
                    }
                }
            }
        }
    }

    return ret;
}

bool DynamicPrintConfig::support_different_extruders(int& extruder_count) const
{
    std::set<std::string> variant_set;

    auto nozzle_diameters_opt = dynamic_cast<const ConfigOptionFloats*>(this->option("nozzle_diameter"));
    if (nozzle_diameters_opt != nullptr) {
        int size = nozzle_diameters_opt->size();
        extruder_count = size;
        auto extruder_variant_opt = dynamic_cast<const ConfigOptionStrings*>(this->option("extruder_variant_list"));
        if (extruder_variant_opt != nullptr) {
            for (int index = 0; index < size; index++) {
                std::string variant = extruder_variant_opt->get_at(index);
                std::vector<std::string> variants_list;
                boost::split(variants_list, variant, boost::is_any_of(","), boost::token_compress_on);
                if (!variants_list.empty())
                    variant_set.insert(variants_list.begin(), variants_list.end());
            }
        }
    }

    return (variant_set.size() > 1);
}

int DynamicPrintConfig::get_index_for_extruder(int extruder_or_filament_id, std::string id_name, ExtruderType extruder_type, NozzleVolumeType nozzle_volume_type, std::string variant_name, unsigned int stride) const
{
    int ret = -1;

    auto variant_opt = dynamic_cast<const ConfigOptionStrings*>(this->option(variant_name));
    const ConfigOptionInts* id_opt = id_name.empty()?nullptr: dynamic_cast<const ConfigOptionInts*>(this->option(id_name));
    const ConfigOptionStrings* extruder_variant_list_opt = dynamic_cast<const ConfigOptionStrings*>(this->option("extruder_variant_list"));
    auto generated_extruder_id = [extruder_variant_list_opt](int target_index) {
        if (!extruder_variant_list_opt)
            return 0;

        int variant_index = 0;
        for (int extruder_index = 0; extruder_index < int(extruder_variant_list_opt->values.size()); ++extruder_index) {
            std::vector<std::string> variants_list;
            boost::split(variants_list, extruder_variant_list_opt->get_at(extruder_index), boost::is_any_of(","), boost::token_compress_on);
            for (std::string variant : variants_list) {
                boost::trim(variant);
                if (variant.empty())
                    continue;
                if (variant_index == target_index)
                    return extruder_index + 1;
                ++variant_index;
            }
        }
        return 0;
    };

    if (variant_opt != nullptr) {
        int v_size = variant_opt->values.size();
        const bool has_complete_id_map = id_opt && int(id_opt->values.size()) >= v_size;
        // nvtHybrid not supported in presets, switch to nvtStandard to match the preset values
        if (nozzle_volume_type == nvtHybrid)
            nozzle_volume_type = nvtStandard;
        std::string extruder_variant = get_extruder_variant_string(extruder_type, nozzle_volume_type);
        for (int index = 0; index < v_size; index++)
        {
            const std::string variant = variant_opt->get_at(index);
            if (extruder_variant == variant) {
                if (id_opt) {
                    const int id = has_complete_id_map ? id_opt->get_at(index) : generated_extruder_id(index);
                    if (id == extruder_or_filament_id) {
                        ret = index * stride;
                        break;
                    }
                }
                else {
                    ret = index * stride;
                    break;
                }

            }
        }
    }
    return ret;
}

//only used for cli
//update values in single extruder process config to values in multi-extruder process
//limit the new values
int DynamicPrintConfig::update_values_from_single_to_multi(DynamicPrintConfig& multi_config, std::set<std::string>& key_set, std::string id_name, std::string variant_name)
{
    auto print_variant_opt = dynamic_cast<const ConfigOptionStrings*>(multi_config.option(variant_name));
    if (!print_variant_opt) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%:%2%, can not get %3% from config")%__FUNCTION__ %__LINE__ % variant_name;
        return -1;
    }
    int variant_count = print_variant_opt->size();

    const ConfigDef  *config_def     = this->def();
    if (!config_def) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", Line %1%: can not find config define")%__LINE__;
        return -1;
    }
    for (auto& key: key_set)
    {
        const ConfigOptionDef *optdef  = config_def->get(key);
        if (!optdef) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: can not find opt define for %2%")%__LINE__%key;
            continue;
        }
        switch (optdef->type) {
            case coStrings:
            {
                ConfigOptionStrings* src_opt = multi_config.option<ConfigOptionStrings>(key);
                if (src_opt) {
                    ConfigOptionStrings* opt = this->option<ConfigOptionStrings>(key, true);

                    opt->values = src_opt->values;
                }
                break;
            }
            case coInts:
            {
                ConfigOptionInts* src_opt = multi_config.option<ConfigOptionInts>(key);
                if (src_opt) {
                    ConfigOptionInts* opt = this->option<ConfigOptionInts>(key, true);

                    opt->values = src_opt->values;
                }
                break;
            }
            case coFloats:
            {
                ConfigOptionFloats * src_opt = multi_config.option<ConfigOptionFloats>(key);
                if (src_opt) {
                    ConfigOptionFloats * opt = this->option<ConfigOptionFloats>(key, true);

                    assert(variant_count == src_opt->size());
                    opt->resize(variant_count, opt);

                    for (int index = 0; index < variant_count; index++)
                    {
                        if (opt->values[index] > src_opt->values[index])
                            opt->values[index] = src_opt->values[index];
                    }
                }
                break;
            }
            case coFloatsOrPercents:
            {
                ConfigOptionFloatsOrPercents * src_opt = multi_config.option<ConfigOptionFloatsOrPercents>(key);
                if (src_opt) {
                    ConfigOptionFloatsOrPercents * opt = this->option<ConfigOptionFloatsOrPercents>(key, true);

                    assert(variant_count == src_opt->size());
                    opt->resize(variant_count, opt);

                    for (int index = 0; index < variant_count; index++)
                    {
                        if (opt->values[index].value > src_opt->values[index].value)
                            opt->values[index] = src_opt->values[index];
                    }
                }
                break;
            }
            case coBools:
            {
                ConfigOptionBools * src_opt = multi_config.option<ConfigOptionBools>(key);
                if (src_opt)
                {
                    ConfigOptionBools * opt = this->option<ConfigOptionBools>(key, true);

                    assert(variant_count == src_opt->size());
                    opt->resize(variant_count, opt);
                }

                break;
            }
            default:
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: unsupported option type for %2%")%__LINE__%key;
                break;
        }
    }

    return 0;
}

//used for object/region config
//duplicate single to multiple
/*int DynamicPrintConfig::update_values_from_single_to_multi_2(DynamicPrintConfig& multi_config, std::set<std::string>& key_set)
{
    const ConfigDef  *config_def     = this->def();
    if (!config_def) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", Line %1%: can not find config define")%__LINE__;
        return -1;
    }

    t_config_option_keys keys = this->keys();
    for (auto& key: keys)
    {
        if (key_set.find(key) == key_set.end())
            continue;

        const ConfigOptionDef *optdef  = config_def->get(key);
        if (!optdef) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: can not find opt define for %2%")%__LINE__%key;
            continue;
        }
        switch (optdef->type) {
            case coFloats:
            {
                ConfigOptionFloatsNullable * opt = this->option<ConfigOptionFloatsNullable>(key);
                ConfigOptionFloatsNullable* src_opt = multi_config.option<ConfigOptionFloatsNullable>(key);

                if (src_opt && !opt->is_nil(0))
                    opt->values.resize(src_opt->size(), opt->values[0]);
                break;
            }
            case coFloatsOrPercents:
            {
                ConfigOptionFloatsOrPercentsNullable* opt = this->option<ConfigOptionFloatsOrPercentsNullable>(key);
                ConfigOptionFloatsOrPercentsNullable* src_opt = multi_config.option<ConfigOptionFloatsOrPercentsNullable>(key);

                if (src_opt &&!opt->is_nil(0))
                    opt->values.resize(src_opt->size(), opt->values[0]);
                break;
            }
            case coBools:
            {
                ConfigOptionBoolsNullable* opt = this->option<ConfigOptionBoolsNullable>(key);
                ConfigOptionBoolsNullable* src_opt = multi_config.option<ConfigOptionBoolsNullable>(key);

                if (src_opt &&!opt->is_nil(0))
                    opt->values.resize(src_opt->size(), opt->values[0]);

                break;
            }
            default:
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: unsupported option type for %2%")%__LINE__%key;
                break;
        }
    }

    return 0;
}*/

//update global process config for multi variant to multi variant case
//1. skip the key-values not in key_set
//2. update the key-value to the new one, then check whether the old one with the same variant can be used or not
int DynamicPrintConfig::update_values_from_multi_to_multi(DynamicPrintConfig& new_config, std::set<std::string>& key_set, std::string id_name, std::string variant_name, std::vector<std::string>& new_extruder_variants)
{
    int new_extruder_count = new_extruder_variants.size();
    std::vector<int> new_variant_indices(new_extruder_count, -1);

    auto print_variant_opt = dynamic_cast<const ConfigOptionStrings*>(this->option(variant_name));
    auto new_variant_opt = dynamic_cast<const ConfigOptionStrings*>(new_config.option(variant_name));
    auto new_print_id_opt = dynamic_cast<const ConfigOptionInts*>(new_config.option(id_name));
    if (!print_variant_opt || !new_variant_opt || !new_print_id_opt) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%:%2%, can not get variant %3%, id %4% from config")%__FUNCTION__ %__LINE__ % variant_name  % id_name;
        return -1;
    }
    int variant_count = print_variant_opt->size(), new_variant_count = new_variant_opt->size();

    std::vector<std::vector<int>> extruder_variant_indices;
    for (int i = 0; i < new_extruder_count; i++)
    {
        std::vector<int> variant_indices;
        for (int j = 0; j < variant_count; j++)
        {
            if (new_extruder_variants[i] == print_variant_opt->values[j]) {
                variant_indices.push_back(j);
            }
        }

        if (variant_indices.empty())
        {
            //can not find any
            variant_indices.resize(variant_count, 0);
            for (int j = 0; j < variant_count; j++)
                variant_indices[j] = j;
        }
        extruder_variant_indices.emplace_back(variant_indices);
    }

    for (int i = 0; i < new_extruder_count; i++)
    {
        for (int j = 0; j < new_variant_count; j++)
        {
            if ((i+1 == new_print_id_opt->values[j]) && (new_extruder_variants[i] == new_variant_opt->values[j])) {
                new_variant_indices[i] = j;
                break;
            }
        }
    }

    const ConfigDef* config_def = this->def();
    if (!config_def) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", Line %1%: can not find config define") % __LINE__;
        return -1;
    }
    for (auto& key : key_set)
    {
        const ConfigOptionDef* optdef = config_def->get(key);
        if (!optdef) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: can not find opt define for %2%") % __LINE__ % key;
            continue;
        }
        switch (optdef->type) {
        case coStrings:
        {
            ConfigOptionStrings* src_opt = new_config.option<ConfigOptionStrings>(key);
            if (src_opt) {
                ConfigOptionStrings* opt = this->option<ConfigOptionStrings>(key, true);

                //assert(variant_count == opt->size());
                opt->values = src_opt->values;
            }
            break;
        }
        case coInts:
        {
            ConfigOptionInts* src_opt = new_config.option<ConfigOptionInts>(key);
            if (src_opt) {
                ConfigOptionInts* opt = this->option<ConfigOptionInts>(key, true);

                //assert(variant_count == opt->size());
                opt->values = src_opt->values;
            }
            break;
        }
        case coFloats:
        {
            ConfigOptionFloats* src_opt = new_config.option<ConfigOptionFloats>(key);
            if (src_opt) {
                ConfigOptionFloats* opt = this->option<ConfigOptionFloats>(key, true);

                std::vector<double> old_values = opt->values;
                int old_count = old_values.size();
                int new_count = src_opt->values.size();

                assert(variant_count == old_count);
                assert(new_variant_count == new_count);
                opt->values = src_opt->values;

                for (int i = 0; i < new_extruder_count; i++)
                {
                    std::vector<int>& variant_indices = extruder_variant_indices[i];
                    int new_variant_index = new_variant_indices[i];
                    if ((new_variant_index == -1) || variant_indices.empty())
                        continue;

                    for(auto idx : variant_indices){
                        assert(idx < old_count);
                        if (old_values[idx] < opt->values[new_variant_index])
                            opt->values[new_variant_index] = old_values[idx];
                    }
                }
            }
            break;
        }
        case coFloatsOrPercents:
        {
            ConfigOptionFloatsOrPercents* src_opt = new_config.option<ConfigOptionFloatsOrPercents>(key);
            if (src_opt) {
                ConfigOptionFloatsOrPercents* opt = this->option<ConfigOptionFloatsOrPercents>(key, true);

                std::vector<FloatOrPercent> old_values = opt->values;
                int old_count = old_values.size();
                int new_count = src_opt->values.size();

                assert(variant_count == old_count);
                assert(new_variant_count == new_count);
                opt->values = src_opt->values;

                for (int i = 0; i < new_extruder_count; i++)
                {
                    std::vector<int>& variant_indices = extruder_variant_indices[i];
                    int new_variant_index = new_variant_indices[i];
                    if ((new_variant_index == -1) || variant_indices.empty())
                        continue;

                    for(auto idx : variant_indices){
                        assert(idx < old_count);
                        if (old_values[idx] < opt->values[new_variant_index])
                            opt->values[new_variant_index] = old_values[idx];
                    }
                }
            }
            break;
        }
        case coBools:
        {
            ConfigOptionBools* src_opt = new_config.option<ConfigOptionBools>(key);
            if (src_opt) {
                ConfigOptionBools* opt = this->option<ConfigOptionBools>(key, true);

                std::vector<unsigned char> old_values = opt->values;
                int old_count = old_values.size();
                int new_count = src_opt->values.size();

                assert(variant_count == old_count);
                assert(new_variant_count == new_count);
                opt->values = src_opt->values;

                for (int i = 0; i < new_extruder_count; i++)
                {
                    std::vector<int>& variant_indices = extruder_variant_indices[i];
                    int new_variant_index = new_variant_indices[i];
                    if ((new_variant_index == -1) || variant_indices.empty())
                        continue;

                    for(auto idx : variant_indices){
                        assert(idx < old_count);
                        if (old_values[idx]) //enabled
                            opt->values[new_variant_index] = old_values[idx];
                    }
                }
            }

            break;
        }
        default:
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: unsupported option type for %2%") % __LINE__ % key;
            break;
        }
    }

    return 0;
}

int DynamicPrintConfig::update_values_from_multi_to_multi_2(const std::vector<std::string>& src_extruder_variants, const std::vector<std::string>& dst_extruder_variants, const DynamicPrintConfig& dst_config, const std::set<std::string>& key_sets)
{
    const ConfigDef  *config_def     = this->def();
    if (!config_def) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", Line %1%: can not find config define")%__LINE__;
        return -1;
    }

    auto get_same_variant_indices = [](const std::vector<std::string>& extruder_variants, const std::string& variant){
        std::vector<int> indices;
        for(int i=0;i<extruder_variants.size();++i)
            if(extruder_variants[i] == variant)
                indices.push_back(i);
        return indices;
    };

    std::vector<std::vector<int>> same_variant_indices;
    for(size_t dst_idx =0 ;dst_idx < dst_extruder_variants.size(); ++dst_idx){
        auto& dst_variant = dst_extruder_variants[dst_idx];
        auto indices =get_same_variant_indices(src_extruder_variants, dst_variant);
        same_variant_indices.emplace_back(indices);
    }

    t_config_option_keys keys = this->keys();
    for(auto& key : keys){
        if(key_sets.find(key) == key_sets.end())
            continue;
        const ConfigOptionDef* optdef = config_def->get(key);
        if(!optdef){
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: can not find opt define for %2%")%__LINE__%key;
            continue;
        }

        switch (optdef->type){
            case coFloats:
            {
                ConfigOptionFloatsNullable* opt = this->option<ConfigOptionFloatsNullable>(key);
                auto src_values = opt->values;
                auto dst_values = dst_config.option<ConfigOptionFloatsNullable>(key) ->values;
                for(size_t dst_idx =0; dst_idx < same_variant_indices.size(); ++dst_idx){
                    auto& indices = same_variant_indices[dst_idx];
                    if(indices.empty())
                        continue;
                    bool has_value = false;
                    double target_value = std::numeric_limits<double>::max();
                    for(auto idx : indices){
                        if(opt && idx < opt->values.size() && !opt->is_nil(idx)){
                            has_value = true;
                            target_value = std::min(target_value, src_values[idx]);
                        }
                    }

                    if(has_value)
                        dst_values[dst_idx] = target_value;
                }
                opt->values = dst_values;
                break;
            }
            case coFloatsOrPercents:
            {
                ConfigOptionFloatsOrPercentsNullable* opt = this->option<ConfigOptionFloatsOrPercentsNullable>(key);
                auto src_values = opt->values;
                auto dst_values = dst_config.option<ConfigOptionFloatsOrPercentsNullable>(key) ->values;
                for(size_t dst_idx =0; dst_idx < same_variant_indices.size(); ++dst_idx){
                    auto& indices = same_variant_indices[dst_idx];
                    if(indices.empty())
                        continue;
                    bool has_value = false;
                    FloatOrPercent target_value{9999.f, true};
                    for(auto idx : indices){
                        if(opt && !opt->is_nil(idx)){
                            has_value = true;
                            target_value = src_values[idx].value < target_value.value ? src_values[idx] : target_value;
                        }
                    }

                    if(has_value)
                        dst_values[dst_idx] = target_value;
                }
                opt->values = dst_values;
                break;
            }
            case coBools:
            {
                ConfigOptionBoolsNullable* opt = this->option<ConfigOptionBoolsNullable>(key);
                auto src_values = opt->values;
                auto dst_values = dst_config.option<ConfigOptionBoolsNullable>(key) ->values;
                for(size_t dst_idx =0; dst_idx < same_variant_indices.size(); ++dst_idx){
                    auto indices = same_variant_indices[dst_idx];
                    if(indices.empty())
                        continue;
                    bool has_value = false;
                    bool target_value;
                    for(auto idx : indices){
                        if(opt && !opt->is_nil(idx)){
                            has_value = true;
                            target_value = src_values[idx];
                            break;
                        }
                    }

                    if(has_value)
                        dst_values[dst_idx] = target_value;
                }

                opt->values = dst_values;
                break;
            }
            default:
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: unsupported option type for %2%")%__LINE__%key;
                break;
        }

    }

    return 0;

}


//used for object/region config
//use the smallest of multiple to single
/*int DynamicPrintConfig::update_values_from_multi_to_single_2(std::set<std::string>& key_set)
{
    const ConfigDef  *config_def     = this->def();
    if (!config_def) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", Line %1%: can not find config define")%__LINE__;
        return -1;
    }

    t_config_option_keys keys = this->keys();
    for (auto& key: keys)
    {
        if (key_set.find(key) == key_set.end())
            continue;

        const ConfigOptionDef *optdef  = config_def->get(key);
        if (!optdef) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: can not find opt define for %2%")%__LINE__%key;
            continue;
        }
        switch (optdef->type) {
            case coFloats:
            {
                ConfigOptionFloatsNullable* opt = this->option<ConfigOptionFloatsNullable>(key);
                double min = 9999.0;
                bool has_value = false;

                for (int index = 0; index < opt->values.size(); index++)
                {
                    if (!opt->is_nil(index) && (opt->values[index] < min)) {
                        min = opt->values[index];
                        has_value = true;
                    }
                }

                opt->values.erase(opt->values.begin() + 1, opt->values.end());
                if (has_value)
                    opt->values[0] = min;
                break;
            }
            case coFloatsOrPercents:
            {
                ConfigOptionFloatsOrPercentsNullable * opt = this->option<ConfigOptionFloatsOrPercentsNullable>(key);
                FloatOrPercent min{9999.f, true};
                bool has_value = false;

                for (int index = 0; index < opt->values.size(); index++)
                {
                    if (!opt->is_nil(index) && (opt->values[index].value < min.value)) {
                        min = opt->values[index];
                        has_value = true;
                    }
                }

                opt->values.erase(opt->values.begin() + 1, opt->values.end());
                if (has_value)
                    opt->values[0] = min;
                break;
            }
            case coBools:
            {
                ConfigOptionBoolsNullable* opt = this->option<ConfigOptionBoolsNullable>(key);

                bool min, has_value = false;
                for (int index = 0; index < opt->values.size(); index++)
                {
                    if (!opt->is_nil(index)) {
                        min = opt->values[index];
                        has_value = true;
                        break;
                    }
                }

                opt->values.erase(opt->values.begin() + 1, opt->values.end());
                if (has_value)
                    opt->values[0] = min;
                break;
            }
            default:
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: unsupported option type for %2%")%__LINE__%key;
                break;
        }
    }

    return 0;
}*/

std::string
DynamicPrintConfig::get_filament_vendor() const
{
    const ConfigOptionStrings* opt = dynamic_cast<const ConfigOptionStrings*> (option("filament_vendor"));
    if (opt && !opt->values.empty())
    {
        return opt->values[0];
    }

    return std::string();
}


std::string
DynamicPrintConfig::get_filament_type() const
{
    const ConfigOptionStrings* opt = dynamic_cast<const ConfigOptionStrings*> (option("filament_type"));
    if (opt && !opt->values.empty())
    {
        return opt->values[0];
    }

    return std::string();
}

int DynamicPrintConfig::get_extruder_nozzle_volume_count(int extruder_count, std::vector<std::vector<NozzleVolumeType>>& nozzle_volume_types) const
{
    int count = extruder_count;
    auto opt_extruder_nozzle_stats = dynamic_cast<const ConfigOptionStrings*>(this->option("extruder_nozzle_stats"));
    nozzle_volume_types.resize(extruder_count, std::vector<NozzleVolumeType>{});
    if (opt_extruder_nozzle_stats && (int(opt_extruder_nozzle_stats->values.size()) == extruder_count)) {
        std::vector<std::map<NozzleVolumeType, int>> extruder_nozzle_counts = get_extruder_nozzle_stats(opt_extruder_nozzle_stats->values);
        count = 0;
        for (int i = 0; i < extruder_count; i++)
        {
            count += extruder_nozzle_counts[i].size();
            // std::map iteration order gives the canonical slot order: ascending NozzleVolumeType
            for (auto& iter : extruder_nozzle_counts[i])
                nozzle_volume_types[i].push_back(iter.first);
        }
    }
    return count;
}

// Orca: BBL system profiles ship full-width print_extruder_id/print_extruder_variant columns, but
// custom multi-extruder printers only ever get the machine-scope columns synthesized for them (see
// extend_extruder_variant); the process scope keeps the length-1 defaults, both in presets and in
// 3mf project configs. Expanding with that degenerate map makes every per-extruder lookup fail, and
// because both keys are themselves in print_options_with_variant, the expansion then latches a
// full-width-but-wrong [1,1,...] map that also defeats the generated_extruder_id fallback in
// get_index_for_extruder. Synthesize the process columns from the printer's extruder_variant_list
// (same token walk as extend_extruder_variant) before expanding.
static void ensure_process_variant_columns(DynamicPrintConfig &config, const DynamicPrintConfig &printer_config)
{
    auto id_opt      = dynamic_cast<ConfigOptionInts *>(config.option("print_extruder_id"));
    auto variant_opt = dynamic_cast<ConfigOptionStrings *>(config.option("print_extruder_variant"));
    auto list_opt    = dynamic_cast<const ConfigOptionStrings *>(printer_config.option("extruder_variant_list"));
    if (!id_opt || !variant_opt || !list_opt)
        return;
    if (id_opt->values.size() != 1 || variant_opt->values.size() != 1)
        return;

    std::vector<int>         ids;
    std::vector<std::string> variants;
    for (int i = 0; i < int(list_opt->values.size()); ++i) {
        std::vector<std::string> tokens;
        boost::split(tokens, list_opt->get_at(i), boost::is_any_of(","), boost::token_compress_on);
        for (std::string &token : tokens) {
            boost::trim(token);
            if (token.empty())
                continue;
            ids.push_back(i + 1);
            variants.push_back(token);
        }
    }
    // A single column is the legitimate single-extruder layout, not a degenerate one.
    if (ids.size() <= 1)
        return;
    id_opt->values      = std::move(ids);
    variant_opt->values = std::move(variants);
}

std::vector<int> DynamicPrintConfig::update_values_to_printer_extruders(DynamicPrintConfig& printer_config, int extruder_count, int extruder_nozzle_volume_count, std::vector<std::vector<NozzleVolumeType>>& nv_types,
    std::set<std::string>& key_set, std::string id_name, std::string variant_name, unsigned int stride, unsigned int extruder_id, NozzleVolumeType filament_nvt)
{
    std::vector<int> variant_index;
    int variant_count = extruder_count;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: extruder_count %2%, extruder_nozzle_volume_count %3%")%__LINE__ %extruder_count %extruder_nozzle_volume_count;

    auto opt_extruder_type = dynamic_cast<const ConfigOptionEnumsGeneric*>(printer_config.option("extruder_type"));
    auto opt_nozzle_volume_type = dynamic_cast<const ConfigOptionEnumsGeneric*>(printer_config.option("nozzle_volume_type"));
    if (!opt_extruder_type || !opt_nozzle_volume_type) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: extruder_type or nozzle_volume_type option not found, skipping")%__LINE__;
        return variant_index;
    }

    if (extruder_id > 0 && extruder_id <= static_cast<unsigned> (extruder_count)) {
        variant_index.resize(1);
        ExtruderType extruder_type = (ExtruderType)(opt_extruder_type->get_at(extruder_id - 1));
        NozzleVolumeType nozzle_volume_type = (NozzleVolumeType)(opt_nozzle_volume_type->get_at(extruder_id - 1));

        if (nozzle_volume_type == nvtHybrid) {
            // a mixed-nozzle extruder has no preset column of its own: the filament's concrete
            // volume type selects the slot
            nozzle_volume_type = filament_nvt;
        }
        else if (nozzle_volume_type != filament_nvt) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__
                                    << boost::format(", Line %1%: nozzle_volume_type is %2%,  not equal to filament_nvt %3%") % __LINE__ % nozzle_volume_type % filament_nvt;
        }

        //variant index
        variant_index[0] = get_index_for_extruder(extruder_id, id_name, extruder_type, nozzle_volume_type, variant_name);

        if (variant_index[0] < 0) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", Line %1%: could not found extruder_type %2%, nozzle_volume_type %3%, for filament")
                % __LINE__ % s_keys_names_ExtruderType[extruder_type] % s_keys_names_NozzleVolumeType[nozzle_volume_type];
        }

        variant_count = 1;
    }
    else {
        if (id_name == "print_extruder_id")
            ensure_process_variant_columns(*this, printer_config);
        // Orca: emit the slots first, then size variant_count from what was actually
        // emitted. extruder_nozzle_volume_count only equals the emitted total when every
        // extruder carries per-type stats; an extruder with an empty stats entry combined
        // with a Hybrid extruder would otherwise overrun a table pre-sized from that count.
        for (int e_index = 0; e_index < extruder_count; e_index++)
        {
            ExtruderType extruder_type = (ExtruderType)(opt_extruder_type->get_at(e_index));
            NozzleVolumeType nozzle_volume_type = (NozzleVolumeType)(opt_nozzle_volume_type->get_at(e_index));

            // Orca: latch the decision before the inner loop reassigns nozzle_volume_type;
            // re-testing the reassigned value would make every slot after the first reuse
            // the first slot's volume type.
            const bool per_type_slots = extruder_nozzle_volume_count > extruder_count || nozzle_volume_type == nvtHybrid;
            const int nvt_count = per_type_slots ? int(nv_types[e_index].size()) : 1;
            for (int nvt_index = 0; nvt_index < nvt_count; nvt_index++)
            {
                if (per_type_slots)
                    nozzle_volume_type = nv_types[e_index][nvt_index];
                //variant index
                int slot_index = get_index_for_extruder(e_index+1, id_name, extruder_type, nozzle_volume_type, variant_name);
                if (slot_index < 0) {
                    // Orca: This is expected during transient UI states (e.g. popup windows),
                    // fall back to 0 silently.
                    slot_index = 0;
                }
                variant_index.push_back(slot_index);
            }
        }
        variant_count = int(variant_index.size());
    }

    const ConfigDef       *config_def     = this->def();
    if (!config_def) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", Line %1%: can not find config define")%__LINE__;
        return variant_index;
    }
    for (auto& key: key_set)
    {
        const ConfigOptionDef *optdef  = config_def->get(key);
        if (!optdef) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: can not find opt define for %2%")%__LINE__%key;
            continue;
        }
        switch (optdef->type) {
            case coStrings:
            {
                ConfigOptionStrings * opt = this->option<ConfigOptionStrings>(key);
                if (!opt) continue;
                std::vector<std::string> new_values;

                new_values.resize(variant_count * stride);
                for (int e_index = 0; e_index < variant_count; e_index++)
                {
                    for (unsigned int i = 0; i < stride; i++)
                        new_values[e_index*stride + i] = opt->get_at(variant_index[e_index]*stride + i);
                }
                opt->values = new_values;
                break;
            }
            case coInts:
            {
                ConfigOptionInts * opt = this->option<ConfigOptionInts>(key);
                if (!opt) continue;
                std::vector<int> new_values;

                new_values.resize(variant_count * stride);
                for (int e_index = 0; e_index < variant_count; e_index++)
                {
                    for (unsigned int i = 0; i < stride; i++)
                        new_values[e_index*stride + i] = opt->get_at(variant_index[e_index]*stride + i);
                }
                opt->values = new_values;
                break;
            }
            case coFloats:
            {
                ConfigOptionFloats * opt = this->option<ConfigOptionFloats>(key);
                if (!opt) continue;
                std::vector<double> new_values;

                new_values.resize(variant_count * stride);
                for (int e_index = 0; e_index < variant_count; e_index++)
                {
                    for (unsigned int i = 0; i < stride; i++)
                        new_values[e_index*stride + i] = opt->get_at(variant_index[e_index]*stride + i);
                }
                opt->values = new_values;
                break;
            }
            case coPercents:
            {
                ConfigOptionPercents * opt = this->option<ConfigOptionPercents>(key);
                if (!opt) continue;
                std::vector<double> new_values;

                new_values.resize(variant_count * stride);
                for (int e_index = 0; e_index < variant_count; e_index++)
                {
                    for (unsigned int i = 0; i < stride; i++)
                        new_values[e_index*stride + i] = opt->get_at(variant_index[e_index]*stride + i);
                }
                opt->values = new_values;
                break;
            }
            case coFloatsOrPercents:
            {
                ConfigOptionFloatsOrPercents * opt = this->option<ConfigOptionFloatsOrPercents>(key);
                if (!opt) continue;
                std::vector<FloatOrPercent> new_values;

                new_values.resize(variant_count * stride);
                for (int e_index = 0; e_index < variant_count; e_index++)
                {
                    for (unsigned int i = 0; i < stride; i++)
                        new_values[e_index*stride + i] = opt->get_at(variant_index[e_index]*stride + i);
                }
                opt->values = new_values;
                break;
            }
            case coBools:
            {
                ConfigOptionBools * opt = this->option<ConfigOptionBools>(key);
                if (!opt) continue;
                std::vector<unsigned char> new_values;

                new_values.resize(variant_count * stride);
                for (int e_index = 0; e_index < variant_count; e_index++)
                {
                    for (unsigned int i = 0; i < stride; i++)
                        new_values[e_index*stride + i] = opt->get_at(variant_index[e_index]*stride + i);
                }
                opt->values = new_values;
                break;
            }
            case coEnums:
            {
                ConfigOptionEnumsGeneric * opt = this->option<ConfigOptionEnumsGeneric>(key);
                if (!opt) continue;
                std::vector<int> new_values;

                new_values.resize(variant_count * stride);
                for (int e_index = 0; e_index < variant_count; e_index++)
                {
                    for (unsigned int i = 0; i < stride; i++)
                        new_values[e_index*stride + i] = opt->get_at(variant_index[e_index]*stride + i);
                }
                opt->values = new_values;
                break;
            }
            default:
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: unsupported option type for %2%")%__LINE__%key;
                break;
        }
    }

    return variant_index;
}

void DynamicPrintConfig::update_values_to_printer_extruders_for_multiple_filaments(DynamicPrintConfig& printer_config, int extruder_count, int extruder_nozzle_volume_count, std::set<std::string>& key_set, std::string id_name, std::string variant_name)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: extruder_count %2%, extruder_nozzle_volume_count %3%")%__LINE__ %extruder_count %extruder_nozzle_volume_count;

    {
        auto opt_filament_map = printer_config.option<ConfigOptionInts>("filament_map");
        if (!opt_filament_map) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: filament_map option not found, skipping")%__LINE__;
            return;
        }
        std::vector<int> filament_maps = opt_filament_map->values;
        size_t filament_count = filament_maps.size();
        //apply process settings
        auto opt_extruder_type = dynamic_cast<const ConfigOptionEnumsGeneric*>(printer_config.option("extruder_type"));
        auto opt_nozzle_volume_type = dynamic_cast<const ConfigOptionEnumsGeneric*>(printer_config.option("nozzle_volume_type"));
        if (!opt_extruder_type || !opt_nozzle_volume_type) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: extruder_type or nozzle_volume_type option not found, skipping")%__LINE__;
            return;
        }

        auto opt_filament_volume_maps = dynamic_cast<const ConfigOptionInts*>(printer_config.option("filament_volume_map"));
        std::vector<int> filament_volume_maps;
        // Orca: honour the per-filament volume map only when a producer sized it to the
        // filament count. The full-config producers (PresetBundle injection, engine
        // write-back) always size it; mis-sized maps (stale project values, CLI runs until
        // the per-filament synthesis lands there) must not distort slot resolution nor be
        // indexed out of bounds.
        if (opt_filament_volume_maps && opt_filament_volume_maps->values.size() == filament_count)
            filament_volume_maps = opt_filament_volume_maps->values;
        auto opt_ids = id_name.empty()? nullptr: dynamic_cast<const ConfigOptionInts*>(this->option(id_name));
        std::vector<int> variant_index;

        variant_index.resize(filament_count, -1);

        for (int f_index = 0; f_index < filament_count; f_index++)
        {
            ExtruderType extruder_type = (ExtruderType)(opt_extruder_type->get_at(filament_maps[f_index] - 1));
            NozzleVolumeType nozzle_volume_type = (NozzleVolumeType)(opt_nozzle_volume_type->get_at(filament_maps[f_index] - 1));

            if ((extruder_nozzle_volume_count > extruder_count || nozzle_volume_type == nvtHybrid) && (!filament_volume_maps.empty())) {
                nozzle_volume_type = (NozzleVolumeType)(filament_volume_maps[f_index]);
            }

            //variant index
            variant_index[f_index] = get_index_for_extruder(f_index+1, id_name, extruder_type, nozzle_volume_type, variant_name);
            if (variant_index[f_index] < 0) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", Line %1%: could not found extruder_type %2%, nozzle_volume_type %3%, filament_index %4%, extruder index %5%")
                    %__LINE__ %s_keys_names_ExtruderType[extruder_type] % s_keys_names_NozzleVolumeType[nozzle_volume_type] % (f_index+1) %filament_maps[f_index];
                assert(false);
                //for some updates happens in a invalid state(caused by popup window)
                //we need to avoid crash
                variant_index[f_index] = 0;
                if (opt_ids) {
                    for (int i = 0; i < opt_ids->values.size(); i++)
                        if (opt_ids->values[i] == (f_index+1)) {
                            variant_index[f_index] = i;
                            break;
                        }
                }
            }
        }

        const ConfigDef       *config_def     = this->def();
        if (!config_def) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", Line %1%: can not find config define")%__LINE__;
            return;
        }
        bool has_id_key = !id_name.empty() && key_set.count(id_name) > 0;
        for (auto& key: key_set)
        {
            if (has_id_key && key == id_name)
                continue;
            const ConfigOptionDef *optdef  = config_def->get(key);
            if (!optdef) {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: can not find opt define for %2%")%__LINE__%key;
                continue;
            }

            switch (optdef->type) {
                case coStrings:
                {
                    ConfigOptionStrings * opt = this->option<ConfigOptionStrings>(key);
                    if (!opt) {
                        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% not found, skipping")%__LINE__%key;
                        break;
                    }
                    std::vector<std::string> new_values;

                    new_values.resize(filament_count);
                    for (int f_index = 0; f_index < filament_count; f_index++)
                    {
                        if (variant_index[f_index] < 0 || static_cast<size_t>(variant_index[f_index]) >= opt->size()) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% variant index %3% out of range, skipping")%__LINE__%key%variant_index[f_index];
                            continue;
                        }
                        new_values[f_index] = opt->get_at(variant_index[f_index]);
                    }
                    opt->values = new_values;
                    break;
                }
                case coInts:
                {
                    ConfigOptionInts * opt = this->option<ConfigOptionInts>(key);
                    if (!opt) {
                        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% not found, skipping")%__LINE__%key;
                        break;
                    }
                    std::vector<int> new_values;

                    new_values.resize(filament_count);
                    for (int f_index = 0; f_index < filament_count; f_index++)
                    {
                        if (variant_index[f_index] < 0 || static_cast<size_t>(variant_index[f_index]) >= opt->size()) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% variant index %3% out of range, skipping")%__LINE__%key%variant_index[f_index];
                            continue;
                        }
                        new_values[f_index] = opt->get_at(variant_index[f_index]);
                    }
                    opt->values = new_values;
                    break;
                }
                case coFloats:
                {
                    ConfigOptionFloats * opt = this->option<ConfigOptionFloats>(key);
                    if (!opt) {
                        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% not found, skipping")%__LINE__%key;
                        break;
                    }
                    std::vector<double> new_values;

                    new_values.resize(filament_count);
                    for (int f_index = 0; f_index < filament_count; f_index++)
                    {
                        if (variant_index[f_index] < 0 || static_cast<size_t>(variant_index[f_index]) >= opt->size()) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% variant index %3% out of range, skipping")%__LINE__%key%variant_index[f_index];
                            continue;
                        }
                        new_values[f_index] = opt->get_at(variant_index[f_index]);
                    }
                    opt->values = new_values;
                    break;
                }
                case coPercents:
                {
                    ConfigOptionPercents * opt = this->option<ConfigOptionPercents>(key);
                    if (!opt) {
                        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% not found, skipping")%__LINE__%key;
                        break;
                    }
                    std::vector<double> new_values;

                    new_values.resize(filament_count);
                    for (int f_index = 0; f_index < filament_count; f_index++)
                    {
                        if (variant_index[f_index] < 0 || static_cast<size_t>(variant_index[f_index]) >= opt->size()) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% variant index %3% out of range, skipping")%__LINE__%key%variant_index[f_index];
                            continue;
                        }
                        new_values[f_index] = opt->get_at(variant_index[f_index]);
                    }
                    opt->values = new_values;
                    break;
                }
                case coFloatsOrPercents:
                {
                    ConfigOptionFloatsOrPercents * opt = this->option<ConfigOptionFloatsOrPercents>(key);
                    if (!opt) {
                        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% not found, skipping")%__LINE__%key;
                        break;
                    }
                    std::vector<FloatOrPercent> new_values;

                    new_values.resize(filament_count);
                    for (int f_index = 0; f_index < filament_count; f_index++)
                    {
                        if (variant_index[f_index] < 0 || static_cast<size_t>(variant_index[f_index]) >= opt->size()) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% variant index %3% out of range, skipping")%__LINE__%key%variant_index[f_index];
                            continue;
                        }
                        new_values[f_index] = opt->get_at(variant_index[f_index]);
                    }
                    opt->values = new_values;
                    break;
                }
                case coBools:
                {
                    ConfigOptionBools * opt = this->option<ConfigOptionBools>(key);
                    if (!opt) {
                        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% not found, skipping")%__LINE__%key;
                        break;
                    }
                    std::vector<unsigned char> new_values;

                    new_values.resize(filament_count);
                    for (int f_index = 0; f_index < filament_count; f_index++)
                    {
                        if (variant_index[f_index] < 0 || static_cast<size_t>(variant_index[f_index]) >= opt->size()) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% variant index %3% out of range, skipping")%__LINE__%key%variant_index[f_index];
                            continue;
                        }
                        new_values[f_index] = opt->get_at(variant_index[f_index]);
                    }
                    opt->values = new_values;
                    break;
                }
                case coEnums:
                {
                    ConfigOptionEnumsGeneric * opt = this->option<ConfigOptionEnumsGeneric>(key);
                    if (!opt) {
                        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% not found, skipping")%__LINE__%key;
                        break;
                    }
                    std::vector<int> new_values;

                    new_values.resize(filament_count);
                    for (int f_index = 0; f_index < filament_count; f_index++)
                    {
                        if (variant_index[f_index] < 0 || static_cast<size_t>(variant_index[f_index]) >= opt->size()) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% variant index %3% out of range, skipping")%__LINE__%key%variant_index[f_index];
                            continue;
                        }
                        new_values[f_index] = opt->get_at(variant_index[f_index]);
                    }
                    opt->values = new_values;
                    break;
                }
                default:
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: unsupported option type for %2%")%__LINE__%key;
                    break;
            }
        }
        // Orca: also require a non-empty id list; get_at on an empty vector is undefined.
        if (has_id_key && opt_ids && !opt_ids->values.empty()) {
            // remap the id list itself last: the lookups above still need the original ids
            std::vector<int> new_values;
            new_values.resize(filament_count);
            for (int f_index = 0; f_index < filament_count; f_index++) {
                new_values[f_index] = opt_ids->get_at(variant_index[f_index]);
            }
            const_cast<ConfigOptionInts*>(opt_ids)->values = new_values;
        }
    }
}

// Regathers a vector option's values through per-slot source indices (one input index per
// output slot). Out-of-range indices keep the first value, matching get_at's fallback.
template<typename OptType, typename ValueType>
static void gather_option_values(const std::string &key, OptType *opt, const std::vector<int> &slot_param_indices)
{
    if (!opt || opt->values.empty()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% not found or empty, skipping")%__LINE__%key;
        return;
    }
    std::vector<ValueType> new_values;
    new_values.reserve(slot_param_indices.size());
    for (int idx : slot_param_indices) {
        if (idx < 0 || static_cast<size_t>(idx) >= opt->values.size()) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: option %2% slot index %3% out of range, keeping first value")%__LINE__%key%idx;
            new_values.emplace_back(opt->values.front());
        }
        else
            new_values.emplace_back(opt->values[idx]);
    }
    opt->values = std::move(new_values);
}

void DynamicPrintConfig::update_filament_config_values_for_multiple_extruders(DynamicPrintConfig& printer_config,
    const std::unordered_map<int, std::vector<FilamentVariantUse>>& filament_variant_uses,
    int extruder_count, int extruder_nozzle_volume_count,
    std::set<std::string>& key_set, std::string id_name, std::string variant_name,
    std::vector<int>* slot_machine_indices)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: extruder_count %2%, extruder_nozzle_volume_count %3%")%__LINE__ %extruder_count %extruder_nozzle_volume_count;

    auto opt_filament_map = printer_config.option<ConfigOptionInts>("filament_map");
    if (!opt_filament_map) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: filament_map option not found, skipping")%__LINE__;
        return;
    }
    std::vector<int> filament_maps = opt_filament_map->values;
    size_t filament_count = filament_maps.size();
    auto opt_extruder_type = dynamic_cast<const ConfigOptionEnumsGeneric*>(printer_config.option("extruder_type"));
    auto opt_nozzle_volume_type = dynamic_cast<const ConfigOptionEnumsGeneric*>(printer_config.option("nozzle_volume_type"));
    if (!opt_extruder_type || !opt_nozzle_volume_type) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: extruder_type or nozzle_volume_type option not found, skipping")%__LINE__;
        return;
    }

    auto opt_filament_volume_maps = dynamic_cast<const ConfigOptionInts*>(printer_config.option("filament_volume_map"));
    std::vector<int> filament_volume_maps;
    // Orca: same sizing guard as the single-slot rebuild above — honour the per-filament volume
    // map only when a producer sized it to the filament count.
    if (opt_filament_volume_maps && opt_filament_volume_maps->values.size() == filament_count)
        filament_volume_maps = opt_filament_volume_maps->values;
    auto opt_ids = id_name.empty() ? nullptr : dynamic_cast<const ConfigOptionInts*>(this->option(id_name));

    std::vector<int> slot_param_indices;
    slot_param_indices.reserve(filament_count * 2);
    if (slot_machine_indices) {
        slot_machine_indices->clear();
        slot_machine_indices->reserve(filament_count * 2);
    }

    auto resolve_slot = [&](int f_index, ExtruderType extruder_type, NozzleVolumeType nozzle_volume_type, int extruder_id_1based) {
        int param_index = get_index_for_extruder(f_index + 1, id_name, extruder_type, nozzle_volume_type, variant_name);
        if (param_index < 0) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", Line %1%: could not found extruder_type %2%, nozzle_volume_type %3%, filament_index %4%, extruder index %5%")
                %__LINE__ %s_keys_names_ExtruderType[extruder_type] % s_keys_names_NozzleVolumeType[nozzle_volume_type] % (f_index+1) %extruder_id_1based;
            assert(false);
            //for some updates happens in a invalid state(caused by popup window)
            //we need to avoid crash
            param_index = 0;
            if (opt_ids) {
                for (int i = 0; i < opt_ids->values.size(); i++)
                    if (opt_ids->values[i] == (f_index + 1)) {
                        param_index = i;
                        break;
                    }
            }
        }
        slot_param_indices.push_back(param_index);
        if (slot_machine_indices) {
            // Orca: key each output slot to the machine slot of its own variant (the same
            // derivation the per-filament slot map uses), so the retract-override nil fallback
            // stays aligned when a filament occupies more than one slot.
            int machine_index = printer_config.get_index_for_extruder(extruder_id_1based, "print_extruder_id", extruder_type, nozzle_volume_type, "print_extruder_variant");
            slot_machine_indices->push_back(std::max(machine_index, 0));
        }
    };

    for (int f_index = 0; f_index < static_cast<int>(filament_count); f_index++) {
        auto uses_it = filament_variant_uses.find(f_index);
        if (uses_it != filament_variant_uses.end() && !uses_it->second.empty()) {
            for (const FilamentVariantUse &use : uses_it->second)
                resolve_slot(f_index, use.extruder_type, use.nozzle_volume_type, use.extruder_id + 1);
        }
        else {
            ExtruderType extruder_type = (ExtruderType)(opt_extruder_type->get_at(filament_maps[f_index] - 1));
            NozzleVolumeType nozzle_volume_type = (NozzleVolumeType)(opt_nozzle_volume_type->get_at(filament_maps[f_index] - 1));
            if ((extruder_nozzle_volume_count > extruder_count || nozzle_volume_type == nvtHybrid) && !filament_volume_maps.empty())
                nozzle_volume_type = (NozzleVolumeType)(filament_volume_maps[f_index]);
            resolve_slot(f_index, extruder_type, nozzle_volume_type, filament_maps[f_index]);
        }
    }

    const ConfigDef *config_def = this->def();
    if (!config_def) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", Line %1%: can not find config define")%__LINE__;
        return;
    }
    bool has_id_key = !id_name.empty() && key_set.count(id_name) > 0;
    for (auto &key : key_set) {
        if (has_id_key && key == id_name)
            continue;
        const ConfigOptionDef *optdef = config_def->get(key);
        if (!optdef) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: can not find opt define for %2%")%__LINE__%key;
            continue;
        }
        switch (optdef->type) {
            case coStrings:          gather_option_values<ConfigOptionStrings, std::string>(key, this->option<ConfigOptionStrings>(key), slot_param_indices); break;
            case coInts:             gather_option_values<ConfigOptionInts, int>(key, this->option<ConfigOptionInts>(key), slot_param_indices); break;
            case coFloats:           gather_option_values<ConfigOptionFloats, double>(key, this->option<ConfigOptionFloats>(key), slot_param_indices); break;
            case coPercents:         gather_option_values<ConfigOptionPercents, double>(key, this->option<ConfigOptionPercents>(key), slot_param_indices); break;
            case coFloatsOrPercents: gather_option_values<ConfigOptionFloatsOrPercents, FloatOrPercent>(key, this->option<ConfigOptionFloatsOrPercents>(key), slot_param_indices); break;
            case coBools:            gather_option_values<ConfigOptionBools, unsigned char>(key, this->option<ConfigOptionBools>(key), slot_param_indices); break;
            case coEnums:            gather_option_values<ConfigOptionEnumsGeneric, int>(key, this->option<ConfigOptionEnumsGeneric>(key), slot_param_indices); break;
            default:
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: unsupported option type for %2%")%__LINE__%key;
                break;
        }
    }
    // Orca: also require a non-empty id list; get_at on an empty vector is undefined.
    if (has_id_key && opt_ids && !opt_ids->values.empty()) {
        // remap the id list itself last: the slot resolution above still needs the original ids
        std::vector<int> new_values;
        new_values.reserve(slot_param_indices.size());
        for (int idx : slot_param_indices)
            new_values.push_back(opt_ids->get_at(idx));
        const_cast<ConfigOptionInts*>(opt_ids)->values = std::move(new_values);
    }
}

namespace {
// Options in printer_options_with_variant_2 are stored as (normal,silent) pairs per printer variant.
// Some legacy presets/projects carry a variant list but still store only one pair; normalize to avoid crashes.
static void normalize_stride2_floats(ConfigOptionFloats &opt, size_t expected_size)
{
    auto &v = opt.values;
    if (expected_size == 0) {
        v.clear();
        return;
    }
    if (v.empty()) {
        // Fallback: keep behavior predictable instead of crashing. This should be rare.
        v.resize(expected_size, 0.0);
        return;
    }

    const double first  = v[0];
    const double second = (v.size() >= 2) ? v[1] : first;

    // Ensure we have at least one (normal,silent) pair to replicate.
    if (v.size() < 2) {
        v.resize(2, first);
        v[1] = second;
    }
    // Keep pair alignment if some legacy preset produced odd length.
    if (v.size() % 2 != 0)
        v.push_back(second);

    if (v.size() > expected_size) {
        v.resize(expected_size);
        return;
    }

    const size_t have_variants = v.size() / 2;
    const size_t want_variants = expected_size / 2;
    v.resize(expected_size);
    for (size_t vi = have_variants; vi < want_variants; ++vi) {
        v[vi * 2] = first;
        if (vi * 2 + 1 < v.size())
            v[vi * 2 + 1] = second;
    }
}

static void log_normalize_legacy_vector_size(const char *fn, const std::string &key, int stride, size_t src_size, size_t dest_size, size_t expected_size,
                                            size_t restore_n, int cur_variant_count, int target_variant_count, size_t cur_ids, size_t target_ids,
                                            const ConfigOption *opt_src, const ConfigOption *opt_target)
{
    BOOST_LOG_TRIVIAL(debug) << fn << ": normalizing legacy vector size for key '" << key << "'"
                             << " stride=" << stride << " src_size=" << src_size << " dest_size=" << dest_size << " expected=" << expected_size
                             << " restore_index.size=" << restore_n << " cur_variants=" << cur_variant_count << " target_variants=" << target_variant_count
                             << " cur_ids=" << cur_ids << " target_ids=" << target_ids << " cur_value=" << opt_src->serialize()
                             << " target_value=" << opt_target->serialize();
}
} // namespace

void DynamicPrintConfig::update_non_diff_values_to_base_config(DynamicPrintConfig& new_config, const t_config_option_keys& keys, const std::set<std::string>& different_keys,
    std::string extruder_id_name, std::string extruder_variant_name, std::set<std::string>& key_set1, std::set<std::string>& key_set2)
{
    std::vector<int> cur_extruder_ids, target_extruder_ids, variant_index;
    std::vector<std::string> cur_extruder_variants, target_extruder_variants;

    if (!extruder_id_name.empty()) {
        if (this->option(extruder_id_name))
            cur_extruder_ids = this->option<ConfigOptionInts>(extruder_id_name)->values;
        if (new_config.option(extruder_id_name))
            target_extruder_ids = new_config.option<ConfigOptionInts>(extruder_id_name)->values;
    }
    if (this->option(extruder_variant_name))
        cur_extruder_variants = this->option<ConfigOptionStrings>(extruder_variant_name, true)->values;
    if (new_config.option(extruder_variant_name))
        target_extruder_variants = new_config.option<ConfigOptionStrings>(extruder_variant_name, true)->values;

    int cur_variant_count = cur_extruder_variants.size();
    int target_variant_count = target_extruder_variants.size();

    variant_index.resize(target_variant_count, -1);
    if (cur_variant_count == 0) {
        // Defensive: target_variant_count may be 0 if the preset doesn't carry extruder_variant_name.
        // In that case keep variant_index empty and let the downstream size checks produce a useful error.
        if (!variant_index.empty())
            variant_index[0] = 0;
    }
    else if ((cur_extruder_ids.size() > 0) && cur_variant_count != cur_extruder_ids.size()){
        //should not happen
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" size of %1% = %2%, not equal to size of %3% = %4%")
             %extruder_variant_name %cur_variant_count %extruder_id_name %cur_extruder_ids.size();
    }
    else if ((target_extruder_ids.size() > 0) && target_variant_count != target_extruder_ids.size()){
        //should not happen
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" size of %1% = %2%, not equal to size of %3% = %4%")
             %extruder_variant_name %target_variant_count %extruder_id_name %target_extruder_ids.size();
    }
    else {
        for (int i = 0; i < target_variant_count; i++)
        {
            for (int j = 0; j < cur_variant_count; j++)
            {
                if ((target_extruder_variants[i] == cur_extruder_variants[j])
                    &&(target_extruder_ids.empty() || (target_extruder_ids[i] == cur_extruder_ids[j])))
                {
                    variant_index[i] = j;
                    break;
                }
            }
        }
    }

    for (auto& opt : keys) {
        ConfigOption *opt_src = this->option(opt);
        const ConfigOption *opt_target = new_config.option(opt);
        if (opt_src && opt_target && (*opt_src != *opt_target)) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" change key %1% from old_value %2% to inherit's value %3%")
                    %opt %(opt_src->serialize()) %(opt_target->serialize());
            if (different_keys.find(opt) == different_keys.end()) {
                opt_src->set(opt_target);
            }
            else {
                if (opt_target->is_scalar()
                    || ((key_set1.find(opt) == key_set1.end()) && (key_set2.empty() || (key_set2.find(opt) == key_set2.end())))) {
                    //nothing to do, keep the original one
                }
                else {
                    // Guard: set_with_restore is parent-shaped and would truncate the child's
                    // vector when the child has more extruders than the parent (e.g. an IDEX
                    // preset inheriting from a single-nozzle base). The child's saved value is
                    // authoritative for its own extruder count, so skip the merge for this key.
                    if (cur_variant_count > target_variant_count)
                        continue;

                    int stride = 1;
                    if (key_set2.find(opt) != key_set2.end())
                        stride = 2;

                    const size_t restore_n     = variant_index.size();
                    const size_t expected_size = restore_n * size_t(stride);

                    if (stride == 2) {
                        // Options in key_set2 are machine limits stored as (normal,silent) pairs per printer variant.
                        if (opt_src->type() != coFloats || opt_target->type() != coFloats)
                            throw ConfigurationError((boost::format("%1%: key '%2%' is expected to be ConfigOptionFloats for stride=2.") % __FUNCTION__ % opt).str());

                        auto *src_f = static_cast<ConfigOptionFloats*>(opt_src);
                        ConfigOptionFloats rhs_tmp(*static_cast<const ConfigOptionFloats*>(opt_target));

                        const size_t src_size  = src_f->values.size();
                        const size_t dest_size = rhs_tmp.values.size();
                        if (src_size != expected_size || dest_size != expected_size)
                            log_normalize_legacy_vector_size(__FUNCTION__, opt, stride, src_size, dest_size, expected_size, restore_n, cur_variant_count,
                                                             target_variant_count, cur_extruder_ids.size(), target_extruder_ids.size(), opt_src, opt_target);

                        // Normalize src in-place so backup_values indexing is safe, normalize rhs via a temporary copy.
                        normalize_stride2_floats(*src_f, expected_size);
                        normalize_stride2_floats(rhs_tmp, expected_size);
                        src_f->set_with_restore(&rhs_tmp, variant_index, stride);
                    } else {
                        ConfigOptionVectorBase* opt_vec_src = static_cast<ConfigOptionVectorBase*>(opt_src);

                        const size_t src_size  = opt_vec_src->size();
                        const size_t dest_size = static_cast<const ConfigOptionVectorBase*>(opt_target)->size();
                        if (src_size != expected_size || dest_size != expected_size)
                            log_normalize_legacy_vector_size(__FUNCTION__, opt, stride, src_size, dest_size, expected_size, restore_n, cur_variant_count,
                                                             target_variant_count, cur_extruder_ids.size(), target_extruder_ids.size(), opt_src, opt_target);

                        if (opt_vec_src->size() != expected_size)
                            opt_vec_src->resize(expected_size, opt_target);

                        // Normalize rhs via a cloned temporary (rhs itself is const).
                        ConfigOptionUniquePtr rhs_owner(opt_target->clone());
                        ConfigOptionVectorBase *rhs_vec = dynamic_cast<ConfigOptionVectorBase*>(rhs_owner.get());
                        if (rhs_vec == nullptr)
                            throw ConfigurationError((boost::format("%1%: key '%2%' is expected to be a vector option.") % __FUNCTION__ % opt).str());
                        if (rhs_vec->size() != expected_size)
                            rhs_vec->resize(expected_size, opt_target);

                        opt_vec_src->set_with_restore(rhs_vec, variant_index, stride);
                    }
                }
            }
        }
    }
    return;
}

void DynamicPrintConfig::update_diff_values_to_child_config(DynamicPrintConfig& new_config, std::string extruder_id_name, std::string extruder_variant_name, std::set<std::string>& key_set1, std::set<std::string>& key_set2)
{
    std::vector<int> cur_extruder_ids, target_extruder_ids, variant_index;
    std::vector<std::string> cur_extruder_variants, target_extruder_variants;

    if (!extruder_id_name.empty()) {
        if (this->option(extruder_id_name))
            cur_extruder_ids = this->option<ConfigOptionInts>(extruder_id_name)->values;
        if (new_config.option(extruder_id_name))
            target_extruder_ids = new_config.option<ConfigOptionInts>(extruder_id_name)->values;
    }
    if (this->option(extruder_variant_name))
        cur_extruder_variants = this->option<ConfigOptionStrings>(extruder_variant_name, true)->values;
    if (new_config.option(extruder_variant_name))
        target_extruder_variants = new_config.option<ConfigOptionStrings>(extruder_variant_name, true)->values;

    int cur_variant_count = cur_extruder_variants.size();
    int target_variant_count = target_extruder_variants.size();

    if (cur_variant_count > 0)
        variant_index.resize(cur_variant_count, -1);
    else
        variant_index.resize(1, 0);

    if (target_variant_count == 0) {
        variant_index[0] = 0;
    }
    else if ((cur_extruder_ids.size() > 0) && cur_variant_count != cur_extruder_ids.size()){
        //should not happen
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" size of %1% = %2%, not equal to size of %3% = %4%")
             %extruder_variant_name %cur_variant_count %extruder_id_name %cur_extruder_ids.size();
    }
    else if ((target_extruder_ids.size() > 0) && target_variant_count != target_extruder_ids.size()){
        //should not happen
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" size of %1% = %2%, not equal to size of %3% = %4%")
             %extruder_variant_name %target_variant_count %extruder_id_name %target_extruder_ids.size();
    }
    else {
        for (int i = 0; i < cur_variant_count; i++)
        {
            for (int j = 0; j < target_variant_count; j++)
            {
                if ((cur_extruder_variants[i] == target_extruder_variants[j])
                    &&(cur_extruder_ids.empty() || (cur_extruder_ids[i] == target_extruder_ids[j])))
                {
                    variant_index[i] = j;
                    break;
                }
            }
        }
    }

    const t_config_option_keys &keys = new_config.keys();
    for (auto& opt : keys) {
        if ((opt == extruder_id_name) || (opt == extruder_variant_name))
            continue;
        ConfigOption *opt_src = this->option(opt);
        const ConfigOption *opt_target = new_config.option(opt);
        if (opt_src && opt_target && (*opt_src != *opt_target)) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" change key %1% from base_value %2% to child's value %3%")
                    %opt %(opt_src->serialize()) %(opt_target->serialize());
            if (opt_target->is_scalar()
                || ((key_set1.find(opt) == key_set1.end()) && (key_set2.empty() || (key_set2.find(opt) == key_set2.end())))) {
                //nothing to do, keep the original one
                opt_src->set(opt_target);
            }
            else {
                ConfigOptionVectorBase* opt_vec_src = static_cast<ConfigOptionVectorBase*>(opt_src);
                const ConfigOptionVectorBase* opt_vec_dest = static_cast<const ConfigOptionVectorBase*>(opt_target);
                int stride = 1;
                if (key_set2.find(opt) != key_set2.end())
                    stride = 2;
                // set_only_diff() requires the base vector length to equal variant_index.size()*stride, where
                // variant_index is sized from the parent's printer_extruder_variant list (or 1 when it has none).
                // Legacy / multi-extruder / multi-variant presets can violate this in either direction when a key's
                // stored length and the variant-list length were sized inconsistently - e.g. a Snapmaker U1 base
                // (4 nozzles) whose stride-2 machine limits were length-extended to nozzles*2 while its
                // printer_extruder_variant list is empty (variant_index == 1), so base length != variant_index*stride.
                // Rather than throw (which is caught upstream and DELETES the user preset file), fall back to the
                // child's explicit value, which is authoritative for its own extruder/variant layout.
                if (opt_vec_src->size() != variant_index.size() * size_t(stride)) {
                    opt_src->set(opt_target);
                }
                else
                    opt_vec_src->set_only_diff(opt_vec_dest, variant_index, stride);
            }
        }
    }
    return;
}

void compute_filament_override_value(const std::string& opt_key, const ConfigOption *opt_old_machine, const ConfigOption *opt_new_machine, const ConfigOption *opt_new_filament, const DynamicPrintConfig& new_full_config,
    t_config_option_keys& diff_keys, DynamicPrintConfig& filament_overrides, std::vector<int>& f_map_indices)
{
    bool is_nil = opt_new_filament->is_nil();

    // ugly code, for these params, we should ignore the value in filament params
    ConfigOptionBoolsNullable opt_long_retraction_default;
    if (opt_key == "long_retractions_when_cut" && new_full_config.option<ConfigOptionInt>("enable_long_retraction_when_cut")->value != LongRectrationLevel::EnableFilament) {
        auto ptr = dynamic_cast<const ConfigOptionBoolsNullable*>(opt_new_filament);
        for (size_t idx = 0; idx < ptr->values.size(); ++idx)
            opt_long_retraction_default.values.push_back(ptr->nil_value());
        opt_new_filament = &opt_long_retraction_default;
    }

    ConfigOptionFloatsNullable opt_retraction_distance_default;
    if (opt_key == "retraction_distances_when_cut" && new_full_config.option<ConfigOptionInt>("enable_long_retraction_when_cut")->value != LongRectrationLevel::EnableFilament) {
        auto ptr = dynamic_cast<const ConfigOptionFloatsNullable*>(opt_new_filament);
        for (size_t idx = 0; idx < ptr->values.size(); ++idx)
            opt_long_retraction_default.values.push_back(ptr->nil_value());
        opt_new_filament = &opt_retraction_distance_default;
    }

    auto opt_copy = opt_new_machine->clone();
    opt_copy->apply_override(opt_new_filament, f_map_indices);
    bool changed = *opt_old_machine != *opt_copy;

    if (changed) {
        diff_keys.emplace_back(opt_key);
        filament_overrides.set_key_value(opt_key, opt_copy);
    }
    else
        delete opt_copy;
}


void update_static_print_config_from_dynamic(ConfigBase& config, const DynamicPrintConfig& dest_config, std::vector<int> variant_index, std::set<std::string>& key_set1, int stride)
{
    if (variant_index.size() > 0) {
        const t_config_option_keys &keys = dest_config.keys();
        for (auto& opt : keys) {
            ConfigOption *opt_src = config.option(opt);
            const ConfigOption *opt_dest = dest_config.option(opt);
            if (opt_src && opt_dest && (*opt_src != *opt_dest)) {
                if (opt_dest->is_scalar() || (key_set1.find(opt) == key_set1.end()))
                    opt_src->set(opt_dest);
                else {
                    ConfigOptionVectorBase* opt_vec_src = static_cast<ConfigOptionVectorBase*>(opt_src);
                    const ConfigOptionVectorBase* opt_vec_dest = static_cast<const ConfigOptionVectorBase*>(opt_dest);
                    opt_vec_src->set_to_index(opt_vec_dest, variant_index, stride);
                }
            }
        }
    }
    else
        config.apply(dest_config, true);
}

//BBS: pass map to recording all invalid valies
//FIXME localize this function.
std::map<std::string, std::string> validate(const FullPrintConfig &cfg, bool under_cli)
{
    std::map<std::string, std::string> error_message;
    // --layer-height
    if (cfg.get_abs_value("layer_height") <= 0) {
        error_message.emplace("layer_height", L("invalid value ") + std::to_string(cfg.get_abs_value("layer_height")));
    }
    else if (fabs(fmod(cfg.get_abs_value("layer_height"), SCALING_FACTOR)) > 1e-4) {
        error_message.emplace("layer_height", L("invalid value ") + std::to_string(cfg.get_abs_value("layer_height")));
    }

    // --first-layer-height
    if (cfg.initial_layer_print_height.value <= 0) {
        error_message.emplace("initial_layer_print_height", L("invalid value ") + std::to_string(cfg.initial_layer_print_height.value));
    }

    // --filament-diameter
    for (double fd : cfg.filament_diameter.values)
        if (fd < 1) {
            error_message.emplace("filament_diameter", L("invalid value ") + cfg.filament_diameter.serialize());
            break;
        }

    // --nozzle-diameter
    for (double nd : cfg.nozzle_diameter.values)
        if (nd < 0.005) {
            error_message.emplace("nozzle_diameter", L("invalid value ") + cfg.nozzle_diameter.serialize());
            break;
        }

    // --perimeters
    if (cfg.wall_loops.value < 0) {
        error_message.emplace("wall_loops", L("invalid value ") + std::to_string(cfg.wall_loops.value));
    }

    // --solid-layers
    if (cfg.top_shell_layers < 0) {
        error_message.emplace("top_shell_layers", L("invalid value ") + std::to_string(cfg.top_shell_layers));
    }
    if (cfg.bottom_shell_layers < 0) {
        error_message.emplace("bottom_shell_layers", L("invalid value ") + std::to_string(cfg.bottom_shell_layers));
    }

    if (cfg.use_firmware_retraction.value &&
        cfg.gcode_flavor.value != gcfKlipper &&
        cfg.gcode_flavor.value != gcfSmoothie &&
        cfg.gcode_flavor.value != gcfRepRapSprinter &&
        cfg.gcode_flavor.value != gcfRepRapFirmware &&
        cfg.gcode_flavor.value != gcfMarlinLegacy &&
        cfg.gcode_flavor.value != gcfMarlinFirmware &&
        cfg.gcode_flavor.value != gcfMachinekit &&
        cfg.gcode_flavor.value != gcfRepetier)
        error_message.emplace("use_firmware_retraction","--use-firmware-retraction is only supported by Klipper, Marlin, Smoothie, RepRapFirmware, Repetier and Machinekit firmware");

    if (cfg.use_firmware_retraction.value)
        for (unsigned char wipe : cfg.wipe.values)
             if (wipe)
                error_message.emplace("use_firmware_retraction", "--use-firmware-retraction is not compatible with --wipe");
                
    // --gcode-flavor
    if (! print_config_def.get("gcode_flavor")->has_enum_value(cfg.gcode_flavor.serialize())) {
        error_message.emplace("gcode_flavor", L("invalid value ") + cfg.gcode_flavor.serialize());
    }

    // --fill-pattern
    if (! print_config_def.get("sparse_infill_pattern")->has_enum_value(cfg.sparse_infill_pattern.serialize())) {
        error_message.emplace("sparse_infill_pattern", L("invalid value ") + cfg.sparse_infill_pattern.serialize());
    }

    // --top-fill-pattern
    if (! print_config_def.get("top_surface_pattern")->has_enum_value(cfg.top_surface_pattern.serialize())) {
        error_message.emplace("top_surface_pattern", L("invalid value ") + cfg.top_surface_pattern.serialize());
    }

    // --bottom-fill-pattern
    if (! print_config_def.get("bottom_surface_pattern")->has_enum_value(cfg.bottom_surface_pattern.serialize())) {
        error_message.emplace("bottom_surface_pattern", L("invalid value ") + cfg.bottom_surface_pattern.serialize());
    }

    // --soild-fill-pattern
    if (!print_config_def.get("internal_solid_infill_pattern")->has_enum_value(cfg.internal_solid_infill_pattern.serialize())) {
        error_message.emplace("internal_solid_infill_pattern", L("invalid value ") + cfg.internal_solid_infill_pattern.serialize());
    }

    // --skirt-height
    if (cfg.skirt_height < 0) {
        error_message.emplace("skirt_height", L("invalid value ") + std::to_string(cfg.skirt_height));
    }

    // --bridge-flow-ratio
    if (cfg.bridge_flow <= 0) {
        error_message.emplace("bridge_flow", L("invalid value ") + std::to_string(cfg.bridge_flow));
    }
    
    // --internal-bridge-flow-ratio
    if (cfg.internal_bridge_flow <= 0) {
        error_message.emplace("internal_bridge_flow", L("invalid value ") + std::to_string(cfg.internal_bridge_flow));
    }

    // extruder clearance
    if (cfg.extruder_clearance_radius <= 0) {
        error_message.emplace("extruder_clearance_radius", L("invalid value ") + std::to_string(cfg.extruder_clearance_radius));
    }
    if (cfg.extruder_clearance_height_to_rod <= 0) {
        error_message.emplace("extruder_clearance_height_to_rod", L("invalid value ") + std::to_string(cfg.extruder_clearance_height_to_rod));
    }
    if (cfg.extruder_clearance_height_to_lid <= 0) {
        error_message.emplace("extruder_clearance_height_to_lid", L("invalid value ") + std::to_string(cfg.extruder_clearance_height_to_lid));
    }
    if (cfg.nozzle_height <= 0)
        error_message.emplace("nozzle_height", L("invalid value ") + std::to_string(cfg.nozzle_height));

    // --extrusion-multiplier
    for (double em : cfg.filament_flow_ratio.values)
        if (em <= 0) {
            error_message.emplace("filament_flow_ratio", L("invalid value ") + cfg.filament_flow_ratio.serialize());
            break;
        }

    // --spiral-vase
    //for non-cli case, we will popup dialog for spiral mode correction
    if (cfg.spiral_mode && under_cli) {
        // Note that we might want to have more than one perimeter on the bottom
        // solid layers.
        if (cfg.wall_loops != 1) {
            error_message.emplace("wall_loops", L("Invalid value when spiral vase mode is enabled: ") + std::to_string(cfg.wall_loops));
            //return "Can't make more than one perimeter when spiral vase mode is enabled";
            //return "Can't make less than one perimeter when spiral vase mode is enabled";
        }

        if (cfg.sparse_infill_density > 0) {
            error_message.emplace("sparse_infill_density", L("Invalid value when spiral vase mode is enabled: ") + std::to_string(cfg.sparse_infill_density));
            //return "Spiral vase mode can only print hollow objects, so you need to set Fill density to 0";
        }

        if (cfg.top_shell_layers > 0) {
            error_message.emplace("top_shell_layers", L("Invalid value when spiral vase mode is enabled: ") + std::to_string(cfg.top_shell_layers));
            //return "Spiral vase mode is not compatible with top solid layers";
        }

        if (cfg.enable_support ) {
            error_message.emplace("enable_support", L("Invalid value when spiral vase mode is enabled: ") + std::to_string(cfg.enable_support));
            //return "Spiral vase mode is not compatible with support";
        }
        if (cfg.enforce_support_layers > 0) {
            error_message.emplace("enforce_support_layers", L("Invalid value when spiral vase mode is enabled: ") + std::to_string(cfg.enforce_support_layers));
            //return "Spiral vase mode is not compatible with support";
        }
    }

    // extrusion widths
    {
        double max_nozzle_diameter = 0.;
        double min_nozzle_diameter = std::numeric_limits<double>::max();
        for (double dmr : cfg.nozzle_diameter.values)
        {
            max_nozzle_diameter = std::max(max_nozzle_diameter, dmr);
            min_nozzle_diameter = std::min(min_nozzle_diameter, dmr);
        }
        const char *widths[] = {
            "outer_wall_line_width",
            "inner_wall_line_width",
            "sparse_infill_line_width",
            "internal_solid_infill_line_width",
            "bridge_line_width",
            "top_surface_line_width",
            "support_line_width",
            "initial_layer_line_width",
            "skin_infill_line_width",
            "skeleton_infill_line_width"};
        for (size_t i = 0; i < sizeof(widths) / sizeof(widths[i]); ++ i) {
            std::string key(widths[i]);
            double abs_width = cfg.get_abs_value(key, max_nozzle_diameter);
            double allowed_max = (key == "bridge_line_width") ? min_nozzle_diameter : MAX_LINE_WIDTH_MULTIPLIER * max_nozzle_diameter;
            if (abs_width > allowed_max) {
                if (key == "bridge_line_width")
                    error_message.emplace(key, L("Bridge line width must not exceed nozzle diameter: ") + std::to_string(abs_width));
                else
                    error_message.emplace(key, L("too large line width ") + std::to_string(abs_width));
                //return std::string("Too Large line width: ") + key;
            }
        }
    }

    // Out of range validation of numeric values.
    for (const std::string &opt_key : cfg.keys()) {
        const ConfigOption      *opt    = cfg.optptr(opt_key);
        assert(opt != nullptr);
        const ConfigOptionDef   *optdef = print_config_def.get(opt_key);
        assert(optdef != nullptr);
        bool out_of_range = false;
        switch (opt->type()) {
        case coFloat:
        case coPercent:
        case coFloatOrPercent:
        {
            auto *fopt = static_cast<const ConfigOptionFloat*>(opt);
            out_of_range = !optdef->is_value_valid(fopt->value);
            break;
        }
        case coFloats:
        case coPercents:
            for (double v : static_cast<const ConfigOptionVector<double>*>(opt)->values)
                if (!optdef->is_value_valid(v)) {
                    out_of_range = true;
                    break;
                }
            break;
        case coInt:
        {
            auto *iopt = static_cast<const ConfigOptionInt*>(opt);
            out_of_range = !optdef->is_value_valid(iopt->value);
            break;
        }
        case coInts:
            for (int v : static_cast<const ConfigOptionVector<int>*>(opt)->values)
                if (!optdef->is_value_valid(v)) {
                    out_of_range = true;
                    break;
                }
            break;
        default:;
        }
        if (out_of_range) {
            if (error_message.find(opt_key) == error_message.end())
                error_message.emplace(opt_key, opt->serialize() + L(" not in range ") +"[" + std::to_string(optdef->min) + "," + std::to_string(optdef->max) + "]");
            //return std::string("Value out of range: " + opt_key);
        }
    }

    // The configuration is valid.
    return error_message;
}

// Declare and initialize static caches of StaticPrintConfig derived classes.
#define PRINT_CONFIG_CACHE_ELEMENT_DEFINITION(r, data, CLASS_NAME) StaticPrintConfig::StaticCache<class Slic3r::CLASS_NAME> BOOST_PP_CAT(CLASS_NAME::s_cache_, CLASS_NAME);
#define PRINT_CONFIG_CACHE_ELEMENT_INITIALIZATION(r, data, CLASS_NAME) Slic3r::CLASS_NAME::initialize_cache();
#define PRINT_CONFIG_CACHE_INITIALIZE(CLASSES_SEQ)     BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CACHE_ELEMENT_DEFINITION, _, BOOST_PP_TUPLE_TO_SEQ(CLASSES_SEQ))     int print_config_static_initializer() {         /* For some reason it's important this function doesn't get optimized out, so this should work. */         static volatile int ret = 1;         BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CACHE_ELEMENT_INITIALIZATION, _, BOOST_PP_TUPLE_TO_SEQ(CLASSES_SEQ))         return ret;     }
PRINT_CONFIG_CACHE_INITIALIZE((
    PrintObjectConfig, PrintRegionConfig, MachineEnvelopeConfig, GCodeConfig, PrintConfig, FullPrintConfig,
    SLAMaterialConfig, SLAPrintConfig, SLAPrintObjectConfig, SLAPrinterConfig, SLAFullPrintConfig))
static int print_config_static_initialized = print_config_static_initializer();

//BBS: remove unused command currently
CLIActionsConfigDef::CLIActionsConfigDef()
{
    ConfigOptionDef* def;

    // Actions:
    /*def = this->add("export_obj", coBool);
    def->label = L("Export OBJ");
    def->tooltip = L("Export the model(s) as OBJ.");
    def->set_default_value(new ConfigOptionBool(false));*/

/*
    def = this->add("export_svg", coBool);
    def->label = L("Export SVG");
    def->tooltip = L("Slice the model and export solid slices as SVG.");
    def->set_default_value(new ConfigOptionBool(false));
*/

    /*def = this->add("export_sla", coBool);
    def->label = L("Export SLA");
    def->tooltip = L("Slice the model and export SLA printing layers as PNG.");
    def->cli = "export-sla|sla";
    def->set_default_value(new ConfigOptionBool(false));*/

    def = this->add("export_3mf", coString);
    def->label = L("Export 3MF");
    def->tooltip = L("This exports the project as a 3MF file.");
    def->cli_params = "filename.3mf";
    def->set_default_value(new ConfigOptionString("output.3mf"));

    def = this->add("export_slicedata", coString);
    def->label = L("Export slicing data");
    def->tooltip = L("Export slicing data to a folder");
    def->cli_params = "slicing_data_directory";
    def->set_default_value(new ConfigOptionString("cached_data"));

    def = this->add("load_slicedata", coStrings);
    def->label = L("Load slicing data");
    def->tooltip = L("Load cached slicing data from directory.");
    def->cli_params = "slicing_data_directory";
    def->set_default_value(new ConfigOptionString("cached_data"));

    /*def = this->add("export_amf", coBool);
    def->label = L("Export AMF");
    def->tooltip = L("Export the model(s) as AMF.");
    def->set_default_value(new ConfigOptionBool(false));*/

    def = this->add("export_stl", coBool);
    def->label = L("Export STL");
    def->tooltip = L("Export the objects as single STL.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("export_stls", coString);
    def->label = L("Export multiple STLs");
    def->tooltip = L("Export the objects as multiple STLs to directory.");
    def->set_default_value(new ConfigOptionString("stl_path"));

    /*def = this->add("export_gcode", coBool);
    def->label = L("Export G-code");
    def->tooltip = L("Slice the model and export toolpaths as G-code.");
    def->cli = "export-gcode|gcode|g";
    def->set_default_value(new ConfigOptionBool(false));*/

    /*def = this->add("gcodeviewer", coBool);
    // BBS: remove _L()
    def->label = L("G-code viewer");
    def->tooltip = L("Visualize an already sliced and saved G-code.");
    def->cli = "gcodeviewer";
    def->set_default_value(new ConfigOptionBool(false));*/

    def = this->add("slice", coInt);
    def->label = L("Slice");
    def->tooltip = L("Slice the plates: 0-all plates, i-plate i, others-invalid");
    def->cli = "slice";
    def->cli_params = "option";
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("help", coBool);
    def->label = L("Help");
    def->tooltip = L("This shows command help.");
    def->cli = "help|h";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("uptodate", coBool);
    def->label = L("UpToDate");
    def->tooltip = L("Update the config values of 3MF to latest.");
    def->cli = "uptodate";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("load_defaultfila", coBool);
    def->label = L("Load default filaments");
    def->tooltip = L("Load first filament as default for those not loaded.");
    def->cli_params = "option";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("min_save", coBool);
    def->label = L("Minimum save");
    def->tooltip = L("Export 3MF with minimum size.");
    def->cli_params = "option";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("mtcpp", coInt);
    def->label = L("mtcpp");
    def->tooltip = L("max triangle count per plate for slicing");
    def->cli = "mtcpp";
    def->cli_params = "count";
    def->set_default_value(new ConfigOptionInt(1000000));

    def = this->add("mstpp", coInt);
    def->label = L("mstpp");
    def->tooltip = L("max slicing time per plate in seconds");
    def->cli = "mstpp";
    def->cli_params = "time";
    def->set_default_value(new ConfigOptionInt(300));

    // must define new params here, otherwise comamnd param check will fail
    def = this->add("no_check", coBool);
    def->label = L("No check");
    def->tooltip = L("Do not run any validity checks, such as G-code path conflicts check.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("normative_check", coBool);
    def->label = L("Normative check");
    def->tooltip = L("Check the normative items.");
    def->cli_params = "option";
    def->set_default_value(new ConfigOptionBool(true));

    /*def = this->add("help_fff", coBool);
    def->label = L("Help (FFF options)");
    def->tooltip = L("Show the full list of print/G-code configuration options.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("help_sla", coBool);
    def->label = L("Help (SLA options)");
    def->tooltip = L("Show the full list of SLA print configuration options.");
    def->set_default_value(new ConfigOptionBool(false));*/

    def = this->add("info", coBool);
    def->label = L("Output Model Info");
    def->tooltip = L("This outputs the model\u2019s information.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("export_settings", coString);
    def->label = L("Export Settings");
    def->tooltip = L("This exports settings to a file.");
    def->cli_params = "settings.json";
    def->set_default_value(new ConfigOptionString("output.json"));

    def = this->add("pipe", coString);
    def->label = L("Send progress to pipe");
    def->tooltip = L("Send progress to pipe.");
    def->cli_params = "pipename";
    def->set_default_value(new ConfigOptionString());
}

//BBS: remove unused command currently
CLITransformConfigDef::CLITransformConfigDef()
{
    ConfigOptionDef* def;

    // Transform options:
    /*def = this->add("align_xy", coPoint);
    def->label = L("Align XY");
    def->tooltip = L("Align the model to the given point.");
    def->set_default_value(new ConfigOptionPoint(Vec2d(100,100)));

    def = this->add("cut", coFloat);
    def->label = L("Cut");
    def->tooltip = L("Cut model at the given Z.");
    def->set_default_value(new ConfigOptionFloat(0));*/

/*
    def = this->add("cut_grid", coFloat);
    def->label = L("Cut");
    def->tooltip = L("Cut model in the XY plane into tiles of the specified max size.");
    def->set_default_value(new ConfigOptionPoint());

    def = this->add("cut_x", coFloat);
    def->label = L("Cut");
    def->tooltip = L("Cut model at the given X.");
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("cut_y", coFloat);
    def->label = L("Cut");
    def->tooltip = L("Cut model at the given Y.");
    def->set_default_value(new ConfigOptionFloat(0));
*/

    /*def = this->add("center", coPoint);
    def->label = L("Center");
    def->tooltip = L("Center the print around the given center.");
    def->set_default_value(new ConfigOptionPoint(Vec2d(100,100)));*/

    def = this->add("arrange", coInt);
    def->label = L("Arrange Options");
    def->tooltip = L("Arrange options: 0-disable, 1-enable, others-auto");
    def->cli_params = "option";
    //def->cli = "arrange|a";
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("repetitions", coInt);
    def->label = L("Repetition count");
    def->tooltip = L("Repetition count of the whole model.");
    def->cli_params = "count";
    def->set_default_value(new ConfigOptionInt(1));

    def = this->add("ensure_on_bed", coBool);
    def->label = L("Ensure on bed");
    def->tooltip = L("Lift the object above the bed when it is partially below. Disabled by default.");
    def->set_default_value(new ConfigOptionBool(false));

    /*def = this->add("copy", coInt);
    def->label = L("Copy");
    def->tooltip =L("Duplicate copies of model.");
    def->min = 1;
    def->set_default_value(new ConfigOptionInt(1));*/

    /*def = this->add("duplicate_grid", coPoint);
    def->label = L("Duplicate by grid");
    def->tooltip = L("Multiply copies by creating a grid.");*/

    def = this->add("assemble", coBool);
    def->label = L("Assemble");
    def->tooltip = L("Arrange the supplied models in a plate and merge them in a single model in order to perform actions once.");
    //def->cli = "merge|m";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("convert_unit", coBool);
    def->label = L("Convert Unit");
    def->tooltip = L("Convert the units of model.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("orient", coInt);
    def->label = L("Orient Options");
    def->tooltip = L("Orient options: 0-disable, 1-enable, others-auto");
    //def->cli = "orient|o";
    def->set_default_value(new ConfigOptionInt(0));

    /*def = this->add("repair", coBool);
    def->label = L("Repair");
    def->tooltip = L("Repair the model's meshes if it is non-manifold mesh.");
    def->set_default_value(new ConfigOptionBool(false));*/

    def = this->add("rotate", coFloat);
    def->label = L("Rotate");
    def->tooltip = L("Rotation angle around the Z axis in degrees.");
    def->sidetext = u8"°";	// degrees, don't need translation
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("rotate_x", coFloat);
    def->label = L("Rotate around X");
    def->tooltip = L("Rotation angle around the X axis in degrees.");
    def->sidetext = u8"°";	// degrees, don't need translation
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("rotate_y", coFloat);
    def->label = L("Rotate around Y");
    def->tooltip = L("Rotation angle around the Y axis in degrees.");
    def->sidetext = u8"°";	// degrees, don't need translation
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("scale", coFloat);
    def->label = L("Scale");
    def->tooltip = L("Scale the model by a float factor.");
    def->cli_params = "factor";
    def->set_default_value(new ConfigOptionFloat(1.f));

    /*def = this->add("split", coBool);
    def->label = L("Split");
    def->tooltip = L("Detect unconnected parts in the given model(s) and split them into separate objects.");

    def = this->add("scale_to_fit", coPoint3);
    def->label = L("Scale to Fit");
    def->tooltip = L("Scale to fit the given volume.");
    def->set_default_value(new ConfigOptionPoint3(Vec3d(0,0,0)));*/
}

CLIMiscConfigDef::CLIMiscConfigDef()
{
    ConfigOptionDef* def;

    /*def = this->add("ignore_nonexistent_config", coBool);
    def->label = L("Ignore non-existent config files");
    def->tooltip = L("Do not fail if a file supplied to --load does not exist.");

    def = this->add("config_compatibility", coEnum);
    def->label = L("Forward-compatibility rule when loading configurations from config files and project files (3MF, AMF).");
    def->tooltip = L("This version of OrcaSlicer may not understand configurations produced by the newest OrcaSlicer versions. "
                     "For example, newer OrcaSlicer may extend the list of supported firmware flavors. One may decide to "
                     "bail out or to substitute an unknown value with a default silently or verbosely.");
    def->enum_keys_map = &ConfigOptionEnum<ForwardCompatibilitySubstitutionRule>::get_enum_values();
    def->enum_values.push_back("disable");
    def->enum_values.push_back("enable");
    def->enum_values.push_back("enable_silent");
    def->enum_labels.push_back(L("Bail out on unknown configuration values"));
    def->enum_labels.push_back(L("Enable reading unknown configuration values by verbosely substituting them with defaults."));
    def->enum_labels.push_back(L("Enable reading unknown configuration values by silently substituting them with defaults."));
    def->set_default_value(new ConfigOptionEnum<ForwardCompatibilitySubstitutionRule>(ForwardCompatibilitySubstitutionRule::Enable));*/

    /*def = this->add("load", coStrings);
    def->label = L("Load config file");
    def->tooltip = L("Load configuration from the specified file. It can be used more than once to load options from multiple files.");*/

    def = this->add("load_settings", coStrings);
    def->label = L("Load General Settings");
    def->tooltip = L("Load process/machine settings from the specified file.");
    def->cli_params = "\"setting1.json;setting2.json\"";
    def->set_default_value(new ConfigOptionStrings());

    def = this->add("load_filaments", coStrings);
    def->label = L("Load Filament Settings");
    def->tooltip = L("Load filament settings from the specified file list.");
    def->cli_params = "\"filament1.json;filament2.json;...\"";
    def->set_default_value(new ConfigOptionStrings());

    def = this->add("skip_objects", coInts);
    def->label = L("Skip Objects");
    def->tooltip = L("Skip some objects in this print.");
    def->cli_params = "\"3,5,10,77\"";
    def->set_default_value(new ConfigOptionInts());

    def = this->add("clone_objects", coInts);
    def->label = L("Clone Objects");
    def->tooltip = L("Clone objects in the load list.");
    def->cli_params = "\"1,3,1,10\"";
    def->set_default_value(new ConfigOptionInts());

    def = this->add("uptodate_settings", coStrings);
    def->label = L("Load uptodate process/machine settings when using uptodate");
    def->tooltip = L("load up-to-date process/machine settings from the specified file when using up-to-date");
    def->cli_params = "\"setting1.json;setting2.json\"";
    def->set_default_value(new ConfigOptionStrings());

    def = this->add("uptodate_filaments", coStrings);
    def->label = L("Load uptodate filament settings when using uptodate");
    def->tooltip = L("Load uptodate filament settings from the specified file when using uptodate.");
    def->cli_params = "\"filament1.json;filament2.json;...\"";
    def->set_default_value(new ConfigOptionStrings());

    def = this->add("downward_check", coBool);
    def->label = L("Downward machines check");
    def->tooltip = L("If enabled, check whether current machine downward compatible with the machines in the list.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("downward_settings", coStrings);
    def->label = L("Downward machines settings");
    def->tooltip = L("The machine settings list needs to do downward checking.");
    def->cli_params = "\"machine1.json;machine2.json;...\"";
    def->set_default_value(new ConfigOptionStrings());

    def = this->add("load_assemble_list", coString);
    def->label = L("Load assemble list");
    def->tooltip = L("Load assemble object list from config file.");
    def->cli_params = "assemble_list.json";
    def->set_default_value(new ConfigOptionString());

    /*def = this->add("output", coString);
    def->label = L("Output File");
    def->tooltip = L("The file where the output will be written (if not specified, it will be based on the input file).");
    def->cli = "output|o";

    def = this->add("single_instance", coBool);
    def->label = L("Single instance mode");
    def->tooltip = L("If enabled, the command line arguments are sent to an existing instance of GUI OrcaSlicer, "
                     "or an existing OrcaSlicer window is activated. "
                     "Overrides the \"single_instance\" configuration value from application preferences.");*/

/*
    def = this->add("autosave", coString);
    def->label = L("Autosave");
    def->tooltip = L("Automatically export current configuration to the specified file.");
*/

    def = this->add("datadir", coString);
    def->label = L("Data directory");
    def->tooltip = L("Load and store settings at the given directory. This is useful for maintaining different profiles or including configurations from a network storage.");


    def = this->add("outputdir", coString);
    def->label = L("Output directory");
    def->tooltip = L("This is the output directory for exported files.");
    def->cli_params = "dir";
    def->set_default_value(new ConfigOptionString());

    def = this->add("debug", coInt);
    def->label = L("Debug level");
    def->tooltip = L("Sets debug logging level. 0:fatal, 1:error, 2:warning, 3:info, 4:debug, 5:trace\n");
    def->min = 0;
    def->cli_params = "level";
    def->set_default_value(new ConfigOptionInt(1));

    def = this->add("logfile", coInt);
    def->label = L("Log file");
    def->tooltip = L("Redirects debug logging to file.\n");
    def->cli_params = "file";
    def->set_default_value(new ConfigOptionString());

    def = this->add("enable_timelapse", coBool);
    def->label = L("Enable timelapse for print");
    def->tooltip = L("If enabled, this slicing will be considered using timelapse.");
    def->set_default_value(new ConfigOptionBool(false));

#if (defined(_MSC_VER) || defined(__MINGW32__)) && defined(SLIC3R_GUI)
    /*def = this->add("sw_renderer", coBool);
    def->label = L("Render with a software renderer");
    def->tooltip = L("Render with a software renderer. The bundled MESA software renderer is loaded instead of the default OpenGL driver.");
    def->min = 0;*/
#endif /* _MSC_VER */

    def = this->add("load_custom_gcodes", coString);
    def->label = L("Load custom G-code");
    def->tooltip = L("Load custom G-code from json.");
    def->cli_params = "custom_gcode_toolchange.json";
    def->set_default_value(new ConfigOptionString());

    def = this->add("load_filament_ids", coInts);
    def->label = L("Load filament IDs");
    def->tooltip = L("Load filament IDs for each object.");
    def->cli_params = "\"1,2,3,1\"";
    def->set_default_value(new ConfigOptionInts());

    def = this->add("allow_multicolor_oneplate", coBool);
    def->label = L("Allow multiple colors on one plate");
    def->tooltip = L("If enabled, Arrange will allow multiple colors on one plate.");
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("allow_rotations", coBool);
    def->label = L("Allow rotation when arranging");
    def->tooltip = L("If enabled, Arrange will allow rotation when placing objects.");
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("avoid_extrusion_cali_region", coBool);
    def->label = L("Avoid extrusion calibrate region when arranging");
    def->tooltip = L("If enabled, Arrange will avoid extrusion calibrate region when placing objects.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("skip_modified_gcodes", coBool);
    def->label = L("Skip modified G-code in 3MF");
    def->tooltip = L("Skip the modified G-code in 3MF from printer or filament presets.");
    def->cli_params = "option";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("makerlab_name", coString);
    def->label = L("MakerLab name");
    def->tooltip = L("MakerLab name to generate this 3MF.");
    def->cli_params = "name";
    def->set_default_value(new ConfigOptionString());

    def = this->add("makerlab_version", coString);
    def->label = L("MakerLab version");
    def->tooltip = L("MakerLab version to generate this 3MF.");
    def->cli_params = "version";
    def->set_default_value(new ConfigOptionString());

    def = this->add("metadata_name", coStrings);
    def->label = L("Metadata name list");
    def->tooltip = L("Metadata name list added into 3MF.");
    def->cli_params = "\"name1;name2;...\"";
    def->set_default_value(new ConfigOptionStrings());

    def = this->add("metadata_value", coStrings);
    def->label = L("Metadata value list");
    def->tooltip = L("Metadata value list added into 3MF.");
    def->cli_params = "\"value1;value2;...\"";
    def->set_default_value(new ConfigOptionStrings());

    def = this->add("allow_newer_file", coBool);
    def->label = L("Allow 3MF with newer version to be sliced");
    def->tooltip = L("Allow 3MF with newer version to be sliced.");
    def->cli_params = "option";
    def->set_default_value(new  ConfigOptionBool(false));

    def = this->add("allow_mix_temp", coBool);
    // internal use only, don't need translation
    def->label = "Allow filaments with high/low temperature to be printed together";
    def->tooltip = "Allow filaments with high/low temperature to be printed together.";
    def->cli_params = "option";
    def->set_default_value(new  ConfigOptionBool(false));
}

const CLIActionsConfigDef    cli_actions_config_def;
const CLITransformConfigDef  cli_transform_config_def;
const CLIMiscConfigDef       cli_misc_config_def;

DynamicPrintAndCLIConfig::PrintAndCLIConfigDef DynamicPrintAndCLIConfig::s_def;

void DynamicPrintAndCLIConfig::handle_legacy(t_config_option_key &opt_key, std::string &value) const
{
    if (cli_actions_config_def  .options.find(opt_key) == cli_actions_config_def  .options.end() &&
        cli_transform_config_def.options.find(opt_key) == cli_transform_config_def.options.end() &&
        cli_misc_config_def     .options.find(opt_key) == cli_misc_config_def     .options.end()) {
        PrintConfigDef::handle_legacy(opt_key, value);
    }
}

// SlicingStatesConfigDefs

// Create a new config definition with a label and tooltip
// Note: the L() macro is already used for LABEL and TOOLTIP
#define new_def(OPT_KEY, TYPE, LABEL, TOOLTIP)         def = this->add(OPT_KEY, TYPE);         def->label = L(LABEL);         def->tooltip = L(TOOLTIP);

ReadOnlySlicingStatesConfigDef::ReadOnlySlicingStatesConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("zhop", coFloat);
    def->label = L("Current Z-hop");
    def->tooltip = L("Contains Z-hop present at the beginning of the custom G-code block.");
}

ReadWriteSlicingStatesConfigDef::ReadWriteSlicingStatesConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("position", coFloats);
    def->label = L("Position");
    def->tooltip = L("Position of the extruder at the beginning of the custom G-code block. If the custom G-code travels somewhere else, "
                     "it should write to this variable so OrcaSlicer knows where it travels from when it gets control back.");

    def = this->add("e_retracted", coFloats);
    def->label = L("Retraction");
    def->tooltip = L("Retraction state at the beginning of the custom G-code block. If the custom G-code moves the extruder axis, "
                     "it should write to this variable so OrcaSlicer de-retracts correctly when it gets control back.");

    def = this->add("e_restart_extra", coFloats);
    def->label = L("Extra de-retraction");
    def->tooltip = L("Currently planned extra extruder priming after de-retraction.");

   def = this->add("e_position", coFloats);
   def->label = L("Absolute E position");
   def->tooltip = L("Current position of the extruder axis. Only used with absolute extruder addressing.");
}

OtherSlicingStatesConfigDef::OtherSlicingStatesConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("current_extruder", coInt);
    def->label = L("Current extruder");
    def->tooltip = L("Zero-based index of currently used extruder.");

    def = this->add("current_object_idx", coInt);
    def->label = L("Current object index");
    def->tooltip = L("Specific for sequential printing. Zero-based index of currently printed object.");

    def = this->add("has_wipe_tower", coBool);
    def->label = L("Has wipe tower");
    def->tooltip = L("Whether or not wipe tower is being generated in the print.");

    def = this->add("initial_extruder", coInt);
    def->label = L("Initial extruder");
    def->tooltip = L("Zero-based index of the first extruder used in the print. Same as initial_tool.");

    def = this->add("initial_tool", coInt);
    def->label = L("Initial tool");
    def->tooltip = L("Zero-based index of the first extruder used in the print. Same as initial_extruder.");

    def = this->add("is_extruder_used", coBools);
    def->label = L("Is extruder used?");
    def->tooltip = L("Vector of booleans stating whether a given extruder is used in the print.");

    def = this->add("num_extruders", coInt);
    def->label   = L("Number of extruders");
    def->tooltip = L("Total number of extruders, regardless of whether they are used in the current print.");

    // Options from PS not used in Orca
    //    def = this->add("initial_filament_type", coString);
    //    def->label = L("Initial filament type");
    //    def->tooltip = L("String containing filament type of the first used extruder.");

    def          = this->add("has_single_extruder_multi_material_priming", coBool);
    def->label   = L("Has single extruder MM priming");
    def->tooltip = L("Are the extra multi-material priming regions used in this print?");

    new_def("initial_no_support_extruder", coInt, "Initial no support extruder", "Zero-based index of the first extruder used for printing without support. Same as initial_no_support_tool.");
    new_def("in_head_wrap_detect_zone", coBool, "In head wrap detect zone", "Indicates if the first layer overlaps with the head wrap zone.");
}

PrintStatisticsConfigDef::PrintStatisticsConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("extruded_volume", coFloats);
    def->label = L("Volume per extruder");
    def->tooltip = L("Total filament volume extruded per extruder during the entire print.");

    def = this->add("total_toolchanges", coInt);
    def->label = L("Total tool changes");
    def->tooltip = L("Number of tool changes during the print.");

    def = this->add("extruded_volume_total", coFloat);
    def->label = L("Total volume");
    def->tooltip = L("Total volume of filament used during the entire print.");

    def = this->add("extruded_weight", coFloats);
    def->label = L("Weight per extruder");
    def->tooltip = L("Weight per extruder extruded during the entire print. Calculated from filament_density value in Filament Settings.");

    def = this->add("extruded_weight_total", coFloat);
    def->label = L("Total weight");
    def->tooltip = L("Total weight of the print. Calculated from filament_density value in Filament Settings.");

    def = this->add("total_layer_count", coInt);
    def->label = L("Total layer count");
    def->tooltip = L("Number of layers in the entire print.");

    def = this->add("normal_print_time", coString);
    def->label = L("Print time (normal mode)");
    def->tooltip = L("Estimated print time when printed in normal mode (i.e. not in silent mode). Same as print_time.");

    //def = this->add("num_printing_extruders", coInt);
    //def->label = L("Number of printing extruders");
    //def->tooltip = L("Number of extruders used during the print.");

    def = this->add("print_time", coString);
    def->label = L("Print time (normal mode)");
    def->tooltip = L("Estimated print time when printed in normal mode (i.e. not in silent mode). Same as normal_print_time.");

    //def = this->add("printing_filament_types", coString);
    //def->label = L("Used filament types");
    //def->tooltip = L("Comma-separated list of all filament types used during the print.");

    def = this->add("silent_print_time", coString);
    def->label = L("Print time (silent mode)");
    def->tooltip = L("Estimated print time when printed in silent mode.");

    def = this->add("total_cost", coFloat);
    def->label = L("Total cost");
    def->tooltip = L("Total cost of all material used in the print. Calculated from filament_cost value in Filament Settings.");

    def = this->add("total_weight", coFloat);
    def->label = L("Total weight");
    def->tooltip = L("Total weight of the print. Calculated from filament_density value in Filament Settings.");

    def = this->add("total_wipe_tower_cost", coFloat);
    def->label = L("Total wipe tower cost");
    def->tooltip = L("Total cost of the material wasted on the wipe tower. Calculated from filament_cost value in Filament Settings.");

    def = this->add("total_wipe_tower_filament", coFloat);
    def->label = L("Wipe tower volume");
    def->tooltip = L("Total filament volume extruded on the wipe tower.");

    def = this->add("used_filament", coFloat);
    def->label = L("Used filament");
    def->tooltip = L("Total length of filament used in the print.");

    def = this->add("print_time_sec", coString);
    def->label = L("Print time (seconds)");
    def->tooltip = L("Total estimated print time in seconds. Replaced with actual value during post-processing.");

    def = this->add("used_filament_length", coString);
    def->label = L("Filament length (meters)");
    def->tooltip = L("Total filament length used in meters. Replaced with actual value during post-processing.");
}

ObjectsInfoConfigDef::ObjectsInfoConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("num_objects", coInt);
    def->label = L("Number of objects");
    def->tooltip = L("Total number of objects in the print.");

    def = this->add("num_instances", coInt);
    def->label = L("Number of instances");
    def->tooltip = L("Total number of object instances in the print, summed over all objects.");

    def = this->add("scale", coStrings);
    def->label = L("Scale per object");
    def->tooltip = L("Contains a string with the information about what scaling was applied to the individual objects. "
                     "Indexing of the objects is zero-based (first object has index 0).\n"
                     "Example: 'x:100% y:50% z:100%'.");

    def = this->add("input_filename_base", coString);
    def->label = L("Input filename without extension");
    def->tooltip = L("Source filename of the first object, without extension.");

    new_def("input_filename", coString, "Full input filename", "Source filename of the first object.");
    new_def("plate_name", coString, "Plate name", "Name of the plate sliced.");
}

DimensionsConfigDef::DimensionsConfigDef()
{
    ConfigOptionDef* def;

    const std::string point_tooltip   = L("The vector has two elements: X and Y coordinate of the point. Values in mm.");
    const std::string bb_size_tooltip = L("The vector has two elements: X and Y dimension of the bounding box. Values in mm.");

    def = this->add("first_layer_print_convex_hull", coPoints);
    def->label = L("First layer convex hull");
    def->tooltip = L("Vector of points of the first layer convex hull. Each element has the following format:"
                     "'[x, y]' (x and y are floating-point numbers in mm).");

    def = this->add("first_layer_print_min", coFloats);
    def->label = L("Bottom-left corner of the first layer bounding box");
    def->tooltip = point_tooltip;

    def = this->add("first_layer_print_max", coFloats);
    def->label = L("Top-right corner of the first layer bounding box");
    def->tooltip = point_tooltip;

    def = this->add("first_layer_print_size", coFloats);
    def->label = L("Size of the first layer bounding box");
    def->tooltip = bb_size_tooltip;

    def = this->add("print_bed_min", coFloats);
    def->label = L("Bottom-left corner of print bed bounding box");
    def->tooltip = point_tooltip;

    def = this->add("print_bed_max", coFloats);
    def->label = L("Top-right corner of print bed bounding box");
    def->tooltip = point_tooltip;

    def = this->add("print_bed_size", coFloats);
    def->label = L("Size of the print bed bounding box");
    def->tooltip = bb_size_tooltip;

    new_def("first_layer_center_no_wipe_tower", coFloats, "First layer center without wipe tower", point_tooltip);
    new_def("first_layer_height", coFloat, "First layer height", "Height of the first layer.");
}

TemperaturesConfigDef::TemperaturesConfigDef()
{
    ConfigOptionDef* def;

    new_def("bed_temperature", coInts, "Bed temperature", "Vector of bed temperatures for each extruder/filament.")
    new_def("bed_temperature_initial_layer", coInts, "First layer bed temperature", "Vector of first layer bed temperatures for each extruder/filament. Provides the same value as first_layer_bed_temperature.")
    new_def("bed_temperature_initial_layer_single", coInt, "First layer bed temperature (initial extruder)", "First layer bed temperature for the initial extruder. Same as bed_temperature_initial_layer[initial_extruder]")
    new_def("chamber_temperature", coInts, "Chamber temperature", "Vector of chamber temperatures for each extruder/filament.")
    new_def("chamber_minimal_temperature", coInts, "Chamber minimal temperature", "Vector of minimal chamber temperatures for each extruder/filament.")
    new_def("overall_chamber_temperature", coInt, "Overall chamber temperature", "Overall chamber temperature. This value is the maximum chamber temperature of any extruder/filament used.")
    new_def("first_layer_bed_temperature", coInts, "First layer bed temperature", "Vector of first layer bed temperatures for each extruder/filament. Provides the same value as bed_temperature_initial_layer.")
    new_def("first_layer_temperature", coInts, "First layer temperature", "Vector of first layer temperatures for each extruder/filament.")
}


TimestampsConfigDef::TimestampsConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("timestamp", coString);
    def->label = L("Timestamp");
    def->tooltip = L("String containing current time in yyyyMMdd-hhmmss format.");

    def = this->add("year", coInt);
    def->label = L("Year");

    def = this->add("month", coInt);
    def->label = L("Month");

    def = this->add("day", coInt);
    def->label = L("Day");

    def = this->add("hour", coInt);
    def->label = L("Hour");

    def = this->add("minute", coInt);
    def->label = L("Minute");

    def = this->add("second", coInt);
    def->label = L("Second");
}

OtherPresetsConfigDef::OtherPresetsConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("print_preset", coString);
    def->label = L("Print preset name");
    def->tooltip = L("Name of the print preset used for slicing.");

    def = this->add("filament_preset", coString);
    def->label = L("Filament preset name");
    def->tooltip = L("Names of the filament presets used for slicing. The variable is a vector "
                     "containing one name for each extruder.");

    def = this->add("printer_preset", coString);
    def->label = L("Printer preset name");
    def->tooltip = L("Name of the printer preset used for slicing.");

    def = this->add("physical_printer_preset", coString);
    def->label = L("Physical printer name");
    def->tooltip = L("Name of the physical printer used for slicing.");
}


static std::map<t_custom_gcode_key, t_config_option_keys> s_CustomGcodeSpecificPlaceholders{
    // Machine G-code
    {"file_start_gcode",           {}},
    {"machine_start_gcode",         {}},
    {"machine_end_gcode",           {"layer_num", "layer_z", "max_layer_z", "filament_extruder_id"}},
    {"before_layer_change_gcode",   {"layer_num", "layer_z", "max_layer_z"}},
    {"layer_change_gcode",          {"layer_num", "layer_z", "max_layer_z"}},
    {"timelapse_gcode",             {"layer_num", "layer_z", "max_layer_z"}},
    {"change_filament_gcode",       {"layer_num", "layer_z", "max_layer_z", "next_extruder", "previous_extruder", "fan_speed",
                               "first_flush_volume", "flush_length_1", "flush_length_2", "flush_length_3", "flush_length_4",
                               "new_filament_e_feedrate", "new_filament_temp", "new_retract_length",
                               "new_retract_length_toolchange", "old_filament_e_feedrate", "old_filament_temp", "old_retract_length",
                               "old_retract_length_toolchange", "relative_e_axis", "second_flush_volume", "toolchange_count", "toolchange_z",
                               "travel_point_1_x", "travel_point_1_y", "travel_point_2_x", "travel_point_2_y", "travel_point_3_x",
                               "travel_point_3_y", "x_after_toolchange", "y_after_toolchange", "z_after_toolchange"}},
    {"change_extrusion_role_gcode", {"layer_num", "layer_z", "extrusion_role", "last_extrusion_role"}},
    {"filament_change_extrusion_role_gcode", {"layer_num", "layer_z", "extrusion_role", "last_extrusion_role"}},
    {"process_change_extrusion_role_gcode", {"layer_num", "layer_z", "extrusion_role", "last_extrusion_role"}},
    {"printing_by_object_gcode",    {}},
    {"machine_pause_gcode",         {}},
    {"template_custom_gcode",       {}},
    // Filament G-code
    {"filament_start_gcode",        {"filament_extruder_id"}},
    {"filament_end_gcode",          {"layer_num", "layer_z", "max_layer_z", "filament_extruder_id"}},
};

const std::map<t_custom_gcode_key, t_config_option_keys>& custom_gcode_specific_placeholders()
{
    return s_CustomGcodeSpecificPlaceholders;
}

CustomGcodeSpecificConfigDef::CustomGcodeSpecificConfigDef()
{
    ConfigOptionDef* def;

// Common Defs
    def = this->add("layer_num", coInt);
    def->label = L("Layer number");
    def->tooltip = L("Index of the current layer. One-based (i.e. first layer is number 1).");

    def = this->add("layer_z", coFloat);
    def->label = L("Layer Z");
    def->tooltip = L("Height of the current layer above the print bed, measured to the top of the layer.");

    def = this->add("max_layer_z", coFloat);
    def->label = L("Maximal layer Z");
    def->tooltip = L("Height of the last layer above the print bed.");

    def = this->add("filament_extruder_id", coInt);
    def->label = L("Filament extruder ID");
    def->tooltip = L("The current extruder ID. The same as current_extruder.");

// change_filament_gcode
    new_def("previous_extruder", coInt, "Previous extruder", "Index of the extruder that is being unloaded. The index is zero based (first extruder has index 0).");
    new_def("next_extruder", coInt, "Next extruder", "Index of the extruder that is being loaded. The index is zero based (first extruder has index 0).");
    new_def("relative_e_axis", coBool, "Relative e-axis", "Indicates if relative positioning is being used.");
    new_def("toolchange_count", coInt, "Toolchange count", "The number of toolchanges throught the print.");
    new_def("fan_speed", coNone, "", ""); //Option is no longer used and is zeroed by placeholder parser for compatability
    new_def("old_retract_length", coFloat, "Old retract length", "The retraction length of the previous filament.");
    new_def("new_retract_length", coFloat, "New retract length", "The retraction lenght of the new filament.");
    new_def("old_retract_length_toolchange", coFloat, "Old retract length toolchange", "The toolchange retraction length of the previous filament.");
    new_def("new_retract_length_toolchange", coFloat, "New retract length toolchange", "The toolchange retraction length of the new filament.");
    new_def("old_filament_temp", coInt, "Old filament temp", "The old filament temp.");
    new_def("new_filament_temp", coInt, "New filament temp", "The new filament temp.");
    new_def("x_after_toolchange", coFloat, "X after toolchange", "The X pos after toolchange.");
    new_def("y_after_toolchange", coFloat, "Y after toolchange", "The Y pos after toolchange.");
    new_def("z_after_toolchange", coFloat, "Z after toolchange", "The Z pos after toolchange.");
    new_def("first_flush_volume", coFloat, "First flush volume", "The first flush volume.");
    new_def("second_flush_volume", coFloat, "Second flush volume", "The second flush volume.");
    new_def("old_filament_e_feedrate", coInt, "Old filament e feedrate", "The old filament extruder feedrate.");
    new_def("new_filament_e_feedrate", coInt, "New filament e feedrate", "The new filament extruder feedrate.");
    new_def("travel_point_1_x", coFloat, "Travel point 1 X", "The travel point 1 X.");
    new_def("travel_point_1_y", coFloat, "Travel point 1 Y", "The travel point 1 Y.");
    new_def("travel_point_2_x", coFloat, "Travel point 2 X", "The travel point 2 X.");
    new_def("travel_point_2_y", coFloat, "Travel point 2 Y", "The travel point 2 Y.");
    new_def("travel_point_3_x", coFloat, "Travel point 3 X", "The travel point 3 X.");
    new_def("travel_point_3_y", coFloat, "Travel point 3 Y", "The travel point 3 Y.");
    new_def("flush_length_1", coFloat, "Flush Length 1", "The first flush length.");
    new_def("flush_length_2", coFloat, "Flush Length 2", "The second flush length.");
    new_def("flush_length_3", coFloat, "Flush Length 3", "The third flush length.");
    new_def("flush_length_4", coFloat, "Flush Length 4", "The fourth flush length.");

// change_extrusion_role_gcode
    std::string extrusion_role_types = "Possible Values:\n[\"Perimeter\", \"ExternalPerimeter\", "
                                                     "\"OverhangPerimeter\", \"InternalInfill\", \"SolidInfill\", \"TopSolidInfill\", \"BottomSurface\", \"BridgeInfill\", \"GapFill\", \"Ironing\", "
                                                     "\"Skirt\", \"Brim\", \"SupportMaterial\", \"SupportMaterialInterface\", \"SupportTransition\", \"WipeTower\", \"Mixed\"]";

    new_def("extrusion_role", coString, "Extrusion role", "The new extrusion role/type that is going to be used\n" + extrusion_role_types);
    new_def("last_extrusion_role", coString, "Last extrusion role", "The previously used extrusion role/type\nPossible Values:\n" + extrusion_role_types);
}

const CustomGcodeSpecificConfigDef custom_gcode_specific_config_def;

#undef new_def

uint64_t ModelConfig::s_last_timestamp = 1;

static Points to_points(const std::vector<Vec2d> &dpts)
{
    Points pts; pts.reserve(dpts.size());
    for (auto &v : dpts)
        pts.emplace_back( coord_t(scale_(v.x())), coord_t(scale_(v.y())) );
    return pts;
}

Polygon get_shared_poly(const std::vector<Pointfs>& extruder_polys)
{
    Polygon result;
    for (int index = 0; index < extruder_polys.size(); index++)
    {
        const Pointfs& extruder_area = extruder_polys[index];
        if (index == 0)
            result.points = to_points(extruder_area);
        else {
            Polygon extruer_poly;
            extruer_poly.points = to_points(extruder_area);
            Polygons result_polygon = intersection(extruer_poly, result);
            result = result_polygon[0];
        }
    }
    return result;
}
Points get_bed_shape(const DynamicPrintConfig &config, bool use_share)
{
    const ConfigOptionPoints *bed_shape_opt = config.opt<ConfigOptionPoints>("printable_area");
    if (!bed_shape_opt) {

        // Here, it is certain that the bed shape is missing, so an infinite one
        // has to be used, but still, the center of bed can be queried
        if (auto center_opt = config.opt<ConfigOptionPoint>("center"))
            return { scaled(center_opt->value) };

        return {};
    }

    Polygon bed_poly;
    if (use_share) {
        const ConfigOptionPointsGroups *extruder_area_opt = config.opt<ConfigOptionPointsGroups>("extruder_printable_area");
        if (extruder_area_opt && (extruder_area_opt->size() > 0)) {
            const std::vector<Pointfs>& extruder_areas = extruder_area_opt->values;
            bed_poly = get_shared_poly(extruder_areas);
        }
        else
            bed_poly.points = to_points(make_counter_clockwise(bed_shape_opt->values));
    }
    else
        bed_poly.points = to_points(make_counter_clockwise(bed_shape_opt->values));

    return bed_poly.points;
}

Points get_bed_shape(const PrintConfig &cfg, bool use_share)
{
    Polygon bed_poly;
    if (use_share) {
        const std::vector<Pointfs>& extruder_areas = cfg.extruder_printable_area.values;
        if (extruder_areas.size() > 0) {
            bed_poly = get_shared_poly(extruder_areas);
        }
        else
            bed_poly.points = to_points(make_counter_clockwise(cfg.printable_area.values));
    }
    else
        bed_poly.points = to_points(make_counter_clockwise(cfg.printable_area.values));

    return bed_poly.points;
}

Points get_bed_shape(const SLAPrinterConfig &cfg) { return to_points(make_counter_clockwise(cfg.printable_area.values)); }

Polygons get_bed_excluded_area(const PrintConfig& cfg)
{
    const Pointfs exclude_area_points = cfg.bed_exclude_area.values;

    Polygon exclude_poly;
    for (int i = 0; i < exclude_area_points.size(); i++) {
        auto pt = exclude_area_points[i];
        exclude_poly.points.emplace_back(scale_(pt.x()), scale_(pt.y()));
    }

    exclude_poly.make_counter_clockwise();

    return {exclude_poly};
}

Polygon get_bed_shape_with_excluded_area(const PrintConfig& cfg, bool use_share)
{
    Polygon bed_poly;
    bed_poly.points = get_bed_shape(cfg, use_share);


    Polygons exclude_polys = get_bed_excluded_area(cfg);
    auto tmp = diff({ bed_poly }, exclude_polys);
    if (!tmp.empty()) bed_poly = tmp[0];
    return bed_poly;
}
bool has_skirt(const DynamicPrintConfig& cfg)
{
    auto opt_skirt_height = cfg.option("skirt_height");
    auto opt_skirt_loops = cfg.option("skirt_loops");
    auto opt_draft_shield = cfg.option("draft_shield");
    return (opt_skirt_height && opt_skirt_height->getInt() > 0 && opt_skirt_loops && opt_skirt_loops->getInt() > 0)
        || (opt_draft_shield && opt_draft_shield->getInt() != dsDisabled);
}
float get_real_skirt_dist(const DynamicPrintConfig& cfg) {
    if (!has_skirt(cfg)) return 0.f;

    float dist = cfg.opt_float("skirt_distance");

    int loops = cfg.opt_int("skirt_loops");
    auto opt_draft_shield = cfg.option("draft_shield");
    if (opt_draft_shield && opt_draft_shield->getInt() != dsDisabled && loops == 0) {
        loops = 1;
    }

    float width = cfg.opt_float("initial_layer_line_width");
    if (width <= 0.f) {
        width = cfg.opt_float("line_width");
    }
    if (width <= 0.f) {
        auto* nd = cfg.opt<ConfigOptionFloats>("nozzle_diameter");
        if (nd && !nd->values.empty()) {
            width = *std::max_element(nd->values.begin(), nd->values.end());
        } else {
            width = 0.4f;
        }
    }

    return dist + loops * width;
}

static bool is_XL_printer(const std::string& printer_notes)
{
    return boost::algorithm::contains(printer_notes, "PRINTER_VENDOR_PRUSA3D")
        && boost::algorithm::contains(printer_notes, "PRINTER_MODEL_XL");
}

bool is_XL_printer(const DynamicPrintConfig &cfg)
{
    auto *printer_notes = cfg.opt<ConfigOptionString>("printer_notes");
    return printer_notes && is_XL_printer(printer_notes->value);
}

bool is_XL_printer(const PrintConfig &cfg)
{
    return is_XL_printer(cfg.printer_notes.value);
}
} // namespace Slic3r

#include <cereal/types/polymorphic.hpp>
CEREAL_REGISTER_TYPE(Slic3r::DynamicPrintConfig)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::DynamicConfig, Slic3r::DynamicPrintConfig)
