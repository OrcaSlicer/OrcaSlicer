#pragma once
#include <string>
#define calib_pressure_advance_dd

#include "GCodeWriter.hpp"
#include "PrintConfig.hpp"
#include "BoundingBox.hpp"
#include "CustomGCode.hpp"

namespace Slic3r {

class GCode;
class Model;
class ModelObject;

enum class CalibMode : int {
    Calib_None = 0,
    Calib_PA_Line,
    Calib_PA_Pattern,
    Calib_PA_Tower,
    Calib_Auto_PA_Line,
    Calib_Flow_Rate,
    Calib_Temp_Tower,
    Calib_Vol_speed_Tower,
    Calib_VFA_Tower,
    Calib_Retraction_tower,
    Calib_Input_shaping_freq,
    Calib_Input_shaping_damp,
    Calib_Cornering
};

enum class CalibState { Start = 0, Preset, Calibration, CoarseSave, FineCalibration, Save, Finish };

struct Calib_Params
{
    Calib_Params() : mode(CalibMode::Calib_None){};
    int extruder_id = 0;
    double    start, end, step;
    bool      print_numbers;
    double freqStartX, freqEndX, freqStartY, freqEndY;
    int test_model;
    std::string shaper_type;
    std::vector<double> accelerations;
    std::vector<double> speeds;

    CalibMode mode;
};

enum FlowRatioCalibrationType {
    COMPLETE_CALIBRATION = 0,
    FINE_CALIBRATION,
};

class X1CCalibInfos
{
public:
    struct X1CCalibInfo
    {
        int         extruder_id = 0;
        int         tray_id;
        int         ams_id = 0;
        int         slot_id = 0;
        int         bed_temp;
        ExtruderType        extruder_type{ExtruderType::etDirectDrive};
        NozzleVolumeType    nozzle_volume_type = NozzleVolumeType::nvtStandard;
        int         nozzle_temp;
        float       nozzle_diameter;
        std::string filament_id;
        std::string setting_id;
        float       max_volumetric_speed;
        float       flow_rate = 0.98f; // for flow ratio
    };

    std::vector<X1CCalibInfo> calib_datas;
    CalibMode                 cali_mode{ CalibMode::Calib_None };
};

class CaliPresetInfo
{
public:
    int         tray_id;
    int         extruder_id;
    NozzleVolumeType nozzle_volume_type;
    BedType     bed_type;
    float       nozzle_diameter;
    std::string filament_id;
    std::string setting_id;
    std::string name;

    CaliPresetInfo &operator=(const CaliPresetInfo &other)
    {
        this->tray_id         = other.tray_id;
        this->extruder_id     = other.extruder_id;
        this->nozzle_volume_type = other.nozzle_volume_type;
        this->nozzle_diameter = other.nozzle_diameter;
        this->filament_id     = other.filament_id;
        this->setting_id      = other.setting_id;
        this->name            = other.name;
        return *this;
    }
};

struct PrinterCaliInfo
{
    std::string                 dev_id;
    bool                        cali_finished = true;
    float                       cache_flow_ratio;
    std::vector<CaliPresetInfo> selected_presets;
    FlowRatioCalibrationType    cache_flow_rate_calibration_type = FlowRatioCalibrationType::COMPLETE_CALIBRATION;
};

class PACalibResult
{
public:
    enum CalibResult {
        CALI_RESULT_SUCCESS = 0,
        CALI_RESULT_PROBLEM = 1,
        CALI_RESULT_FAILED  = 2,
    };
    int         extruder_id = 0;
    NozzleVolumeType nozzle_volume_type;
    int         tray_id = 0;
    int         ams_id = 0;
    int         slot_id = 0;
    int         cali_idx = -1;
    float       nozzle_diameter;
    std::string filament_id;
    std::string setting_id;
    std::string name;
    float       k_value    = 0.0;
    float       n_coef     = 0.0;
    int         confidence = -1; // 0: success  1: uncertain  2: failed
};

struct PACalibIndexInfo
{
    int         extruder_id = 0;
    NozzleVolumeType nozzle_volume_type;
    int         tray_id = 0;
    int         ams_id = 0;
    int         slot_id = 0;
    int         cali_idx = -1; // -1 means default
    float       nozzle_diameter;
    std::string filament_id;
};

struct PACalibExtruderInfo
{
    int              extruder_id = 0;
    NozzleVolumeType nozzle_volume_type;
    float            nozzle_diameter;
    std::string      filament_id = "";
    bool             use_extruder_id{true};
    bool             use_nozzle_volume_type{true};
};

struct PACalibTabInfo
{
    float pa_calib_tab_nozzle_dia;
    int   extruder_id;
    NozzleVolumeType nozzle_volume_type;
};

class FlowRatioCalibResult
{
public:
    int         tray_id;
    float       nozzle_diameter;
    std::string filament_id;
    std::string setting_id;
    float       flow_ratio;
    int         confidence; // 0: success  1: uncertain  2: failed
};

struct DrawBoxOptArgs
{
    DrawBoxOptArgs(int num_perimeters, double height, double line_width, double speed)
        : num_perimeters{num_perimeters}, height{height}, line_width{line_width}, speed{speed} {};
    DrawBoxOptArgs() = default;

    bool   is_filled{false};
    int    num_perimeters;
    double height;
    double line_width;
    double speed;
};
class CalibPressureAdvance
{
public:
    static float find_optimal_PA_speed(const DynamicPrintConfig &config, double line_width, double layer_height, int extruder_id = 0, int filament_idx = 0);

protected:
    CalibPressureAdvance()  = default;
    CalibPressureAdvance(const DynamicPrintConfig& config) : m_config(config){};
    CalibPressureAdvance(const FullPrintConfig &config) { m_config.apply(config); };
    ~CalibPressureAdvance() = default;

    enum class DrawDigitMode { Left_To_Right, Bottom_To_Top };

    void delta_scale_bed_ext(BoundingBoxf &bed_ext) const { bed_ext.scale(1.0f / 1.41421f); }

    std::string move_to(Vec2d pt, GCodeWriter &writer, std::string comment = std::string(), double z = 0, double layer_height = -1);
    double e_per_mm(double line_width, double layer_height, float nozzle_diameter, float filament_diameter, float print_flow_ratio) const;
    double speed_adjust(int speed) const { return speed * 60; };

    std::string convert_number_to_string(double num, unsigned precision = 0) const;
    double      number_spacing() const { return m_digit_segment_len + m_digit_gap_len; };
    std::string draw_digit(double                              startx,
                           double                              starty,
                           char                                c,
                           CalibPressureAdvance::DrawDigitMode mode,
                           double                              line_width,
                           double                              e_per_mm,
                           GCodeWriter                        &writer);
    std::string draw_number(double                              startx,
                            double                              starty,
                            double                              value,
                            CalibPressureAdvance::DrawDigitMode mode,
                            double                              line_width,
                            double                              e_per_mm,
                            double                              speed,
                            GCodeWriter                        &writer);

    std::string draw_line(
        GCodeWriter &writer, Vec2d to_pt, double line_width, double layer_height, double speed, const std::string &comment = std::string());
    std::string draw_box(GCodeWriter &writer, double min_x, double min_y, double size_x, double size_y, DrawBoxOptArgs opt_args);

    double to_radians(double degrees) const { return degrees * M_PI / 180; };
    double get_distance(Vec2d from, Vec2d to) const;

    Vec3d m_last_pos;
    DynamicPrintConfig m_config;

    const double m_encroachment{1. / 3.};
    DrawDigitMode                m_draw_digit_mode{DrawDigitMode::Left_To_Right};
    const double                 m_digit_segment_len{2};
    const double                 m_digit_gap_len{1};
    const std::string::size_type m_max_number_len{5};
    std::string::size_type       m_number_len{m_max_number_len}; /* Current length of number labels */
};

class CalibPressureAdvanceLine : public CalibPressureAdvance
{
public:
    CalibPressureAdvanceLine(GCode* gcodegen);
    ~CalibPressureAdvanceLine(){};

    std::string generate_test(double start_pa = 0, double step_pa = 0.002, int count = 50);

    void set_speed(double fast = 100.0, double slow = 20.0)
    {
        m_slow_speed = slow;
        m_fast_speed = fast;
    }

    const double &line_width() { return m_line_width; };
    const double &height_layer() { return m_height_layer; };
    bool          is_delta() const;
    bool         &draw_numbers() { return m_draw_numbers; }

private:
    std::string print_pa_lines(double start_x, double start_y, double start_pa, double step_pa, int num);

    void delta_modify_start(double &startx, double &starty, int count);

    GCode *mp_gcodegen;

    double m_nozzle_diameter;
    double m_slow_speed, m_fast_speed;

    double m_height_layer{0.2};
    double m_line_width{0.6};
    double m_thin_line_width{0.44};
    double m_number_line_width{0.48};
    const double m_space_y{3.5};

    double m_length_short{20.0}, m_length_long{40.0};
    bool   m_draw_numbers{true};
};

struct SuggestedConfigCalibPAPattern
{
    const std::vector<std::pair<std::string, double>> float_pairs{{"initial_layer_print_height", 0.25},
                                                                  {"layer_height", 0.2},
                                                                  {"initial_layer_speed", 30}};

    const std::vector<std::pair<std::string, double>> nozzle_ratio_pairs{{"line_width", 112.5}, {"initial_layer_line_width", 140}};

    const std::vector<std::pair<std::string, int>> int_pairs{{"skirt_loops", 0}, {"wall_loops", 3}};

    const std::pair<std::string, BrimType> brim_pair{"brim_type", BrimType::btNoBrim};
};

class CalibPressureAdvancePattern : public CalibPressureAdvance
{
    friend struct DrawBoxOptArgs;

public:
    CalibPressureAdvancePattern(
        const Calib_Params &params, const DynamicPrintConfig &config, bool is_bbl_machine, const ModelObject &object, const Vec3d &origin);

    double handle_xy_size() const { return m_handle_xy_size; };
    double handle_spacing() const { return m_handle_spacing; };
    Vec3d handle_pos_offset() const;
    double print_size_x() const { return object_size_x() + pattern_shift(); };
    double print_size_y() const { return object_size_y(); };
    double max_layer_z() const { return height_first_layer() + ((m_num_layers - 1) * height_layer()); };
    double flow_val() const;

    CustomGCode::Info generate_custom_gcodes(const DynamicPrintConfig &config, bool is_bbl_machine, const ModelObject &object, const Vec3d &origin);

    void set_start_offset(const Vec3d &offset);
    Vec3d get_start_offset();

protected:
    // todo multi_extruders:
    double speed_first_layer() const { return m_config.option<ConfigOptionFloat>("initial_layer_speed")->value; };
    double speed_perimeter() const { return m_config.option<ConfigOptionFloat>("outer_wall_speed")->value; };
    double accel_perimeter() const { return m_config.option<ConfigOptionFloat>("outer_wall_acceleration")->value; }
    double line_width_first_layer() const
    {
        // TODO: FIXME: find out current filament/extruder?
        const double nozzle_diameter = m_config.opt_float("nozzle_diameter", 0);
        return m_config.get_abs_value("initial_layer_line_width", nozzle_diameter);
    };
    double line_width() const;
    int    wall_count() const { return m_config.option<ConfigOptionInt>("wall_loops")->value; };

private:
    void refresh_setup(const DynamicPrintConfig &config, bool is_bbl_machine, const ModelObject &object, const Vec3d &origin);
    void _refresh_starting_point(const ModelObject &object);
    void _refresh_writer(bool is_bbl_machine, const ModelObject &object, const Vec3d &origin);

    double    height_first_layer() const { return m_config.option<ConfigOptionFloat>("initial_layer_print_height")->value; };
    double    height_z_offset() const { return m_config.option<ConfigOptionFloat>("z_offset")->value; };
    double    height_layer() const { return m_config.option<ConfigOptionFloat>("layer_height")->value; };
    const int get_num_patterns() const { return std::ceil((m_params.end - m_params.start) / m_params.step + 1); }

    /*
    from slic3r documentation: spacing = extrusion_width - layer_height * (1 - PI/4)
    "spacing" = center-to-center distance of adjacent extrusions, which partially overlap
        https://manual.slic3r.org/advanced/flow-math
        https://ellis3dp.com/Print-Tuning-Guide/articles/misconceptions.html#two-04mm-perimeters--08mm
    */
    double line_spacing() const { return line_width() - height_layer() * (1 - M_PI / 4); };
    double line_spacing_first_layer() const { return line_width_first_layer() - height_first_layer() * (1 - M_PI / 4); };
    double line_spacing_angle() const { return line_spacing() / std::sin(to_radians(m_corner_angle) / 2); };

    double object_size_x() const;
    double object_size_y() const;
    double frame_size_y() const { return std::sin(to_radians(double(m_corner_angle) / 2)) * m_wall_side_length * 2; };

    double glyph_start_x(int pattern_i = 0) const;
    double glyph_length_x() const;
    double glyph_tab_max_x() const;
    double max_numbering_height() const;
    size_t max_numbering_length() const;

    double pattern_shift() const;

    const Calib_Params &m_params;

    GCodeWriter        m_writer;
    Vec3d              m_starting_point;
    bool               m_is_start_point_fixed = false;

    const double m_handle_xy_size{5};
    const double m_handle_spacing{1.2};
    const int    m_num_layers{4};

    const double m_wall_side_length{30.0};
    const int    m_corner_angle{90};
    const int    m_pattern_spacing{2};

    const double m_glyph_padding_horizontal{1};
    const double m_glyph_padding_vertical{1};
};

} // namespace Slic3r