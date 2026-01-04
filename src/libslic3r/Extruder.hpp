#ifndef slic3r_Extruder_hpp_
#define slic3r_Extruder_hpp_

#include "libslic3r.h"
#include "Point.hpp"

namespace Slic3r {

class GCodeConfig;

class Extruder
{
public:
    Extruder(unsigned int id, GCodeConfig *config, bool share_extruder);
    virtual ~Extruder() {}

    void   reset() {
        // BBS
        if (m_share_extruder) {
            m_share_E = std::vector<double>(MAXIMUM_EXTRUDER_NUMBER, 0);
            m_share_retracted = std::vector<double>(MAXIMUM_EXTRUDER_NUMBER, 0);
        } else {
            m_E             = 0;
            m_retracted     = 0;
        }
        m_restart_extra = 0;
        m_absolute_E    = 0;
    }

    unsigned int id() const { return m_id; }

    unsigned int extruder_id() const;
    double extrude(double dE);
    double retract(double length, double restart_extra);
    double unretract();
    double E() const { return m_share_extruder ? m_share_E[extruder_id()] : m_E; }
    void   reset_E() { m_E = 0.; m_share_E[extruder_id()] = 0.; }
    // e_per_mm is extrusion_per_mm = geometric volume * (filament flow ratio / cross-sectional area)  [Doesn't account for print_flow_ratio, or modifiers like bridge flow ratio etc.]
    double e_per_mm(double mm3_per_mm) const { return mm3_per_mm * m_e_per_mm3; }
    // e_per_mm3 is extrusion_per_mm3 = filament flow ratio / cross-sectional area    [Doesn't account for print_flow_ratio, or modifiers like bridge flow ratio etc.]
    double e_per_mm3() const { return m_e_per_mm3; }
    // Used filament volume in mm^3.
    double extruded_volume() const;
    // Used filament length in mm.
    double used_filament() const;
    
    // Getters for the PlaceholderParser.
    // Get current extruder position. Only applicable with absolute extruder addressing.
    double position() const { return m_E; }
    // Get current retraction value. Only non-negative values.
    double retracted() const { return m_retracted; }
    // Get extra retraction planned after
    double restart_extra() const { return m_restart_extra; }
    // Setters for the PlaceholderParser.
    // Set current extruder position. Only applicable with absolute extruder addressing.
    void   set_position(double e) { m_E = e; }
    // Sets current retraction value & restart extra filament amount if retracted > 0.
    void   set_retracted(double retracted, double restart_extra);
    
    double filament_diameter() const;
    double filament_crossection() const { return this->filament_diameter() * this->filament_diameter() * 0.25 * PI; }
    double filament_density() const;
    double filament_cost() const;
    double filament_flow_ratio() const;
    double retract_before_wipe() const;
    // Orca:
    double retract_after_wipe() const;
    double retraction_length() const;
    double retract_lift() const;
    int    retract_speed() const;
    int    deretract_speed() const;
    double retract_restart_extra() const;
    double retract_length_toolchange() const;
    double retract_restart_extra_toolchange() const;
    double travel_slope() const;

    bool   use_firmware_retraction() const;

private:
    // Private constructor to create a key for a search in std::set.
    Extruder(unsigned int id) : m_id(id) {}

    // Reference to GCodeWriter instance owned by GCodeWriter.
    GCodeConfig *m_config;
    // Print-wide global ID of this extruder.
    unsigned int m_id;
    // Current state of the extruder axis, may be resetted if use_relative_e_distances.
    double       m_E;
    // Current state of the extruder tachometer, used to output the extruded_volume() and used_filament() statistics.
    double       m_absolute_E;
    // Current positive amount of retraction.
    double       m_retracted;
    // When retracted, this value stores the extra amount of priming on deretraction.
    double       m_restart_extra;
    double       m_e_per_mm3;

    // BBS.
    // Create shared E and retraction data for single extruder multi-material machine
    bool          m_share_extruder;
    static std::vector<double> m_share_E;
    static std::vector<double> m_share_retracted;
};

// Sort Extruder objects by the extruder id by default.
inline bool operator==(const Extruder &e1, const Extruder &e2) { return e1.id() == e2.id(); }
inline bool operator!=(const Extruder &e1, const Extruder &e2) { return e1.id() != e2.id(); }
inline bool operator< (const Extruder &e1, const Extruder &e2) { return e1.id() < e2.id(); }
inline bool operator> (const Extruder &e1, const Extruder &e2) { return e1.id() > e2.id(); }

}

#endif
