#ifndef WipeTowerWriter_hpp_
#define WipeTowerWriter_hpp_

#include <cmath>
#include <string>
#include <vector>
#include <sstream>
#include <limits>
#include <algorithm>
#include <iomanip>

#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Circle.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "GCodeProcessor.hpp"
#include "WipeTower.hpp"

#include "libslic3r/libslic3r.h"

namespace Slic3r {

constexpr float         flat_iron_speed                = 10.f * 60.f;
static const double wipe_tower_wall_infill_overlap = 0.0;
static constexpr double WIPE_TOWER_RESOLUTION = 0.1;
#define WT_SIMPLIFY_TOLERANCE_SCALED (0.001 / SCALING_FACTOR)
static constexpr int    arc_fit_size = 20;
#define SCALED_WIPE_TOWER_RESOLUTION (WIPE_TOWER_RESOLUTION / SCALING_FACTOR)

struct Segment
{
    Vec2f start;
    Vec2f end;
    bool  is_arc = false;
    ArcSegment arcsegment;
    Segment(const Vec2f &s, const Vec2f &e) : start(s), end(e) {}
    bool is_valid() const { return start.y() < end.y(); }
};

class WipeTowerWriter
{
public:
	WipeTowerWriter(float layer_height, float line_width, GCodeFlavor flavor, const std::vector<WipeTower::FilamentParameters>& filament_parameters) :
		m_current_pos(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()),
		m_current_z(0.f),
		m_current_feedrate(0.f),
		m_layer_height(layer_height),
		m_extrusion_flow(0.f),
		m_preview_suppressed(false),
		m_elapsed_time(0.f),
    m_gcode_flavor(flavor),
    m_filpar(filament_parameters)
    {
            // ORCA: This class is only used by BBL printers, so set the parameter appropriately.
            // This fixes an issue where the wipe tower was using BBL tags resulting in statistics for purging in the purge tower not being displayed.
            GCodeProcessor::s_IsBBLPrinter = true;
            // adds tag for analyzer:
            std::ostringstream str;
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) << std::to_string(m_layer_height) << "\n"; // don't rely on GCodeAnalyzer knowing the layer height - it knows nothing at priming
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) << ExtrusionEntity::role_to_string(erWipeTower) << "\n";
            m_gcode += str.str();
            change_analyzer_line_width(line_width);
    }

    WipeTowerWriter& change_analyzer_line_width(float line_width) {
        // adds tag for analyzer:
        std::stringstream str;
        str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) << std::to_string(line_width) << "\n";
        m_gcode += str.str();
        return *this;
    }

	WipeTowerWriter& 			 set_initial_position(const Vec2f &pos, float width = 0.f, float depth = 0.f, float internal_angle = 0.f) {
        m_wipe_tower_width = width;
        m_wipe_tower_depth = depth;
        m_internal_angle = internal_angle;
		m_start_pos = this->rotate(pos);
		m_current_pos = pos;
		return *this;
	}

    WipeTowerWriter&				 set_initial_tool(size_t tool) { m_current_tool = tool; return *this; }

	WipeTowerWriter&				 set_z(float z)
		{ m_current_z = z; return *this; }

	WipeTowerWriter& 			 set_extrusion_flow(float flow)
		{ m_extrusion_flow = flow; return *this; }

	WipeTowerWriter&				 set_y_shift(float shift) {
        m_current_pos.y() -= shift-m_y_shift;
        m_y_shift = shift;
        return (*this);
    }

    WipeTowerWriter&            disable_linear_advance() {
        if (m_gcode_flavor == gcfKlipper)
            m_gcode += "SET_PRESSURE_ADVANCE ADVANCE=0\n";
        else if (m_gcode_flavor == gcfRepRapFirmware)
            m_gcode += std::string("M572 D") + std::to_string(m_current_tool) + " S0\n";
        else
            m_gcode += "M900 K0\n";

        return *this;
    }

	// Suppress / resume G-code preview in Slic3r. Slic3r will have difficulty to differentiate the various
	// filament loading and cooling moves from normal extrusion moves. Therefore the writer
	// is asked to suppres output of some lines, which look like extrusions.
    WipeTowerWriter& 			 suppress_preview() { m_preview_suppressed = true; return *this; }
  	WipeTowerWriter& 			 resume_preview()   { m_preview_suppressed = false; return *this; }

	WipeTowerWriter& 			 feedrate(float f)
	{
        if (f != m_current_feedrate) {
			m_gcode += "G1" + set_format_F(f) + "\n";
            m_current_feedrate = f;
        }
		return *this;
	}

	const std::string&   gcode() const { return m_gcode; }
	const std::vector<WipeTower::Extrusion>& extrusions() const { return m_extrusions; }
	float                x()     const { return m_current_pos.x(); }
	float                y()     const { return m_current_pos.y(); }
	const Vec2f& 		 pos()   const { return m_current_pos; }
	const Vec2f	 		 start_pos_rotated() const { return m_start_pos; }
	const Vec2f  		 pos_rotated() const { return this->rotate(m_current_pos); }
	float 				 elapsed_time() const { return m_elapsed_time; }
    float                get_and_reset_used_filament_length() { float temp = m_used_filament_length; m_used_filament_length = 0.f; return temp; }

	// Extrude with an explicitely provided amount of extrusion.
	WipeTowerWriter& extrude_explicit(float x, float y, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
	{
		if (x == m_current_pos.x() && y == m_current_pos.y() && e == 0.f && (f == 0.f || f == m_current_feedrate))
			// Neither extrusion nor a travel move.
			return *this;

		float dx = x - m_current_pos.x();
		float dy = y - m_current_pos.y();
        float len = std::sqrt(dx*dx+dy*dy);
        if (record_length)
            m_used_filament_length += e;

		// Now do the "internal rotation" with respect to the wipe tower center
		Vec2f rotated_current_pos(this->pos_rotated());
		Vec2f rot(this->rotate(Vec2f(x,y)));                               // this is where we want to go

        if (! m_preview_suppressed && e > 0.f && len > 0.f) {
      // Width of a squished extrusion, corrected for the roundings of the squished extrusions.
			// This is left zero if it is a travel move.
      float width = e * m_filpar[0].filament_area / (len * m_layer_height);
			// Correct for the roundings of a squished extrusion.
			width += m_layer_height * float(1. - M_PI / 4.);
			if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos)
				m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
			m_extrusions.emplace_back(WipeTower::Extrusion(rot, width, m_current_tool));
		}

		m_gcode += "G1";
        if (std::abs(rot.x() - rotated_current_pos.x()) > (float)EPSILON)
			m_gcode += set_format_X(rot.x());

        if (std::abs(rot.y() - rotated_current_pos.y()) > (float)EPSILON)
			m_gcode += set_format_Y(rot.y());


		if (e != 0.f)
			m_gcode += set_format_E(e);

		if (f != 0.f && f != m_current_feedrate) {
            if (limit_volumetric_flow) {
                float e_speed = e / (((len == 0.f) ? std::abs(e) : len) / f * 60.f);
                f /= std::max(1.f, e_speed / m_filpar[m_current_tool].max_e_speed);
            }
			m_gcode += set_format_F(f);
        }

        m_current_pos.x() = x;
        m_current_pos.y() = y;

		// Update the elapsed time with a rough estimate.
        m_elapsed_time += ((len == 0.f) ? std::abs(e) : len) / m_current_feedrate * 60.f;
		m_gcode += "\n";
		return *this;
	}

    	// Extrude with an explicitely provided amount of extrusion.
    WipeTowerWriter &extrude_arc_explicit(ArcSegment &arc, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
    {
        float x   = (float)unscale(arc.end_point).x();
        float y   = (float)unscale(arc.end_point).y();
        float len = unscaled<float>(arc.length);
        float e   = len * m_extrusion_flow;
        if (len < (float) EPSILON && e == 0.f && (f == 0.f || f == m_current_feedrate))
            // Neither extrusion nor a travel move.
            return *this;
        if (record_length) m_used_filament_length += e;

        // Now do the "internal rotation" with respect to the wipe tower center
        Vec2f rotated_current_pos(this->pos_rotated());
        Vec2f rot(this->rotate(Vec2f(x, y))); // this is where we want to go

        if (!m_preview_suppressed && e > 0.f && len > 0.f) {
       // Width of a squished extrusion, corrected for the roundings of the squished extrusions.
       // This is left zero if it is a travel move.
            float width = e * m_filpar[0].filament_area / (len * m_layer_height);
            // Correct for the roundings of a squished extrusion.
            width += m_layer_height * float(1. - M_PI / 4.);
            if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos) m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
            {
                int   n            = arc_fit_size;
                for (int j = 0; j < n; j++) {
                    float cur_angle = arc.polar_start_theta + (float) j / n * arc.angle_radians;
                    if (cur_angle > 2 * PI)
                        cur_angle -= 2 * PI;
                    else if (cur_angle < 0)
                        cur_angle += 2 * PI;
                    Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius * std::sin(cur_angle)};
                    m_extrusions.emplace_back(WipeTower::Extrusion(this->rotate(unscaled<float>(tmp)), width, m_current_tool));
                }
                m_extrusions.emplace_back(WipeTower::Extrusion(rot, width, m_current_tool));
            }

        }
        m_gcode += arc.direction == ArcDirection::Arc_Dir_CCW ? "G3" : "G2";
        const Vec2f center_offset = this->rotate(unscaled<float>(arc.center)) - rotated_current_pos;
        m_gcode += set_format_X(rot.x());
        m_gcode += set_format_Y(rot.y());
        m_gcode += set_format_I(center_offset.x());
        m_gcode += set_format_J(center_offset.y());

        if (e != 0.f) m_gcode += set_format_E(e);

        if (f != 0.f && f != m_current_feedrate) {
            if (limit_volumetric_flow) {
                float e_speed = e / (((len == 0.f) ? std::abs(e) : len) / f * 60.f);
                f /= std::max(1.f, e_speed / m_filpar[m_current_tool].max_e_speed);
            }
            m_gcode += set_format_F(f);
        }

        m_current_pos.x() = x;
        m_current_pos.y() = y;

        // Update the elapsed time with a rough estimate.
        m_elapsed_time += ((len == 0.f) ? std::abs(e) : len) / m_current_feedrate * 60.f;
        m_gcode += "\n";
        return *this;
    }

	WipeTowerWriter& extrude_explicit(const Vec2f &dest, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
		{ return extrude_explicit(dest.x(), dest.y(), e, f, record_length); }

	// Travel to a new XY position. f=0 means use the current value.
	WipeTowerWriter& travel(float x, float y, float f = 0.f)
		{ return extrude_explicit(x, y, 0.f, f); }

	WipeTowerWriter& travel(const Vec2f &dest, float f = 0.f)
		{ return extrude_explicit(dest.x(), dest.y(), 0.f, f); }

	// Extrude a line from current position to x, y with the extrusion amount given by m_extrusion_flow.
	WipeTowerWriter& extrude(float x, float y, float f = 0.f)
	{
		float dx = x - m_current_pos.x();
		float dy = y - m_current_pos.y();
        return extrude_explicit(x, y, std::sqrt(dx*dx+dy*dy) * m_extrusion_flow, f, true);
	}
    WipeTowerWriter &extrude_arc(ArcSegment &arc, float f = 0.f)
    {
        return extrude_arc_explicit(arc, f, true);
    }

	WipeTowerWriter& extrude(const Vec2f &dest, const float f = 0.f)
		{ return extrude(dest.x(), dest.y(), f); }

    WipeTowerWriter& rectangle(const Vec2f& ld,float width,float height,const float f = 0.f)
    {
        Vec2f corners[4];
        corners[0] = ld;
        corners[1] = ld + Vec2f(width,0.f);
        corners[2] = ld + Vec2f(width,height);
        corners[3] = ld + Vec2f(0.f,height);
        int index_of_closest = 0;
        if (x()-ld.x() > ld.x()+width-x())    // closer to the right
            index_of_closest = 1;
        if (y()-ld.y() > ld.y()+height-y())   // closer to the top
            index_of_closest = (index_of_closest==0 ? 3 : 2);

        travel(corners[index_of_closest].x(), y());      // travel to the closest corner
        travel(x(),corners[index_of_closest].y());

        int i = index_of_closest;
        do {
            ++i;
            if (i==4) i=0;
            extrude(corners[i], f);
        } while (i != index_of_closest);
        return (*this);
    }

    WipeTowerWriter &rectangle_fill_box(const WipeTower* wipe_tower, const Vec2f &ld, float width, float height, const float f = 0.f)
    {
        bool need_change_flow = wipe_tower->need_thick_bridge_flow(ld.y());

        Vec2f corners[4];
        corners[0]           = ld;
        corners[1]           = ld + Vec2f(width, 0.f);
        corners[2]           = ld + Vec2f(width, height);
        corners[3]           = ld + Vec2f(0.f, height);
        int index_of_closest = 0;
        if (x() - ld.x() > ld.x() + width - x()) // closer to the right
            index_of_closest = 1;
        if (y() - ld.y() > ld.y() + height - y()) // closer to the top
            index_of_closest = (index_of_closest == 0 ? 3 : 2);

        travel(corners[index_of_closest].x(), y()); // travel to the closest corner
        travel(x(), corners[index_of_closest].y());

        int i = index_of_closest;
        bool flow_changed = false;
        do {
            ++i;
            if (i == 4) i = 0;
            if (need_change_flow) {
                if (i == 1) {
                    // using bridge flow in bridge area, and add notes for gcode-check when flow changed
                    set_extrusion_flow(wipe_tower->extrusion_flow(0.2));
                    append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(0.2) + "\n");
                    flow_changed = true;
                } else if (i == 2 && flow_changed) {
                    set_extrusion_flow(wipe_tower->get_extrusion_flow());
                    append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(m_layer_height) + "\n");
                }
            }
            extrude(corners[i], f);
        } while (i != index_of_closest);
        return (*this);
    }
    WipeTowerWriter &line(const WipeTower *wipe_tower, Vec2f p0, Vec2f p1,const float f = 0.f)
    {
        bool need_change_flow = wipe_tower->need_thick_bridge_flow(p0.y());
        if (need_change_flow) {
            set_extrusion_flow(wipe_tower->extrusion_flow(0.2));
            append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(0.2) + "\n");
        }
        if (abs(x() - p0.x()) > abs(x() - p1.x())) std::swap(p0, p1);
        travel(p0.x(), y());
        travel(x(), p0.y());
        extrude(p1, f);
        if (need_change_flow) {
            set_extrusion_flow(wipe_tower->get_extrusion_flow());
            append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(m_layer_height) + "\n");
        }
        return (*this);
    }

    WipeTowerWriter &rectangle_fill_box(const WipeTower *wipe_tower, const WipeTower::box_coordinates &fill_box, std::vector<Vec2f> &finish_rect_wipe_path, const float f = 0.f)
    {
        float width  = fill_box.rd.x() - fill_box.ld.x();
        float height = fill_box.ru.y() - fill_box.rd.y();
        if (height > wipe_tower->m_perimeter_width - wipe_tower->WT_EPSILON) {
            rectangle_fill_box(wipe_tower, fill_box.ld, width, height, f);
            Vec2f target = (pos() == fill_box.ld ? fill_box.rd : (pos() == fill_box.rd ? fill_box.ru : (pos() == fill_box.ru ? fill_box.lu : fill_box.ld)));
            finish_rect_wipe_path.emplace_back(pos());
            finish_rect_wipe_path.emplace_back(target);
        } else if (height > wipe_tower->WT_EPSILON) {
            line(wipe_tower, fill_box.ld, fill_box.rd);
            Vec2f target = (pos() == fill_box.ld ? fill_box.rd : fill_box.ld);
            finish_rect_wipe_path.emplace_back(pos());
            finish_rect_wipe_path.emplace_back(target);
        }
        return (*this);
    }
    WipeTowerWriter& rectangle(const WipeTower::box_coordinates& box, const float f = 0.f)
    {
        rectangle(Vec2f(box.ld.x(), box.ld.y()),
                  box.ru.x() - box.lu.x(),
                  box.ru.y() - box.rd.y(), f);
        return (*this);
    }
    WipeTowerWriter &polygon(const Polygon &wall_polygon, const float f = 0.f)
    {
        Polyline    pl = to_polyline(wall_polygon);
        pl.simplify(WT_SIMPLIFY_TOLERANCE_SCALED);
        pl.simplify_by_fitting_arc(SCALED_WIPE_TOWER_RESOLUTION);

        auto get_closet_idx = [this](std::vector<Segment> &corners) -> int {
            Vec2f anchor{this->m_current_pos.x(), this->m_current_pos.y()};
            int   closestIndex = -1;
            float minDistance  = std::numeric_limits<float>::max();
            for (int i = 0; i < corners.size(); ++i) {
                float distance = (corners[i].start - anchor).squaredNorm();
                if (distance < minDistance) {
                    minDistance  = distance;
                    closestIndex = i;
                }
            }
            return closestIndex;
        };
        std::vector<Segment> segments;
        for (int i = 0; i < pl.fitting_result.size(); i++) {
            if (pl.fitting_result[i].path_type == EMovePathType::Linear_move) {
                for (int j = pl.fitting_result[i].start_point_index; j < pl.fitting_result[i].end_point_index; j++)
                    segments.push_back({unscaled<float>(pl.points[j]), unscaled<float>(pl.points[j + 1])});
            } else {
                int beg = pl.fitting_result[i].start_point_index;
                int end = pl.fitting_result[i].end_point_index;
                segments.push_back({unscaled<float>(pl.points[beg]), unscaled<float>(pl.points[end])});
                segments.back().is_arc     = true;
                segments.back().arcsegment = pl.fitting_result[i].arc_data;
            }
        }

        int index_of_closest = get_closet_idx(segments);
        int i                = index_of_closest;
        travel(segments[i].start); // travel to the closest points
        segments[i].is_arc ? extrude_arc(segments[i].arcsegment, f) : extrude(segments[i].end, f);
        do {
            i = (i + 1) % segments.size();
            if (i == index_of_closest) break;
            segments[i].is_arc ? extrude_arc(segments[i].arcsegment, f) : extrude(segments[i].end, f);
        } while (1);
        return (*this);
    }

	WipeTowerWriter& load(float e, float f = 0.f)
	{
		if (e == 0.f && (f == 0.f || f == m_current_feedrate))
			return *this;
		m_gcode += "G1";
		if (e != 0.f)
			m_gcode += set_format_E(e);
		if (f != 0.f && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	WipeTowerWriter& retract(float e, float f = 0.f)
		{ return load(-e, f); }

// Loads filament while also moving towards given points in x-axis (x feedrate is limited by cutting the distance short if necessary)
    WipeTowerWriter& load_move_x_advanced(float farthest_x, float loading_dist, float loading_speed, float max_x_speed = 50.f)
    {
        float time = std::abs(loading_dist / loading_speed); // time that the move must take
        float x_distance = std::abs(farthest_x - x());       // max x-distance that we can travel
        float x_speed = x_distance / time;                   // x-speed to do it in that time

        if (x_speed > max_x_speed) {
            // Necessary x_speed is too high - we must shorten the distance to achieve max_x_speed and still respect the time.
            x_distance = max_x_speed * time;
            x_speed = max_x_speed;
        }

        float end_point = x() + (farthest_x > x() ? 1.f : -1.f) * x_distance;
        return extrude_explicit(end_point, y(), loading_dist, x_speed * 60.f, false, false);
    }

	// Elevate the extruder head above the current print_z position.
	WipeTowerWriter& z_hop(float hop, float f = 0.f)
	{
		m_gcode += std::string("G1") + set_format_Z(m_current_z + hop);
		if (f != 0 && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	// Lower the extruder head back to the current print_z position.
	WipeTowerWriter& z_hop_reset(float f = 0.f)
		{ return z_hop(0, f); }

	// Move to x1, +y_increment,
	// extrude quickly amount e to x2 with feed f.
	WipeTowerWriter& ram(float x1, float x2, float dy, float e0, float e, float f)
	{
		extrude_explicit(x1, m_current_pos.y() + dy, e0, f, true, false);
		extrude_explicit(x2, m_current_pos.y(), e, 0.f, true, false);
		return *this;
	}

	// Let the end of the pulled out filament cool down in the cooling tube
	// by moving up and down and moving the print head left / right
	// at the current Y position to spread the leaking material.
	WipeTowerWriter& cool(float x1, float x2, float e1, float e2, float f)
	{
		extrude_explicit(x1, m_current_pos.y(), e1, f, false, false);
		extrude_explicit(x2, m_current_pos.y(), e2, false, false);
		return *this;
	}

    WipeTowerWriter& set_tool(size_t tool)
	{
		m_current_tool = tool;
		return *this;
	}

	// Set extruder temperature, don't wait by default.
	WipeTowerWriter& set_extruder_temp(int temperature, bool wait = false)
	{
        m_gcode += "M" + std::to_string(wait ? 109 : 104) + " S" + std::to_string(temperature) + "\n";
        return *this;
    }

    // BBL parity: M104 with physical extruder mapping and optional M400 sync
    WipeTowerWriter &format_line_M104(int target_temp, int target_extruder, bool wait_for_moves = true, const std::string &comment = std::string())
    {
        std::string buffer;
        if (wait_for_moves)
            buffer += "M400\n";
        buffer += "M104";
        if (target_extruder != -1)
            buffer += (" T" + std::to_string(m_physical_extruder_map[target_extruder]));
        buffer += " S" + std::to_string(target_temp) + " N0";
        if (!comment.empty()) buffer += " ;" + comment;
        buffer += '\n';
        append(buffer);
        return *this;
    }

    // BBL parity: M109 with physical extruder mapping
    WipeTowerWriter &format_line_M109(int target_temp, int target_extruder, const std::string &comment = std::string())
    {
        std::string buffer = "M109";
        if (target_extruder != -1)
            buffer += (" T" + std::to_string(m_physical_extruder_map[target_extruder]));
        buffer += " S" + std::to_string(target_temp) + " N0";
        if (!comment.empty()) buffer += " ;" + comment;
        buffer += '\n';
        append(buffer);
        return *this;
    }

    // Wait for a period of time (seconds).
	WipeTowerWriter& wait(float time)
	{
        if (time==0.f)
            return *this;
        m_gcode += "G4 S" + Slic3r::float_to_string_decimal_point(time, 3) + "\n";
		return *this;
    }

	// Set speed factor override percentage.
	WipeTowerWriter& speed_override(int speed)
	{
        m_gcode += "M220 S" + std::to_string(speed) + "\n";
		return *this;
    }

	// Let the firmware back up the active speed override value.
	WipeTowerWriter& speed_override_backup()
    {
        // BBS: BBL machine don't support speed backup
        if (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware)
            m_gcode += "M220 B\n";
		return *this;
    }

	// Let the firmware restore the active speed override value.
	WipeTowerWriter& speed_override_restore()
	{
	    // BBS: BBL machine don't support speed restore
        if (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware)
            m_gcode += "M220 R\n";
		return *this;
    }

	// Set digital trimpot motor
	WipeTowerWriter& set_extruder_trimpot(int current)
	{
		return *this;
    }

	WipeTowerWriter& flush_planner_queue()
	{
		m_gcode += "G4 S0\n";
		return *this;
	}

	// Reset internal extruder counter.
	WipeTowerWriter& reset_extruder()
	{
		m_gcode += "G92 E0\n";
		return *this;
	}

	WipeTowerWriter& comment_with_value(const char *comment, int value)
    {
        m_gcode += std::string(";") + comment + std::to_string(value) + "\n";
		return *this;
    }


    WipeTowerWriter& set_fan(unsigned speed)
	{
		if (speed == m_last_fan_speed)
			return *this;
		if (speed == 0)
			m_gcode += "M107\n";
        else
            m_gcode += "M106 S" + std::to_string(unsigned(255.0 * speed / 100.0)) + "\n";
		m_last_fan_speed = speed;
		return *this;
	}

	WipeTowerWriter& append(const std::string& text) { m_gcode += text; return *this; }

    const std::vector<Vec2f>& wipe_path() const
    {
        return m_wipe_path;
    }

    WipeTowerWriter& add_wipe_point(const Vec2f& pt)
    {
        m_wipe_path.push_back(rotate(pt));
        return *this;
    }

    WipeTowerWriter& add_wipe_point(float x, float y)
    {
        return add_wipe_point(Vec2f(x, y));
    }

    WipeTowerWriter &add_wipe_path(const Polygon & polygon,double wipe_dist)
    {
        int closest_idx = polygon.closest_point_index(scaled(m_current_pos));
        Polyline wipe_path   = polygon.split_at_index(closest_idx);
        wipe_path.reverse();
        for (int i = 0; i < wipe_path.size(); ++i) {
            if (wipe_dist < EPSILON) break;
            add_wipe_point(unscaled<float>(wipe_path[i]));
            if (i != 0) wipe_dist -= (unscaled(wipe_path[i]) - unscaled(wipe_path[i - 1])).norm();
        }
        return *this;
    }
    void generate_path(Polylines &pls, float feedrate, float retract_length, float retract_speed, bool used_fillet)
    {
        auto get_closet_idx = [this](std::vector<Segment> &corners) -> int {
            Vec2f anchor{this->m_current_pos.x(), this->m_current_pos.y()};
            int   closestIndex = -1;
            float minDistance  = std::numeric_limits<float>::max();
            for (int i = 0; i < corners.size(); ++i) {
                float distance = (corners[i].start - anchor).squaredNorm();
                if (distance < minDistance) {
                    minDistance  = distance;
                    closestIndex = i;
                }
            }
            return closestIndex;
        };
        for (auto &pl : pls) pl.simplify_by_fitting_arc(SCALED_WIPE_TOWER_RESOLUTION);

        std::vector<Segment> segments;
        for (const auto &pl : pls) {
            if (pl.points.size()<2) continue;
            for (int i = 0; i < pl.fitting_result.size(); i++) {
                if (pl.fitting_result[i].path_type == EMovePathType::Linear_move) {
                    for (int j = pl.fitting_result[i].start_point_index; j < pl.fitting_result[i].end_point_index; j++)
                        segments.push_back({unscaled<float>(pl.points[j]), unscaled<float>(pl.points[j + 1])});
                } else {
                    int beg = pl.fitting_result[i].start_point_index;
                    int end = pl.fitting_result[i].end_point_index;
                    segments.push_back({unscaled<float>(pl.points[beg]), unscaled<float>(pl.points[end])});
                    segments.back().is_arc = true;
                    segments.back().arcsegment = pl.fitting_result[i].arc_data;
                }

            }
        }
        int index_of_closest = get_closet_idx(segments);
        int i = index_of_closest;
        travel(segments[i].start); // travel to the closest points
        segments[i].is_arc? extrude_arc(segments[i].arcsegment,feedrate) : extrude(segments[i].end, feedrate);
        do {
            i         = (i + 1) % segments.size();
            if (i == index_of_closest) break;
            float dx  = segments[i].start.x() - m_current_pos.x();
            float dy  = segments[i].start.y() - m_current_pos.y();
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > EPSILON) {
                retract(retract_length, retract_speed);
                travel(segments[i].start, 600.);
                retract(-retract_length, retract_speed);
            }
            segments[i].is_arc ? extrude_arc(segments[i].arcsegment, feedrate) : extrude(segments[i].end, feedrate);
        } while (1);
    }
    void spiral_flat_ironing(const Vec2f &center, float area, float step_length, float feedrate)
    {
        float edge_length = std::sqrt(area);
        Vec2f box_max     = center + Vec2f{step_length, step_length};
        Vec2f box_min     = center - Vec2f{step_length, step_length};
        int   n           = std::ceil(edge_length / step_length / 2.f);
        assert(n > 0);
        while (n--) {
            travel(box_max.x(), m_current_pos.y(), feedrate);
            travel(m_current_pos.x(), box_max.y(), feedrate);
            travel(box_min.x(), m_current_pos.y(), feedrate);
            travel(m_current_pos.x(), box_min.y(), feedrate);

            box_max += Vec2f{step_length, step_length};
            box_min -= Vec2f{step_length, step_length};
        }
    }
    // BBL parity: acceleration/layer/extruder setters
    void set_first_layer(bool is_first_layer) { m_is_first_layer = is_first_layer; }
    void set_normal_acceleration(const std::vector<unsigned int> &accelerations) { m_normal_accelerations = accelerations; }
    void set_first_layer_normal_acceleration(const std::vector<unsigned int> &accelerations) { m_first_layer_normal_accelerations = accelerations; }
    void set_travel_acceleration(const std::vector<unsigned int> &accelerations) { m_travel_accelerations = accelerations; }
    void set_first_layer_travel_acceleration(const std::vector<unsigned int> &accelerations) { m_first_layer_travel_accelerations = accelerations; }
    void set_max_acceleration(unsigned int acceleration) { m_max_acceleration = acceleration; }
    void set_accel_to_decel_enable(bool enable) { m_accel_to_decel_enable = enable; }
    void set_accel_to_decel_factor(float factor) { m_accel_to_decel_factor = factor; }
    void set_layer_id(int layer_id) { m_layer_id = layer_id; }
    void set_multi_nozzle_group_result(const MultiNozzleUtils::LayeredNozzleGroupResult *multi_nozzle_group_result) { m_multi_nozzle_group_result = multi_nozzle_group_result; }
    void set_physical_extruder_map(const std::vector<int> &physical_extruder_map) { m_physical_extruder_map = physical_extruder_map; }

private:
    std::string set_normal_acceleration() {
        std::vector<unsigned int> accelerations = m_is_first_layer ? m_first_layer_normal_accelerations : m_normal_accelerations;
        if (accelerations.empty() || !m_multi_nozzle_group_result)
            return std::string();
        int extruder_id = m_multi_nozzle_group_result->get_extruder_id(m_current_tool, m_layer_id);
        unsigned int acc = accelerations[extruder_id];
        return set_acceleration_impl(acc);
    }
    std::string set_travel_acceleration()
    {
        std::vector<unsigned int> accelerations = m_is_first_layer ? m_first_layer_travel_accelerations : m_travel_accelerations;
        if (accelerations.empty() || !m_multi_nozzle_group_result)
            return std::string();
        int extruder_id = m_multi_nozzle_group_result->get_extruder_id(m_current_tool, m_layer_id);
        unsigned int acc = accelerations[extruder_id];
        return set_acceleration_impl(acc);
    }
    std::string set_acceleration_impl(unsigned int acceleration) {
        if (m_max_acceleration > 0 && acceleration > m_max_acceleration)
            acceleration = m_max_acceleration;
        if (acceleration == 0 || acceleration == m_last_acceleration)
            return std::string();
        m_last_acceleration = acceleration;
        std::ostringstream gcode;
        if (m_gcode_flavor == gcfRepetier) {
            gcode << "M201 X" << acceleration << " Y" << acceleration;
            gcode << "\n";
            gcode << "M202 X" << acceleration << " Y" << acceleration;
        } else if (m_gcode_flavor == gcfRepRapFirmware) {
            gcode << "M204 P" << acceleration;
        } else if (m_gcode_flavor == gcfMarlinFirmware) {
            gcode << "M204 P" << acceleration;
        } else if (m_gcode_flavor == gcfKlipper && m_accel_to_decel_enable) {
            gcode << "SET_VELOCITY_LIMIT ACCEL_TO_DECEL=" << acceleration * m_accel_to_decel_factor / 100;
            gcode << "\nM204 S" << acceleration;
        } else {
            gcode << "M204 S" << acceleration;
        }
        gcode << "\n";
        return gcode.str();
    }
    std::vector<unsigned int> m_normal_accelerations;
    std::vector<unsigned int> m_first_layer_normal_accelerations;
    std::vector<unsigned int> m_travel_accelerations;
    std::vector<unsigned int> m_first_layer_travel_accelerations;
    bool                      m_is_first_layer{false};
    unsigned int              m_max_acceleration{0};
    unsigned int              m_last_acceleration{0};
    bool                      m_accel_to_decel_enable{false};
    float                     m_accel_to_decel_factor{1.f};
    const MultiNozzleUtils::LayeredNozzleGroupResult *m_multi_nozzle_group_result{nullptr};
    int                       m_layer_id = -1;
    std::vector<int>          m_physical_extruder_map;

private:
	Vec2f         m_start_pos;
	Vec2f         m_current_pos;
    std::vector<Vec2f>  m_wipe_path;
	float    	  m_current_z;
	float 	  	  m_current_feedrate;
    size_t        m_current_tool;
	float 		  m_layer_height;
	float 	  	  m_extrusion_flow;
	bool		  m_preview_suppressed;
	std::string   m_gcode;
	std::vector<WipeTower::Extrusion> m_extrusions;
	float         m_elapsed_time;
	float   	  m_internal_angle = 0.f;
	float		  m_y_shift = 0.f;
	float 		  m_wipe_tower_width = 0.f;
	float		  m_wipe_tower_depth = 0.f;
    unsigned      m_last_fan_speed = 0;
    int           current_temp = -1;
    float         m_used_filament_length = 0.f;
    GCodeFlavor   m_gcode_flavor;
    const std::vector<WipeTower::FilamentParameters>& m_filpar;

	std::string   set_format_X(float x)
    {
        m_current_pos.x() = x;
        return " X" + Slic3r::float_to_string_decimal_point(x, 3);
	}

	std::string   set_format_Y(float y) {
        m_current_pos.y() = y;
        return " Y" + Slic3r::float_to_string_decimal_point(y, 3);
	}

	std::string   set_format_Z(float z) {
        return " Z" + Slic3r::float_to_string_decimal_point(z, 3);
	}

	std::string   set_format_E(float e) {
        return " E" + Slic3r::float_to_string_decimal_point(e, 4);
	}

	std::string   set_format_F(float f) {
        char buf[64];
        sprintf(buf, " F%d", int(floor(f + 0.5f)));
        m_current_feedrate = f;
        return buf;
	}
    std::string set_format_I(float i) { return " I" + Slic3r::float_to_string_decimal_point(i, 3); }
    std::string set_format_J(float j) { return " J" + Slic3r::float_to_string_decimal_point(j, 3); }

	WipeTowerWriter& operator=(const WipeTowerWriter &rhs);

	// Rotate the point around center of the wipe tower about given angle (in degrees)
	Vec2f rotate(Vec2f pt) const
	{
		pt.x() -= m_wipe_tower_width / 2.f;
		pt.y() += m_y_shift - m_wipe_tower_depth / 2.f;
	    double angle = m_internal_angle * float(M_PI/180.);
	    double c = cos(angle);
	    double s = sin(angle);
	    return Vec2f(float(pt.x() * c - pt.y() * s) + m_wipe_tower_width / 2.f, float(pt.x() * s + pt.y() * c) + m_wipe_tower_depth / 2.f);
	}

};

} // namespace Slic3r

#endif // WipeTowerWriter_hpp_
