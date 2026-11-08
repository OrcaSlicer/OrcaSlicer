#ifndef slic3r_CoolingBuffer_hpp_
#define slic3r_CoolingBuffer_hpp_

#include "../Point.hpp"
#include <cstddef>
#include <map>
#include <string>
#include <cfloat>
#include <vector>

namespace Slic3r {

class GCode;
class Layer;
class PrintConfig;
struct PerExtruderAdjustments;

// A standalone G-code filter, to control cooling of the print.
// The G-code is processed per layer. Once a layer is collected, fan start / stop commands are edited
// and the print is modified to stretch over a minimum layer time.
//
// The simple it sounds, the actual implementation is significantly more complex.
// Namely, for a multi-extruder print, each material may require a different cooling logic.
// For example, some materials may not like to print too slowly, while with some materials
// we may slow down significantly.
//
class CoolingBuffer {
public:
    CoolingBuffer(GCode &gcodegen);
    ~CoolingBuffer();
    void        reset(const Vec3d &position);
    void        set_current_extruder(unsigned int extruder_id, unsigned int nozzle_id) { m_current_extruder = extruder_id; m_current_nozzle = nozzle_id; }
    std::string process_layer(std::string &&gcode, size_t layer_id, bool flush);

    // Layer time smoothing needs the layer times of the whole print, so the layers are processed in two passes:
    // collect_layer() parses each layer and applies the minimum layer time slow down, finish_deferred() then smooths
    // the layer times over all collected layers and writes the layers. The G-code of all collected layers is held in memory.
    void                     begin_deferred();
    void                     collect_layer(std::string &&gcode, size_t layer_id, bool flush);
    // Returns the adjusted G-code of every collected layer, in order.
    std::vector<std::string> finish_deferred();

private:
	CoolingBuffer& operator=(const CoolingBuffer&) = delete;
    // final_extruder, if not null, receives the extruder active at the end of the layer.
    std::vector<PerExtruderAdjustments> parse_layer_gcode(const std::string &gcode, std::vector<float> &current_pos, unsigned int *final_extruder = nullptr) const;
    float       calculate_layer_slowdown(std::vector<PerExtruderAdjustments> &per_extruder_adjustments);
    // Apply slow down over G-code lines stored in per_extruder_adjustments, enable fan if needed.
    // Returns the adjusted G-code.
    std::string apply_layer_cooldown(const std::string &gcode, size_t layer_id, float layer_time, std::vector<PerExtruderAdjustments> &per_extruder_adjustments);

    // G-code snippet cached for the support layers preceding an object layer.
    std::string                 m_gcode;
    // A layer collected by collect_layer(), waiting for the layer time smoothing.
    struct DeferredLayer;
    std::vector<DeferredLayer>  m_deferred_layers;
    // Extruder active at the end of the last collected layer.
    unsigned int                m_deferred_extruder { 0 };
    // Internal data.
    // BBS: X,Y,Z,E,F,I,J
    std::vector<char>           m_axis;
    std::vector<float>          m_current_pos;
    // Current known fan speed or -1 if not known yet.
    int                         m_fan_speed;
    int                         m_additional_fan_speed;
    // Cached from GCodeWriter.
    // Printing extruder IDs, zero based.
    std::vector<unsigned int>   m_extruder_ids;
    // Highest of m_extruder_ids plus 1.
    unsigned int                m_num_extruders { 0 };
    const std::string           m_toolchange_prefix;
    // Referencs GCode::m_config, which is FullPrintConfig. While the PrintObjectConfig slice of FullPrintConfig is being modified,
    // the PrintConfig slice of FullPrintConfig is constant, thus no thread synchronization is required.
    const PrintConfig          &m_config;
    // Resolves the filament config index of the per-variant options.
    const GCode                &m_gcodegen;
    unsigned int                m_current_extruder;
    unsigned int                m_current_nozzle;
    //BBS: current fan speed
    int                         m_current_fan_speed;
    // Belt printers: the extrusion being processed lies in the first-layer band above the
    // belt (between a ";_BELT_BAND_START" and a ";_BELT_BAND_END"). Kept across layers.
    bool                        m_belt_band_active = false;
};

}

#endif
