#include "calib.hpp"
#include "BoundingBox.hpp"
#include "Config.hpp"
#include "Model.hpp"
#include "GCode.hpp"
#include <cmath>

namespace Slic3r {

// Calculate the optimal Pressure Advance speed
float CalibPressureAdvance::find_optimal_PA_speed(const DynamicPrintConfig &config, double line_width, double layer_height, int extruder_id, int filament_idx)
{
    const double general_suggested_min_speed   = 100.0;
    double       filament_max_volumetric_speed = config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(filament_idx);
    // todo multi_extruders:
    const float  nozzle_diameter               = config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(extruder_id);
    if (line_width <= 0.) line_width = Flow::auto_extrusion_width(frPerimeter, nozzle_diameter);
    Flow         pattern_line = Flow(line_width, layer_height, nozzle_diameter);
    auto         pa_speed     = std::min(std::max(general_suggested_min_speed, config.option<ConfigOptionFloat>("outer_wall_speed")->value),
                                         filament_max_volumetric_speed / pattern_line.mm3_per_mm());

    return std::floor(pa_speed);
}

std::string CalibPressureAdvance::move_to(Vec2d pt, GCodeWriter &writer, std::string comment, double z, double layer_height)
{
    std::stringstream gcode;

    gcode << writer.retract(); // retract before z move or move
    if(z > EPSILON && layer_height >= 0){
        gcode << writer.travel_to_z(z, "z-hop"); // Perform z hop
        gcode << writer.travel_to_xy(pt, comment); // Travel with z move
        gcode << writer.travel_to_z(layer_height, "undo z-hop"); // Undo z hop
    }else {
        gcode << writer.travel_to_xy(pt, comment);
    }
    gcode << writer.unretract(); // unretract after z move is complete

    m_last_pos = Vec3d(pt.x(), pt.y(), 0);

    return gcode.str();
}

double CalibPressureAdvance::e_per_mm(
    double line_width, double layer_height, float nozzle_diameter, float filament_diameter, float print_flow_ratio) const
{
    const Flow   line_flow     = Flow(line_width, layer_height, nozzle_diameter);
    const double filament_area = M_PI * std::pow(filament_diameter / 2, 2);

    return line_flow.mm3_per_mm() * print_flow_ratio / filament_area ;
}

std::string CalibPressureAdvance::convert_number_to_string(double num, unsigned int precision) const
{
    std::ostringstream stream;

    if (precision) {
        /* if number is > 1000 then there are no way we'll fit fractional part into 5 glyphs, so
         * in this case we keep full precision.
         * Otherwise we reduce precision by 1 to accomodate decimal separator */
        stream << std::setprecision(num >= 1000 ? precision : precision - 1);
    }

    stream << num;

    return stream.str();
}

std::string CalibPressureAdvance::draw_digit(
    double startx, double starty, char c, CalibPressureAdvance::DrawDigitMode mode, double line_width, double e_per_mm, GCodeWriter &writer)
{
    const double len = m_digit_segment_len;
    const double gap = line_width / 2.0;

    const auto dE     = e_per_mm * len;
    const auto two_dE = dE * 2;

    Vec2d p0, p1, p2, p3, p4, p5;
    Vec2d p0_5, p4_5;
    Vec2d gap_p0_toward_p3, gap_p2_toward_p3;
    Vec2d dot_direction;

    if (mode == CalibPressureAdvance::DrawDigitMode::Bottom_To_Top) {
        //  1-------2-------5
        //  |       |       |
        //  |       |       |
        //  0-------3-------4
        p0   = Vec2d(startx, starty);
        p0_5 = Vec2d(startx, starty + len / 2);
        p1   = Vec2d(startx, starty + len);
        p2   = Vec2d(startx + len, starty + len);
        p3   = Vec2d(startx + len, starty);
        p4   = Vec2d(startx + len * 2, starty);
        p4_5 = Vec2d(startx + len * 2, starty + len / 2);
        p5   = Vec2d(startx + len * 2, starty + len);

        gap_p0_toward_p3 = p0 + Vec2d(gap, 0);
        gap_p2_toward_p3 = p2 + Vec2d(0, gap);

        dot_direction = Vec2d(-len / 2, 0);
    } else {
        //  0-------1
        //  |       |
        //  3-------2
        //  |       |
        //  4-------5
        p0   = Vec2d(startx, starty);
        p0_5 = Vec2d(startx + len / 2, starty);
        p1   = Vec2d(startx + len, starty);
        p2   = Vec2d(startx + len, starty - len);
        p3   = Vec2d(startx, starty - len);
        p4   = Vec2d(startx, starty - len * 2);
        p4_5 = Vec2d(startx + len / 2, starty - len * 2);
        p5   = Vec2d(startx + len, starty - len * 2);

        gap_p0_toward_p3 = p0 - Vec2d(0, gap);
        gap_p2_toward_p3 = p2 - Vec2d(gap, 0);

        dot_direction = Vec2d(0, len / 2);
    }

    std::stringstream gcode;

    switch (c) {
    case '0':
        gcode << move_to(p0, writer, "Glyph: 0");
        gcode << writer.extrude_to_xy(p1, dE);
        gcode << writer.extrude_to_xy(p5, two_dE);
        gcode << writer.extrude_to_xy(p4, dE);
        gcode << writer.extrude_to_xy(gap_p0_toward_p3, two_dE);
        break;
    case '1':
        gcode << move_to(p0_5, writer, "Glyph: 1");
        gcode << writer.extrude_to_xy(p4_5, two_dE);
        break;
    case '2':
        gcode << move_to(p0, writer, "Glyph: 2");
        gcode << writer.extrude_to_xy(p1, dE);
        gcode << writer.extrude_to_xy(p2, dE);
        gcode << writer.extrude_to_xy(p3, dE);
        gcode << writer.extrude_to_xy(p4, dE);
        gcode << writer.extrude_to_xy(p5, dE);
        break;
    case '3':
        gcode << move_to(p0, writer, "Glyph: 3");
        gcode << writer.extrude_to_xy(p1, dE);
        gcode << writer.extrude_to_xy(p5, two_dE);
        gcode << writer.extrude_to_xy(p4, dE);
        gcode << move_to(gap_p2_toward_p3, writer);
        gcode << writer.extrude_to_xy(p3, dE);
        break;
    case '4':
        gcode << move_to(p0, writer, "Glyph: 4");
        gcode << writer.extrude_to_xy(p3, dE);
        gcode << writer.extrude_to_xy(p2, dE);
        gcode << move_to(p1, writer);
        gcode << writer.extrude_to_xy(p5, two_dE);
        break;
    case '5':
        gcode << move_to(p1, writer, "Glyph: 5");
        gcode << writer.extrude_to_xy(p0, dE);
        gcode << writer.extrude_to_xy(p3, dE);
        gcode << writer.extrude_to_xy(p2, dE);
        gcode << writer.extrude_to_xy(p5, dE);
        gcode << writer.extrude_to_xy(p4, dE);
        break;
    case '6':
        gcode << move_to(p1, writer, "Glyph: 6");
        gcode << writer.extrude_to_xy(p0, dE);
        gcode << writer.extrude_to_xy(p4, two_dE);
        gcode << writer.extrude_to_xy(p5, dE);
        gcode << writer.extrude_to_xy(p2, dE);
        gcode << writer.extrude_to_xy(p3, dE);
        break;
    case '7':
        gcode << move_to(p0, writer, "Glyph: 7");
        gcode << writer.extrude_to_xy(p1, dE);
        gcode << writer.extrude_to_xy(p5, two_dE);
        break;
    case '8':
        gcode << move_to(p2, writer, "Glyph: 8");
        gcode << writer.extrude_to_xy(p3, dE);
        gcode << writer.extrude_to_xy(p4, dE);
        gcode << writer.extrude_to_xy(p5, dE);
        gcode << writer.extrude_to_xy(p1, two_dE);
        gcode << writer.extrude_to_xy(p0, dE);
        gcode << writer.extrude_to_xy(p3, dE);
        break;
    case '9':
        gcode << move_to(p5, writer, "Glyph: 9");
        gcode << writer.extrude_to_xy(p1, two_dE);
        gcode << writer.extrude_to_xy(p0, dE);
        gcode << writer.extrude_to_xy(p3, dE);
        gcode << writer.extrude_to_xy(p2, dE);
        break;
    case '.':
        gcode << move_to(p4_5, writer, "Glyph: .");
        gcode << writer.extrude_to_xy(p4_5 + dot_direction, dE);
        break;
    default: break;
    }

    return gcode.str();
}

std::string CalibPressureAdvance::draw_number(double                              startx,
                                              double                              starty,
                                              double                              value,
                                              CalibPressureAdvance::DrawDigitMode mode,
                                              double                              line_width,
                                              double                              e_per_mm,
                                              double                              speed,
                                              GCodeWriter                        &writer)
{
    auto              sNumber = convert_number_to_string(value, m_number_len);
    std::stringstream gcode;
    gcode << writer.set_speed(speed);

    for (std::string::size_type i = 0; i < sNumber.length(); ++i) {
        if (i >= m_number_len) {
            break;
        }
        switch (mode) {
        case DrawDigitMode::Bottom_To_Top:
            gcode << draw_digit(startx, starty + i * number_spacing(), sNumber[i], mode, line_width, e_per_mm, writer);
            break;
        default: gcode << draw_digit(startx + i * number_spacing(), starty, sNumber[i], mode, line_width, e_per_mm, writer);
        }
    }

    return gcode.str();
}


double CalibPressureAdvance::get_distance(Vec2d from, Vec2d to) const
{
    return std::hypot((to.x() - from.x()), (to.y() - from.y()));
}

std::string CalibPressureAdvance::draw_line(
    GCodeWriter &writer, Vec2d to_pt, double line_width, double layer_height, double speed, const std::string &comment)
{
    const double e_per_mm = CalibPressureAdvance::e_per_mm(line_width, layer_height,
                                                           m_config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0),
                                                           m_config.option<ConfigOptionFloats>("filament_diameter")->get_at(0),
                                                           m_config.option<ConfigOptionFloatsNullable>("filament_flow_ratio")->get_at(0));

    const double length = get_distance(Vec2d(m_last_pos.x(), m_last_pos.y()), to_pt);
    auto         dE     = e_per_mm * length;

    std::stringstream gcode;

    gcode << writer.set_speed(speed);
    gcode << writer.extrude_to_xy(to_pt, dE, comment);

    m_last_pos = Vec3d(to_pt.x(), to_pt.y(), 0);

    return gcode.str();
}

std::string CalibPressureAdvance::draw_box(GCodeWriter &writer, double min_x, double min_y, double size_x, double size_y, DrawBoxOptArgs opt_args)
{
    std::stringstream gcode;

    double       x     = min_x;
    double       y     = min_y;
    const double max_x = min_x + size_x;
    const double max_y = min_y + size_y;

    const double spacing = opt_args.line_width - opt_args.height * (1 - M_PI / 4);

    // if number of perims exceeds size of box, reduce it to max
    const int max_perimeters = std::min(
        // this is the equivalent of number of perims for concentric fill
        std::floor(size_x * std::sin(to_radians(45))) / (spacing / std::sin(to_radians(45))),
        std::floor(size_y * std::sin(to_radians(45))) / (spacing / std::sin(to_radians(45))));

    opt_args.num_perimeters = std::min(opt_args.num_perimeters, max_perimeters);

    gcode << move_to(Vec2d(min_x, min_y), writer, "Move to box start");

    // DrawLineOptArgs line_opt_args(*this);
    auto line_arg_height     = opt_args.height;
    auto line_arg_line_width = opt_args.line_width;
    auto line_arg_speed      = opt_args.speed;
    std::string comment = "";

    for (int i = 0; i < opt_args.num_perimeters; ++i) {
        if (i != 0) { // after first perimeter, step inwards to start next perimeter
            x += spacing;
            y += spacing;
            gcode << move_to(Vec2d(x, y), writer, "Step inwards to print next perimeter");
        }

        y += size_y - i * spacing * 2;
        comment = "Draw perimeter (up)";
        gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);

        x += size_x - i * spacing * 2;
        comment = "Draw perimeter (right)";
        gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);

        y -= size_y - i * spacing * 2;
        comment = "Draw perimeter (down)";
        gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);

        x -= size_x - i * spacing * 2;
        comment = "Draw perimeter (left)";
        gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);
    }

    if (!opt_args.is_filled) {
        return gcode.str();
    }

    // create box infill
    const double spacing_45 = spacing / std::sin(to_radians(45));

    const double bound_modifier = (spacing * (opt_args.num_perimeters - 1)) + (opt_args.line_width * (1 - m_encroachment));
    const double x_min_bound    = min_x + bound_modifier;
    const double x_max_bound    = max_x - bound_modifier;
    const double y_min_bound    = min_y + bound_modifier;
    const double y_max_bound    = max_y - bound_modifier;
    const int    x_count        = std::floor((x_max_bound - x_min_bound) / spacing_45);
    const int    y_count        = std::floor((y_max_bound - y_min_bound) / spacing_45);

    double x_remainder = std::fmod((x_max_bound - x_min_bound), spacing_45);
    double y_remainder = std::fmod((y_max_bound - y_min_bound), spacing_45);

    x = x_min_bound;
    y = y_min_bound;

    gcode << move_to(Vec2d(x, y), writer, "Move to fill start");

    for (int i = 0; i < x_count + y_count + (x_remainder + y_remainder >= spacing_45 ? 1 : 0);
         ++i) { // this isn't the most robust way, but less expensive than finding line intersections
        if (i < std::min(x_count, y_count)) {
            if (i % 2 == 0) {
                x += spacing_45;
                y = y_min_bound;
                gcode << move_to(Vec2d(x, y), writer, "Fill: Step right");

                y += x - x_min_bound;
                x                     = x_min_bound;
                comment = "Fill: Print up/left";
                gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);
            } else {
                y += spacing_45;
                x = x_min_bound;
                gcode << move_to(Vec2d(x, y), writer, "Fill: Step up");

                x += y - y_min_bound;
                y                     = y_min_bound;
                comment = "Fill: Print down/right";
                gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);
            }
        } else if (i < std::max(x_count, y_count)) {
            if (x_count > y_count) {
                // box is wider than tall
                if (i % 2 == 0) {
                    x += spacing_45;
                    y = y_min_bound;
                    gcode << move_to(Vec2d(x, y), writer, "Fill: Step right");

                    x -= y_max_bound - y_min_bound;
                    y                     = y_max_bound;
                    comment = "Fill: Print up/left";
                    gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);
                } else {
                    if (i == y_count) {
                        x += spacing_45 - y_remainder;
                        y_remainder = 0;
                    } else {
                        x += spacing_45;
                    }
                    y = y_max_bound;
                    gcode << move_to(Vec2d(x, y), writer, "Fill: Step right");

                    x += y_max_bound - y_min_bound;
                    y                     = y_min_bound;
                    comment = "Fill: Print down/right";
                    gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);
                }
            } else {
                // box is taller than wide
                if (i % 2 == 0) {
                    x = x_max_bound;
                    if (i == x_count) {
                        y += spacing_45 - x_remainder;
                        x_remainder = 0;
                    } else {
                        y += spacing_45;
                    }
                    gcode << move_to(Vec2d(x, y), writer, "Fill: Step up");

                    x = x_min_bound;
                    y += x_max_bound - x_min_bound;
                    comment = "Fill: Print up/left";
                    gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);
                } else {
                    x = x_min_bound;
                    y += spacing_45;
                    gcode << move_to(Vec2d(x, y), writer, "Fill: Step up");

                    x = x_max_bound;
                    y -= x_max_bound - x_min_bound;
                    comment = "Fill: Print down/right";
                    gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);
                }
            }
        } else {
            if (i % 2 == 0) {
                x = x_max_bound;
                if (i == x_count) {
                    y += spacing_45 - x_remainder;
                } else {
                    y += spacing_45;
                }
                gcode << move_to(Vec2d(x, y), writer, "Fill: Step up");

                x -= y_max_bound - y;
                y                     = y_max_bound;
                comment = "Fill: Print up/left";
                gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);
            } else {
                if (i == y_count) {
                    x += spacing_45 - y_remainder;
                } else {
                    x += spacing_45;
                }
                y = y_max_bound;
                gcode << move_to(Vec2d(x, y), writer, "Fill: Step right");

                y -= x_max_bound - x;
                x                     = x_max_bound;
                comment = "Fill: Print down/right";
                gcode << draw_line(writer, Vec2d(x, y), line_arg_line_width, line_arg_height, line_arg_speed, comment);
            }
        }
    }

    return gcode.str();
}
CalibPressureAdvanceLine::CalibPressureAdvanceLine(GCode* gcodegen)
    : CalibPressureAdvance(gcodegen->config()), mp_gcodegen(gcodegen), m_nozzle_diameter(gcodegen->config().nozzle_diameter.get_at(0))
{
    m_line_width        = m_nozzle_diameter < 0.51 ? m_nozzle_diameter * 1.5 : m_nozzle_diameter * 1.05;
    m_height_layer      = gcodegen->config().initial_layer_print_height;
    m_number_line_width = m_thin_line_width = m_nozzle_diameter;
};

std::string CalibPressureAdvanceLine::generate_test(double start_pa /*= 0*/, double step_pa /*= 0.002*/, int count /*= 10*/)
{
    BoundingBoxf bed_ext = get_extents(mp_gcodegen->config().printable_area.values);
    if (is_delta()) {
        CalibPressureAdvanceLine::delta_scale_bed_ext(bed_ext);
    }

    auto        bed_sizes = mp_gcodegen->config().printable_area.values;
    const auto  w         = bed_ext.size().x();
    const auto  h         = bed_ext.size().y();
    count                 = std::min(count, int((h - 10) / m_space_y));

    m_length_long = 40 + std::min(w - 120.0, 0.0);

    auto startx = bed_ext.min.x() + (w - m_length_short * 2 - m_length_long - 20) / 2;
    auto starty = bed_ext.min.y() + (h - count * m_space_y) / 2;

    return print_pa_lines(startx, starty, start_pa, step_pa, count);
}

bool CalibPressureAdvanceLine::is_delta() const { return mp_gcodegen->config().printable_area.values.size() > 4; }

std::string CalibPressureAdvanceLine::print_pa_lines(double start_x, double start_y, double start_pa, double step_pa, int num)
{
    auto       &writer = mp_gcodegen->writer();
    const auto &config = mp_gcodegen->config();

    const auto filament_diameter = config.filament_diameter.get_at(0);
    const auto print_flow_ratio  = config.print_flow_ratio;
    const auto z_offset          = config.z_offset;

    const double e_per_mm        = CalibPressureAdvance::e_per_mm(m_line_width, m_height_layer, m_nozzle_diameter, filament_diameter,
                                                                  print_flow_ratio);
    const double thin_e_per_mm   = CalibPressureAdvance::e_per_mm(m_thin_line_width, m_height_layer, m_nozzle_diameter, filament_diameter,
                                                                  print_flow_ratio);
    const double number_e_per_mm = CalibPressureAdvance::e_per_mm(m_number_line_width, m_height_layer, m_nozzle_diameter, filament_diameter,
                                                                  print_flow_ratio);

    const double      fast = CalibPressureAdvance::speed_adjust(m_fast_speed);
    const double      slow = CalibPressureAdvance::speed_adjust(m_slow_speed);
    std::stringstream gcode;
    gcode << mp_gcodegen->writer().travel_to_z(m_height_layer + z_offset);
    double y_pos = start_y;

    // prime line
    gcode << writer.set_pressure_advance(0.0);
    auto prime_x = start_x;
    gcode << move_to(Vec2d(prime_x, y_pos + (num) * m_space_y), writer);
    gcode << writer.set_speed(slow);
    gcode << writer.extrude_to_xy(Vec2d(prime_x, y_pos), e_per_mm * m_space_y * num * 1.2);

    for (int i = 0; i < num; ++i) {
        gcode << writer.set_pressure_advance(start_pa + i * step_pa);
        gcode << move_to(Vec2d(start_x, y_pos + i * m_space_y), writer);
        gcode << writer.set_speed(slow);
        gcode << writer.extrude_to_xy(Vec2d(start_x + m_length_short, y_pos + i * m_space_y), e_per_mm * m_length_short);
        gcode << writer.set_speed(fast);
        gcode << writer.extrude_to_xy(Vec2d(start_x + m_length_short + m_length_long, y_pos + i * m_space_y), e_per_mm * m_length_long);
        gcode << writer.set_speed(slow);
        gcode << writer.extrude_to_xy(Vec2d(start_x + m_length_short + m_length_long + m_length_short, y_pos + i * m_space_y),
                                      e_per_mm * m_length_short);

        if (i == 0) {
            // Print extra anchor line
            gcode << writer.set_pressure_advance(0.0);
            gcode << writer.extrude_to_xy(Vec2d(start_x + m_length_short + m_length_long + m_length_short, y_pos + (num) * m_space_y), e_per_mm * m_space_y * num * 1.2);
        }
    }
    gcode << writer.set_pressure_advance(0.0);

    if (m_draw_numbers) {

        // Orca: skip drawing indicator lines
        // gcode << writer.set_speed(fast);
        // gcode << move_to(Vec2d(start_x + m_length_short, y_pos + (num - 1) * m_space_y + 2), writer);
        // gcode << writer.extrude_to_xy(Vec2d(start_x + m_length_short, y_pos + (num - 1) * m_space_y + 7), thin_e_per_mm * 7);
        // gcode << move_to(Vec2d(start_x + m_length_short + m_length_long, y_pos + (num - 1) * m_space_y + 7), writer);
        // gcode << writer.extrude_to_xy(Vec2d(start_x + m_length_short + m_length_long, y_pos + (num - 1) * m_space_y + 2), thin_e_per_mm * 7);

        const auto     box_start_x = start_x + m_length_short + m_length_long + m_length_short + m_line_width;
        DrawBoxOptArgs default_box_opt_args(2, m_height_layer, m_line_width, fast);
        default_box_opt_args.is_filled = true;
        gcode << draw_box(writer, box_start_x, start_y - m_space_y,
                          number_spacing() * 8, (num + 1) * m_space_y, default_box_opt_args);
        gcode << writer.travel_to_z(m_height_layer*2 + z_offset);
        for (int i = 0; i < num; i += 2) {
            gcode << draw_number(box_start_x + 3 + m_line_width, y_pos + i * m_space_y + m_space_y / 2, start_pa + i * step_pa, m_draw_digit_mode,
                                 m_number_line_width, number_e_per_mm, 3600, writer);
        }
    }
    return gcode.str();
}

void CalibPressureAdvanceLine::delta_modify_start(double &startx, double &starty, int count)
{
    startx = -startx;
    starty = -(count * m_space_y) / 2;
}

CalibPressureAdvancePattern::CalibPressureAdvancePattern(
    const Calib_Params &params, const DynamicPrintConfig &config, bool is_bbl_machine, const ModelObject &object, const Vec3d &origin)
    : m_params(params),CalibPressureAdvance(config)
{
    this->m_draw_digit_mode = DrawDigitMode::Bottom_To_Top;

    refresh_setup(config, is_bbl_machine, object, origin);
}

Vec3d CalibPressureAdvancePattern::handle_pos_offset() const
{
    return Vec3d{0 - print_size_x() / 2 + handle_xy_size() / 2 + handle_spacing(),
                 0 - max_numbering_height() / 2 - m_glyph_padding_vertical,
                 max_layer_z() / 2};
}

double CalibPressureAdvancePattern::flow_val() const
{
    double flow_mult = m_config.option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0);
    double nozzle_diameter = m_config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0);
    double line_width = m_config.get_abs_value("line_width", nozzle_diameter);
    if (line_width <= 0.) line_width = Flow::auto_extrusion_width(frPerimeter, nozzle_diameter);
    double layer_height = m_config.get_abs_value("layer_height");
    double speed = speed_perimeter();
    Flow pattern_line = Flow(line_width, layer_height, nozzle_diameter);

    return speed * pattern_line.mm3_per_mm() * flow_mult;
};

CustomGCode::Info CalibPressureAdvancePattern::generate_custom_gcodes(const DynamicPrintConfig &config,
                                                                      bool                      is_bbl_machine,
                                                                      const ModelObject        &object,
                                                                      const Vec3d              &origin)
{
    std::stringstream gcode;
    gcode << "; start pressure advance pattern for layer\n";

        refresh_setup(config, is_bbl_machine, object, origin);

    gcode << move_to(Vec2d(m_starting_point.x(), m_starting_point.y()), m_writer, "Move to start XY position");
    gcode << m_writer.travel_to_z(height_first_layer() + height_z_offset(), "Move to start Z position");
    gcode << m_writer.set_pressure_advance(m_params.start);

    const DrawBoxOptArgs default_box_opt_args(wall_count(), height_first_layer(), line_width_first_layer(),
                                              speed_adjust(speed_first_layer()));

    // create anchoring frame
    gcode << draw_box(m_writer, m_starting_point.x(), m_starting_point.y(), print_size_x(), frame_size_y(), default_box_opt_args);

    // create tab for numbers
    DrawBoxOptArgs draw_box_opt_args = default_box_opt_args;
    draw_box_opt_args.is_filled      = true;
    draw_box_opt_args.num_perimeters = wall_count();
    gcode << draw_box(m_writer, m_starting_point.x(), m_starting_point.y() + frame_size_y() + line_spacing_first_layer(),
                      print_size_x(),
                      max_numbering_height() + line_spacing_first_layer() + m_glyph_padding_vertical * 2, draw_box_opt_args);

    std::vector<CustomGCode::Item> gcode_items;
    const int                      num_patterns = get_num_patterns(); // "cache" for use in loops

    const double zhop_config_value = m_config.option<ConfigOptionFloats>("z_hop")->get_at(0);
    const auto accel = accel_perimeter();

    // draw pressure advance pattern
    for (int i = 0; i < m_num_layers; ++i) {
        const double layer_height = height_first_layer() + height_z_offset() + (i * height_layer());
        const double zhop_height = layer_height + zhop_config_value;

        if (i > 0) {
            gcode << "; end pressure advance pattern for layer\n";
            CustomGCode::Item item;
            item.print_z = height_first_layer() + (i - 1) * height_layer();
            item.type    = CustomGCode::Type::Custom;
            item.extra   = gcode.str();
            gcode_items.push_back(item);

            gcode = std::stringstream(); // reset for next layer contents
            gcode << "; start pressure advance pattern for layer\n";

            gcode << m_writer.travel_to_z(layer_height, "Move to layer height");
            gcode << m_writer.reset_e();
        }

        // line numbering
        if (i == 1) {
            m_number_len = max_numbering_length();

            gcode << m_writer.set_pressure_advance(m_params.start);

            double number_e_per_mm = e_per_mm(line_width(), height_layer(),
                                              m_config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0),
                                              m_config.option<ConfigOptionFloats>("filament_diameter")->get_at(0),
                                              m_config.option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0));

            // glyph on every other line
            for (int j = 0; j < num_patterns; j += 2) {
                gcode << draw_number(glyph_start_x(j), m_starting_point.y() + frame_size_y() + m_glyph_padding_vertical + line_width(),
                                     m_params.start + (j * m_params.step), m_draw_digit_mode, line_width(), number_e_per_mm,
                                     speed_first_layer(), m_writer);
            }

            // flow value
            int line_num = num_patterns + 2;
            gcode << draw_number(glyph_start_x(line_num), m_starting_point.y() + frame_size_y() + m_glyph_padding_vertical + line_width(),
                                 flow_val(), m_draw_digit_mode, line_width(), number_e_per_mm,
                                 speed_first_layer(), m_writer);

            // acceleration
            line_num = num_patterns + 4;
            gcode << draw_number(glyph_start_x(line_num), m_starting_point.y() + frame_size_y() + m_glyph_padding_vertical + line_width(),
                                 accel, m_draw_digit_mode, line_width(), number_e_per_mm,
                                 speed_first_layer(), m_writer);
        }


        double to_x        = m_starting_point.x() + pattern_shift();
        double to_y        = m_starting_point.y();
        double side_length = m_wall_side_length;

        // shrink first layer to fit inside frame
        if (i == 0) {
            double shrink = (line_spacing_first_layer() * (wall_count() - 1) + (line_width_first_layer() * (1 - m_encroachment))) /
                            std::sin(to_radians(m_corner_angle) / 2);
            side_length = m_wall_side_length - shrink;
            to_x += shrink * std::sin(to_radians(90) - to_radians(m_corner_angle) / 2);
            to_y += line_spacing_first_layer() * (wall_count() - 1) + (line_width_first_layer() * (1 - m_encroachment));
        } else {
            /* Draw a line at slightly slower accel and speed in order to trick gcode writer to force update acceleration and speed.
             * We do this since several tests may be generated by their own gcode writers which are
             * not aware about their neighbours updating acceleration/speed */
            gcode << m_writer.set_print_acceleration(std::max<int>(1, accel - 1));
            gcode << move_to(Vec2d(m_starting_point.x(), m_starting_point.y()), m_writer, "Move to starting point", zhop_height, layer_height);
            gcode << draw_line(m_writer, Vec2d(m_starting_point.x(), m_starting_point.y() + frame_size_y()), line_width(), height_layer(), speed_adjust(std::max<int>(1, speed_perimeter() - 1)), "Accel/flow trick line");
            gcode << m_writer.set_print_acceleration(accel);
        }

        double initial_x = to_x;
        double initial_y = to_y;

        gcode << move_to(Vec2d(to_x, to_y), m_writer, "Move to pattern start",zhop_height,layer_height);

        for (int j = 0; j < num_patterns; ++j) {
            // increment pressure advance
            gcode << m_writer.set_pressure_advance(m_params.start + (j * m_params.step));

            for (int k = 0; k < wall_count(); ++k) {
                to_x += std::cos(to_radians(m_corner_angle) / 2) * side_length;
                to_y += std::sin(to_radians(m_corner_angle) / 2) * side_length;

                auto draw_line_arg_height = i == 0 ? height_first_layer() : height_layer();
                auto draw_line_arg_line_width = line_width(); // don't use line_width_first_layer so results are consistent across all layers
                auto draw_line_arg_speed   = i == 0 ? speed_adjust(speed_first_layer()) : speed_adjust(speed_perimeter());
                auto draw_line_arg_comment = "Print pattern wall";
                gcode << draw_line(m_writer, Vec2d(to_x, to_y), draw_line_arg_line_width, draw_line_arg_height, draw_line_arg_speed, draw_line_arg_comment);

                to_x -= std::cos(to_radians(m_corner_angle) / 2) * side_length;
                to_y += std::sin(to_radians(m_corner_angle) / 2) * side_length;

                gcode << draw_line(m_writer, Vec2d(to_x, to_y), draw_line_arg_line_width, draw_line_arg_height, draw_line_arg_speed, draw_line_arg_comment);

                to_y = initial_y;
                if (k != wall_count() - 1) {
                    // perimeters not done yet. move to next perimeter
                    to_x += line_spacing_angle();
                    gcode << move_to(Vec2d(to_x, to_y), m_writer, "Move to start next pattern wall", zhop_height, layer_height); // Call move to command with XY as well as z hop and layer height to invoke and undo z lift
                } else if (j != num_patterns - 1) {
                    // patterns not done yet. move to next pattern
                    to_x += m_pattern_spacing + line_width();
                    gcode << move_to(Vec2d(to_x, to_y), m_writer, "Move to next pattern", zhop_height, layer_height); // Call move to command with XY as well as z hop and layer height to invoke and undo z lift
                } else if (i != m_num_layers - 1) {
                    // layers not done yet. move back to start
                    to_x = initial_x;
                    gcode << move_to(Vec2d(to_x, to_y), m_writer, "Move back to start position", zhop_height, layer_height); // Call move to command with XY as well as z hop and layer height to invoke and undo z lift
                    gcode << m_writer.reset_e(); // reset extruder before printing placeholder cube to avoid over extrusion
                } else {
                    // everything done
                }
            }
        }
    }

    gcode << m_writer.set_pressure_advance(m_params.start);
    gcode << "; end pressure advance pattern for layer\n";

    CustomGCode::Item item;
    item.print_z = max_layer_z();
    item.type    = CustomGCode::Type::Custom;
    item.extra   = gcode.str();
    gcode_items.push_back(item);

    CustomGCode::Info info;
    info.mode   = CustomGCode::Mode::SingleExtruder;
    info.gcodes = gcode_items;

    return info;
}

void CalibPressureAdvancePattern::set_start_offset(const Vec3d &offset)
{
    m_starting_point = offset;
    m_is_start_point_fixed = true;
}

Vec3d CalibPressureAdvancePattern::get_start_offset()
{
    return m_starting_point;
}

double CalibPressureAdvancePattern::line_width() const
{
    // TODO: FIXME: find out current filament/extruder?
    const double nozzle_diameter = m_config.opt_float("nozzle_diameter", 0);
    const double width           = m_config.get_abs_value("line_width", nozzle_diameter);
    if (width <= 0.)
        return Flow::auto_extrusion_width(frExternalPerimeter, nozzle_diameter);
    return width;
};

void CalibPressureAdvancePattern::refresh_setup(const DynamicPrintConfig &config,
                                                bool                      is_bbl_machine,
                                                const ModelObject         &object,
                                                const Vec3d              &origin)
{
    m_config = config;
    m_config.apply(object.config.get(), true);
    m_config.apply(object.volumes.front()->config.get(), true);

    _refresh_starting_point(object);
    _refresh_writer(is_bbl_machine, object, origin);
}

void CalibPressureAdvancePattern::_refresh_starting_point(const ModelObject &object)
{
    if (m_is_start_point_fixed)
        return;

    BoundingBoxf3 bbox = object.instance_bounding_box(*object.instances.front(), false);

    m_starting_point = Vec3d(bbox.min.x(), bbox.max.y(), 0);
    m_starting_point.x() -= m_handle_spacing;
    m_starting_point.y() -= std::sin(to_radians(m_corner_angle) / 2) * m_wall_side_length + (bbox.max.y() - bbox.min.y()) / 2;
}

void CalibPressureAdvancePattern::_refresh_writer(bool is_bbl_machine, const ModelObject &object, const Vec3d &origin)
{
    PrintConfig print_config;
    print_config.apply(m_config, true);

    m_writer.apply_print_config(print_config);
    m_writer.set_xy_offset(origin(0), origin(1));
    m_writer.set_is_bbl_machine(is_bbl_machine);

    const unsigned int extruder_id = object.volumes.front()->extruder_id();
    m_writer.set_extruders({extruder_id});
    m_writer.set_extruder(extruder_id);
}

double CalibPressureAdvancePattern::object_size_x() const
{
    return get_num_patterns() * ((wall_count() - 1) * line_spacing_angle()) +
           (get_num_patterns() - 1) * (m_pattern_spacing + line_width()) + std::cos(to_radians(m_corner_angle) / 2) * m_wall_side_length +
           line_spacing_first_layer() * wall_count();
}

double CalibPressureAdvancePattern::object_size_y() const
{
    return 2 * (std::sin(to_radians(m_corner_angle) / 2) * m_wall_side_length) + max_numbering_height() + m_glyph_padding_vertical * 2 +
           line_width_first_layer();
}

double CalibPressureAdvancePattern::glyph_start_x(int pattern_i) const
{
    // note that pattern_i is zero-based!
    // align glyph's start with first perimeter of specified pattern
    double x =
        // starting offset
        m_starting_point.x() + pattern_shift() +

        // width of pattern extrusions
        pattern_i * (wall_count() - 1) * line_spacing_angle() + // center to center distance of extrusions
        pattern_i * line_width() +                              // endcaps. center to end on either side = 1 line width

        // space between each pattern
        pattern_i * m_pattern_spacing;

    // align to middle of pattern walls
    x += wall_count() * line_spacing_angle() / 2;

    // shift so glyph is centered on pattern
    // m_digit_segment_len = half of X length of glyph
    x -= (glyph_length_x() / 2);

    return x;
}

double CalibPressureAdvancePattern::glyph_length_x() const
{
    // half of line_width sticks out on each side
    return line_width() + (2 * m_digit_segment_len);
}

double CalibPressureAdvancePattern::glyph_tab_max_x() const
{
    // only every other glyph is shown, starting with 1
    int num     = get_num_patterns();
    int max_num = (num % 2 == 0) ? num - 1 : num;

    // padding at end should be same as padding at start
    double padding = glyph_start_x(0) - m_starting_point.x();

    return glyph_start_x(max_num - 1) + // glyph_start_x is zero-based
           (glyph_length_x() - line_width() / 2) + padding;
}

size_t CalibPressureAdvancePattern::max_numbering_length() const
{
    std::string::size_type most_characters = 0;
    const int              num_patterns    = get_num_patterns();

    // note: only every other number is printed
    for (std::string::size_type i = 0; i < num_patterns; i += 2) {
        std::string sNumber = convert_number_to_string(m_params.start + (i * m_params.step));

        if (sNumber.length() > most_characters) {
            most_characters = sNumber.length();
        }
    }

    std::string sAccel = convert_number_to_string(accel_perimeter());
    most_characters = std::max(most_characters, sAccel.length());

    /* don't actually check flow value: we'll print as many fractional digits as fits */

    return std::min(most_characters, m_max_number_len);
}

double CalibPressureAdvancePattern::max_numbering_height() const
{
    std::string::size_type num_characters = max_numbering_length();
    return (num_characters * m_digit_segment_len) + ((num_characters - 1) * m_digit_gap_len);
}

double CalibPressureAdvancePattern::pattern_shift() const
{
    return (wall_count() - 1) * line_spacing_first_layer() + line_width_first_layer() + m_glyph_padding_horizontal;
}

} // namespace Slic3r
