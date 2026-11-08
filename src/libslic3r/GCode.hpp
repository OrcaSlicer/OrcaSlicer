#ifndef slic3r_GCode_hpp_
#define slic3r_GCode_hpp_

#include "ExtrusionEntity.hpp"
#include "Polygon.hpp"
#include "Config.hpp"
#include "Print.hpp"
#include "libslic3r.h"
#include "GCodeWriter.hpp"
#include "Layer.hpp"
#include "Point.hpp"
#include "PlaceholderParser.hpp"
#include "PrintConfig.hpp"
#include "GCode/AvoidCrossingPerimeters.hpp"
#include "GCode/CoolingBuffer.hpp"
#include "Support/SegmentSupportBounds.hpp"
#include "GCode/ExclusionVolumeTravelAvoidance.hpp"
#include "GCode/FanMover.hpp"
#include "GCode/RetractWhenCrossingPerimeters.hpp"
#include "GCode/SpiralVase.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/SeamPlacer.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "GCode/ThumbnailData.hpp"
#include "libslic3r/ObjectID.hpp"
#include "GCode/ExtrusionProcessor.hpp"

#include "GCode/PressureEqualizer.hpp"
#include "GCode/SmallAreaInfillFlowCompensator.hpp"
// ORCA: post processor below used for Dynamic Pressure advance
#include "GCode/AdaptivePAProcessor.hpp"

#include "GCode/TimelapsePosPicker.hpp"
#include "libslic3r_version.h"

#include <cstddef>
#include <limits>
#include <cstdio>
#include <array>
#include <cstdlib>
#include <memory>
#include <map>
#include <unordered_map>
#include <set>
#include <string>
#include <cfloat>
#include <vector>
#include <utility>
#include "BoundingBox.hpp"
#include "Polyline.hpp"

namespace Slic3r { class ExtrusionEntityCollection; }

namespace Slic3r {

// Forward declarations.
class GCode;
struct WipeInwardSupport;

namespace CustomGCode{ struct Item; }
struct PrintInstance;
class ConstPrintObjectPtrsAdaptor;

class OozePrevention {
public:
    bool enable;

    OozePrevention() : enable(false) {}
    std::string pre_toolchange(GCode &gcodegen);
    std::string post_toolchange(GCode &gcodegen);

private:
    int _get_temp(const GCode &gcodegen) const;
};

class Wipe {
public:
    bool enable;
    Polyline path;

    // Orca: retraction portions emitted before, during, and after the wipe move.
    struct RetractionValues{
        double retraction_length_before_wipe = 0.;
        double retraction_length_during_wipe = 0.;
        double retraction_length_after_wipe  = 0.;
    };

    Wipe() : enable(false) {}
    bool has_path() const { return !this->path.points.empty(); }
    void reset_path() { this->path = Polyline(); }
    std::string wipe(GCode &gcodegen, double length, bool toolchange = false, bool is_last = false);

    // Orca: calculate the retraction portions that can be emitted at wipe speed.
    RetractionValues calculateWipeRetractionLengths(GCode& gcodegen, bool toolchange);
    // Orca: rebuild the stored path while deduplicating shared path boundaries.
    void update_path(const ExtrusionPaths &paths, bool reverse = false);
};

class WipeTowerIntegration {
public:
    WipeTowerIntegration(
        const PrintConfig                                           &print_config,
        // BBS: add partplate logic
        const int                                                    plate_idx,
        const Vec3d                                                  plate_origin,
        const std::vector<WipeTower::ToolChangeResult>              &priming,
        const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
        const WipeTower::ToolChangeResult                           &final_purge,
        const std::vector<unsigned int>                             &slice_used_filaments) :
        m_left(/*float(print_config.wipe_tower_x.value)*/ 0.f),
        m_right(float(/*print_config.wipe_tower_x.value +*/ print_config.prime_tower_width.value)),
        m_wipe_tower_pos(float(print_config.wipe_tower_x.get_at(plate_idx)), float(print_config.wipe_tower_y.get_at(plate_idx))),
        m_wipe_tower_rotation(float(print_config.wipe_tower_rotation_angle)),
        m_priming(priming),
        m_tool_changes(tool_changes),
        m_final_purge(final_purge),
        m_layer_idx(-1),
        m_tool_change_idx(0),
        m_plate_origin(plate_origin),
        m_single_extruder_multi_material(print_config.single_extruder_multi_material),
        m_enable_timelapse_print(print_config.timelapse_type.value == TimelapseType::tlSmooth),
        m_enable_wrapping_detection(print_config.enable_wrapping_detection && (print_config.wrapping_exclude_area.values.size() > 2) && (slice_used_filaments.size() <= 1)),
        m_is_first_print(true),
        m_print_config(&print_config),
        m_last_wipe_tower_print_z(print_config.z_offset.value),
        m_sparse_layers_skipped(wipe_tower_sparse_layers_skipped(print_config)),
        m_sparse_layers_combined(wipe_tower_sparse_layers_combined(print_config))
    {
        // Precomputed rather than accumulated while emitting, so that the clearance validator and
        // the emitter cannot disagree about where the compacted tower sits on any given layer.
        if (m_sparse_layers_skipped)
            m_compacted_tower_z = compute_compacted_wipe_tower_z(tool_changes, float(print_config.z_offset.value));
        // initialize with the extruder offset of master extruder id
        m_extruder_offsets.resize(print_config.filament_map.size(), print_config.extruder_offset.get_at(print_config.master_extruder_id.value - 1));
        const auto& filament_map = print_config.filament_map.values; // 1 based idx
        for (size_t idx = 0; idx < filament_map.size(); ++idx)
            m_extruder_offsets[idx] = print_config.extruder_offset.get_at(filament_map[idx] - 1);
    }

    std::string prime(GCode &gcodegen);
    void next_layer() { ++ m_layer_idx; m_tool_change_idx = 0; }
    std::string tool_change(GCode &gcodegen, int extruder_id, bool finish_layer);
    bool is_empty_wipe_tower_gcode(GCode &gcodegen, int extruder_id, bool finish_layer);
    std::string finalize(GCode &gcodegen);
    std::vector<float> used_filament_length() const;

    bool is_first_print() const { return m_is_first_print;}
    void set_is_first_print(bool is) { m_is_first_print = is; }

    bool enable_timelapse_print() const { return m_enable_timelapse_print; }
    void set_wipe_tower_depth(float depth) { m_wipe_tower_depth = depth; }
    void set_wipe_tower_bbx(const BoundingBoxf & bbx) { m_wipe_tower_bbx = bbx; }
    void set_rib_offset(const Vec2f &rib_offset) { m_rib_offset = rib_offset; }

private:
    WipeTowerIntegration& operator=(const WipeTowerIntegration&);
    std::string append_tcr(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z = -1.) const;
    std::string tower_height_tag(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, const std::string &tcr_gcode) const;
    Polyline generate_path_to_wipe_tower(const Point &start_pos, const Point &end_pos, const BoundingBox &avoid_polygon, const Polygons &bed_polygons) const;
    std::string append_tcr2(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z = -1.) const;
    std::string travel_to_tower_gap(GCode &gcodegen, const Point &route_start, const Point &start_wipe_pos) const;
    Vec2f transform_wt2_pt(const Vec2f &pt) const;
    Polygons shared_printable_area(GCode &gcodegen) const;

    // Postprocesses gcode: rotates and moves G1 extrusions and returns result
    std::string post_process_wipe_tower_moves(const WipeTower::ToolChangeResult& tcr, const Vec2f& translation, float angle) const;
    // Left / right edges of the wipe tower, for the planning of wipe moves.
    const float                                                  m_left;
    const float                                                  m_right;
    const Vec2f                                                  m_wipe_tower_pos;
    const float                                                  m_wipe_tower_rotation;
    std::vector<Vec2d>                                           m_extruder_offsets;

    // Reference to cached values at the Printer class.
    const std::vector<WipeTower::ToolChangeResult>              &m_priming;
    const std::vector<std::vector<WipeTower::ToolChangeResult>> &m_tool_changes;
    const WipeTower::ToolChangeResult                           &m_final_purge;
    // Current layer index.
    int                                                          m_layer_idx;
    int                                                          m_tool_change_idx;
    double                                                       m_last_wipe_tower_print_z;

    // BBS
    Vec3d                                                        m_plate_origin;
    bool                                                         m_single_extruder_multi_material;
    bool                                                         m_enable_timelapse_print;
    bool                                                         m_enable_wrapping_detection;
    bool                                                         m_is_first_print;
    const PrintConfig *                                          m_print_config;
    float                                                        m_wipe_tower_depth;
    BoundingBoxf                                                 m_wipe_tower_bbx;
    Vec2f                                                        m_rib_offset{Vec2f(0, 0)};
    // wipe_tower_no_sparse_layers, as answered by the shared compaction rule rather than by the raw
    // option: smooth timelapse and wrapping detection keep a tower on every layer regardless.
    const bool                                                   m_sparse_layers_skipped;
    // Combined tower layers are thicker than the object layer they sit on, the only case where the
    // tower's height is not the one process_layer already declared.
    const bool                                                   m_sparse_layers_combined;
    // Print z of the compacted tower per planned layer. Empty when the tower is not compacted.
    std::vector<float>                                           m_compacted_tower_z;
};

class ColorPrintColors
{
    static const std::vector<std::string> Colors;
public:
    static const std::vector<std::string>& get() { return Colors; }
};

struct LayerResult {
    std::string gcode;
    size_t      layer_id;
    // Is spiral vase post processing enabled for this layer?
    bool        spiral_vase_enable { false };
    // Should the cooling buffer content be flushed at the end of this layer?
    bool        cooling_buffer_flush { false };
	// Is indicating if this LayerResult should be processed, or it is just inserted artificial LayerResult.
    // It is used for the pressure equalizer because it needs to buffer one layer back.
    bool        nop_layer_result { false };

    static LayerResult make_nop_layer_result() { return {"", std::numeric_limits<size_t>::max(), false, false, true}; }
};

class GCode {

public:
    GCode() :
    	m_origin(Vec2d::Zero()),
        m_enable_loop_clipping(true),
        m_resonance_avoidance(true),
        m_enable_cooling_markers(false),
        m_enable_extrusion_role_markers(false),
        m_last_processor_extrusion_role(erNone),
        m_layer_count(0),
        m_layer_index(-1),
        m_layer(nullptr),
        m_object_layer_over_raft(false),
        //m_volumetric_speed(0),
        m_last_pos_defined(false), 
        m_last_trafo(Transform3d::Identity()),
        m_last_extrusion_role(erNone),
        m_last_width(0.0f),
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_last_mm3_per_mm(0.0),
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_brim_done(false),
        m_second_layer_things_done(false),
        m_last_obj_copy(nullptr, Point(std::numeric_limits<coord_t>::max(), std::numeric_limits<coord_t>::max())),
        // BBS
        m_toolchange_count(0),
        m_nominal_z(0.)
        {}
    ~GCode() = default;

    // throws std::runtime_exception on error,
    // throws CanceledException through print->throw_if_canceled().
    void            do_export(Print* print, const char* path, GCodeProcessorResult* result = nullptr, ThumbnailsGeneratorCallback thumbnail_cb = nullptr);
    void            export_layer_filaments(GCodeProcessorResult* result);
    //BBS: set offset for gcode writer
    void set_gcode_offset(double x, double y) { m_writer.set_xy_offset(x, y); m_processor.set_xy_offset(x, y);}
    // IMEX firmware-managed slice: writer offset is augmented by the IMEX shift so the
    // emitted gcode is centered at bed origin (firmware fans copies/mirrors out from there).
    // Processor offset stays at plate_origin only so the gcode-preview visualizer renders
    // the centered toolpath at bed center, not at the prepare-view zone placement. When
    // imex_x/imex_y are zero this is byte-identical to set_gcode_offset(x, y).
    void set_gcode_offset_with_imex_shift(double x, double y, double imex_x, double imex_y) {
        m_writer.set_xy_offset(x + imex_x, y + imex_y);
        m_processor.set_xy_offset(x, y);
    }

    // Exported for the helper classes (OozePrevention, Wipe) and for the Perl binding for unit tests.
    const Vec2d&    origin() const { return m_origin; }
    void            set_origin(const Vec2d &pointf);
    void            set_origin(const coordf_t x, const coordf_t y) { this->set_origin(Vec2d(x, y)); }
    Point           last_pos() const { return m_last_pos.to_point(); }
    const Transform3d& last_trafo() const { return m_last_trafo; }
    // UNLAYERED_MOD — true when the export may carry B/C rotary words: only when non-planar
    // printing is enabled for this machine and virtual (3-axis) 5-axis output was not requested.
    // Note this must NOT be `enable_nonplanar && virtual_5axis`: with the feature off there are no
    // rotary axes at all, so B/C words would be invalid G-code on an ordinary 3-axis printer.
    // BELT_MOD: a belt printer has no rotary axes; its tilt is a fixed machine property.
    bool            emit_rotary_axes() const { return m_config.enable_nonplanar.value && ! m_config.virtual_5axis.value; }
    // BELT_MOD -- rotation that takes the bed frame to the gantry frame (identity on a flat bed):
    // B/C words describe the tool axis relative to the machine's nominal build direction.
    Matrix3d        m_world_to_gantry = Matrix3d::Identity();
    // BELT_MOD -- the orientation the machine rests in between groups and at the end: the tool
    // axis along the machine's build direction, which on a belt is the gantry normal, not world +Z.
    Transform3d     home_trafo() const { Transform3d t = Transform3d::Identity(); t.linear() = m_world_to_gantry.transpose(); return t; }
    Vec2d           point_to_gcode(const Point &point) const;
    Vec3d                    point_to_gcode(const Point3& point) const;
    Point           gcode_to_point(const Vec2d &point) const;
    Vec2d point_to_gcode_quantized(const Point& point) const;
    Vec3d                    point_to_gcode_quantized(const Point3& point) const;
    const FullPrintConfig &config() const { return m_config; }
    const Layer*    layer() const { return m_layer; }
    GCodeWriter&    writer() { return m_writer; }
    const GCodeWriter& writer() const { return m_writer; }
    PlaceholderParser& placeholder_parser() { return m_placeholder_parser_integration.parser; }
    const PlaceholderParser& placeholder_parser() const { return m_placeholder_parser_integration.parser; }
    // Process a template through the placeholder parser, collect error messages to be reported
    // inside the generated string and after the G-code export finishes.
    std::string     placeholder_parser_process(const std::string &name, const std::string &templ, unsigned int current_filament_id, const DynamicConfig *config_override = nullptr);
    bool            enable_cooling_markers() const { return m_enable_cooling_markers; }
    std::string     extrusion_role_to_string_for_parser(const ExtrusionRole &);

    // Calculate the interpolated value for the current layer between start_value and end_value.
    // Step will create equal layers steps from first to last value.
    // Step = 0 means gradual interpolation finishing at last value.
    float interpolate_value_across_layers(float start_value, float end_value, float step = 0.0f) const;

    // For Perl bindings, to be used exclusively by unit tests.
    unsigned int    layer_count() const { return m_layer_count; }
    void            set_layer_count(unsigned int value) { m_layer_count = value; }
    void            apply_print_config(const PrintConfig &print_config);

    std::string travel_to(const Point& point, ExtrusionRole role, std::string comment, double z = DBL_MAX, const Transform3d& trafo = Transform3d::Identity());
    bool            needs_retraction(const Polyline& travel, ExtrusionRole role, LiftType& lift_type);
    std::string     retract(bool toolchange = false, bool is_last_retraction = false, LiftType lift_type = LiftType::NormalLift, bool apply_instantly = false, ExtrusionRole role = erNone);
    // extra_retract forwards a PETG pre-extrusion over-extrusion; default 0 -> identical to the plain deretract.
    std::string     unretract(float extra_retract = 0.f) { return m_writer.unlift() + m_writer.unretract(extra_retract); }
    std::string     set_extruder(unsigned int extruder_id, double print_z, bool by_object=false, int toolchange_temp_override = -1, bool defer_temp_wait = false);
    // Sets the pressure advance of the filament's extruder variant, if enabled for it.
    // tool addresses one IMEX carriage explicitly. -1 means no carriage: it omits the tool
    // qualifier on Klipper, Marlin and BBL, and keeps RepRapFirmware's historical `D0`, since a
    // bare M572 there applies to whatever tool is selected and errors when none is. That is
    // what every non-IMEX caller wants, and what imex_pem_tool_for() returns off IMEX.
    std::string     set_filament_pressure_advance(unsigned int filament_id, int tool = -1);
    bool is_BBL_Printer();
    WipeTowerType wipe_tower_type();

    void assign_object_label_ids(Print &print);
    // SoftFever
    std::string set_object_info(Print* print);

    // append full config to the given string
    static void append_full_config(const Print& print, std::string& str);

    // Per-filament config-slot resolvers for the current layer (m_cur_layer_idx): the filament
    // resolver keys filament-indexed arrays, the nozzle resolver keys (extruder x volume-type)
    // slot arrays. Both degenerate to filament_id / extruder index on single-volume printers.
    size_t get_filament_config_index(int filament_id) const;
    // The filament resolver for a given layer, for the export pipeline stages after the generator,
    // which run behind the current layer and concurrently with the generator.
    size_t get_filament_config_index(int filament_id, size_t layer_id) const;
    size_t get_nozzle_config_index(int filament_id) const;

    // Holds the last slot a resolver returned without locking, so only the G-code generator may
    // call the resolvers.
    struct ConfigIndexCache
    {
        bool   valid{false};
        int    filament_id{0};
        size_t layer_idx{0};
        size_t generation{0};
        size_t index{0};

        template<class Lookup> size_t get(int filament, size_t layer, size_t gen, Lookup &&lookup)
        {
            if (!valid || filament_id != filament || layer_idx != layer || generation != gen)
                *this = {true, filament, layer, gen, size_t(lookup())};
            return index;
        }
    };

    // Object and support extrusions of the same PrintObject at the same print_z.
    // public, so that it could be accessed by free helper functions from GCode.cpp
    struct LayerToPrint
    {
        LayerToPrint() : object_layer(nullptr), support_layer(nullptr), original_object(nullptr) {}
        const Layer* 		object_layer;
        const SupportLayer* support_layer;
        const PrintObject*  original_object; //BBS: used for shared object logic
        // BELT_MOD -- a belt brim apron band at this print_z (BeltBrim.hpp); may be the only entry.
        const BeltBrimBand* belt_brim_band { nullptr };
        const Layer* 		layer()   const
        {
            if (object_layer != nullptr)
                return object_layer;

            if (support_layer != nullptr)
                return support_layer;

            return nullptr;
        }

        const PrintObject* 	object()   const
        {
            return (this->layer() != nullptr) ? this->layer()->object() : nullptr;
        }
        coordf_t            print_z() const
        {
            coordf_t sum_z = 0.;
            size_t count = 0;
            if (object_layer != nullptr) {
                sum_z += object_layer->print_z;
                count++;
            }

            if (support_layer != nullptr) {
                sum_z += support_layer->print_z;
                count++;
            }
            if (count == 0 && belt_brim_band != nullptr)   // BELT_MOD
                return belt_brim_band->print_z;

            return sum_z / count;
        }
    };

private:
    class GCodeOutputStream {
    public:
        GCodeOutputStream(FILE *f, GCodeProcessor &processor) : f(f), m_processor(processor) {}
        ~GCodeOutputStream() { this->close(); }

        bool is_open() const { return f; }
        bool is_error() const;

        void flush();
        void close();

        // Write a string into a file.
        void write(const std::string& what) { this->write(what.c_str()); }
        void write(const char* what);

        // Write a string into a file.
        // Add a newline, if the string does not end with a newline already.
        // Used to export a custom G-code section processed by the PlaceholderParser.
        void writeln(const std::string& what);

        // Formats and write into a file the given data.
        void write_format(const char* format, ...);

    private:
        FILE *f = nullptr;
        GCodeProcessor &m_processor;
    };
    void            _do_export(Print &print, GCodeOutputStream &file, ThumbnailsGeneratorCallback thumbnail_cb);

    static std::vector<LayerToPrint>        		                   collect_layers_to_print(const PrintObject &object);
    static std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> collect_layers_to_print(const Print &print);

    std::string generate_skirt(const Print &print,
        const ExtrusionEntityCollection &skirt,
        const Point& offset,
        const float skirt_start_angle,
        const LayerTools &layer_tools,
        const Layer& layer,
        unsigned int extruder_id,
        std::vector<coordf_t> &skirt_done);
    std::string generate_object_skirt_group(const Print &print,
        const PrintObject &object,
        size_t instance_id,
        const LayerTools &layer_tools,
        const Layer& layer,
        unsigned int extruder_id);
    std::string generate_object_brim(const Print &print,
        const PrintObject &object,
        size_t instance_id,
        bool first_layer,
        unsigned int extruder_id);

    // BELT_MOD -- a brim-only apron layer below the object's first layer, and the emission of the
    // apron bands carried by a set of layers (BeltBrim.hpp). The apron layer's layer change G-code
    // uses process_layer_change_gcode(), so a band counts in the running printed mass like an object layer.
    LayerResult process_belt_brim_layer(const Print &print, const std::vector<LayerToPrint> &layers,
                                        const LayerTools &layer_tools, bool last_layer, size_t single_object_instance_idx,
                                        int most_used_extruder);
    std::string emit_belt_brim_bands(const Print &print, const std::vector<LayerToPrint> &layers,
                                     size_t single_object_instance_idx, int only_extruder_id = -1);
    std::string generate_belt_brim_on_layer(const Print &print, const PrintObject &object, size_t instance_id, const Layer *object_layer);
    std::string generate_belt_brim_fills(const Print &print, const PrintObject &object, size_t instance_id,
                                         const ExtrusionEntityCollection &fills, coordf_t print_z);

    std::string process_layer_change_gcode(const Print &print, coordf_t print_z, int most_used_extruder, bool has_wipe_tower);

    LayerResult process_layer(
        const Print                     &print,
        // Set of object & print layers of the same PrintObject and with the same print_z.
        const std::vector<LayerToPrint> &layers,
        const LayerTools  				&layer_tools,
        const bool                       last_layer,
		// Pairs of PrintObject index and its instance index.
		const std::vector<const PrintInstance*> *ordering,
        // idientiy timelapse pos
        const int                        most_used_extruder,
        // If set to size_t(-1), then print all copies of all objects.
        // Otherwise print a single copy of a single object.
        const size_t                     single_object_idx = size_t(-1),
        // BBS
        const bool                       prime_extruder = false);
    // Process all layers of all objects (non-sequential mode) with a parallel pipeline:
    // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
    // and export G-code into file.
    void process_layers(
        const Print                                                         &print,
        const ToolOrdering                                                  &tool_ordering,
        const std::vector<const PrintInstance*>                             &print_object_instances_ordering,
        const std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>>   &layers_to_print,
        GCodeOutputStream                                                   &output_stream);
    // Process all layers of a single object instance (sequential mode) with a parallel pipeline:
    // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
    // and export G-code into file.
    void process_layers(
        const Print                             &print,
        const ToolOrdering                      &tool_ordering,
        std::vector<LayerToPrint>                layers_to_print,
        const size_t                             single_object_idx,
        GCodeOutputStream                       &output_stream,
        // BBS
        const bool                               prime_extruder = false);

    //BBS
    void check_placeholder_parser_failed();
    size_t cur_extruder_index() const;
    size_t get_extruder_id(unsigned int filament_id) const;
    void   update_placeholder_parser_with_variant_params();

    void            set_last_pos(const Point &pos) {
        m_last_pos = Point3(pos, 0);
        m_last_pos_defined = true;
        m_pending_start_gcode_position.reset();
    }
    void            set_last_pos(const Point3 &pos) {
        m_last_pos = pos;
        m_last_pos_defined = true;
        m_pending_start_gcode_position.reset();
    }
    bool            last_pos_defined() const { return m_last_pos_defined; }

    void            set_last_trafo(const Transform3d& trafo) { m_last_trafo = trafo; }
    // BELT_MOD -- the object's belt brim filament (1-based), restricted to the filaments the writer
    // was handed. An object whose every extrusion is flushable (flush_into_objects) contributes no
    // filament to the tool ordering, so its own outer-wall filament may be unknown to the writer;
    // the brim then takes the current filament instead of crashing in the tool change.
    unsigned int    belt_brim_filament(const PrintObject &object) const;

    void            set_extruders(const std::vector<unsigned int> &extruder_ids);
    std::string     preamble();
    // BBS
    std::string     change_layer(coordf_t print_z, const Transform3d &trafo = Transform3d::Identity());
    // Bedslinger model: derive the Y-axis acceleration limit from the machine force/bed-mass config
    // and the mass already printed. Yields the min machine Y acceleration when the A2L config keys are
    // unset (i.e. every existing printer), so it is inert for them.
    void            mass_load_limited_machine_acceleration(const PrintStatistics &curr_print_statistics, const Print &print,
                                                           double &y_acceleration_limit_res, double &accumulated_mass_res);
    // Orca: pass the complete collection of region perimeters to the extrude loop to check whether the wipe before external loop
    // should be executed
    std::string extrude_entity(const ExtrusionEntity&                     entity,
                               const std::string&                         description       = "",
                               double                                     speed             = -1.,
                               const std::vector<const ExtrusionEntity*>& region_perimeters = {},
                               const Transform3d&                         trafo             = Transform3d::Identity(),
                               const WipeInwardSupport*                   wipe_support      = nullptr);
    // Orca: pass the complete collection of region perimeters to the extrude loop to check whether the wipe before external loop
    // should be executed
    std::string extrude_loop(const ExtrusionLoop&                       loop,
                             const std::string&                         description,
                             double                                     speed             = -1.,
                             const std::vector<const ExtrusionEntity*>& region_perimeters = {},
                             const Point*                               start_point       = nullptr,
                             const Transform3d&                         trafo             = Transform3d::Identity(),
                             const WipeInwardSupport*                   wipe_support      = nullptr);
    std::string extrude_multi_path(const ExtrusionMultiPath& multipath, const std::string& description = "", double speed = -1., const Transform3d& trafo = Transform3d::Identity());
    std::string extrude_path(const ExtrusionPath& path, const std::string& description = "", double speed = -1., const Transform3d& trafo = Transform3d::Identity());

    // Orca: Adaptive PA variables
    // Used for adaptive PA when extruding paths with multiple, varying flow segments.
    // This contains the sum of the mm3_per_mm values weighted by the length of each path segment.
    // The m_multi_flow_segment_path_pa_set constrains the PA change request to the first extrusion segment.
    // It sets the mm3_mm value for the adaptive PA post processor to be the average of that path
    // as calculated and stored in the m_multi_segment_path_average_mm3_per_mm value
    double          m_multi_flow_segment_path_average_mm3_per_mm = 0;
    bool            m_multi_flow_segment_path_pa_set = false;
    // Adaptive PA last set flow to enable issuing of PA change commands when adaptive PA for overhangs
    // is enabled
    double          m_last_mm3_mm = 0;
    // Orca: Adaptive PA code segment end

    // Extruding multiple objects with soluble / non-soluble / combined supports
    // on a multi-material printer, trying to minimize tool switches.
    // Following structures sort extrusions by the extruder ID, by an order of objects and object islands.
    struct ObjectByExtruder
    {
        ObjectByExtruder() : support(nullptr), support_extrusion_role(erNone) {}
        const ExtrusionEntityCollection  *support;
        // erSupportMaterial / erSupportMaterialInterface / erSupportTransition or erMixed.
        ExtrusionRole                     support_extrusion_role;

        struct Island
        {
            struct Region {
            	// Non-owned references to LayerRegion::perimeters::entities
                std::vector<const ExtrusionEntity*> perimeters;
            	// Non-owned references to LayerRegion::fills::entities
                std::vector<const ExtrusionEntity*> infills;

                std::vector<const WipingExtrusions::ExtruderPerCopy*> infills_overrides;
                std::vector<const WipingExtrusions::ExtruderPerCopy*> perimeters_overrides;

	            enum Type {
	            	PERIMETERS,
	            	INFILL,
	            };

                // Appends perimeter/infill entities and writes don't indices of those that are not to be extruder as part of perimeter/infill wiping
                void append(const Type type, const ExtrusionEntityCollection* eec, const WipingExtrusions::ExtruderPerCopy* copy_extruders);
            };


            std::vector<Region> by_region;                                    // all extrusions for this island, grouped by regions

            // Fills in by_region_per_copy_cache and returns its reference.
            const std::vector<Region>& by_region_per_copy(std::vector<Region> &by_region_per_copy_cache, unsigned int copy, unsigned int extruder, bool wiping_entities = false) const;
        };
        std::vector<Island>         islands;
    };

	struct InstanceToPrint
	{
		InstanceToPrint(ObjectByExtruder &object_by_extruder, size_t layer_id, const PrintObject &print_object, size_t instance_id, size_t label_object_id) :
			object_by_extruder(object_by_extruder), layer_id(layer_id), print_object(print_object), instance_id(instance_id), label_object_id(label_object_id) {}

		// Repository
		ObjectByExtruder		&object_by_extruder;
		// Index into std::vector<LayerToPrint>, which contains Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
		const size_t       		 layer_id;
		const PrintObject 		&print_object;
		// Instance idx of the copy of a print object.
		const size_t			 instance_id;
        //BBS: Unique id to label object to support skiping during printing
        const size_t             label_object_id;
	};

	std::vector<InstanceToPrint> sort_print_object_instances(
		std::vector<ObjectByExtruder> 					&objects_by_extruder,
		// Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
		const std::vector<LayerToPrint> 				&layers,
		// Ordering must be defined for normal (non-sequential print).
		const std::vector<const PrintInstance*>     	*ordering,
		// For sequential print, the instance of the object to be printing has to be defined.
		const size_t                     				 single_object_instance_idx);

    std::string     extrude_perimeters(const Print& print, const std::vector<ObjectByExtruder::Island::Region>& by_region, bool is_first_layer, bool is_infill_first, const Transform3d &trafo = Transform3d::Identity(), bool unsupported_loops_only = false);
    std::string     extrude_infill(const Print& print, const std::vector<ObjectByExtruder::Island::Region>& by_region, bool ironing, const Transform3d &trafo = Transform3d::Identity());
    // trafo un-rotates support toolpaths out of the owning segment's slice frame, exactly as the
    // object body is un-rotated (see the post_trafo_inverse() use in process_layer). Support and
    // object extrusion for one segment MUST share these frame semantics: mixing them prints the
    // support in the slice frame while the part it holds up is un-rotated.
    std::string     extrude_support(const ExtrusionEntityCollection& support_fills, const ExtrusionRole support_extrusion_role, const Transform3d& trafo = Transform3d::Identity());

    // Farthest-point timelapse: find the extrusion point farthest from camera (0,0)
    void compute_farthest_point(const std::vector<LayerToPrint> &layers, int most_used_extruder,
                                const std::map<std::pair<const SupportLayer *, ExtrusionRole>, unsigned int> &support_filaments);
    // Requires an active filament.
    DynamicConfig   timelapse_gcode_config(const Print &print, coordf_t print_z, int most_used_extruder,
                                           const std::vector<const PrintObject*> *printed_objects,
                                           bool skip_pos_pick);
    // Build the per-layer timelapse snapshot g-code (safe-position or, when skip_pos_pick,
    // an inline photo at the current head position). Extracted from the former process_layer lambda so the
    // per-extrusion farthest-point hook (_extrude) can call it too. Identical to the old lambda when the
    // farthest-point subsystem is disabled (skip_pos_pick=false, m_farthest_point_timelapse.enabled=false).
    std::string     generate_timelapse_gcode(const Print &print, coordf_t print_z, int most_used_extruder,
                                              const std::set<size_t> *layer_object_label_ids,
                                              const std::vector<const PrintObject*> *printed_objects,
                                              bool skip_pos_pick = false);

    // BBS
    LiftType to_lift_type(ZHopType z_hop_types);

    std::set<ObjectInstanceID>      m_objsWithBrim; // indicates the object instances with brim
    // Cache for custom seam enforcers/blockers for each layer.
    SeamPlacer                          m_seam_placer;

    // One stop of the island-level tour: consecutive islands of a single instance. An instance
    // can have several visits per layer when its islands are toured non-consecutively.
    struct InstanceVisit
    {
        // Index into the per-filament InstanceToPrint vector.
        size_t              instance_idx;
        // Islands to print, in order (indices into ObjectByExtruder::islands). Empty: print all
        // islands, ordered at extrusion time.
        std::vector<size_t> islands;
        // First visit of this instance this layer; skirt, brim and support are emitted here.
        bool                first_visit;
    };

    // One node of the island-level tour, also used as cache key: identity plus quantized position.
    struct IslandOrderNode
    {
        ObjectID object_id;
        size_t   instance_id;
        // Index into ObjectByExtruder::islands, or size_t(-1) for an instance without chainable
        // islands (e.g. support only), which is toured as a single stop.
        size_t   island_idx;
        // Island centroid in G-code coordinates, quantized to 1 mm for cache stability.
        Point    pos;
        bool operator==(const IslandOrderNode &rhs) const {
            return object_id == rhs.object_id && instance_id == rhs.instance_id &&
                   island_idx == rhs.island_idx && pos == rhs.pos;
        }
    };

    // Cache the per-filament island tour to avoid recomputing while the layer's island layout is
    // unchanged. Key: filament_id. Value: the nodes the tour was computed from, the per-instance
    // island layout (count and whether the trailing catch-all island has anything to print), and
    // the resulting visits.
    // UNLAYERED_MOD: the layout is part of the key. Nodes only cover the chainable islands, so two
    // layers with the same centroids but a different number of islands (thin walls, negative
    // volumes come and go) matched the cache and the visit's catch-all index -- islands.size() - 1
    // of the OLD layer -- ran past the new layer's islands (found by fuzzing: segfault in
    // extrude_perimeters on multi-part objects).
    struct IslandOrderCacheEntry
    {
        std::vector<IslandOrderNode>         nodes;
        std::vector<std::pair<size_t, bool>> layout;
        std::vector<InstanceVisit>           visits;
    };
    std::map<unsigned int, IslandOrderCacheEntry> m_ordering_cache;

    ExtrusionQualityEstimator m_extrusion_quality_estimator;


    /* Origin of print coordinates expressed in unscaled G-code coordinates.
       This affects the input arguments supplied to the extrude*() and travel_to()
       methods. */
    Vec2d                               m_origin;
    FullPrintConfig                     m_config;
    DynamicConfig                       m_calib_config;
    // scaled G-code resolution
    double                              m_scaled_resolution;
    GCodeWriter                         m_writer;

    struct PlaceholderParserIntegration {
        void reset();
        void init(const GCodeWriter &config);
        void update_from_gcodewriter(const GCodeWriter &writer);
        void validate_output_vector_variables();

        PlaceholderParser                   parser;
        // For random number generator etc.
        PlaceholderParser::ContextData      context;
        // Collection of templates, on which the placeholder substitution failed.
        std::map<std::string, std::string>  failed_templates;
        // Input/output from/to custom G-code block, for returning position, retraction etc.
        DynamicConfig                       output_config;
        ConfigOptionFloats                 *opt_position { nullptr };
        ConfigOptionFloat                  *opt_zhop { nullptr };
        ConfigOptionFloats                 *opt_e_position { nullptr };
        ConfigOptionFloats                 *opt_e_retracted { nullptr };
        ConfigOptionFloats                 *opt_e_restart_extra { nullptr };
        ConfigOptionFloats                 *opt_extruded_volume { nullptr };
        ConfigOptionFloats                 *opt_extruded_weight { nullptr };
        ConfigOptionFloat                  *opt_extruded_volume_total { nullptr };
        ConfigOptionFloat                  *opt_extruded_weight_total { nullptr };
        // Caches of the data passed to the script.
        size_t                              num_extruders;
        std::vector<double>                 position;
        std::vector<double>                 e_position;
        std::vector<double>                 e_retracted;
        std::vector<double>                 e_restart_extra;
    } m_placeholder_parser_integration;

    OozePrevention                      m_ooze_prevention;
    Wipe                                m_wipe;
    AvoidCrossingPerimeters             m_avoid_crossing_perimeters;
    ExclusionVolumeTravelAvoidance      m_exclusion_volume_travel_avoidance;
    RetractWhenCrossingPerimeters       m_retract_when_crossing_perimeters;
    TimelapsePosPicker                  m_timelapse_pos_picker;
    bool                                m_timelapse_uses_pos{ false };
    bool                                m_timelapse_uses_clear_to_x0{ false };

    // Farthest-point timelapse context. Corexy-only refinement layered on top of the existing
    // timelapse_type. All fields default to the inert state; `enabled` is (re)computed each layer in
    // process_layer and is false whenever the farthest_point_timelapse config toggle is off, the printer
    // is i3 (psI3), or timelapse_type is not traditional — so every shipping printer that does not set the
    // toggle is identical to the previous path.
    struct FarthestPointTimelapseContext {
        // Whether farthest-point timelapse is active for this layer
        bool    enabled{false};
        // The farthest extrusion point from camera (0,0) in global scaled coordinates (includes plate origin + inst.shift)
        Point   farthest_point;
        // farthest_point converted to mm (gcode coordinate space, includes plate origin)
        Vec2d   farthest_gcode_pos{0, 0};
        // Extruder index (0-based) that prints the farthest point
        int     farthest_extruder_id{0};
        // Whether the farthest point is printed by the photo head (most_used_extruder)
        bool    farthest_is_photo_head{false};
        // Whether inline timelapse gcode has already been inserted on this layer
        bool    inserted_this_layer{false};
        // The extruder used most on this layer, chosen as the photo head
        int     most_used_extruder{0};
        // Object labels for the current layer, used when inline timelapse is inserted from extrusion code.
        std::set<size_t> layer_object_label_ids;
    };
    FarthestPointTimelapseContext m_farthest_point_timelapse;

    bool                                m_enable_loop_clipping;
    //resonance avoidance
    bool                                m_resonance_avoidance; 
    // If enabled, the G-code generator will put following comments at the ends
    // of the G-code lines: _EXTRUDE_SET_SPEED, _WIPE, _OVERHANG_FAN_START, _OVERHANG_FAN_END
    // Those comments are received and consumed (removed from the G-code) by the CoolingBuffer.pm Perl module.
    bool                                m_enable_cooling_markers;
    
    bool m_enable_exclude_object;
    std::vector<size_t> m_label_objects_ids;
    // Object label names by instance, built on first use from the ids assign_object_label_ids() assigns.
    std::unordered_map<const PrintInstance*, std::string> m_instance_names;
    const std::string& instance_name(const PrintInstance &instance);
    std::string _encode_label_ids_to_base64(std::vector<size_t> ids);
    // ORCA: Add support for role based fan speed control
    std::array<bool, ExtrusionRole::erCount> m_is_role_based_fan_on;
    std::array<int, ExtrusionRole::erCount>  m_role_based_fan_marker_layer;

    // BELT_MOD -- contact-aware first layer. "First layer" is decided per path by what the material
    // rests on and how far above it the path lies, not by the slice layer index: on a belt every
    // layer has a strip lying on the belt, and a non-root cut-plane segment's layer 0 rests on
    // plastic. Active only for rotated frames and non-root segments, so planar output is unchanged.
    struct PathContact {
        enum Class { None, Bed, Object };
        Class cls   = None;
        int   index = 0;   // height above the contact surface in bands (the "layer index" for ramps)
    };
    struct ContactFrame {
        const PrintObject *object  = nullptr;
        Transform3d        trafo   = Transform3d::Identity();
        bool               active  = false;
        Vec3d              bed_n   = Vec3d::UnitZ();  // world z = bed_n . (gx, gy, z) + bed_k in _extrude's frame
        double             bed_k   = 0.;
        double             band    = 0.;              // first-layer boolean band (initial layer height)
        double             thickness = 0.;            // ramp band thickness
        double             offset  = 0.;              // first_layer_plane_offset
        int                max_index = 1;             // indices above this are all the same to every consumer
        bool               has_floor = false;         // non-root segment: object contact above its floor
        SegmentSupportBounds floor;                   // slice-frame floor planes of that segment
        double             object_band = 0.;          // object_contact_layers * thickness
    };
    ContactFrame                        m_contact_frame;
    PathContact                         m_path_contact;
    // Marker state for the CoolingBuffer (same scheme as the role-based fan markers).
    PathContact::Class                  m_contact_marker_cls   = PathContact::None;
    int                                 m_contact_marker_index = -1;
    int                                 m_contact_marker_layer = -1;
    void        update_contact_frame(const PrintObject *object, const Transform3d &trafo);
    // Classifies `path` in the current contact frame. Returns true when the path crosses a band
    // boundary and has been split into `parts` (each uniformly classified); otherwise sets
    // m_path_contact for the whole path and returns false.
    bool        classify_path_contact(const ExtrusionPath &path, std::vector<std::pair<ExtrusionPath, PathContact>> &parts);
    bool                                m_path_contact_locked = false;   // emitting the pieces of a split path
    // Contact-aware replacements for on_first_layer() / layer_id() inside _extrude.
    bool        path_on_first_layer() const;
    int         path_first_layer_index() const;
    // Speed / flow on printed material, 0 = use the first-layer values.
    double      path_object_contact_speed() const;
    double      path_object_contact_flow_ratio() const;
    std::string contact_fan_markers();
    // Belt: initial-layer temperatures are held for belt_initial_temperature_length mm of belt travel.
    bool        on_initial_temperature_layer(double print_z) const;
    bool        on_initial_temperature_layer() const { return m_layer != nullptr ? this->on_initial_temperature_layer(m_layer->print_z) : this->on_first_layer(); }
    // Markers for the Pressure Equalizer to recognize the extrusion type.
    // The Pressure Equalizer removes the markers from the final G-code.
    bool                                m_enable_extrusion_role_markers;
    // Keeps track of the last extrusion role passed to the processor
    ExtrusionRole                       m_last_processor_extrusion_role;
    // How many times will change_layer() be called?
    // change_layer() will update the progress bar.
    unsigned int                        m_layer_count;
    // Progress bar indicator. Increments from -1 up to layer_count.
    int                                 m_layer_index;
    // Current layer processed. In sequential printing mode, only a single copy will be printed.
    // In non-sequential mode, all its copies will be printed.
    const Layer*                        m_layer;
    // m_layer is an object layer and it is being printed over raft surface.
    bool                                m_object_layer_over_raft;
    //double                              m_volumetric_speed;
    // Support for the extrusion role markers. Which marker is active?
    ExtrusionRole                       m_last_extrusion_role;
    // To ignore gapfill role for retract_lift_enforce
    ExtrusionRole                       m_last_notgapfill_extrusion_role;
    // Support for G-Code Processor
    float                               m_last_height{ 0.0f };
    float                               m_last_layer_z{ 0.0f };
    float                               m_max_layer_z{ 0.0f };
    // UNLAYERED_MOD 2026-10-09: what G-code templates see as {max_layer_z}, and the height a by-object
    // travel clears. m_max_layer_z is the highest print_z, but for a tilted or later multiplanar chunk
    // print_z is a distance along that chunk's build normal, not a height above the bed: a virtual
    // 5-axis Benchy on a Bambu H2S got max_layer_z 37.4 while the part ended at ~67.5, and the stock
    // end G-code "G1 Z{max_layer_z + 0.5} ; lower z a little" drove the nozzle 30 mm down into it.
    // The highest Z material was actually laid at (bed frame, z_offset removed) is the true top.
    // Identical to m_max_layer_z for an ordinary planar print.
    float                               max_layer_z_for_templates() const;
    float                               m_last_width{ 0.0f };
    // Bedslinger mass model: cumulative printed mass at the previous layer change, used to derive
    // previous_layer_mass, the mass of the layer printed since then.
    double                              m_last_layer_accumulated_mass{ 0.0 };

    // Always check gcode placeholders when building in debug mode.
#if !defined(NDEBUG)
#undef ORCA_CHECK_GCODE_PLACEHOLDERS
#define ORCA_CHECK_GCODE_PLACEHOLDERS 1
#endif
    
#if ORCA_CHECK_GCODE_PLACEHOLDERS
    std::map<std::string, std::vector<std::string>> m_placeholder_error_messages;
#endif

    Point3                              m_last_pos;
    bool                                m_last_pos_defined;

    Transform3d m_last_trafo;
    // BELT_MOD -- belt brim: the apron band's Z while its paths are emitted (no Layer of its own),
    // and the count of apron layers emitted (their layer index in the result).
    std::optional<coordf_t> m_belt_brim_z;
    size_t                  m_belt_brim_layer_idx { 0 };

    // Orca: accumulated wave-extrusion time on the current layer (seconds).
    // Used by wave_overhang_min_layer_time; reset at each layer change.
    double                              m_wave_layer_accumulated_time = 0.;

    struct ToolchangePositionState
    {
        Vec2d emitted_xy{Vec2d::Zero()};
    };

    // Capture and restore the actual command-space position around a physical
    // nozzle change. Model-space m_last_pos is tied to the active nozzle offset,
    // while the machine position itself does not change when Tx is emitted.
    std::optional<ToolchangePositionState> capture_toolchange_position();
    void synchronize_toolchange_position(const ToolchangePositionState &before,
                                         const std::string             &emitted_gcode);

    std::unique_ptr<CoolingBuffer>      m_cooling_buffer;
    std::unique_ptr<SpiralVase>         m_spiral_vase;

    std::unique_ptr<PressureEqualizer>  m_pressure_equalizer;
    
    std::unique_ptr<AdaptivePAProcessor>      m_pa_processor;

    // IMEX: active parallel mode name ("primary", "copy", "iq-copy", etc.).
    // Set at the start of export. Empty string means non-IMEX or not yet set.
    // PA and temperature tool-qualification is only applied when this is not "primary".
    std::string m_imex_parallel_mode;
    // IMEX: parsed per-plate head→filament overrides (physical T-index → 1-based filament slot).
    // Cached from imex_head_filament_map at print start. Empty map means "fall back to pem".
    std::map<int,int> m_imex_head_filament_map;

    std::unique_ptr<WipeTowerIntegration> m_wipe_tower;

    std::unique_ptr<SmallAreaInfillFlowCompensator> m_small_area_infill_flow_compensator;
    
    // Heights (print_z) at which each grouped skirt has already been extruded.
    std::vector<std::vector<coordf_t>>   m_skirt_group_done;
    // Has the brim been extruded already? Brim is being extruded only for the first object of a multi-object print.
    bool                                m_brim_done;
    // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
    bool                                m_second_layer_things_done;
    // Index of a last object copy extruded.
    std::pair<const PrintObject*, Point> m_last_obj_copy;

    // 1 << 0: A1 series cannot supprot traditional timelapse when printing by object (cannot turn on timelapse)
    // 1 << 1: A1 series cannot supprot traditional timelapse with spiral vase mode   (cannot turn on timelapse)
    // 1 << 2: Timelapse in smooth mode without wipe tower (turn on with prompt)
    int m_timelapse_warning_code = 0;
    bool m_support_traditional_timelapse = true;


    Print *m_print{nullptr};

    std::vector<const PrintObject*> m_printed_objects;

    // Processor
    GCodeProcessor m_processor;
    // Command-space endpoint after startup G-code. The processor sees direct
    // file writes synchronously, while the generator has not established its
    // first model-space point yet.
    std::optional<GCodeProcessor::PositionState> m_pending_start_gcode_position;

    //some post-processing on the file, with their data class
    std::unique_ptr<FanMover> m_fan_mover;

    // BBS
    Print* m_curr_print = nullptr;
    unsigned int m_toolchange_count;
    coordf_t m_nominal_z;
    // Mixed-color sublayer state. Non-zero only while emitting a mixed slot's sub-layer:
    // scales extrusion flow to the sub-layer's share of the nominal layer height, and
    // reports that sub-height as the effective extrusion height. Reset to 0 afterwards.
    double   m_sub_layer_flow_ratio = 0.0;
    double   m_sub_layer_height     = 0.0;
    bool m_need_change_layer_lift_z = false;
    // UNLAYERED_MOD: nonplanar layers travel guard. Set once a curved-shell path
    // (ExtrusionPath::nonplanar_shell) has been extruded: every travel then rises vertically to
    // m_np_safe_z (slice frame, like m_nominal_z) before moving in XY and descends vertically onto
    // its target, and wiping is suppressed, so the nozzle never drags through printed curved
    // material. Cleared by the first travel made once the layer's nominal Z is above it.
    bool   m_np_guard  = false;
    double m_np_safe_z = 0.;
    // Set for the whole export when any object prints nonplanar layers: then every extrusion is
    // recorded into a coarse height map (0.5 mm cells, G-code XY, slice-frame Z) and a guarded
    // travel rises only as high as the material under its own XY path, not to the patch top.
    bool m_np_enabled = false;
    std::unordered_map<uint64_t, float> m_np_hmap;
    // UNLAYERED_MOD — ByGroup: set when a cross-orientation hop to the next group member is
    // pending. The next travel_to must traverse at machine-vertical height and descend straight
    // down onto the destination (tool-axis lifts tilt/dive through the bed). One-shot.
    // UNLAYERED_MOD — shared four-move inter-chunk transition (ByObject + ByGroup).
    std::string chunk_transition_gcode(const Print &print, const PrintObject &object, const Transform3d &trafo, double staging_z_local);
    bool m_bygroup_safe_hop = false;
    // UNLAYERED_MOD — ByGroup cross-orientation hop: target rotary orientation (deg) and total
    // angular travel, carried from the orientation change to the deferred hop travel so the B/C
    // swing runs simultaneously with the XY traverse at a whip-safe feedrate.
    double m_bygroup_hop_b_deg = 0.;
    double m_bygroup_hop_c_deg = 0.;
    double m_bygroup_hop_bc_dist_deg = 0.;
    // UNLAYERED_MOD -- multiplanar base C (GCode/MultiplanarBaseC.hpp): while a By-group group prints
    // its planar base, the C the table is held at (radians, the first angled chunk's), standing in for
    // the C = 0 that trafo_to_bc() reports for a vertical tool axis. Empty outside a base; cleared by
    // the first transition onto an angled chunk and at the end of the group.
    std::optional<double> m_multiplanar_base_c;
    int m_start_gcode_filament = -1;
    std::string m_filament_instances_code;

    // Object layer id of the layer being generated; keys the per-filament config-slot
    // resolvers. Distinct from m_layer_index (an export progress counter starting at -1).
    size_t m_cur_layer_idx{0};
    mutable ConfigIndexCache m_filament_index_cache;
    mutable ConfigIndexCache m_nozzle_index_cache;

    std::set<unsigned int>                  m_initial_layer_extruders;
    std::vector<std::vector<unsigned int>>  m_sorted_layer_filaments;
    // BBS
    int get_bed_temperature(const int extruder_id, const bool is_first_layer, const BedType bed_type) const;
    int get_highest_bed_temperature(const bool is_first_layer,const Print &print) const;

    void update_layer_related_config(int layer_id);

    double      calc_max_volumetric_speed(const double layer_height, const double line_width, const std::string co_str);
    std::string _extrude(const ExtrusionPath &path, const std::string &path_description = "", double speed = -1, const Transform3d &trafo = Transform3d::Identity());
    bool _needSAFC(const ExtrusionPath &path);
    void print_machine_envelope(GCodeOutputStream& file, Print& print);
    void _print_first_layer_bed_temperature(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait);
    void _print_first_layer_extruder_temperatures(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait);
    // On the first printing layer. This flag triggers first layer speeds.
    //BBS
    bool    on_first_layer() const { return m_layer != nullptr && m_layer->id() == 0 && abs(m_layer->bottom_z()) < EPSILON; }
    int layer_id() const {
        if (m_layer == nullptr)
            return -1;
        return m_layer->id();
    }
    // To control print speed of 1st object layer over raft interface.
    bool                                object_layer_over_raft() const { return m_object_layer_over_raft; }

    friend ObjectByExtruder& object_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>> &by_extruder,
        unsigned int                                           extruder_id,
        size_t                                                 object_idx,
        size_t                                                 num_objects);
    friend std::vector<ObjectByExtruder::Island>& object_islands_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>>  &by_extruder,
        unsigned int                                            extruder_id,
        size_t                                                  object_idx,
        size_t                                                  num_objects,
        size_t                                                  num_islands);

    friend class OozePrevention;
    friend class Wipe;
    friend class WipeTowerIntegration;
    friend class PressureEqualizer;
    friend class Print;
    friend class SmallAreaInfillFlowCompensator;
};

std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print, bool init_order = false);

// The overhang data ExtrusionQualityEstimator needs for the object layers in `layers`, computed ahead of the generator;
// `overhang_fan` says whether the overhang fan can switch on for any filament.
std::vector<PrecomputedOverhangLayer> precompute_overhang_layers(const std::vector<GCode::LayerToPrint> &layers, bool overhang_fan);

}

#endif
