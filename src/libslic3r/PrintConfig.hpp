// Configuration store of Slic3r.
//
// The configuration store is either static or dynamic.
// DynamicPrintConfig is used mainly at the user interface. while the StaticPrintConfig is used
// during the slicing and the g-code generation.
//
// The classes derived from StaticPrintConfig form a following hierarchy.
//
// FullPrintConfig
//    PrintObjectConfig
//    PrintRegionConfig
//    PrintConfig
//        GCodeConfig
//

#ifndef slic3r_PrintConfig_hpp_
#define slic3r_PrintConfig_hpp_


#include "PrintConfigBase.hpp"

#include <boost/container_hash/hash.hpp>
#include <boost/preprocessor/facilities/empty.hpp>
#include <boost/preprocessor/punctuation/comma_if.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/seq/for_each_i.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/tuple/elem.hpp>
#include <boost/preprocessor/tuple/to_seq.hpp>

namespace Slic3r {


#define STATIC_PRINT_CONFIG_CACHE_BASE(CLASS_NAME) \
public: \
    /* Overrides ConfigBase::optptr(). Find ando/or create a ConfigOption instance for a given name. */ \
    const ConfigOption*      optptr(const t_config_option_key &opt_key) const override \
        { return s_cache_##CLASS_NAME.optptr(opt_key, this); } \
    /* Overrides ConfigBase::optptr(). Find ando/or create a ConfigOption instance for a given name. */ \
    ConfigOption*            optptr(const t_config_option_key &opt_key, bool create = false) override \
        { return s_cache_##CLASS_NAME.optptr(opt_key, this); } \
    /* Overrides ConfigBase::keys(). Collect names of all configuration values maintained by this configuration store. */ \
    t_config_option_keys     keys() const override { return s_cache_##CLASS_NAME.keys(); } \
    const t_config_option_keys& keys_ref() const override { return s_cache_##CLASS_NAME.keys(); } \
    static const CLASS_NAME& defaults() { assert(s_cache_##CLASS_NAME.initialized()); return s_cache_##CLASS_NAME.defaults(); } \
private: \
    friend int print_config_static_initializer(); \
    static void initialize_cache() \
    { \
        assert(! s_cache_##CLASS_NAME.initialized()); \
        if (! s_cache_##CLASS_NAME.initialized()) { \
            CLASS_NAME *inst = new CLASS_NAME(1); \
            inst->initialize(s_cache_##CLASS_NAME, (const char*)inst); \
            s_cache_##CLASS_NAME.finalize(inst, inst->def()); \
        } \
    } \
    /* Cache object holding a key/option map, a list of option keys and a copy of this static config initialized with the defaults. */ \
    static StaticPrintConfig::StaticCache<CLASS_NAME> s_cache_##CLASS_NAME;

#define STATIC_PRINT_CONFIG_CACHE(CLASS_NAME) \
    STATIC_PRINT_CONFIG_CACHE_BASE(CLASS_NAME) \
public: \
    /* Public default constructor will initialize the key/option cache and the default object copy if needed. */ \
    CLASS_NAME() { assert(s_cache_##CLASS_NAME.initialized()); *this = s_cache_##CLASS_NAME.defaults(); } \
protected: \
    /* Protected constructor to be called when compounded. */ \
    CLASS_NAME(int) {}

#define STATIC_PRINT_CONFIG_CACHE_DERIVED(CLASS_NAME) \
    STATIC_PRINT_CONFIG_CACHE_BASE(CLASS_NAME) \
public: \
    /* Overrides ConfigBase::def(). Static configuration definition. Any value stored into this ConfigBase shall have its definition here. */ \
    const ConfigDef*    def() const override { return &print_config_def; } \
    /* Handle legacy and obsoleted config keys */ \
    void                handle_legacy(t_config_option_key &opt_key, std::string &value) const override \
        { PrintConfigDef::handle_legacy(opt_key, value); }

#define PRINT_CONFIG_CLASS_ELEMENT_DEFINITION(r, data, elem) BOOST_PP_TUPLE_ELEM(0, elem) BOOST_PP_TUPLE_ELEM(1, elem);
#define PRINT_CONFIG_CLASS_ELEMENT_VISIT(r, data, elem) if (! f(BOOST_PP_STRINGIZE(BOOST_PP_TUPLE_ELEM(1, elem)), this->BOOST_PP_TUPLE_ELEM(1, elem), rhs.BOOST_PP_TUPLE_ELEM(1, elem))) return;
#define PRINT_CONFIG_CLASS_ELEMENT_VISIT_SELF(r, data, elem) if (! f(BOOST_PP_STRINGIZE(BOOST_PP_TUPLE_ELEM(1, elem)), self.BOOST_PP_TUPLE_ELEM(1, elem), rhs.BOOST_PP_TUPLE_ELEM(1, elem))) return;
// Each option list is expanded into the members and again into for_each_option_pair(), which calls
// f(key, this->option, rhs.option) in declaration order and stops when f returns false. hash(),
// operator==, operator<, initialize() and apply_to() iterate the options through that visitor.
#define PRINT_CONFIG_CLASS_COMMON_BODY(CLASS_NAME) \
    size_t hash() const throw() \
    { \
        size_t seed = 0; \
        this->for_each_option_pair(*this, [&seed](const char*, const auto &a, const auto&) { boost::hash_combine(seed, a.hash()); return true; }); \
        return seed; \
    } \
    bool operator==(const CLASS_NAME &rhs) const throw() \
    { \
        bool eq = true; \
        this->for_each_option_pair(rhs, [&eq](const char*, const auto &a, const auto &b) { eq = (a == b); return eq; }); \
        return eq; \
    } \
    bool operator!=(const CLASS_NAME &rhs) const throw() { return ! (*this == rhs); } \
    bool operator<(const CLASS_NAME &rhs) const throw() \
    { \
        int c = 0; \
        this->for_each_option_pair(rhs, [&c](const char*, const auto &a, const auto &b) { if (a < b) c = -1; else if (! (a == b)) c = 1; return c == 0; }); \
        return c < 0; \
    } \
protected: \
    void initialize(StaticCacheBase &cache, const char *base_ptr) \
    { \
        this->for_each_option_pair(*this, [&cache, base_ptr](const char *key, const auto &a, const auto&) { cache.opt_add(key, base_ptr, a); return true; }); \
    }

#define PRINT_CONFIG_CLASS_DEFINE(CLASS_NAME, PARAMETER_DEFINITION_SEQ) \
class CLASS_NAME : public StaticPrintConfig { \
    STATIC_PRINT_CONFIG_CACHE(CLASS_NAME) \
public: \
    BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CLASS_ELEMENT_DEFINITION, _, PARAMETER_DEFINITION_SEQ) \
    template<typename F> void for_each_option_pair(const CLASS_NAME &rhs, F &&f) const { visit_option_pairs(*this, rhs, f); } \
    /* Defined in PrintConfig.cpp. */ \
    bool apply_to(ConfigBase &target) const override; \
    PRINT_CONFIG_CLASS_COMMON_BODY(CLASS_NAME) \
private: \
    /* The one expansion of the option list, for a const self and for apply_to()'s mutable target. */ \
    template<typename Self, typename F> static void visit_option_pairs(Self &self, const CLASS_NAME &rhs, F &&f) \
    { \
        BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CLASS_ELEMENT_VISIT_SELF, _, PARAMETER_DEFINITION_SEQ) \
    } \
};

#define PRINT_CONFIG_CLASS_DERIVED_CLASS_LIST_ITEM(r, data, i, elem) BOOST_PP_COMMA_IF(i) public elem
#define PRINT_CONFIG_CLASS_DERIVED_CLASS_LIST(CLASSES_PARENTS_TUPLE) BOOST_PP_SEQ_FOR_EACH_I(PRINT_CONFIG_CLASS_DERIVED_CLASS_LIST_ITEM, _, BOOST_PP_TUPLE_TO_SEQ(CLASSES_PARENTS_TUPLE))
#define PRINT_CONFIG_CLASS_DERIVED_INITIALIZER_ITEM(r, VALUE, i, elem) BOOST_PP_COMMA_IF(i) elem(VALUE)
#define PRINT_CONFIG_CLASS_DERIVED_INITIALIZER(CLASSES_PARENTS_TUPLE, VALUE) BOOST_PP_SEQ_FOR_EACH_I(PRINT_CONFIG_CLASS_DERIVED_INITIALIZER_ITEM, VALUE, BOOST_PP_TUPLE_TO_SEQ(CLASSES_PARENTS_TUPLE))
#define PRINT_CONFIG_CLASS_DERIVED_INITCACHE_ITEM(r, data, elem) this->elem::initialize(cache, base_ptr);
#define PRINT_CONFIG_CLASS_DERIVED_INITCACHE(CLASSES_PARENTS_TUPLE) BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CLASS_DERIVED_INITCACHE_ITEM, _, BOOST_PP_TUPLE_TO_SEQ(CLASSES_PARENTS_TUPLE))
#define PRINT_CONFIG_CLASS_DERIVED_HASH(r, data, elem) boost::hash_combine(seed, static_cast<const elem*>(this)->hash());
#define PRINT_CONFIG_CLASS_DERIVED_EQUAL(r, data, elem) \
    if (! (*static_cast<const elem*>(this) == static_cast<const elem&>(rhs))) return false;

// Generic version, with or without new parameters. Don't use this directly.
#define PRINT_CONFIG_CLASS_DERIVED_DEFINE1(CLASS_NAME, CLASSES_PARENTS_TUPLE, PARAMETER_DEFINITION, PARAMETER_VISIT) \
class CLASS_NAME : PRINT_CONFIG_CLASS_DERIVED_CLASS_LIST(CLASSES_PARENTS_TUPLE) { \
    STATIC_PRINT_CONFIG_CACHE_DERIVED(CLASS_NAME) \
    CLASS_NAME() : PRINT_CONFIG_CLASS_DERIVED_INITIALIZER(CLASSES_PARENTS_TUPLE, 0) { assert(s_cache_##CLASS_NAME.initialized()); *this = s_cache_##CLASS_NAME.defaults(); } \
public: \
    PARAMETER_DEFINITION \
    template<typename F> void for_each_option_pair(const CLASS_NAME &rhs, F &&f) const { PARAMETER_VISIT } \
    /* Its parents each apply themselves to a target member by member, so this one keeps the lookup by name. */ \
    bool apply_to(ConfigBase &/*target*/) const override { return false; } \
    size_t hash() const throw() \
    { \
        size_t seed = 0; \
        BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CLASS_DERIVED_HASH, _, BOOST_PP_TUPLE_TO_SEQ(CLASSES_PARENTS_TUPLE)) \
        this->for_each_option_pair(*this, [&seed](const char*, const auto &a, const auto&) { boost::hash_combine(seed, a.hash()); return true; }); \
        return seed; \
    } \
    bool operator==(const CLASS_NAME &rhs) const throw() \
    { \
        BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CLASS_DERIVED_EQUAL, _, BOOST_PP_TUPLE_TO_SEQ(CLASSES_PARENTS_TUPLE)) \
        bool eq = true; \
        this->for_each_option_pair(rhs, [&eq](const char*, const auto &a, const auto &b) { eq = (a == b); return eq; }); \
        return eq; \
    } \
    bool operator!=(const CLASS_NAME &rhs) const throw() { return ! (*this == rhs); } \
protected: \
    CLASS_NAME(int) : PRINT_CONFIG_CLASS_DERIVED_INITIALIZER(CLASSES_PARENTS_TUPLE, 1) {} \
    void initialize(StaticCacheBase &cache, const char* base_ptr) { \
        PRINT_CONFIG_CLASS_DERIVED_INITCACHE(CLASSES_PARENTS_TUPLE) \
        this->for_each_option_pair(*this, [&cache, base_ptr](const char *key, const auto &a, const auto&) { cache.opt_add(key, base_ptr, a); return true; }); \
    } \
};
// Variant without adding new parameters.
#define PRINT_CONFIG_CLASS_DERIVED_DEFINE0(CLASS_NAME, CLASSES_PARENTS_TUPLE) \
    PRINT_CONFIG_CLASS_DERIVED_DEFINE1(CLASS_NAME, CLASSES_PARENTS_TUPLE, BOOST_PP_EMPTY(), BOOST_PP_EMPTY())
// Variant with adding new parameters.
#define PRINT_CONFIG_CLASS_DERIVED_DEFINE(CLASS_NAME, CLASSES_PARENTS_TUPLE, PARAMETER_DEFINITION_SEQ) \
    PRINT_CONFIG_CLASS_DERIVED_DEFINE1(CLASS_NAME, CLASSES_PARENTS_TUPLE, \
        BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CLASS_ELEMENT_DEFINITION, _, PARAMETER_DEFINITION_SEQ), \
        BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CLASS_ELEMENT_VISIT, _, PARAMETER_DEFINITION_SEQ))

// This object is mapped to Perl as Slic3r::Config::PrintObject.
PRINT_CONFIG_CLASS_DEFINE(
    PrintObjectConfig,

    ((ConfigOptionFloat,               brim_object_gap))
    ((ConfigOptionFloat,               brim_flow_ratio))
    ((ConfigOptionBool,                brim_use_efc_outline))
    ((ConfigOptionEnum<BrimType>,      brim_type))
    ((ConfigOptionFloat,               brim_width))
    ((ConfigOptionFloat,               leading_brim_length))
    ((ConfigOptionFloat,               extra_brim_width))
    ((ConfigOptionFloat,               brim_ears_detection_length))
    ((ConfigOptionFloat,               brim_ears_max_angle))
    ((ConfigOptionBool,                brim_ears_outer_only))
    ((ConfigOptionFloat,               skirt_start_angle))
    ((ConfigOptionBool,                bridge_no_support))
    ((ConfigOptionFloat,               elefant_foot_compensation))
    ((ConfigOptionInt,                 elefant_foot_compensation_layers))
    ((ConfigOptionPercent,             elefant_foot_layers_density))
    ((ConfigOptionFloat,               max_bridge_length))
    ((ConfigOptionFloatOrPercent,      line_width))
    // Force the generation of solid shells between adjacent materials/volumes.
    ((ConfigOptionBool,                interface_shells))
    ((ConfigOptionFloat,               layer_height))
    ((ConfigOptionFloat,               mmu_segmented_region_max_width))
    ((ConfigOptionFloat,               mmu_segmented_region_interlocking_depth))
    ((ConfigOptionFloat,               raft_contact_distance))
    ((ConfigOptionFloat,               raft_expansion))
    ((ConfigOptionPercent,             raft_first_layer_density))
    ((ConfigOptionFloat,               raft_first_layer_expansion))
    ((ConfigOptionInt,                 raft_layers))
    ((ConfigOptionEnum<SeamPosition>,  seam_position))
    ((ConfigOptionBool,                staggered_inner_seams))
    ((ConfigOptionFloat,               slice_closing_radius))
    ((ConfigOptionEnum<SlicingMode>,   slicing_mode))
    ((ConfigOptionBool,                enable_support))
    // Automatic supports (generated based on support_threshold_angle).
    ((ConfigOptionEnum<SupportType>,   support_type))
    // Direction of the support pattern (in XY plane).`
    ((ConfigOptionFloat,               support_angle))
    ((ConfigOptionBool,                support_on_build_plate_only))
    ((ConfigOptionBool,                support_critical_regions_only))
    ((ConfigOptionBool,                support_remove_small_overhang))
    ((ConfigOptionFloat,               support_top_z_distance))
    ((ConfigOptionFloat,               support_bottom_z_distance))
    ((ConfigOptionInt,                 enforce_support_layers))
    ((ConfigOptionInt,                 support_filament))
    ((ConfigOptionFloatOrPercent,      support_line_width))
    ((ConfigOptionBool,                support_interface_not_for_body))
    ((ConfigOptionBool,                support_interface_loop_pattern))
    ((ConfigOptionInt,                 support_interface_filament))
    ((ConfigOptionInt,                 support_interface_top_layers))
    ((ConfigOptionInt,                 support_interface_bottom_layers))
    // Spacing between interface lines (the hatching distance). Set zero to get a solid interface.
    ((ConfigOptionFloat,               support_interface_spacing))
    ((ConfigOptionFloatsNullable,      support_interface_speed))
    ((ConfigOptionEnum<SupportMaterialPattern>, support_base_pattern))
    ((ConfigOptionEnum<SupportMaterialInterfacePattern>, support_interface_pattern))
    // Spacing between support material lines (the hatching distance).
    ((ConfigOptionFloat,               support_base_pattern_spacing))
    ((ConfigOptionFloat,               support_expansion))
    ((ConfigOptionFloatsNullable,      support_speed))
    ((ConfigOptionEnum<SupportMaterialStyle>, support_style))

    // Orca: a flag enabling the ability to override flow ratios
    ((ConfigOptionBool,     set_other_flow_ratios))
    // Orca: support-related flow ratios (available for overriding, if set_other_flow_ratios is enabled)
    ((ConfigOptionFloat,    support_flow_ratio))
    ((ConfigOptionFloat,    support_interface_flow_ratio))

    // BBS
    //((ConfigOptionBool,                independent_support_layer_height))
    // Orca internal thick bridge
    ((ConfigOptionBool,                thick_bridges))
    ((ConfigOptionBool,                thick_internal_bridges))
    ((ConfigOptionEnum<InternalBridgeFilter>,  dont_filter_internal_bridges))
    // Orca
    ((ConfigOptionEnum<EnableExtraBridgeLayer>,  enable_extra_bridge_layer))
    ((ConfigOptionPercent,              internal_bridge_density))
    // Overhang angle threshold.
    ((ConfigOptionInt,                 support_threshold_angle))
    ((ConfigOptionFloatOrPercent,      support_threshold_overlap))
    ((ConfigOptionFloat,               support_object_xy_distance))
    ((ConfigOptionFloat,               support_object_first_layer_gap))
    ((ConfigOptionBool,                support_ironing))
    ((ConfigOptionEnum<InfillPattern>, support_ironing_pattern))
    ((ConfigOptionPercent,             support_ironing_flow))
    ((ConfigOptionFloat,               support_ironing_spacing))
    ((ConfigOptionFloat,               xy_hole_compensation))
    ((ConfigOptionFloat,               xy_contour_compensation))
    ((ConfigOptionBool,                flush_into_objects))
    // BBS
    ((ConfigOptionBool,                flush_into_infill))
    ((ConfigOptionBool,                flush_into_support))
    // Marker for the auto-generated belt purge prism; identifies the object to
    // the auto-manager (GUI) and the layer-grid alignment step (backend).
    ((ConfigOptionBool,                belt_purge_tower_object))
    // BBS
    ((ConfigOptionFloat,              tree_support_branch_distance))
    ((ConfigOptionFloat,              tree_support_tip_diameter))
    ((ConfigOptionFloat,              tree_support_branch_diameter))
    ((ConfigOptionFloat,              tree_support_branch_angle))
    ((ConfigOptionFloat,              tree_support_branch_diameter_angle))
    ((ConfigOptionFloat,              tree_support_angle_slow))
    ((ConfigOptionInt,                tree_support_wall_count))
    ((ConfigOptionBool,               tree_support_auto_brim))
    ((ConfigOptionFloat,              tree_support_brim_width))
    ((ConfigOptionBool,               detect_narrow_internal_solid_infill))
    // ((ConfigOptionBool,               adaptive_layer_height))
    ((ConfigOptionFloat,              support_bottom_interface_spacing))
    ((ConfigOptionEnum<PerimeterGeneratorType>, wall_generator))
    ((ConfigOptionPercent,            wall_transition_length))
    ((ConfigOptionPercent,            wall_transition_filter_deviation))
    ((ConfigOptionFloat,              wall_transition_angle))
    ((ConfigOptionInt,                wall_distribution_count))
    ((ConfigOptionPercent,            min_feature_size))
    ((ConfigOptionPercent,            initial_layer_min_bead_width))
    ((ConfigOptionPercent,            min_bead_width))

    // Orca
    ((ConfigOptionFloat,              wall_maximum_resolution))
    ((ConfigOptionFloat,              wall_maximum_deviation))
    ((ConfigOptionFloat,              make_overhang_printable_angle))
    ((ConfigOptionFloat,              make_overhang_printable_hole_size))
    ((ConfigOptionFloat,              tree_support_branch_distance_organic))
    ((ConfigOptionPercent,            tree_support_top_rate))
    ((ConfigOptionFloat,              tree_support_branch_diameter_organic))
    ((ConfigOptionFloat,              tree_support_branch_angle_organic))
    ((ConfigOptionEnum<GapFillTarget>,gap_fill_target))
    ((ConfigOptionFloat,              min_length_factor))

    // Move all acceleration and jerk settings to object
    ((ConfigOptionFloatsNullable,           default_acceleration))
    ((ConfigOptionFloatsNullable,           outer_wall_acceleration))
    ((ConfigOptionFloatsNullable,           inner_wall_acceleration))
    ((ConfigOptionFloatsNullable,           top_surface_acceleration))
    ((ConfigOptionFloatsNullable,           initial_layer_acceleration))
    ((ConfigOptionFloatsOrPercentsNullable, bridge_acceleration))
    ((ConfigOptionFloatsNullable,           travel_acceleration))
    ((ConfigOptionFloatsOrPercentsNullable, sparse_infill_acceleration))
    ((ConfigOptionFloatsOrPercentsNullable, internal_solid_infill_acceleration))

    ((ConfigOptionFloatsNullable,     default_jerk))
    ((ConfigOptionFloatsNullable,     outer_wall_jerk))
    ((ConfigOptionFloatsNullable,     inner_wall_jerk))
    ((ConfigOptionFloatsNullable,     infill_jerk))
    ((ConfigOptionFloatsNullable,     top_surface_jerk))
    ((ConfigOptionFloatsNullable,     initial_layer_jerk))
    ((ConfigOptionFloatsNullable,     travel_jerk))
    ((ConfigOptionBool,               precise_z_height))
    ((ConfigOptionFloatsNullable,     default_junction_deviation))

    ((ConfigOptionBool, interlocking_beam))
    ((ConfigOptionFloat,interlocking_beam_width))
    ((ConfigOptionFloat,interlocking_orientation))
    ((ConfigOptionInt,  interlocking_beam_layer_count))
    ((ConfigOptionInt,  interlocking_depth))
    ((ConfigOptionInt,  interlocking_boundary_avoidance))

    // Orca: internal use only
    ((ConfigOptionBool,  calib_flowrate_topinfill_special_order)) // ORCA: special flag for flow rate calibration

    // IDEX/IQEX parallel print mode (per-print selection, stores mode name or "primary")
    ((ConfigOptionString,              imex_parallel_mode))
    // Per-plate head→filament override for MMU-equipped printers.
    // Serialized as compact "phys:slot,phys:slot" (1-based filament slots, matches UI).
    // Empty string means "use first_filament_for_physical_head defaults everywhere".
    ((ConfigOptionString,              imex_head_filament_map))
)

// This object is mapped to Perl as Slic3r::Config::PrintRegion.
PRINT_CONFIG_CLASS_DEFINE(
    PrintRegionConfig,

    ((ConfigOptionInts,  print_extruder_id))
    ((ConfigOptionStrings,  print_extruder_variant))
    ((ConfigOptionInt,                  bottom_shell_layers))
    ((ConfigOptionFloat,                bottom_shell_thickness))
    ((ConfigOptionInt,                  bottom_color_penetration_layers))
    ((ConfigOptionFloat,                bridge_angle))
    ((ConfigOptionFloat,                internal_bridge_angle)) // ORCA: Internal bridge angle override
    ((ConfigOptionBool,                 relative_bridge_angle)) // ORCA: Relative bridge angle flag
    ((ConfigOptionFloat,                bridge_flow))
    ((ConfigOptionFloatOrPercent,       bridge_line_width))
    ((ConfigOptionFloat,                internal_bridge_flow))
    ((ConfigOptionFloatsNullable,       bridge_speed))
    ((ConfigOptionFloatsOrPercentsNullable, internal_bridge_speed))
    ((ConfigOptionEnum<EnsureVerticalShellThickness>,   ensure_vertical_shell_thickness))
    ((ConfigOptionPercent,              top_surface_density))
    ((ConfigOptionPercent,               bottom_surface_density))
    ((ConfigOptionEnum<InfillPattern>,  top_surface_pattern))
    ((ConfigOptionEnum<InfillPattern>,  bottom_surface_pattern))
    ((ConfigOptionEnum<SurfaceFillOrder>, top_surface_fill_order))
    ((ConfigOptionEnum<SurfaceFillOrder>, bottom_surface_fill_order))
    ((ConfigOptionEnum<InfillPattern>, internal_solid_infill_pattern))
    ((ConfigOptionFloatOrPercent,       outer_wall_line_width))
    ((ConfigOptionFloatsNullable,       outer_wall_speed))
    ((ConfigOptionFloat,                infill_direction))
    ((ConfigOptionFloat,                solid_infill_direction))
    ((ConfigOptionFloat,                top_layer_direction))
    ((ConfigOptionFloat,                bottom_layer_direction))
    ((ConfigOptionString,               solid_infill_rotate_template))
    ((ConfigOptionBool,                 symmetric_infill_y_axis))
    ((ConfigOptionBool,                 infill_complete_top))
    ((ConfigOptionFloat,                infill_shift_step))
    ((ConfigOptionString,               sparse_infill_rotate_template))
    ((ConfigOptionPercent,              sparse_infill_density))
    ((ConfigOptionEnum<InfillPattern>,  sparse_infill_pattern))
    ((ConfigOptionPercent,              sparse_infill_smooth_factor))
    ((ConfigOptionFloat,                lateral_lattice_angle_1))
    ((ConfigOptionFloat,                lateral_lattice_angle_2))
    ((ConfigOptionFloat,                infill_overhang_angle))
    ((ConfigOptionFloat,                lightning_overhang_angle))
    ((ConfigOptionFloat,                lightning_prune_angle))
    ((ConfigOptionFloat,                lightning_straightening_angle))
    ((ConfigOptionBool,                 align_infill_direction_to_model))
    ((ConfigOptionEnum<CenterOfSurfacePattern>, center_of_surface_pattern))
    ((ConfigOptionBool,                 separated_infills))
    ((ConfigOptionString,               extra_solid_infills))
    ((ConfigOptionEnum<FuzzySkinType>,  fuzzy_skin))
    ((ConfigOptionFloat,                fuzzy_skin_thickness))
    ((ConfigOptionFloat,                fuzzy_skin_point_distance))
    ((ConfigOptionBool,                 fuzzy_skin_first_layer))
    ((ConfigOptionEnum<NoiseType>,      fuzzy_skin_noise_type))
    ((ConfigOptionEnum<FuzzySkinMode>,  fuzzy_skin_mode))
    ((ConfigOptionFloat,                fuzzy_skin_scale))
    ((ConfigOptionInt,                  fuzzy_skin_octaves))
    ((ConfigOptionFloat,                fuzzy_skin_persistence))
    ((ConfigOptionInt,                  fuzzy_skin_ripples_per_layer))
    ((ConfigOptionPercent,              fuzzy_skin_ripple_offset))
    ((ConfigOptionInt,                  fuzzy_skin_layers_between_ripple_offset))
    ((ConfigOptionFloatsNullable,       gap_infill_speed))
    ((ConfigOptionInt,                  sparse_infill_filament_id))
    ((ConfigOptionFloatOrPercent,       sparse_infill_line_width))
    ((ConfigOptionPercent,              infill_wall_overlap))
    ((ConfigOptionPercent,              top_bottom_infill_wall_overlap))
    ((ConfigOptionFloatsNullable,       sparse_infill_speed))
    ((ConfigOptionPercent, skeleton_infill_density))
    ((ConfigOptionPercent, skin_infill_density))
    ((ConfigOptionFloat, infill_lock_depth))
    ((ConfigOptionFloat, skin_infill_depth))
    ((ConfigOptionFloatOrPercent, skin_infill_line_width))
    ((ConfigOptionFloatOrPercent, skeleton_infill_line_width))
    ((ConfigOptionBool, infill_combination))
    // Orca:
    ((ConfigOptionFloatOrPercent,                infill_combination_max_layer_height))
    ((ConfigOptionInt,                  fill_multiline))
    ((ConfigOptionEnum<AdaptiveInfillMode>, adaptive_infill_mode))
    ((ConfigOptionPercent,              adaptive_infill_interior_density))
    ((ConfigOptionEnum<AdaptiveInfillGradient>, adaptive_infill_gradient))
    ((ConfigOptionBool,                 gyroid_optimized))
    // Ironing options
    ((ConfigOptionEnum<IroningType>, ironing_type))
    ((ConfigOptionEnum<InfillPattern>, ironing_pattern))
    ((ConfigOptionPercent, ironing_flow))
    ((ConfigOptionFloat, ironing_spacing))
    ((ConfigOptionFloat, ironing_inset))
    ((ConfigOptionFloat, ironing_direction))
    ((ConfigOptionFloat, ironing_speed))
    ((ConfigOptionFloat, ironing_angle))
    ((ConfigOptionBool, ironing_angle_fixed))
    // Filament Ironing
    ((ConfigOptionPercentsNullable, filament_ironing_flow))
    ((ConfigOptionFloatsNullable, filament_ironing_spacing))
    ((ConfigOptionFloatsNullable, filament_ironing_inset))
    ((ConfigOptionFloatsNullable, filament_ironing_speed))
    // Detect bridging perimeters
    ((ConfigOptionBool, detect_overhang_wall))
    ((ConfigOptionBool, unsupported_wall_last))
    ((ConfigOptionInt, outer_wall_filament_id))
    ((ConfigOptionInt, inner_wall_filament_id))
    ((ConfigOptionFloatOrPercent, inner_wall_line_width))
    ((ConfigOptionFloatsNullable, inner_wall_speed))
    // Total number of perimeters.
    ((ConfigOptionInt, wall_loops))
    ((ConfigOptionBool, alternate_extra_wall))
    ((ConfigOptionFloat, minimum_sparse_infill_area))
    ((ConfigOptionInt, internal_solid_filament_id))
    ((ConfigOptionInt, top_surface_filament_id))
    ((ConfigOptionInt, bottom_surface_filament_id))
    ((ConfigOptionFloatOrPercent, internal_solid_infill_line_width))
    ((ConfigOptionFloatsNullable, internal_solid_infill_speed))
    // Detect thin walls.
    ((ConfigOptionBool, detect_thin_wall))
    ((ConfigOptionFloatOrPercent, top_surface_line_width))
    ((ConfigOptionInt, top_shell_layers))
    ((ConfigOptionFloat, top_shell_thickness))
    ((ConfigOptionInt, top_color_penetration_layers))
    ((ConfigOptionFloat, top_surface_expansion))
    ((ConfigOptionFloat, top_surface_expansion_margin))
    ((ConfigOptionEnum<TopSurfaceExpansionDirection>, top_surface_expansion_direction))
    ((ConfigOptionFloatsNullable, top_surface_speed))
    //BBS
    ((ConfigOptionBoolsNullable,            enable_overhang_speed))
    ((ConfigOptionFloatsOrPercentsNullable, overhang_1_4_speed))
    ((ConfigOptionFloatsOrPercentsNullable, overhang_2_4_speed))
    ((ConfigOptionFloatsOrPercentsNullable, overhang_3_4_speed))
    ((ConfigOptionFloatsOrPercentsNullable, overhang_4_4_speed))
    ((ConfigOptionBool,                 only_one_wall_top))

    //SoftFever
    ((ConfigOptionFloatOrPercent,       min_width_top_surface))
    ((ConfigOptionBool,                 only_one_wall_first_layer))
    ((ConfigOptionFloat,                print_flow_ratio))
    ((ConfigOptionFloatOrPercent,       seam_gap))
    ((ConfigOptionBool,                 role_based_wipe_speed))
    ((ConfigOptionFloatOrPercent,       wipe_speed))
    ((ConfigOptionBool,                 wipe_on_loops))
    ((ConfigOptionBool,                 wipe_inward))
    ((ConfigOptionFloatOrPercent,        wipe_inward_distance))
    ((ConfigOptionBool,                 wipe_before_external_loop))
    ((ConfigOptionEnum<WallInfillOrder>, wall_infill_order))
    ((ConfigOptionBool,                 precise_outer_wall))
    ((ConfigOptionPercent,              bridge_density))
    ((ConfigOptionFloat,                 filter_out_gap_fill))
    ((ConfigOptionFloatsOrPercentsNullable, small_perimeter_speed))
    ((ConfigOptionFloatsNullable,           small_perimeter_threshold))
    ((ConfigOptionFloatsOrPercentsNullable, small_support_perimeter_speed))
    ((ConfigOptionFloatsNullable,           small_support_perimeter_threshold))
    ((ConfigOptionFloatsNullable,       top_solid_infill_flow_ratio))
    ((ConfigOptionFloat,                bottom_solid_infill_flow_ratio))
    ((ConfigOptionFloatOrPercent,       infill_anchor))
    ((ConfigOptionFloatOrPercent,       infill_anchor_max))

    // Orca
    ((ConfigOptionBool,                 make_overhang_printable))
    ((ConfigOptionBool,                 extra_perimeters_on_overhangs))
    ((ConfigOptionBoolsNullable,        slowdown_for_curled_perimeters))
    ((ConfigOptionBool,                 hole_to_polyhole))
    ((ConfigOptionFloatOrPercent,       hole_to_polyhole_threshold))
    ((ConfigOptionBool,                 hole_to_polyhole_twisted))
    ((ConfigOptionInt,                  hole_to_polyhole_max_edges))

    ((ConfigOptionBool,                 overhang_reverse))
    ((ConfigOptionBool,                 overhang_reverse_internal_only))
    ((ConfigOptionFloatOrPercent,       overhang_reverse_threshold))
    ((ConfigOptionEnum<CounterboreHoleBridgingOption>, counterbore_hole_bridging))
    ((ConfigOptionEnum<WallSequence>,  wall_sequence))
    ((ConfigOptionBool,                is_infill_first))
    ((ConfigOptionBool,                small_area_infill_flow_compensation))
    ((ConfigOptionEnum<WallDirection>,  wall_direction))

    // Orca: other flow ratios (available for overriding, if set_other_flow_ratios is enabled)
    ((ConfigOptionFloat,                first_layer_flow_ratio))
    ((ConfigOptionFloat,                outer_wall_flow_ratio))
    ((ConfigOptionFloat,                inner_wall_flow_ratio))
    ((ConfigOptionFloat,                overhang_flow_ratio))
    ((ConfigOptionFloat,                sparse_infill_flow_ratio))
    ((ConfigOptionFloat,                internal_solid_infill_flow_ratio))
    ((ConfigOptionFloat,                gap_fill_flow_ratio))

    // Orca: seam slopes
    ((ConfigOptionEnum<SeamScarfType>,  seam_slope_type))
    ((ConfigOptionBool,                 seam_slope_conditional))
    ((ConfigOptionInt,                  scarf_angle_threshold))
    ((ConfigOptionFloatOrPercent,       seam_slope_start_height))
    ((ConfigOptionBool,                 seam_slope_entire_loop))
    ((ConfigOptionFloat,                seam_slope_min_length))
    ((ConfigOptionInt,                  seam_slope_steps))
    ((ConfigOptionBool,                 seam_slope_inner_walls))
    ((ConfigOptionFloatOrPercent,       scarf_joint_speed))
    ((ConfigOptionFloat,                scarf_joint_flow_ratio))
    ((ConfigOptionPercent,              scarf_overhang_threshold))
    
    // Orca: Z Anti-Aliasing (aka Z Contouring)
    ((ConfigOptionBool, zaa_enabled))
    ((ConfigOptionBool, zaa_dont_alternate_fill_direction))
    ((ConfigOptionFloat, zaa_min_z))
    ((ConfigOptionFloat, zaa_minimize_perimeter_height))
    )

PRINT_CONFIG_CLASS_DEFINE(
    MachineEnvelopeConfig,

    // Orca: whether emit machine limits into the beginning of the G-code.
    ((ConfigOptionBool,                 emit_machine_limits_to_gcode))
    // M201 X... Y... Z... E... [mm/sec^2]
    ((ConfigOptionFloats,               machine_max_acceleration_x))
    ((ConfigOptionFloats,               machine_max_acceleration_y))
    ((ConfigOptionFloats,               machine_max_acceleration_z))
    ((ConfigOptionFloats,               machine_max_acceleration_e))
    // M203 X... Y... Z... E... [mm/sec]
    ((ConfigOptionFloats,               machine_max_speed_x))
    ((ConfigOptionFloats,               machine_max_speed_y))
    ((ConfigOptionFloats,               machine_max_speed_z))
    ((ConfigOptionFloats,               machine_max_speed_e))

    // M204 P... R... T...[mm/sec^2]
    ((ConfigOptionFloats,               machine_max_acceleration_extruding))
    ((ConfigOptionFloats,               machine_max_acceleration_retracting))
    ((ConfigOptionFloats,               machine_max_acceleration_travel))

    // M205 X... Y... Z... E... [mm/sec]
    ((ConfigOptionFloats,               machine_max_jerk_x))
    ((ConfigOptionFloats,               machine_max_jerk_y))
    ((ConfigOptionFloats,               machine_max_jerk_z))
    ((ConfigOptionFloats,               machine_max_jerk_e))
    // M205 J... [mm]
    ((ConfigOptionFloats,               machine_max_junction_deviation))
    // M205 T... [mm/sec]
    ((ConfigOptionFloats,               machine_min_travel_rate))
    // M205 S... [mm/sec]
    ((ConfigOptionFloats,               machine_min_extruding_rate))
    // Bedslinger mass/force model: drive the per-layer Y-axis
    // acceleration limit (curr_y_acceleration_limit) and the printed-mass check.
    // Default 0 => inactive for every existing printer (mass model reads them as disabled).
    ((ConfigOptionFloat,                machine_max_force_Y))
    ((ConfigOptionFloat,                machine_bed_mass_Y))
    ((ConfigOptionFloat,                machine_max_printed_mass))

    //resonance avoidance ported from qidi slicer
    ((ConfigOptionBool,                 resonance_avoidance))
    ((ConfigOptionFloat,                min_resonance_avoidance_speed))
    ((ConfigOptionFloat,                max_resonance_avoidance_speed))

    //Orca: Input shaping
    ((ConfigOptionBool,                 input_shaping_emit))
    ((ConfigOptionEnum<InputShaperType>, input_shaping_type))
    ((ConfigOptionFloat,                input_shaping_freq_x))
    ((ConfigOptionFloat,                input_shaping_freq_y))
    ((ConfigOptionFloat,                input_shaping_damp_x))
    ((ConfigOptionFloat,                input_shaping_damp_y))
)

// This object is mapped to Perl as Slic3r::Config::GCode.
PRINT_CONFIG_CLASS_DEFINE(
    GCodeConfig,

    ((ConfigOptionString,              before_layer_change_gcode))
    ((ConfigOptionString,              printing_by_object_gcode))
    ((ConfigOptionFloats,              deretraction_speed))
    //BBS
    ((ConfigOptionEnum<ArcFittingMode>, enable_arc_fitting))
    ((ConfigOptionString,              machine_end_gcode))
    ((ConfigOptionStrings,             filament_end_gcode))
    ((ConfigOptionFloatsNullable,      filament_flow_ratio))
    ((ConfigOptionBools,               enable_pressure_advance))
    ((ConfigOptionFloats,              pressure_advance))
    // Orca: adaptive pressure advance and calibration model
    ((ConfigOptionBools,                adaptive_pressure_advance))
    ((ConfigOptionBools,                adaptive_pressure_advance_overhangs))
    ((ConfigOptionStrings,             adaptive_pressure_advance_model))
    ((ConfigOptionFloats,              adaptive_pressure_advance_bridges))
    //
    ((ConfigOptionFloat,               fan_kickstart))
    ((ConfigOptionBool,                fan_speedup_overhangs))
    ((ConfigOptionFloat,               fan_speedup_time))
    // ORCA: minimum PWM (as a percent 0-100) emitted when the part-cooling fan is asked for a non-zero speed.
    // Used to overcome the PWM start-up threshold on fans that cannot spool below a certain duty cycle.
    // A value of 0 (the default) leaves behaviour unchanged. A fan command of 0 (off) is always honoured.
    ((ConfigOptionInt,                 part_cooling_fan_min_pwm))
    ((ConfigOptionFloats,              filament_diameter))
    ((ConfigOptionBoolsNullable,       filament_adaptive_volumetric_speed))
    ((ConfigOptionStrings,             volumetric_speed_coefficients))
    ((ConfigOptionInts,              filament_adhesiveness_category))
    ((ConfigOptionFloats,              filament_density))
    ((ConfigOptionStrings,             filament_type))
    ((ConfigOptionBools,               filament_soluble))
    ((ConfigOptionStrings,             filament_ids))
    ((ConfigOptionStrings,             filament_colour))
    ((ConfigOptionStrings,             filament_vendor))
    ((ConfigOptionBools,               filament_is_support))
    // Mixed-color filament: a virtual slot realized from 2-3 physical filaments.
    ((ConfigOptionBools,               filament_is_mixed))
    ((ConfigOptionStrings,             filament_mixed_components))
    ((ConfigOptionStrings,             filament_mixed_sublayer_ratios))
    ((ConfigOptionBools,               filament_mixed_gradient))
    ((ConfigOptionStrings,             filament_mixed_gradient_range))
    ((ConfigOptionStrings,             filament_mixed_gradient_curve))
    ((ConfigOptionBools,               filament_mixed_gradient_per_part))
    ((ConfigOptionInts,                filament_printable))
    ((ConfigOptionInts,                filament_extruder_compatibility))
    ((ConfigOptionFloats,              filament_change_length))
    ((ConfigOptionFloats,              filament_cost))
    ((ConfigOptionStrings,             default_filament_colour))
    ((ConfigOptionInts,                temperature_vitrification))  //BBS
    ((ConfigOptionFloats,              filament_max_volumetric_speed))
    ((ConfigOptionInts,                required_nozzle_HRC))
    ((ConfigOptionEnum<FilamentMapMode>, filament_map_mode))
    ((ConfigOptionInts,                filament_map))
    ((ConfigOptionInts,                filament_volume_map))
    ((ConfigOptionInts,                filament_nozzle_map))
    ((ConfigOptionInts,                filament_map_2)) //used for multi nozzle, map filament to the index identified by extruder+nozzle_volume_type
    //((ConfigOptionInts,                filament_extruder_id))
    ((ConfigOptionStrings,             filament_extruder_variant))
    ((ConfigOptionInts,                filament_self_index))
    ((ConfigOptionBool,                support_object_skip_flush))
    ((ConfigOptionEnum<BedTempFormula>, bed_temperature_formula))
    ((ConfigOptionInts,                physical_extruder_map))
    ((ConfigOptionIntsNullable,        nozzle_flush_dataset))
    ((ConfigOptionFloatsNullable,      filament_flush_volumetric_speed))
    ((ConfigOptionIntsNullable,        filament_flush_temp))
    // Fast-purge flush temperature; consumed only when prime_volume_mode==pvmFast.
    ((ConfigOptionIntsNullable,        filament_flush_temp_fast))
    // BBS
    ((ConfigOptionBool,                scan_first_layer))
    ((ConfigOptionEnum<PowerLossRecoveryMode>, enable_power_loss_recovery))
    ((ConfigOptionBool,                enable_wrapping_detection))
    ((ConfigOptionInt,                 wrapping_detection_layers))
    ((ConfigOptionPoints,              wrapping_exclude_area))
    ((ConfigOptionPoints,              thumbnail_size))
    // ((ConfigOptionBool,                spaghetti_detector))
    ((ConfigOptionBool,                gcode_add_line_number))
    ((ConfigOptionBool,                high_precision_gcode))
    ((ConfigOptionBool,                bbl_bed_temperature_gcode))
    ((ConfigOptionEnum<GCodeFlavor>,   gcode_flavor))
    ((ConfigOptionBool,                gcode_skip_config_block))
    ((ConfigOptionFloat,               time_cost)) 
    ((ConfigOptionString,              layer_change_gcode))
    ((ConfigOptionString,              time_lapse_gcode))
    ((ConfigOptionString,              wrapping_detection_gcode))

    ((ConfigOptionFloat,               max_volumetric_extrusion_rate_slope))
    ((ConfigOptionFloat,               max_volumetric_extrusion_rate_slope_segment_length))
    ((ConfigOptionBool,                extrusion_rate_smoothing_external_perimeter_only))

    
    ((ConfigOptionPercents,            retract_before_wipe))
    // Orca
    ((ConfigOptionPercents,            retract_after_wipe))

    ((ConfigOptionFloats,              retraction_length))
    ((ConfigOptionFloats,              retract_length_toolchange))
    ((ConfigOptionInt,                 enable_long_retraction_when_cut))
    ((ConfigOptionFloats,              retraction_distances_when_cut))
    ((ConfigOptionBools,               long_retractions_when_cut))
    ((ConfigOptionFloatsNullable,      retraction_distances_when_ec))
    ((ConfigOptionBoolsNullable,       long_retractions_when_ec))
    ((ConfigOptionFloats,              z_hop))
    // BBS
    ((ConfigOptionEnumsGeneric,        z_hop_types))
    ((ConfigOptionFloats,              travel_slope))
    ((ConfigOptionFloats,              retract_lift_above))
    ((ConfigOptionFloats,              retract_lift_below))
    ((ConfigOptionEnumsGeneric,        retract_lift_enforce))
    ((ConfigOptionFloats,              retract_restart_extra))
    ((ConfigOptionFloats,              retract_restart_extra_toolchange))
    ((ConfigOptionFloats,              retraction_speed))
    ((ConfigOptionString,              file_start_gcode))
    ((ConfigOptionString,              machine_start_gcode))
    ((ConfigOptionStrings,             filament_start_gcode))
    ((ConfigOptionBool,                single_extruder_multi_material))
    ((ConfigOptionBool,                manual_filament_change))
    // IDEX/IQEX (independent X extruder) — parallel printing support for IDEX/IQEX printers
    ((ConfigOptionBool,                is_imex))
    ((ConfigOptionBool,                imex_firmware_managed_zones))
    ((ConfigOptionInt,                 imex_gantry_count))
    ((ConfigOptionInt,                 imex_tools_per_gantry))
    ((ConfigOptionFloat,               imex_nozzle_clearance_x))
    ((ConfigOptionFloat,               imex_nozzle_clearance_y))
    ((ConfigOptionFloat,               imex_carriage_margin))
    ((ConfigOptionEnum<ImexToolLayout>, imex_tool_layout))
    ((ConfigOptionEnum<ImexVizTheme>,  imex_viz_theme))
    ((ConfigOptionStrings,             imex_mode_names))
    ((ConfigOptionStrings,             imex_mode_active_tools))
    ((ConfigOptionStrings,             imex_mode_gcodes))
    ((ConfigOptionBool,                single_extruder_multi_material_priming))
    ((ConfigOptionEnum<ToolChangeOrderingType>, toolchange_ordering))
    ((ConfigOptionString,              toolchange_cyclic_order))
    ((ConfigOptionBool,                toolchange_cyclic_first_layer))
    ((ConfigOptionBool,                wipe_tower_no_sparse_layers))
    ((ConfigOptionBool,                wipe_tower_sparse_layers_combination))
    ((ConfigOptionString,              change_filament_gcode))
    ((ConfigOptionString,              change_extrusion_role_gcode))
    ((ConfigOptionString,              process_change_extrusion_role_gcode))
    ((ConfigOptionStrings,             filament_change_extrusion_role_gcode))
    ((ConfigOptionFloatsNullable,      travel_speed))
    ((ConfigOptionFloatsNullable,      travel_speed_z))
    ((ConfigOptionString,              machine_pause_gcode))
    ((ConfigOptionString,              template_custom_gcode))
    //BBS
    ((ConfigOptionEnumsGenericNullable,nozzle_type))
    ((ConfigOptionInt,                 nozzle_hrc))
    ((ConfigOptionBool,                auxiliary_fan))
    ((ConfigOptionEnum<FanDirection>,  fan_direction))
    ((ConfigOptionBool,                support_air_filtration))
    ((ConfigOptionBool,                support_cooling_filter))
    ((ConfigOptionBool,                cooling_filter_enabled))
    ((ConfigOptionEnum<PrinterStructure>,printer_structure))
    ((ConfigOptionBool,                support_chamber_temp_control))
    ((ConfigOptionEnumsGeneric,        extruder_type))
    ((ConfigOptionEnumsGeneric,        nozzle_volume_type))
    ((ConfigOptionStrings,             extruder_ams_count))
    ((ConfigOptionStrings,             extruder_nozzle_stats))
    ((ConfigOptionInts,                printer_extruder_id))
    ((ConfigOptionInt,                 master_extruder_id))
    ((ConfigOptionStrings,             printer_extruder_variant))


    // SoftFever
    ((ConfigOptionBool,                use_firmware_retraction))
    ((ConfigOptionBool,                use_relative_e_distances))
    ((ConfigOptionBool,                accel_to_decel_enable))
    ((ConfigOptionPercent,             accel_to_decel_factor))
    ((ConfigOptionFloatsOrPercentsNullable, initial_layer_travel_speed))
    ((ConfigOptionFloatsOrPercentsNullable, initial_layer_travel_acceleration))
    ((ConfigOptionFloatsOrPercentsNullable, initial_layer_travel_jerk))
    ((ConfigOptionBool,                bbl_calib_mark_logo))
    ((ConfigOptionBool,                disable_m73))

    // Orca: mmu
    ((ConfigOptionFloat,               cooling_tube_retraction))
    ((ConfigOptionFloat,               cooling_tube_length))
    ((ConfigOptionBool,                high_current_on_filament_swap))
    ((ConfigOptionFloat,               parking_pos_retraction))
    ((ConfigOptionFloat,               extra_loading_move))
    ((ConfigOptionFloat,               machine_load_filament_time))
    ((ConfigOptionFloat,               machine_tool_change_time))
    ((ConfigOptionFloat,               machine_unload_filament_time))
    ((ConfigOptionFloats,              filament_loading_speed))
    ((ConfigOptionFloats,              filament_loading_speed_start))
    ((ConfigOptionFloats,              filament_unloading_speed))
    ((ConfigOptionFloats,              filament_unloading_speed_start))
    ((ConfigOptionFloats,              filament_toolchange_delay))
    ((ConfigOptionInts,                filament_cooling_moves))
    ((ConfigOptionFloats,              filament_cooling_initial_speed))
    ((ConfigOptionFloats,              filament_minimal_purge_on_wipe_tower))
    ((ConfigOptionFloatsNullable,      filament_cooling_before_tower))
    ((ConfigOptionFloats,              filament_tower_interface_pre_extrusion_dist))
    ((ConfigOptionFloats,              filament_tower_interface_pre_extrusion_length))
    ((ConfigOptionFloats,              filament_tower_ironing_area))
    ((ConfigOptionFloats,              filament_tower_interface_purge_volume))
    ((ConfigOptionInts,                filament_tower_interface_print_temp))
    ((ConfigOptionFloats,              filament_cooling_final_speed))
    ((ConfigOptionStrings,             filament_ramming_parameters))
    ((ConfigOptionBools,               filament_multitool_ramming))
    ((ConfigOptionFloats,              filament_multitool_ramming_volume))
    ((ConfigOptionFloats,              filament_multitool_ramming_flow))
    ((ConfigOptionFloats,              filament_stamping_loading_speed))
    ((ConfigOptionFloats,              filament_stamping_distance))
    ((ConfigOptionEnum<WipeTowerType>, wipe_tower_type))
    ((ConfigOptionBool,                purge_in_prime_tower))
    ((ConfigOptionBool,                enable_filament_ramming))
    ((ConfigOptionBool,                tool_change_on_wipe_tower))
    ((ConfigOptionBool,                wait_for_temp_on_wipe_tower))
    ((ConfigOptionBool,                support_multi_bed_types))
    ((ConfigOptionBool,                use_3mf))

    // Small Area Infill Flow Compensation
    ((ConfigOptionStrings,              small_area_infill_flow_compensation_model))

    ((ConfigOptionBool,                has_scarf_joint_seam))

    // Multi-nozzle + pre-heating + nozzle-change (nc) keys. Defaults are no-ops for existing
    // single-nozzle printers; new slicing paths gate on extruder_max_nozzle_count > 1.
    ((ConfigOptionFloat,               machine_hotend_change_time))
    ((ConfigOptionFloat,               machine_prepare_compensation_time))
    ((ConfigOptionBool,                enable_pre_heating))
    ((ConfigOptionFloatsNullable,      hotend_cooling_rate))
    ((ConfigOptionFloatsNullable,      hotend_heating_rate))
    ((ConfigOptionFloats,              filament_change_length_nc))
    ((ConfigOptionFloatsNullable,      filament_ramming_travel_time))
    ((ConfigOptionIntsNullable,        filament_pre_cooling_temperature))
    ((ConfigOptionFloatsNullable,      filament_ramming_volumetric_speed))
    ((ConfigOptionFloatsNullable,      filament_ramming_travel_time_nc))
    ((ConfigOptionIntsNullable,        filament_pre_cooling_temperature_nc))
    ((ConfigOptionFloatsNullable,      filament_ramming_volumetric_speed_nc))
    ((ConfigOptionFloatsNullable,      filament_retract_length_nc))
    ((ConfigOptionIntsNullable,        extruder_max_nozzle_count))
    // Printer flag: whether the printer offers the fast-purge mode selector.
    // Default false; no shipping profile sets it, so the fast-purge UI stays hidden.
    ((ConfigOptionBool,                support_fast_purge_mode))

    //ams chamber
    ((ConfigOptionStrings,  filament_dev_ams_drying_ams_limitations))
    ((ConfigOptionFloats,   filament_dev_ams_drying_temperature))
    ((ConfigOptionFloats,   filament_dev_ams_drying_time))
    ((ConfigOptionFloats,   filament_dev_ams_drying_heat_distortion_temperature))
    ((ConfigOptionFloats,   filament_dev_chamber_drying_bed_temperature))
    ((ConfigOptionFloats,   filament_dev_chamber_drying_time))
    ((ConfigOptionFloats,   filament_dev_drying_softening_temperature))
    ((ConfigOptionFloats,   filament_dev_drying_cooling_temperature))
)

// This object is mapped to Perl as Slic3r::Config::Print.
PRINT_CONFIG_CLASS_DERIVED_DEFINE(
    PrintConfig,
    (MachineEnvelopeConfig, GCodeConfig),

    // Build plate tilt for off-axis gravity support generation (printer-level setting).
    ((ConfigOptionFloat,               build_plate_tilt_x))
    ((ConfigOptionFloat,               build_plate_tilt_y))
    // Belt printer settings (printer-level).
    ((ConfigOptionBool,                belt_printer))
    ((ConfigOptionBool,                belt_printer_infinite_y))
    // Mesh rotation applied before slicing — the single source of truth for the
    // physical belt tilt.  Its angle + axis drive bed rendering, support gravity
    // tilt, the bed-exclusion projection, AND the post-slice machine-frame
    // transform (shear + scale, derived from the tilt angle; see
    // MachineFrameTransform).  Isometric (no distortion) on the mesh side; the
    // g-code back-transform inverts the rotation before the machine-frame stage.
    ((ConfigOptionEnum<BeltRotationAxis>, belt_slice_rotation))
    ((ConfigOptionFloat,                  belt_slice_rotation_angle))
    // Expert override: decouple the machine-frame tilt angle from the pre-slice
    // rotation angle.  When disabled, the machine frame uses belt_slice_rotation_angle.
    ((ConfigOptionBool,                   belt_frame_tilt_decouple))
    ((ConfigOptionFloat,                  belt_frame_tilt_angle))
    ((ConfigOptionEnum<RemapAxis>,  gcode_remap_x))
    ((ConfigOptionEnum<RemapAxis>,  gcode_remap_y))
    ((ConfigOptionEnum<RemapAxis>,  gcode_remap_z))
    ((ConfigOptionFloat,                          belt_support_floor_offset))
    // Width (machine X, across the belt) of the auto-generated belt purge prism.
    ((ConfigOptionFloat,                          belt_purge_tower_width))
    // Belt-printer-only "type" of purge tower: enables the auto-generated belt
    // purge prism (the belt replacement for the classic wipe/prime tower).
    ((ConfigOptionBool,                           enable_belt_purge_tower))
    //BBS
    ((ConfigOptionInts,               additional_cooling_fan_speed))
    ((ConfigOptionInts,               close_additional_fan_first_x_layers))
    ((ConfigOptionInts,               additional_fan_full_speed_layer))
    ((ConfigOptionFloats,             first_x_layer_fan_speed))
    ((ConfigOptionBool,               reduce_crossing_wall))
    ((ConfigOptionFloatOrPercent,     max_travel_detour_distance))
    ((ConfigOptionPoints,             printable_area))
    ((ConfigOptionPointsGroups,       extruder_printable_area))
    ((ConfigOptionBool,               support_parallel_printheads))
    ((ConfigOptionInt,                parallel_printheads_count))
    ((ConfigOptionStrings,            parallel_printheads_bed_exclude_areas))
    //BBS: add bed_exclude_area
    ((ConfigOptionPoints,             bed_exclude_area))
    ((ConfigOptionString,             bed_exclude_volumes))
    ((ConfigOptionEnum<BedExcludeVolumeMode>, bed_exclude_volume_mode))
    ((ConfigOptionStrings,            extruder_bed_exclude_volumes))
    ((ConfigOptionPoints,             head_wrap_detect_zone))
    // BBS
    ((ConfigOptionString,             bed_custom_texture))
    ((ConfigOptionString,             bed_custom_model))
    ((ConfigOptionEnum<BedType>,      curr_bed_type))
    ((ConfigOptionInts,               cool_plate_temp))
    ((ConfigOptionInts,               textured_cool_plate_temp))
    ((ConfigOptionInts,               supertack_plate_temp))
    ((ConfigOptionInts,               eng_plate_temp))
    ((ConfigOptionInts,               hot_plate_temp)) // hot is short for high temperature
    ((ConfigOptionInts,               textured_plate_temp))
    ((ConfigOptionInts,               supertack_plate_temp_initial_layer))
    ((ConfigOptionInts,               cool_plate_temp_initial_layer))
    ((ConfigOptionInts,               textured_cool_plate_temp_initial_layer))
    ((ConfigOptionInts,               eng_plate_temp_initial_layer))
    ((ConfigOptionInts,               hot_plate_temp_initial_layer)) // hot is short for high temperature
    ((ConfigOptionInts,               textured_plate_temp_initial_layer))
    ((ConfigOptionBools,              enable_overhang_bridge_fan))
    ((ConfigOptionInts,               overhang_fan_speed))
    ((ConfigOptionEnumsGeneric,       overhang_fan_threshold))
    ((ConfigOptionEnum<PrintSequence>,print_sequence))
    ((ConfigOptionEnum<PrintOrder>,   print_order))
    ((ConfigOptionInts,               first_layer_print_sequence))
    ((ConfigOptionInts,               other_layers_print_sequence))
    ((ConfigOptionInt,                other_layers_print_sequence_nums))
    ((ConfigOptionBools,              slow_down_for_layer_cooling))
    ((ConfigOptionInts,               close_fan_the_first_x_layers))
    ((ConfigOptionEnum<DraftShield>,  draft_shield))
    ((ConfigOptionFloat,              extruder_clearance_dist_to_rod))//BBS
    ((ConfigOptionFloat,              extruder_clearance_height_to_rod))//BBs
    ((ConfigOptionFloat,              extruder_clearance_height_to_lid))//BBS
    ((ConfigOptionFloat,              extruder_clearance_radius))
    ((ConfigOptionFloat,              nozzle_height))
    ((ConfigOptionStrings,            extruder_colour))
    ((ConfigOptionPoints,             extruder_offset))
    ((ConfigOptionBools,              reduce_fan_stop_start_freq))
    ((ConfigOptionBools,              dont_slow_down_outer_wall))
    ((ConfigOptionFloats,             fan_cooling_layer_time))
    ((ConfigOptionBools,              activate_air_filtration))
    ((ConfigOptionBools,              activate_air_filtration_during_print))
    ((ConfigOptionBools,              activate_air_filtration_on_completion))
    ((ConfigOptionInts,               during_print_exhaust_fan_speed))
    ((ConfigOptionInts,               complete_print_exhaust_fan_speed))
    ((ConfigOptionFloatOrPercent,     initial_layer_line_width))
    ((ConfigOptionFloat,              initial_layer_print_height))
    ((ConfigOptionFloatsNullable,     initial_layer_speed))

    //BBS
    ((ConfigOptionFloatsNullable,     initial_layer_infill_speed))
    ((ConfigOptionInts,               nozzle_temperature_initial_layer))
    ((ConfigOptionInts,               full_fan_speed_layer))
    // ORCA: explicit override for the part cooling fan speed on the first printed layer.
    ((ConfigOptionInts,               initial_layer_fan_speed))
    ((ConfigOptionFloats,               fan_max_speed))
    ((ConfigOptionFloats,             max_layer_height))
    ((ConfigOptionFloats,               fan_min_speed))
    ((ConfigOptionFloats,             min_layer_height))
    ((ConfigOptionFloat,              printable_height))
    ((ConfigOptionFloatsNullable,     extruder_printable_height))
    ((ConfigOptionPoint,              best_object_pos))
    ((ConfigOptionFloats,             slow_down_min_speed))
    ((ConfigOptionFloats,             nozzle_diameter))
    ((ConfigOptionBool,               reduce_infill_retraction))
    ((ConfigOptionBool,               ooze_prevention))
    ((ConfigOptionString,             filename_format))
    ((ConfigOptionStrings,            post_process))
    ((ConfigOptionStrings,            slicing_pipeline_plugin))
    ((ConfigOptionString,             print_plugin_config_overrides))
    ((ConfigOptionString,             printer_model))
    ((ConfigOptionFloat,              resolution))
    ((ConfigOptionFloats,             retraction_minimum_travel))
    ((ConfigOptionBools,              retract_when_changing_layer))
    ((ConfigOptionFloat,              skirt_distance))
    ((ConfigOptionInt,                skirt_height))
    ((ConfigOptionInt,                skirt_loops))
    ((ConfigOptionEnum<SkirtType>,    skirt_type))
    ((ConfigOptionFloat,              skirt_speed))
    ((ConfigOptionBool,               single_loop_draft_shield))
    ((ConfigOptionFloat,              min_skirt_length))
    ((ConfigOptionFloats,             slow_down_layer_time))
    ((ConfigOptionBool,               spiral_mode))
    ((ConfigOptionBool,               spiral_mode_smooth))
    ((ConfigOptionFloatOrPercent,     spiral_mode_max_xy_smoothing))
    ((ConfigOptionFloat,              spiral_finishing_flow_ratio))
    ((ConfigOptionFloat,              spiral_starting_flow_ratio))
    ((ConfigOptionInt,                standby_temperature_delta))
    ((ConfigOptionFloat,                preheat_time))
    ((ConfigOptionInt,                preheat_steps))
    ((ConfigOptionInts,               nozzle_temperature))
    ((ConfigOptionBools,              wipe))
    // BBS
    ((ConfigOptionInts,               nozzle_temperature_range_low))
    ((ConfigOptionInts,               nozzle_temperature_range_high))
    ((ConfigOptionFloats,             wipe_distance))
    ((ConfigOptionBool,               enable_mixed_color_sublayer))
    ((ConfigOptionBool,               enable_prime_tower))
    ((ConfigOptionBool,               prime_tower_enable_framework))
    // BBS: change wipe_tower_x and wipe_tower_y data type to floats to add partplate logic
    ((ConfigOptionFloats,             wipe_tower_x))
    ((ConfigOptionFloats,             wipe_tower_y))
    ((ConfigOptionFloat,              prime_tower_width))
    ((ConfigOptionFloat,              wipe_tower_per_color_wipe))
    ((ConfigOptionFloat,              wipe_tower_rotation_angle))
    ((ConfigOptionFloat,              prime_tower_brim_width))
    ((ConfigOptionPercent,            prime_tower_infill_gap))
    ((ConfigOptionBool,               prime_tower_skip_points))
    ((ConfigOptionBool,               prime_tower_flat_ironing))
    ((ConfigOptionBool,               enable_tower_interface_features))
    ((ConfigOptionBool,               enable_tower_interface_cooldown_during_tower))
    ((ConfigOptionFloat,              wipe_tower_bridging))
    ((ConfigOptionPercent,            wipe_tower_extra_flow))
    ((ConfigOptionFloats,             flush_volumes_matrix))
    ((ConfigOptionFloats,             flush_volumes_vector))

    // Orca: mmu support
    ((ConfigOptionFloat,              wipe_tower_cone_angle))
    ((ConfigOptionPercent,            wipe_tower_extra_spacing))
    ((ConfigOptionFloat,              wipe_tower_max_purge_speed))
    ((ConfigOptionEnum<WipeTowerWallType>,    wipe_tower_wall_type))
    ((ConfigOptionFloat,              wipe_tower_extra_rib_length))
    ((ConfigOptionFloat,              wipe_tower_rib_width))
    ((ConfigOptionBool,               wipe_tower_fillet_wall))
    ((ConfigOptionInt,                wipe_tower_filament))
    ((ConfigOptionFloats,             wiping_volumes_extruders))
    ((ConfigOptionInts,       idle_temperature))


    // BBS: wipe tower is only used for priming
    ((ConfigOptionFloat,              prime_volume))
    // Nozzle-change (nc) prime volume + pre-heat delta
    ((ConfigOptionFloats,             filament_prime_volume))
    ((ConfigOptionFloats,             filament_prime_volume_nc))
    ((ConfigOptionFloatsNullable,     filament_preheat_temperature_delta))
    ((ConfigOptionFloats,             flush_multiplier))
    // Fast-purge mode. Kept out of the g-code config block (banned_keys in
    // GCode::append_full_config) so registering them leaves the shipping fleet's g-code byte-identical;
    // consumed only on the prime_volume_mode==pvmFast / pvmSaving branch (default pvmDefault = inert).
    ((ConfigOptionEnum<PrimeVolumeMode>, prime_volume_mode))
    ((ConfigOptionFloats,             flush_multiplier_fast))
    ((ConfigOptionFloat,              z_offset))
    // BBS: project filaments
    ((ConfigOptionFloats,             filament_colour_new))
    // BBS: not in any preset, calculated before slicing
    ((ConfigOptionFloatsNullable,     nozzle_volume))
    ((ConfigOptionPoints,             start_end_points))
    ((ConfigOptionEnum<TimelapseType>,    timelapse_type))
    // Corexy farthest-point timelapse (default false → inert for existing printers)
    ((ConfigOptionBool,               farthest_point_timelapse))
    ((ConfigOptionString,             thumbnails))
    // BBS: move from PrintObjectConfig
    ((ConfigOptionBool, independent_support_layer_height))
    ((ConfigOptionBool,               combine_brims))
    // SoftFever
    ((ConfigOptionPercents,            filament_shrink))
    ((ConfigOptionPercents,            filament_shrinkage_compensation_z))
    ((ConfigOptionBool,                gcode_label_objects))
    ((ConfigOptionBool,                exclude_object))
    ((ConfigOptionFloat,               exclude_object_tolerance))
    ((ConfigOptionFloats,             grab_length))
    ((ConfigOptionBool,                gcode_comments))
    ((ConfigOptionInt,                 slow_down_layers))
    ((ConfigOptionInts,                support_material_interface_fan_speed))
    ((ConfigOptionInts,                internal_bridge_fan_speed)) // ORCA: Add support for separate internal bridge fan speed control
    ((ConfigOptionInts,                ironing_fan_speed))
    // Orca: notes for profiles from PrusaSlicer
    ((ConfigOptionStrings,             filament_notes))
    ((ConfigOptionString,              notes))
    ((ConfigOptionString,              printer_notes))

    ((ConfigOptionBools,               activate_chamber_temp_control))
    ((ConfigOptionInts ,               chamber_temperature))
    ((ConfigOptionInts ,               chamber_minimal_temperature))

    // Orca: support adaptive bed mesh
    ((ConfigOptionFloat,               preferred_orientation))
    ((ConfigOptionPoint,               bed_mesh_min))
    ((ConfigOptionPoint,               bed_mesh_max))
    ((ConfigOptionPoint,               bed_mesh_probe_distance))
    ((ConfigOptionFloat,               adaptive_bed_mesh_margin))


)

// This object is mapped to Perl as Slic3r::Config::Full.
PRINT_CONFIG_CLASS_DERIVED_DEFINE0(
    FullPrintConfig,
    (PrintObjectConfig, PrintRegionConfig, PrintConfig)
)

// Validate the FullPrintConfig. Returns an empty string on success, otherwise an error message is returned.
std::map<std::string, std::string> validate(const FullPrintConfig &config, bool under_cli = false);

PRINT_CONFIG_CLASS_DEFINE(
    SLAPrintConfig,
    ((ConfigOptionString,     filename_format))
)

PRINT_CONFIG_CLASS_DEFINE(
    SLAPrintObjectConfig,

    ((ConfigOptionFloat, layer_height))

    //Number of the layers needed for the exposure time fade [3;20]
    ((ConfigOptionInt,  faded_layers))/*= 10*/

    ((ConfigOptionFloat, slice_closing_radius))

    // Enabling or disabling support creation
    ((ConfigOptionBool,  supports_enable))

    // Diameter in mm of the pointing side of the head.
    ((ConfigOptionFloat, support_head_front_diameter))/*= 0.2*/

    // How much the pinhead has to penetrate the model surface
    ((ConfigOptionFloat, support_head_penetration))/*= 0.2*/

    // Width in mm from the back sphere center to the front sphere center.
    ((ConfigOptionFloat, support_head_width))/*= 1.0*/

    // Radius in mm of the support pillars.
    ((ConfigOptionFloat, support_pillar_diameter))/*= 0.8*/

    // The percentage of smaller pillars compared to the normal pillar diameter
    // which are used in problematic areas where a normal pilla cannot fit.
    ((ConfigOptionPercent, support_small_pillar_diameter_percent))

    // How much bridge (supporting another pinhead) can be placed on a pillar.
    ((ConfigOptionInt,   support_max_bridges_on_pillar))

    // How the pillars are bridged together
    ((ConfigOptionEnum<SLAPillarConnectionMode>, support_pillar_connection_mode))

    // Generate only ground facing supports
    ((ConfigOptionBool, support_buildplate_only))

    // TODO: unimplemented at the moment. This coefficient will have an impact
    // when bridges and pillars are merged. The resulting pillar should be a bit
    // thicker than the ones merging into it. How much thicker? I don't know
    // but it will be derived from this value.
    ((ConfigOptionFloat, support_pillar_widening_factor))

    // Radius in mm of the pillar base.
    ((ConfigOptionFloat, support_base_diameter))/*= 2.0*/

    // The height of the pillar base cone in mm.
    ((ConfigOptionFloat, support_base_height))/*= 1.0*/

    // The minimum distance of the pillar base from the model in mm.
    ((ConfigOptionFloat, support_base_safety_distance)) /*= 1.0*/

    // The default angle for connecting support sticks and junctions.
    ((ConfigOptionFloat, support_critical_angle))/*= 45*/

    // The max length of a bridge in mm
    ((ConfigOptionFloat, support_max_bridge_length))/*= 15.0*/

    // The max distance of two pillars to get cross linked.
    ((ConfigOptionFloat, support_max_pillar_link_distance))

    // The elevation in Z direction upwards. This is the space between the pad
    // and the model object's bounding box bottom. Units in mm.
    ((ConfigOptionFloat, support_object_elevation))/*= 5.0*/

    /////// Following options influence automatic support points placement:
    ((ConfigOptionInt, support_points_density_relative))
    ((ConfigOptionFloat, support_points_minimal_distance))

    // Now for the base pool (pad) /////////////////////////////////////////////

    // Enabling or disabling support creation
    ((ConfigOptionBool,  pad_enable))

    // The thickness of the pad walls
    ((ConfigOptionFloat, pad_wall_thickness))/*= 2*/

    // The height of the pad from the bottom to the top not considering the pit
    ((ConfigOptionFloat, pad_wall_height))/*= 5*/

    // How far should the pad extend around the contained geometry
    ((ConfigOptionFloat, pad_brim_size))

    // The greatest distance where two individual pads are merged into one. The
    // distance is measured roughly from the centroids of the pads.
    ((ConfigOptionFloat, pad_max_merge_distance))/*= 50*/

    // The smoothing radius of the pad edges
    // ((ConfigOptionFloat, pad_edge_radius))/*= 1*/;

    // The slope of the pad wall...
    ((ConfigOptionFloat, pad_wall_slope))

    // /////////////////////////////////////////////////////////////////////////
    // Zero elevation mode parameters:
    //    - The object pad will be derived from the model geometry.
    //    - There will be a gap between the object pad and the generated pad
    //      according to the support_base_safety_distance parameter.
    //    - The two pads will be connected with tiny connector sticks
    // /////////////////////////////////////////////////////////////////////////

    // Disable the elevation (ignore its value) and use the zero elevation mode
    ((ConfigOptionBool, pad_around_object))

    ((ConfigOptionBool, pad_around_object_everywhere))

    // This is the gap between the object bottom and the generated pad
    ((ConfigOptionFloat, pad_object_gap))

    // How far to place the connector sticks on the object pad perimeter
    ((ConfigOptionFloat, pad_object_connector_stride))

    // The width of the connectors sticks
    ((ConfigOptionFloat, pad_object_connector_width))

    // How much should the tiny connectors penetrate into the model body
    ((ConfigOptionFloat, pad_object_connector_penetration))

    // /////////////////////////////////////////////////////////////////////////
    // Model hollowing parameters:
    //   - Models can be hollowed out as part of the SLA print process
    //   - Thickness of the hollowed model walls can be adjusted
    //   -
    //   - Additional holes will be drilled into the hollow model to allow for
    //   - resin removal.
    // /////////////////////////////////////////////////////////////////////////

    ((ConfigOptionBool, hollowing_enable))

    // The minimum thickness of the model walls to maintain. Note that the
    // resulting walls may be thicker due to smoothing out fine cavities where
    // resin could stuck.
    ((ConfigOptionFloat, hollowing_min_thickness))

    // Indirectly controls the voxel size (resolution) used by openvdb
    ((ConfigOptionFloat, hollowing_quality))

    // Indirectly controls the minimum size of created cavities.
    ((ConfigOptionFloat, hollowing_closing_distance))
)

enum SLAMaterialSpeed { slamsSlow, slamsFast };

PRINT_CONFIG_CLASS_DEFINE(
    SLAMaterialConfig,

    ((ConfigOptionFloat,                       initial_layer_height))
    ((ConfigOptionFloat,                       bottle_cost))
    ((ConfigOptionFloat,                       bottle_volume))
    ((ConfigOptionFloat,                       bottle_weight))
    ((ConfigOptionFloat,                       material_density))
    ((ConfigOptionFloat,                       exposure_time))
    ((ConfigOptionFloat,                       initial_exposure_time))
    ((ConfigOptionFloats,                      material_correction))
    ((ConfigOptionFloat,                       material_correction_x))
    ((ConfigOptionFloat,                       material_correction_y))
    ((ConfigOptionFloat,                       material_correction_z))
    ((ConfigOptionEnum<SLAMaterialSpeed>,      material_print_speed))
)

PRINT_CONFIG_CLASS_DEFINE(
    SLAPrinterConfig,

    ((ConfigOptionEnum<PrinterTechnology>,    printer_technology))
    ((ConfigOptionPoints,                     printable_area))
    ((ConfigOptionFloat,                      printable_height))
    ((ConfigOptionFloat,                      display_width))
    ((ConfigOptionFloat,                      display_height))
    ((ConfigOptionInt,                        display_pixels_x))
    ((ConfigOptionInt,                        display_pixels_y))
    ((ConfigOptionEnum<SLADisplayOrientation>,display_orientation))
    ((ConfigOptionBool,                       display_mirror_x))
    ((ConfigOptionBool,                       display_mirror_y))
    ((ConfigOptionFloats,                     relative_correction))
    ((ConfigOptionFloat,                      relative_correction_x))
    ((ConfigOptionFloat,                      relative_correction_y))
    ((ConfigOptionFloat,                      relative_correction_z))
    ((ConfigOptionFloat,                      absolute_correction))
    ((ConfigOptionFloat,                      elefant_foot_compensation))
    ((ConfigOptionFloat,                      elefant_foot_min_width))
    ((ConfigOptionFloat,                      gamma_correction))
    ((ConfigOptionFloat,                      fast_tilt_time))
    ((ConfigOptionFloat,                      slow_tilt_time))
    ((ConfigOptionFloat,                      area_fill))
    ((ConfigOptionFloat,                      min_exposure_time))
    ((ConfigOptionFloat,                      max_exposure_time))
    ((ConfigOptionFloat,                      min_initial_exposure_time))
    ((ConfigOptionFloat,                      max_initial_exposure_time))
)

PRINT_CONFIG_CLASS_DERIVED_DEFINE0(
    SLAFullPrintConfig,
    (SLAPrinterConfig, SLAPrintConfig, SLAPrintObjectConfig, SLAMaterialConfig)
)

#undef STATIC_PRINT_CONFIG_CACHE
#undef STATIC_PRINT_CONFIG_CACHE_BASE
#undef STATIC_PRINT_CONFIG_CACHE_DERIVED
#undef PRINT_CONFIG_CLASS_ELEMENT_DEFINITION
#undef PRINT_CONFIG_CLASS_ELEMENT_VISIT
#undef PRINT_CONFIG_CLASS_ELEMENT_VISIT_SELF
#undef PRINT_CONFIG_CLASS_COMMON_BODY
#undef PRINT_CONFIG_CLASS_DEFINE
#undef PRINT_CONFIG_CLASS_DERIVED_CLASS_LIST
#undef PRINT_CONFIG_CLASS_DERIVED_CLASS_LIST_ITEM
#undef PRINT_CONFIG_CLASS_DERIVED_DEFINE
#undef PRINT_CONFIG_CLASS_DERIVED_DEFINE0
#undef PRINT_CONFIG_CLASS_DERIVED_DEFINE1
#undef PRINT_CONFIG_CLASS_DERIVED_HASH
#undef PRINT_CONFIG_CLASS_DERIVED_EQUAL
#undef PRINT_CONFIG_CLASS_DERIVED_INITCACHE_ITEM
#undef PRINT_CONFIG_CLASS_DERIVED_INITCACHE
#undef PRINT_CONFIG_CLASS_DERIVED_INITIALIZER
#undef PRINT_CONFIG_CLASS_DERIVED_INITIALIZER_ITEM

} // namespace Slic3r

#endif
