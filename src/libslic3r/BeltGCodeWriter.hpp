#pragma once

#include "GCodeWriter.hpp"
#include "GCode/BeltBackTransform.hpp"
#include "GCode/MachineFrameTransform.hpp"

namespace Slic3r {

class FirstLayerPlane;

// Belt-printer-specific GCode writer.
//
// Inherits from GCodeWriter and overrides movement methods to apply
// coordinate transformation (back-transform, axis remap, machine-frame
// transform) and emit coupled XYZ moves (Y and Z are coupled due to belt tilt).
class BeltGCodeWriter : public GCodeWriter
{
public:
    BeltGCodeWriter() : GCodeWriter() {}

    // Belt configuration (axis remap is inherited from GCodeWriter)
    void set_belt_back_transform(const PrintConfig &config);
    void set_machine_frame_transform(const PrintConfig &config);
    Vec3d to_machine_coords(const Vec3d &pos) const;

    // World-coordinates mode: incoming coordinates are treated as points
    // relative to the physical belt surface (X across, Y along the belt,
    // Z height above it) instead of slicing-frame coordinates — the
    // slicer->world back-transform is skipped. Used by the PA line / PA
    // pattern calibration generators, whose logical bed coordinates describe
    // first-layer drawings on the build surface.
    void set_world_coordinates(bool enable) { m_world_coordinates = enable; }

    // First-layer plane: when set to a non-null active evaluator, travel
    // speed selection consults the plane per-move and uses
    // initial_layer_travel_speed for points within first_layer_height_mm
    // of the plane (regardless of slicing layer index).
    void set_first_layer_plane(const FirstLayerPlane *plane,
                               double first_layer_height_mm) {
        m_first_layer_plane         = plane;
        m_first_layer_thickness_mm  = first_layer_height_mm;
    }

    // Overridden movement methods
    std::string travel_to_xy(const Vec2d &point, const std::string &comment = std::string()) override;
    std::string travel_to_xyz(const Vec3d &point, const std::string &comment = std::string(), bool force_z = false) override;
    std::string extrude_to_xy(const Vec2d &point, double dE, const std::string &comment = std::string(), bool force_no_extrusion = false) override;
    std::string extrude_to_xyz(const Vec3d &point, double dE, const std::string &comment = std::string(), bool force_no_extrusion = false) override;
    std::string lazy_lift(LiftType lift_type = LiftType::NormalLift, bool spiral_vase = false) override;
    std::string eager_lift(const LiftType type) override;

protected:
    std::string _travel_to_z(double z, const std::string &comment) override;

private:
    BeltBackTransform     m_belt_back_transform;
    MachineFrameTransform m_machine_frame_transform;
    bool                  m_world_coordinates = false;
    // Borrowed pointer; lifetime owned by GCode.  null = inactive.
    const FirstLayerPlane *m_first_layer_plane = nullptr;
    double          m_first_layer_thickness_mm = 0.;
};

} // namespace Slic3r
