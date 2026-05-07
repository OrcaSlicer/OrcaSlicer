#include "SpiralVase.hpp"
#include "GCode.hpp"
#include <sstream>
#include <cmath>
#include <limits>

namespace Slic3r {

namespace SpiralVaseHelpers {
/** == Smooth Spiral Helpers == */
/** Distance between a and b */
float distance(SpiralVase::SpiralPoint a, SpiralVase::SpiralPoint b) { return sqrt(pow(a.x - b.x, 2) + pow(a.y - b.y, 2)); }

SpiralVase::SpiralPoint subtract(SpiralVase::SpiralPoint a, SpiralVase::SpiralPoint b)
{
    return SpiralVase::SpiralPoint(a.x - b.x, a.y - b.y);
}

SpiralVase::SpiralPoint add(SpiralVase::SpiralPoint a, SpiralVase::SpiralPoint b) { return SpiralVase::SpiralPoint(a.x + b.x, a.y + b.y); }

SpiralVase::SpiralPoint scale(SpiralVase::SpiralPoint a, float factor) { return SpiralVase::SpiralPoint(a.x * factor, a.y * factor); }

/** dot product */
float dot(SpiralVase::SpiralPoint a, SpiralVase::SpiralPoint b) { return a.x * b.x + a.y * b.y; }

/** Find the point on line ab closes to point c */
SpiralVase::SpiralPoint nearest_point_on_line(SpiralVase::SpiralPoint c, SpiralVase::SpiralPoint a, SpiralVase::SpiralPoint b, float& dist)
{
    SpiralVase::SpiralPoint ab      = subtract(b, a);
    SpiralVase::SpiralPoint ca      = subtract(c, a);
    float                   t       = dot(ca, ab) / dot(ab, ab);
    t                               = t > 1 ? 1 : t;
    t                               = t < 0 ? 0 : t;
    SpiralVase::SpiralPoint closest = SpiralVase::SpiralPoint(add(a, scale(ab, t)));
    dist                            = distance(c, closest);
    return closest;
}

/** Given a set of lines defined by points such as line[n] is the line from points[n] to points[n+1],
 *  find the closest point to p that falls on any of the lines */
SpiralVase::SpiralPoint nearest_point_on_lines(SpiralVase::SpiralPoint               p,
                                               std::vector<SpiralVase::SpiralPoint>* points,
                                               bool&                                 found,
                                               float&                                dist)
{
    if (points->size() < 2) {
        found = false;
        return SpiralVase::SpiralPoint(0, 0);
    }
    float                   min = std::numeric_limits<float>::max();
    SpiralVase::SpiralPoint closest(0, 0);
    for (unsigned long i = 0; i < points->size() - 1; i++) {
        float                   currentDist = 0;
        SpiralVase::SpiralPoint current     = nearest_point_on_line(p, points->at(i), points->at(i + 1), currentDist);
        if (currentDist < min) {
            min     = currentDist;
            closest = current;
            found   = true;
        }
    }
    dist = min;
    return closest;
}
} // namespace SpiralVase

std::string SpiralVase::process_layer(const std::string &gcode, bool last_layer)
{
    /*  This post-processor relies on several assumptions:
        - all layers are processed through it, including those that are not supposed
          to be transformed, in order to update the reader with the XY positions
        - each call to this method includes a full layer, with a single Z move
          at the beginning
        - each layer is composed by suitable geometry (i.e. a single complete loop)
        - loops were not clipped before calling this method  */
    
    // If we're not going to modify G-code, just feed it to the reader
    // in order to update positions.
    if (! m_enabled) {
        m_reader.parse_buffer(gcode);
        return gcode;
    }
    
    // Get total XY length for this layer by summing all extrusion moves.
    float total_layer_length = 0;
    float layer_height = 0;
    float z = 0.f;
    
    {
        //FIXME Performance warning: This copies the GCodeConfig of the reader.
        GCodeReader r = m_reader;  // clone
        bool set_z = false;
        r.parse_buffer(gcode, [&total_layer_length, &layer_height, &z, &set_z]
            (GCodeReader &reader, const GCodeReader::GCodeLine &line) {
            if (line.cmd_is("G1")) {
                if (line.extruding(reader)) {
                    total_layer_length += line.dist_XY(reader);
                } else if (line.has(Z)) {
                    layer_height += line.dist_Z(reader);
                    if (!set_z) {
                        z = line.new_Z(reader);
                        set_z = true;
                    }
                }
            }
        });
    }

    // Remove layer height from initial Z.
    z -= layer_height;

    std::vector<SpiralVase::SpiralPoint>* current_layer = new std::vector<SpiralVase::SpiralPoint>();
    std::vector<SpiralVase::SpiralPoint>* previous_layer = m_previous_layer;

    bool smooth_spiral = m_smooth_spiral;
    std::string new_gcode;
    std::string transition_gcode;
    float max_xy_dist_for_smoothing = m_max_xy_smoothing;
    //FIXME Tapering of the transition layer only works reliably with relative extruder distances.
    // For absolute extruder distances it will be switched off.
    // Tapering the absolute extruder distances requires to process every extrusion value after the first transition
    // layer.
    bool  transition_in = m_transition_layer && m_config.use_relative_e_distances.value;
    bool  transition_out = last_layer && m_config.use_relative_e_distances.value;

    float starting_flowrate  = float(m_config.spiral_starting_flow_ratio.value);
    float finishing_flowrate = float(m_config.spiral_finishing_flow_ratio.value);

    float len = 0.f;
    SpiralVase::SpiralPoint last_point = previous_layer != NULL && previous_layer->size() >0? previous_layer->at(previous_layer->size()-1): SpiralVase::SpiralPoint(0,0);
    m_reader.parse_buffer(gcode, [&new_gcode, &z, total_layer_length, layer_height, transition_in, &len, &current_layer, &previous_layer, &transition_gcode, transition_out, smooth_spiral, &max_xy_dist_for_smoothing, &last_point, starting_flowrate, finishing_flowrate]
        (GCodeReader &reader, GCodeReader::GCodeLine line) {
        if (line.cmd_is("G1")) {
            // Orca: Filter out retractions at layer change
            if (line.retracting(reader) || (line.extruding(reader) && line.dist_XY(reader) < EPSILON)) return;
            if (line.has_z() && !(line.has_x() || line.has_y())) {
                // If this is the initial Z move of the layer, replace it with a
                // (redundant) move to the last Z of previous layer.
                line.set(Z, z);
                new_gcode += line.raw() + '\n';
                return;
            } else {
                float dist_XY = line.dist_XY(reader);
                if (line.has_x() || line.has_y()) { // Sometimes lines have X/Y but the move is to the last position
                    if (dist_XY > 0 && line.extruding(reader)) { // Exclude wipe and retract
                        len += dist_XY;
                        float factor = len / total_layer_length;
                        if (transition_in){
                            // Transition layer, interpolate the amount of extrusion starting from spiral_vase_starting_flow_rate to 100%.
                            float starting_e_factor = starting_flowrate + (factor * (1.f - starting_flowrate));
                            line.set(E, line.e() * starting_e_factor, 5 /*decimal_digits*/);
                        } else if (transition_out) {
                            // We want the last layer to ramp down extrusion, but without changing z height!
                            // So clone the line before we mess with its Z and duplicate it into a new layer that ramps down E
                            // We add this new layer at the very end
                            // As with transition_in, the amount is ramped down from 100% to spiral_vase_finishing_flow_rate
                            GCodeReader::GCodeLine transitionLine(line);
                            float finishing_e_factor = finishing_flowrate + ((1.f -factor) * (1.f - finishing_flowrate));
                            transitionLine.set(E, line.e() * finishing_e_factor, 5 /*decimal_digits*/);
                            transition_gcode += transitionLine.raw() + '\n';
                        }
                        // This line is the core of Spiral Vase mode, ramp up the Z smoothly
                        line.set(Z, z + factor * layer_height);
                        if (smooth_spiral) {
                            // Now we also need to try to interpolate X and Y
                            SpiralVase::SpiralPoint p(line.x(), line.y()); // Get current x/y coordinates
                            current_layer->push_back(p);       // Store that point for later use on the next layer
                            if (previous_layer != NULL) {
                                bool        found    = false;
                                float       dist     = 0;
                                SpiralVase::SpiralPoint nearestp = SpiralVaseHelpers::nearest_point_on_lines(p, previous_layer, found, dist);
                                if (found && dist < max_xy_dist_for_smoothing) {
                                    // Interpolate between the point on this layer and the point on the previous layer
                                    SpiralVase::SpiralPoint target = SpiralVaseHelpers::add(SpiralVaseHelpers::scale(nearestp, 1 - factor), SpiralVaseHelpers::scale(p, factor));

                                    // Remove tiny movement
                                    // We need to figure out the distance of this new line!
                                    float modified_dist_XY = SpiralVaseHelpers::distance(last_point, target);
                                    if (modified_dist_XY < 0.001)
                                        line.clear();
                                    else {
                                        line.set(X, target.x);
                                        line.set(Y, target.y);
                                        // Scale the extrusion amount according to change in length
                                        line.set(E, line.e() * modified_dist_XY / dist_XY, 5 /*decimal_digits*/);
                                        last_point = target;
                                    }
                                } else {
                                    last_point = p;
                                }
                            }
                        }
                        new_gcode += line.raw() + '\n';
                    }
                    return;
                    /*  Skip travel moves: the move to first perimeter point will
                        cause a visible seam when loops are not aligned in XY; by skipping
                        it we blend the first loop move in the XY plane (although the smoothness
                        of such blend depend on how long the first segment is; maybe we should
                        enforce some minimum length?).
                        When smooth_spiral is enabled, we're gonna end up exactly where the next layer should
                        start anyway, so we don't need the travel move */
                }
            }
        }
        new_gcode += line.raw() + '\n';
        if(transition_out) {
            transition_gcode += line.raw() + '\n';
        }
    });

    delete m_previous_layer;
    m_previous_layer = current_layer;
    
    return new_gcode + transition_gcode;
}

}
