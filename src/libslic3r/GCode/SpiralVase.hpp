#ifndef slic3r_SpiralVase_hpp_
#define slic3r_SpiralVase_hpp_

#include "../libslic3r.h"
#include "../GCodeReader.hpp"

namespace Slic3r {

class SpiralVase
{
public:
    class SpiralPoint
    {
    public:
        SpiralPoint(float paramx, float paramy) : x(paramx), y(paramy) {}

    public:
        float x, y;
    };
    SpiralVase(const PrintConfig &config) : m_config(config)
    {
        m_reader.z() = (float)m_config.z_offset;
        m_reader.apply_config(m_config);
        m_previous_layer = NULL;
    };

    void 		enable(bool en) {
   		m_transition_layer = en && ! m_enabled;
    	m_enabled 		   = en;
    }

    void set_layer_params(bool smooth_spiral, float max_xy_smoothing, float starting_flow_ratio, float finishing_flow_ratio, bool filter_short_extrusions)
    {
        m_smooth_spiral            = smooth_spiral;
        m_max_xy_smoothing         = max_xy_smoothing;
        m_starting_flow_ratio      = starting_flow_ratio;
        m_finishing_flow_ratio     = finishing_flow_ratio;
        m_filter_short_extrusions  = filter_short_extrusions;
    }

    std::string process_layer(const std::string &gcode, bool last_spiral_layer);
private:
    const PrintConfig  &m_config;
    GCodeReader 		m_reader;
    float               m_max_xy_smoothing         = 0.f;
    float               m_starting_flow_ratio      = 0.f;
    float               m_finishing_flow_ratio     = 0.f;
    bool                m_filter_short_extrusions  = false;

    bool 				m_enabled = false;
    // First spiral vase layer. Layer height has to be ramped up from zero to the target layer height.
    bool 				m_transition_layer = false;
    bool                m_has_previous_spiral_z = false;
    float               m_previous_spiral_z = 0.f;
    // Whether to interpolate XY coordinates with the previous layer. Results in no seam at layer changes
    bool                m_smooth_spiral = false;
    std::vector<SpiralPoint> * m_previous_layer;
};
}

#endif // slic3r_SpiralVase_hpp_
