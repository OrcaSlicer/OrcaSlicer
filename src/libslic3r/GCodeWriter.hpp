#ifndef slic3r_GCodeWriter_hpp_
#define slic3r_GCodeWriter_hpp_

#include "libslic3r.h"
#include <cstddef>
#include <cassert>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <charconv>
#include <optional>
#include <vector>
#include <functional>
#include <utility>
#include "Extruder.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "GCode/CoolingBuffer.hpp"
#include "GCode/MachineKinematics.hpp"
#include <memory>
#include "Config.hpp"
#include "GCode/MachineKinematics.hpp"
#include <memory>

namespace Slic3r {


class GCodeWriter {
public:
    GCodeConfig config;
    bool multiple_extruders;

    GCodeWriter() :
        multiple_extruders(false),
        m_lifted(0),
        m_to_lift(0),
        m_to_lift_type(LiftType::NormalLift),
        m_is_first_layer(true), m_current_speed(3600),
        m_kinematics(std::make_unique<CartesianKinematics>()),
        m_cached_extruder_idx(0),
        m_curr_filament_extruder(MAXIMUM_EXTRUDER_NUMBER, nullptr),
        m_curr_extruder_id (-1),
        m_single_extruder_multi_material(false),
        m_last_acceleration(0), m_max_acceleration(0),m_last_travel_acceleration(0), m_max_travel_acceleration(0),
        m_last_jerk(0), m_max_jerk_x(0), m_max_jerk_y(0),
        m_last_bed_temperature(0), m_last_bed_temperature_reached(true)
        {}
    Extruder* filament(size_t extruder_id) { assert(extruder_id < m_curr_filament_extruder.size()); return m_curr_filament_extruder[extruder_id]; }
    const Extruder* filament(size_t extruder_id) const { assert(extruder_id < m_curr_filament_extruder.size()); return m_curr_filament_extruder[extruder_id]; }
    Extruder* filament() { if (m_curr_extruder_id == -1) return nullptr; return m_curr_filament_extruder[m_curr_extruder_id]; }
    const Extruder* filament() const { if(m_curr_extruder_id==-1) return nullptr; return m_curr_filament_extruder[m_curr_extruder_id]; }

    void                 apply_print_config(const PrintConfig &print_config);
    // Extruders are expected to be sorted in an increasing order.
    void                 set_extruders(std::vector<unsigned int> extruder_ids);
    const std::vector<Extruder>& extruders() const { return m_filament_extruders; }
    std::vector<unsigned int> extruder_ids() const {
        std::vector<unsigned int> out;
        out.reserve(m_filament_extruders.size());
        for (const Extruder &e : m_filament_extruders)
            out.push_back(e.id());
        return out;
    }
    std::string preamble();
    std::string postamble() const;
    static std::string set_temperature(unsigned int temperature, GCodeFlavor flavor, bool wait = false, int tool = -1, std::string comment = std::string());

    std::string set_temperature(unsigned int temperature, bool wait = false, int tool = -1) const;
    std::string set_bed_temperature(int temperature, bool wait = false);
    std::string set_chamber_temperature(int temperature, bool wait = false);
    std::string set_print_acceleration(unsigned int acceleration)   { return set_acceleration_internal(Acceleration::Print, acceleration); }
    std::string set_travel_acceleration(unsigned int acceleration)  { return set_acceleration_internal(Acceleration::Travel, acceleration); }
    std::string set_jerk_xy(double jerk);
    // Orca: set acceleration and jerk in one command for Klipper
    std::string set_accel_and_jerk(unsigned int acceleration, double jerk);
    // Orca: Klipper's accel_to_decel limit for `acceleration`, sent as the legacy ACCEL_TO_DECEL and as the
    // equivalent MINIMUM_CRUISE_RATIO = 1 - ACCEL_TO_DECEL / ACCEL that replaced it. Klipper before 0.13 and
    // vendor forks read only the first, current Klipper only the second, and 0.13 prefers the second.
    static void append_klipper_accel_to_decel(std::string &gcode, unsigned int acceleration, double accel_to_decel_factor);
    std::string set_junction_deviation(double junction_deviation); 
    std::string set_pressure_advance(double pa, int tool = -1) const;
    std::string set_input_shaping(char axis, float damp, float freq, std::string type) const;
    std::string reset_e(bool force = false);
    std::string update_progress(unsigned int num, unsigned int tot, bool allow_100 = false) const;
    std::string enable_power_loss_recovery(PowerLossRecoveryMode mode);
    // return false if this extruder was already selected
    bool        need_toolchange(unsigned int filament_id) const;
    std::string set_extruder(unsigned int filament_id);
    void init_extruder(unsigned int filament_id);
    // Current parked-retract length of a filament's extruder (share-aware). Used for the
    // new_extruder_retracted_length change-filament placeholder. Returns 0 if the filament is unknown.
    double get_extruder_retracted_length(const int filament_id);
    // Prefix of the toolchange G-code line, to be used by the CoolingBuffer to separate sections of the G-code
    // printed with the same extruder.
    std::string toolchange_prefix() const;
    std::string toolchange(unsigned int filament_id, int nozzle_id);
    std::string set_speed(double F, const std::string &comment = std::string(), const std::string &cooling_marker = std::string());
    // SoftFever NOTE: the returned speed is mm/minute
    double      get_current_speed() const { return m_current_speed;}
    std::string travel_to_xy(const Vec2d &point, const std::string &comment = std::string());
    std::string travel_to_xyz(const Vec3d &point, const std::string &comment = std::string(), bool force_z = false, Transform3d trafo = Transform3d::Identity());
    // Physical Z range which travel_to_xyz() will emit without mutating writer state.
    // This includes an existing lift or a pending lazy lift.
    std::pair<double, double> planned_travel_z(const Vec3d &point, bool force_z = false) const;
    std::string travel_to_z(double z, const std::string &comment = std::string(), bool force = false);
    bool        will_move_z(double z) const;
    std::string extrude_to_xy(const Vec2d &point, double dE, const std::string &comment = std::string(), bool force_no_extrusion = false);
    //BBS: generate G2 or G3 extrude which moves by arc
    std::string extrude_arc_to_xy(const Vec2d &point, const Vec2d &center_offset, double dE, const bool is_ccw, const std::string &comment = std::string(), bool force_no_extrusion = false);
    // Linear approximation of an arc, used when the machine mapping cannot
    // express a G2/G3. Must be called before m_pos is updated: center_offset is
    // relative to the current position.
    void        extrude_arc_as_polyline(std::string &out, const Vec2d &point, const Vec2d &center_offset, double dE, const bool is_ccw, const std::string &comment = std::string(), bool force_no_extrusion = false);
    // is_wipe: the move retracts along a wipe. It counts and emits E like an extrusion but lays no
    // material, so the minimum extrusion Z does not apply to it and it does not raise the maximum.
    std::string extrude_to_xyz(const Vec3d &point, double dE, const std::string &comment = std::string(), bool force_no_extrusion = false, Transform3d trafo = Transform3d::Identity(), bool is_wipe = false);
    // Each appends its line to `out`.
    void        set_speed(std::string &out, double F, const std::string &comment = std::string(), const std::string &cooling_marker = std::string());
    void        extrude_to_xy(std::string &out, const Vec2d &point, double dE, const std::string &comment = std::string(), bool force_no_extrusion = false);
    void        extrude_arc_to_xy(std::string &out, const Vec2d &point, const Vec2d &center_offset, double dE, const bool is_ccw, const std::string &comment = std::string(), bool force_no_extrusion = false);
    void        extrude_to_xyz(std::string &out, const Vec3d &point, double dE, const std::string &comment = std::string(), bool force_no_extrusion = false, Transform3d trafo = Transform3d::Identity(), bool is_wipe = false);
    std::string retract(bool before_wipe = false, double retract_length = 0);
    std::string retract_for_toolchange(bool before_wipe = false, double retract_length = 0);
    // extra_retract adds a small over-extrusion to the deretract move (PETG pre-extrusion).
    // Default 0 -> byte-identical to the plain deretract.
    std::string unretract(float extra_retract = 0.f);
    // do lift instantly
    std::string eager_lift(const LiftType type);
    // record a lift request, do realy lift in next travel
    std::string lazy_lift(LiftType lift_type = LiftType::NormalLift, bool spiral_vase = false);
    std::string unlift();
    const Vec3d& get_position() const { return m_pos; }
    Vec3d&       get_position() { return m_pos; }
    void        set_position(const Vec3d& in) { m_pos = in; }
    double      get_zhop() const { return m_lifted; }
    // UNLAYERED_MOD -- the height below which an extruding move is refused ("GCode: very low z").
    // 0.1 mm by default; GCode lowers it for thin first layers (a belt brim apron band is laid at
    // about half the first layer height above the belt).
    void        set_min_extrusion_z(double z) { m_min_extrusion_z = z; }
    double      min_extrusion_z() const { return m_min_extrusion_z; }
    // UNLAYERED_MOD — highest MACHINE-frame Z at which this file has laid material so far. The
    // nozzle tip sits on top of the bead it is extruding, so this is an upper bound on the height
    // of everything printed, in the frame travels are emitted in. GCode::m_max_layer_z cannot serve
    // that role for multiplanar prints: it tracks Layer::print_z, which for a tilted chunk is a
    // distance along that chunk's build normal, not a bed-frame height.
    double      max_extrusion_z() const { return m_max_extrusion_z; }

    //BBS: set offset for gcode writer
    void set_xy_offset(double x, double y) { m_x_offset = x; m_y_offset = y; }
    Vec2f get_xy_offset() { return Vec2f{m_x_offset, m_y_offset}; };

    // UNLAYERED_MOD — the active extruder's XY nozzle offset, in the MACHINE frame.
    // GCode::point_to_gcode subtracts this while building the point, i.e. BEFORE the nonplanar
    // un-rotation in apply_test_rotation2. For a tilted segment that rotation then carries a
    // machine-frame quantity into the chunk's frame, so one offset lands on a different axis per
    // segment and the chunks come out of register with each other. Measured on a real part with
    // extruder_offset = 0x2: the untilted root shifted 2 mm in Y while a 45-degree chunk shifted
    // 2 mm in X. Zero for every machine whose extruder_offset is 0 (the usual case), which is why
    // only the P1S profile showed it.
    Vec3d extruder_offset_xy() const {
        const Extruder *e = this->filament();
        if (e == nullptr)
            return Vec3d::Zero();
        const size_t id = e->extruder_id();
        if (id >= m_extruder_offsets.size())
            return Vec3d::Zero();
        return Vec3d(m_extruder_offsets[id].x(), m_extruder_offsets[id].y(), 0.);
    }
    // To be called by the CoolingBuffer from another thread.
    // ORCA: `part_cooling_fan_min_pwm` (0-100, default 0) is a floor applied only when `speed` is non-zero, used to overcome
    // PWM start-up thresholds on fans that won't spool below a certain duty cycle. A `speed` of 0 is always honoured.
    static std::string set_fan(const GCodeFlavor gcode_flavor, unsigned int speed, unsigned int part_cooling_fan_min_pwm = 0);
    // To be called by the main thread. It always emits the G-code, it does not remember the previous state.
    // Keeping the state is left to the CoolingBuffer, which runs asynchronously on another thread.
    std::string set_fan(unsigned int speed) const;
    //BBS: set additional fan speed for BBS machine only
    static std::string set_additional_fan(unsigned int speed);
    static std::string set_exhaust_fan(int speed);
    //BBS
    void set_object_start_str(std::string start_string) { m_gcode_label_objects_start = start_string; }
    bool is_object_start_str_empty() { return m_gcode_label_objects_start.empty(); }
    void set_object_end_str(std::string end_string) { m_gcode_label_objects_end = end_string; }
    bool is_object_end_str_empty() { return m_gcode_label_objects_end.empty(); }
    void add_object_start_labels(std::string &gcode);
    void add_object_end_labels(std::string &gcode);
    void add_object_change_labels(std::string& gcode);

    //BBS:
    void set_current_position_clear(bool clear) { m_is_current_pos_clear = clear; };
    bool is_current_position_clear() const { return m_is_current_pos_clear; };
    // UNLAYERED_MOD — emit a collision-safe hop to a destination given in the part's local frame:
    // traverse in XY at the current (already-lifted) machine-vertical height, then descend straight
    // down onto the destination. Unlike the normal travel paths this keeps the traverse height in
    // machine Z (not the tilted tool axis), so it never drags over neighbors or dives through the
    // bed regardless of the member's orientation. Used by ByGroup cross-orientation hops.
    // When rotate is true, the B/C swing to (angle_b_deg, angle_c_deg) is folded into the high XY
    // traverse so the rotary axes move simultaneously with X/Y. bc_angular_dist_deg is the total
    // angular travel and is used to clamp the feedrate so the rotary axes never whip on short hops.
    std::string travel_to_xyz_high_then_down(const Vec3d &local_point, const Transform3d &trafo, const std::string &comment,
                                             bool rotate = false, double angle_b_deg = 0., double angle_c_deg = 0.,
                                             double bc_angular_dist_deg = 0.);
    // UNLAYERED_MOD — emit one travel to an absolute point that is ALREADY in the bed frame, with
    // no trafo applied and none of travel_to_xyz's lazy-lift rewriting. Multiplanar inter-chunk
    // transitions plan their path in the bed frame directly, so they need a dumb emitter; going
    // through travel_to_xyz would re-apply a rotation, and through travel_to_z would risk
    // will_move_z() silently swallowing the lift.
    std::string travel_to_xyz_direct(const Vec3d &bed_point, const std::string &comment);
    // UNLAYERED_MOD — one G0 rapid to an absolute bed-frame point, with optional B/C words.
    // Same dumb-emitter contract as travel_to_xyz_direct (no trafo, no lazy-lift rewriting);
    // used by the ByObject inter-chunk transition, whose staging moves are rapids by design.
    std::string rapid_to_xyzbc(const Vec3d &bed_point, std::optional<double> angle_b_deg, std::optional<double> angle_c_deg,
                               const std::string &comment);
    //BBS:
    static bool full_gcode_comment;
    //SoftFever
    void set_is_bbl_machine(bool bval) {m_is_bbl_printers = bval;}
    const bool is_bbl_printers() const {return m_is_bbl_printers;}
    void set_is_first_layer(bool bval) { m_is_first_layer = bval; }
    // BELT_MOD -- contact-aware first layer: when >= 0, a travel destination counts as first layer
    // when its bed-frame z (m_pos is the bed frame) lies below this height, instead of the
    // per-layer flag. -1 (default) keeps the flag.
    void set_first_layer_band(double z_max) { m_first_layer_band = z_max; }
    // Wave overhangs: XY/XYZ travel feedrate override in mm/s, 0 = none. GCode::_extrude() sets it around a
    // wave-overhang path (RAII) so the travel INTO the path, and any travel it emits, uses the wave travel
    // speed. Every XY travel emitter reads it through travel_xy_speed(), including the 5-axis ones;
    // pure Z moves (travel_speed_z) are not affected.
    void   set_travel_speed_override(double speed_mm_s) { m_travel_speed_override = speed_mm_s; }
    double travel_speed_override() const { return m_travel_speed_override; }
    GCodeFlavor get_gcode_flavor() const { return config.gcode_flavor; }
    void invalidate_acceleration() { m_last_acceleration = 0; m_last_travel_acceleration = 0; }
    void invalidate_jerk() { m_last_jerk = 0; }

    // Axis remap: permute/negate/reverse axes in G-code output.
    // Works standalone (without belt mode) for printers with non-standard axis conventions.
    void set_axis_remap(int rx, int ry, int rz);
    void set_build_volume_max(const Vec3d &max);
    bool has_axis_remap() const;

    // Install the machine frame mapping.  Any axis remap / build volume already
    // configured is carried over, so install order does not matter.
    void set_kinematics(std::unique_ptr<MachineKinematics> kinematics);
    const MachineKinematics& kinematics() const { return *m_kinematics; }

    // Per-point first-layer test.  When set, travel speed selection asks it per
    // destination point (in the writer's logical placed frame) instead of using
    // the layer-coarse m_is_first_layer flag.  GCode installs it on belt printers
    // with the same test its extrusions use (GCode::on_first_layer(point)), so a
    // travel is judged against the belt surface exactly as the path it leads to.
    using FirstLayerPointTest = std::function<bool(const Vec3d &point_logical)>;
    void set_first_layer_point_test(FirstLayerPointTest test) { m_first_layer_point_test = std::move(test); }

    // Force every lift to a plain vertical lift.  Spiral and slope lifts compute
    // their slope in the logical frame and do not account for a machine mapping
    // that couples axes.
    void set_force_normal_lift(bool force) { m_force_normal_lift = force; }

    // Whether extrusions, XY travels and retractions are written now with high precision G-code: high_precision_gcode,
    // unless force_standard_precision() is in effect. Travels that change Z always follow high_precision_gcode, as the
    // Z they set also holds for the moves after them, such as the object printed after its support on the same layer.
    bool high_precision() const { return config.high_precision_gcode.value && !m_force_standard_precision; }
    // Writes the next extrusions, XY travels and retractions with the standard precision. Returns the previous value.
    bool force_standard_precision(bool force) { return std::exchange(m_force_standard_precision, force); }

    // Returns whether this flavor supports separate print and travel acceleration.
    static bool supports_separate_travel_acceleration(GCodeFlavor flavor);
protected:
    // Position/lift/offset state.
    Vec3d           m_pos = Vec3d::Zero();
    double          m_x_offset{ 0 };
    double          m_y_offset{ 0 };
    double          m_lifted;
    double          m_to_lift;
    LiftType        m_to_lift_type;
    bool            m_is_first_layer = true;
    bool            m_is_current_pos_clear = false;
    double          m_current_speed;

    std::string _travel_to_z(double z, const std::string &comment);

    // Whether a destination gets first-layer treatment.  With a point test
    // installed it decides; otherwise the layer-coarse m_is_first_layer flag does.
    bool point_on_first_layer(const Vec3d &point_logical) const;

    // True when a lift must be skipped because this mapping would emit the
    // stored logical X/Y and that position is not yet known.
    bool must_skip_lift_now() const;

    // True when travel speed is selected per destination point rather than per
    // layer. Set for writers that install a first-layer point test. The
    // historical path emits the raw configured travel speed in the final branch
    // of travel_to_xyz(), ignoring the first-layer selection computed at the top
    // of that function; a point-test-driven writer uses the first-layer-aware
    // value throughout. Both are preserved exactly -- unifying them would change
    // emitted feedrates and belongs in its own commit.
    bool uses_pointwise_travel_speed() const { return bool(m_first_layer_point_test); }

    FirstLayerPointTest    m_first_layer_point_test;
    bool                   m_force_normal_lift = false;
    bool                   m_force_standard_precision = false;

    // The machine frame mapping.  Owns the axis-remap state that used to live
    // here as m_remap_* / m_build_vol_max; the setters above forward to it.
    // Never null: a CartesianKinematics at the identity remap reproduces the
    // historical behaviour exactly.
    std::unique_ptr<MachineKinematics> m_kinematics;

    // Last configured remap / build volume, replayed onto a newly installed
    // kinematics so set_kinematics() and the setters are order-independent.
    int             m_remap_x = 0;  // RemapAxis: 0=+X, 1=+Y, 2=+Z, 3=-X, etc.
    int             m_remap_y = 1;
    int             m_remap_z = 2;
    Vec3d           m_build_vol_max = Vec3d::Zero();

    // Apply the machine frame mapping to a point. Returns pos unchanged when the
    // mapping is the identity.
    Vec3d apply_axis_remap(const Vec3d &pos) const;

    // Motion uses the global/base process variant until a filament becomes active.
    // Indexes the per-extruder speed options (travel_speed, travel_speed_z,
    // initial_layer_travel_speed).
    size_t     m_cached_extruder_idx;

private:
	// Extruders are sorted by their ID, so that binary search is possible.
    std::vector<Extruder> m_filament_extruders;
    bool            m_single_extruder_multi_material;
    std::vector<Extruder*> m_curr_filament_extruder;
    int        m_curr_extruder_id;
    unsigned int              m_last_acceleration;
    unsigned int              m_last_travel_acceleration;
    std::vector<unsigned int> m_max_travel_acceleration;

    // Limit for setting the acceleration, to respect the machine limits set for the Marlin firmware.
    // If set to zero, the limit is not in action. Indexed by 0-based physical nozzle id.
    std::vector<unsigned int> m_max_acceleration;
    std::vector<double>       m_max_jerk_x;
    std::vector<double>       m_max_jerk_y;
    double                    m_last_jerk;
    std::vector<double>       m_max_jerk_z;
    std::vector<double>       m_max_jerk_e;
    std::vector<double>       m_max_junction_deviation;

    // unsigned int  m_travel_acceleration;
    // unsigned int  m_travel_jerk;


    //BBS
    int             m_last_bed_temperature;
    bool            m_last_bed_temperature_reached;
    double      m_min_extrusion_z { 0.1 };   // UNLAYERED_MOD, see set_min_extrusion_z()
    // UNLAYERED_MOD — see max_extrusion_z(). Updated only by the extrude_* emitters.
    double          m_max_extrusion_z = 0.;
    // UNLAYERED_MOD — see extruder_offset_xy(). Cached from PrintConfig in apply_print_config
    // because GCodeConfig (this->config) does not carry extruder_offset.
    std::vector<Vec2d> m_extruder_offsets;
    // UNLAYERED_MOD — enable_nonplanar, cached for the lift clamp in lazy_lift()/eager_lift().
    bool            m_nonplanar { false };

    // Orca: slicing resolution in mm
    double          m_resolution = 0.01;
    // Orca: printable area polygons (scaled, bed coordinates) used to keep spiral lifts
    // from colliding with the print boundary. m_extruder_printable_areas holds the
    // per-extruder reachable area (intersected with the bed) when a printer defines
    // different boundaries per extruder; m_bed_printable_area is the global fallback.
    // Storing full polygons (rather than a bounding box) keeps the check correct for
    // non-rectangular beds such as delta/circular printers.
    Polygon              m_bed_printable_area;
    std::vector<Polygon> m_extruder_printable_areas;

    std::string m_gcode_label_objects_start;
    std::string m_gcode_label_objects_end;

    //SoftFever
    bool            m_is_bbl_printers = false;
    // BELT_MOD -- contact-aware first layer: the height above the bed below which a destination
    // gets first-layer travel treatment (set_first_layer_band(); < 0 = the per-layer flag).
    double          m_first_layer_band = -1.;
    // Wave overhangs: see set_travel_speed_override().
    double          m_travel_speed_override = 0.;

    enum class Acceleration {
        Travel,
        Print
    };

    std::string _spiral_travel_to_z(double z, const Vec2d &ij_offset, const std::string &comment);
    // Feedrate (mm/s) for an XY or XYZ travel: the wave-overhang override when one is set, otherwise the
    // active extruder's travel_speed, or initial_layer_travel_speed on the first layer when
    // first_layer_aware (a few emitters historically ignore the first-layer speed; they pass false).
    double      travel_xy_speed(bool first_layer_aware = true, const Vec3d *bed_point = nullptr) const;
    // UNLAYERED_MOD — travel feedrate for a move that also swings B/C. The firmware finishes every
    // axis of a G1 together and interprets F over the linear distance (T = d / F), so bounding the
    // rotary speed to ROT_FEED deg/min means F <= d * ROT_FEED / d_bc.
    double      _clamp_feed_for_rotary(double linear_dist, double bc_angular_dist_deg, double travel_feed) const;
    // Orca: printable area of the active extruder (per-extruder when configured, otherwise the bed). Null when unknown.
    const Polygon *active_printable_area() const;
    // Orca: true if a full spiral-lift circle (center in bed coordinates, mm) fits inside the active printable area.
    bool spiral_lift_fits_printable_area(const Vec2d &center, double radius) const;
    std::string _retract(double length, double restart_extra, const std::string &comment);
    std::string set_acceleration_internal(Acceleration type, unsigned int acceleration);

};

class GCodeFormatter {
public:
    explicit GCodeFormatter(bool high_precision) : m_high_precision(high_precision) {
        this->buf_end = buf + buflen;
        this->ptr_err.ptr = this->buf;
    }

    GCodeFormatter(const GCodeFormatter&) = delete;
    GCodeFormatter& operator=(const GCodeFormatter&) = delete;

    // At extrusion width 0.4mm and layer height 0.15mm, the cross-section of extrusion is 0.4 * 0.15 = 0.06mm2
    // and the cross-section of a 1.75mm filament is 2.405mm2,
    // thus the filament moves 2.405 / 0.06 = 40x slower than the XY axes
    // and we need roughly two decimal digits more on extruder than on XY.
    // High precision G-code adds one digit to both for an order of magnitude smaller extrusion rate errors.
    // Feedrate does not benefit from it and keeps three digits.
    static constexpr int xyz_digits(bool high_precision) { return high_precision ? 4 : 3; }
    static constexpr int e_digits(bool high_precision) { return high_precision ? 6 : 5; }
    static constexpr const int F_EXPORT_DIGITS = 3;
    static constexpr const std::array<double, 10> pow_10 { 1., 10., 100., 1000., 10000., 100000., 1000000., 10000000., 100000000., 1000000000. };
    static constexpr const std::array<double, 10> pow_10_inv { 1. / 1., 1. / 10., 1. / 100., 1. / 1000., 1. / 10000., 1. / 100000., 1. / 1000000., 1. / 10000000., 1. / 100000000., 1. / 1000000000. };

    // Quantize doubles to a resolution of the G-code.
    static double quantize(double v, size_t ndigits) { return std::round(v * pow_10[ndigits]) * pow_10_inv[ndigits]; }
    static double quantize_xyz(double v, bool high_precision) { return quantize(v, xyz_digits(high_precision)); }
    static double quantize_e(double v, bool high_precision) { return quantize(v, e_digits(high_precision)); }

    void emit_axis(const char axis, const double v, size_t digits);

    void emit_xy(const Vec2d &point) {
        this->emit_axis('X', point.x(), xyz_digits(m_high_precision));
        this->emit_axis('Y', point.y(), xyz_digits(m_high_precision));
    }

    void emit_xyz(const Vec3d &point) {
        this->emit_axis('X', point.x(), xyz_digits(m_high_precision));
        this->emit_axis('Y', point.y(), xyz_digits(m_high_precision));
        this->emit_z(point.z());
    }

    void emit_z(const double z) {
        this->emit_axis('Z', z, xyz_digits(m_high_precision));
    }

    void emit_e(double v) {
        this->emit_axis('E', v, e_digits(m_high_precision));
    }

    void emit_f(double speed) {
        this->emit_axis('F', speed, F_EXPORT_DIGITS);
    }
    //BBS
    void emit_ij(const Vec2d &point) {
        this->emit_axis('I', point.x(), xyz_digits(m_high_precision));
        this->emit_axis('J', point.y(), xyz_digits(m_high_precision));
    }

    void emit_string(const std::string &s) {
        strncpy(ptr_err.ptr, s.c_str(), s.size());
        ptr_err.ptr += s.size();
    }

    void emit_comment(bool allow_comments, const std::string &comment) {
        if (allow_comments && ! comment.empty()) {
            *ptr_err.ptr ++ = ' '; *ptr_err.ptr ++ = ';'; *ptr_err.ptr ++ = ' ';
            this->emit_string(comment);
        }
    }

    std::string string() {
        std::string out;
        this->append_to(out);
        return out;
    }

    void append_to(std::string &out) {
        *ptr_err.ptr ++ = '\n';
        out.append(this->buf, ptr_err.ptr - buf);
    }

protected:
    const bool                      m_high_precision;
    static constexpr const size_t   buflen = 256;
    char                            buf[buflen];
    char* buf_end;
    std::to_chars_result            ptr_err;
};

class GCodeG1Formatter : public GCodeFormatter {
public:
    explicit GCodeG1Formatter(bool high_precision) : GCodeFormatter(high_precision) {
        this->buf[0] = 'G';
        this->buf[1] = '1';
        this->buf_end = buf + buflen;
        this->ptr_err.ptr = this->buf + 2;
    }

    GCodeG1Formatter(const GCodeG1Formatter&) = delete;
    GCodeG1Formatter& operator=(const GCodeG1Formatter&) = delete;
};

// UNLAYERED_MOD — G0 rapid, same emission as GCodeG1Formatter.
class GCodeG0Formatter : public GCodeFormatter {
public:
    explicit GCodeG0Formatter(bool high_precision) : GCodeFormatter(high_precision) {
        this->buf[0] = 'G';
        this->buf[1] = '0';
        this->buf_end = buf + buflen;
        this->ptr_err.ptr = this->buf + 2;
    }

    GCodeG0Formatter(const GCodeG0Formatter&) = delete;
    GCodeG0Formatter& operator=(const GCodeG0Formatter&) = delete;
};

class GCodeG2G3Formatter : public GCodeFormatter {
public:
    GCodeG2G3Formatter(bool is_ccw, bool high_precision) : GCodeFormatter(high_precision) {
        this->buf[0] = 'G';
        this->buf[1] = is_ccw ? '3' : '2';
        this->buf_end = buf + buflen;
        this->ptr_err.ptr = this->buf + 2;
    }

    GCodeG2G3Formatter(const GCodeG2G3Formatter&) = delete;
    GCodeG2G3Formatter& operator=(const GCodeG2G3Formatter&) = delete;
};

} /* namespace Slic3r */

#endif /* slic3r_GCodeWriter_hpp_ */
