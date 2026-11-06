#ifndef slic3r_MachineKinematics_hpp_
#define slic3r_MachineKinematics_hpp_

#include "../Point.hpp"

namespace Slic3r {

// The frame contract for emitted movement.
//
// GCodeWriter produces points in the *logical placed* frame: plate offsets have
// already been subtracted, but no machine-specific mapping has been applied.
// A MachineKinematics turns that into the coordinates actually written to
// G-code, and answers the two structural questions the writer needs in order to
// decide which axis words it may omit.
//
// This is a seam for writer-generated movement only.  Start/end/custom G-code,
// classic wipe-tower output and GCodeWriter::extrude_arc_to_xy() do NOT pass
// through it: they write machine coordinates directly.
class MachineKinematics
{
public:
    virtual ~MachineKinematics() = default;

    // Logical placed point -> emitted machine point.
    virtual Vec3d to_machine(const Vec3d &p) const = 0;

    // True when a move must emit X, Y and Z because omitting a word would be
    // wrong under this mapping.  Deliberately not called "couples_axes": a pure
    // axis permutation forces full emission without physically coupling axes.
    virtual bool must_emit_all_axes() const = 0;

    // True when a lift must be suppressed while the current position is unknown,
    // because _travel_to_z() re-emits the logical X/Y through this mapping and an
    // uninitialised position would map to a bogus machine point -- for a reverse
    // mapping, the far corner of the bed.
    virtual bool suppress_lift_at_unknown_position() const = 0;

    // True when a G2/G3 arc in the logical XY plane is still the same arc in the
    // machine frame. Arc moves emit only X, Y, I and J, so this asks a narrower
    // question than must_emit_all_axes(): whether logical X and Y reach the
    // machine unchanged. A mapping that only negates or reverses Z keeps its
    // arcs; one that permutes X or Y moves the arc out of the plane that I/J
    // describes, and a shear turns the circle into an ellipse G2/G3 cannot
    // express at all.
    virtual bool supports_arc_moves() const = 0;

    // Configuration.  GCodeWriter forwards its setters here so that the state
    // lives with the strategy and a strategy installed before the setters run
    // still receives it.
    virtual void set_axis_remap(int rx, int ry, int rz) = 0;
    virtual void set_build_volume_max(const Vec3d &max) = 0;
};

// Axis remap only -- the historical GCodeWriter behaviour, moved verbatim.
//
// The remap encodes, per output axis, which source axis feeds it and how:
//   r < 3 : source axis r, unchanged
//   r < 6 : source axis r-3, negated
//   else  : source axis r-6, reversed within the build volume
class CartesianKinematics : public MachineKinematics
{
public:
    Vec3d to_machine(const Vec3d &p) const override;

    bool must_emit_all_axes() const override { return this->has_axis_remap(); }
    bool suppress_lift_at_unknown_position() const override { return this->has_axis_remap(); }

    // X and Y must reach the machine untouched. Because the remap is a
    // permutation, pinning those two also pins Z to Z, so a mapping that only
    // negates or reverses Z still supports arcs -- every word a G2/G3 emits is
    // unchanged by it.
    bool supports_arc_moves() const override { return m_remap_x == 0 && m_remap_y == 1; }

    void set_axis_remap(int rx, int ry, int rz) override
        { m_remap_x = rx; m_remap_y = ry; m_remap_z = rz; }
    void set_build_volume_max(const Vec3d &max) override { m_build_vol_max = max; }

    bool has_axis_remap() const
        { return m_remap_x != 0 || m_remap_y != 1 || m_remap_z != 2; }

protected:
    Vec3d apply_axis_remap(const Vec3d &pos) const;

    int   m_remap_x { 0 };
    int   m_remap_y { 1 };
    int   m_remap_z { 2 };
    Vec3d m_build_vol_max { Vec3d::Zero() };
};

} // namespace Slic3r

#endif // slic3r_MachineKinematics_hpp_
