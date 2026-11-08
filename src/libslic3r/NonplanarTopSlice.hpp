// UNLAYERED_MOD: nonplanar layers -- the slice-level part (see NonplanarTop.hpp). Included only by the .cpp
// files that call it, so this header can change without a full rebuild.
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace Slic3r {

class PrintObject;

// Z contouring beside curved shells (Layer::make_contour_z): a planar path is never lowered into shell material
// printed before it, nor raised where a shell prints over or beside it later.
class NonplanarZaaLimits
{
public:
    virtual ~NonplanarZaaLimits() = default;
    // At (x, y) (slice frame, mm) of the layer with index layer_id (print_z): z_floor = the top of the shell material
    // printed before that layer within reach of the nozzle (-1e30 if none), no_raise = a shell printed with or after
    // that layer lies within reach, at most 0.5 mm over print_z. Returns false when no shell is near.
    virtual bool query(size_t layer_id, double print_z, double x, double y, double &z_floor, bool &no_raise) const = 0;
    // Under a curved band printed with or after layer layer_id: z = the band's bottom (S - T) at (x, y), the surface the
    // planar part is Z-contoured down to, exactly as a top surface is. False where no shell bead covers (x, y).
    virtual bool band_bottom(size_t layer_id, double x, double y, double &z) const = 0;
};
// nullptr when the object has no curved shells.
std::shared_ptr<const NonplanarZaaLimits> nonplanar_top_zaa_limits(const PrintObject &object);

// Right after slicing, before perimeters and infill: finds the object's curved top surface, checks it is ONE
// top surface the shell can follow (otherwise returns why not, and the object prints planar), builds the
// curved shells (kept aside for make_nonplanar_top) and clips each layer's slices so the planar part is the
// model as if its top were the shell's thickness lower. Returns "" when nonplanar layers apply or are off.
// `skipped_tops`, when given, receives the number of tops whose band failed to fill and was dropped, each keeping its
// planar top surface instead (the rest of the object is sliced as usual).
std::string nonplanar_top_clip_slices(PrintObject &object, size_t *skipped_tops = nullptr);

// After detect_surfaces_type(): the planar tops under the shells are internal solid, not top (no top shells,
// no ironing under a shell).
void nonplanar_top_retype_surfaces(PrintObject &object);
// After bridge_over_infill(): solid and bridge infill in a shell's shadow goes back to sparse (bottom shells kept).
void nonplanar_top_shell_authority(PrintObject &object);

} // namespace Slic3r
