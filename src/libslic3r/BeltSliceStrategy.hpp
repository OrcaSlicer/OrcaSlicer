#pragma once

#include "libslic3r.h"
#include "Point.hpp"
#include "BeltTransform.hpp"
#include "PrintConfig.hpp"
#include "Model.hpp"

namespace Slic3r {

// Belt printer / pre-slice transform strategy.
//
// Composes, in order, the pre-slice mesh transforms applied before slicing:
//   1. Pre-slice axis remap (standalone — works without belt mode)
//   2. Belt rotation (the sole mesh-side belt transform; shear & scale are a
//      g-code-side stage, see MachineFrameTransform)
//   3. Per-object Z-shift that lifts the mesh above the build plate
//
// Isolates this belt/remap-specific logic from the generic slicing pipeline in
// PrintObjectSlice.cpp.
class BeltSliceStrategy
{
public:
    // Apply the pre-slice remap + belt rotation + Z-shift to `trafo` in place.
    // No-op when neither a remap nor a belt rotation is configured.
    //
    // out_belt_min_z (if non-null) receives the minimum mesh Z after the
    // transforms, but only in belt-printer mode — the standalone-remap path
    // never reported it.
    static void apply_preslice_transforms(Transform3d           &trafo,
                                          const PrintConfig     &config,
                                          const ModelVolumePtrs &model_volumes,
                                          double                *out_belt_min_z = nullptr);
};

} // namespace Slic3r
