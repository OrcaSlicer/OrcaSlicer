#pragma once

#include "libslic3r.h"
#include "Point.hpp"
#include "BeltTransform.hpp"
#include "PrintConfig.hpp"
#include "Model.hpp"

namespace Slic3r {

// Belt printer pre-slice transform strategy.
//
// Composes, in order, the mesh transforms applied before slicing on a belt printer:
//   1. Belt rotation (the sole mesh-side belt transform; shear & scale are a
//      g-code-side stage, see MachineFrameTransform)
//   2. Per-object Z-shift that lifts the mesh so its slicing frame starts at the
//      belt below its footprint
//
// Isolates this belt-specific logic from the generic slicing pipeline in
// PrintObjectSlice.cpp.
class BeltSliceStrategy
{
public:
    // Apply the belt rotation + Z-shift to `trafo` in place.  No-op when no belt
    // rotation is configured.
    //
    // out_belt_min_z (if non-null) receives the minimum mesh Z after the transforms.
    static void apply_preslice_transforms(Transform3d           &trafo,
                                          const PrintConfig     &config,
                                          const ModelVolumePtrs &model_volumes,
                                          double                *out_belt_min_z = nullptr);
};

} // namespace Slic3r
