#pragma once
#include "Point.hpp"
#include <math.h>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>
#include <utility>
#include <memory>
#define calib_pressure_advance_dd

#include "GCodeWriter.hpp"
#include "CalibData.hpp"
#include "PrintConfig.hpp"
#include "BoundingBox.hpp"
#include "CustomGCode.hpp"

namespace Slic3r {

class GCode;
class Model;
class ModelObject;

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
    double e_per_mm{0.}; // 0: extrude with the first filament's settings
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

    std::string draw_line(GCodeWriter       &writer,
                          Vec2d              to_pt,
                          double             line_width,
                          double             layer_height,
                          double             speed,
                          const std::string &comment  = std::string(),
                          double             e_per_mm = 0.);
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
    // Return the X‑bounds of the pattern on the given bed.
    BoundingBoxf print_extents(const BoundingBoxf &bed_ext) const;

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
    const std::vector<std::pair<std::string, std::vector<double>>> floats_pairs{{"initial_layer_speed", {30}}};

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
    double speed_first_layer() const { return m_config.get_abs_value_at("initial_layer_speed", m_params.extruder_id); };
    double speed_perimeter() const { return m_config.get_abs_value_at("outer_wall_speed", m_params.extruder_id); };
    double accel_perimeter() const { return m_config.get_abs_value_at("outer_wall_acceleration", m_params.extruder_id); }
    double line_width_first_layer() const;
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

    // Belt printers get belt kinematics installed on it (_refresh_writer);
    // shared_ptr keeps the class copyable — the writer is rebuilt by
    // refresh_setup() before every use anyway.
    std::shared_ptr<GCodeWriter> m_writer{std::make_shared<GCodeWriter>()};
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
