# Deterministic Vulkan slicing plan

The Vulkan backend is opt-in. It must never replace a validated CPU result
with an approximate raster/SDF result.

## Build switch

The backend is compiled only when CMake is configured with
`-DSLIC3R_ENABLE_VULKAN_SLICER=ON`. This requires Vulkan 1.2 development
headers and loader libraries. A normal OrcaSlicer build does not link Vulkan.

## Representation

All wall and infill vertices are signed 64-bit fixed-point coordinates at
1e-6 mm. A job is partitioned into 4 mm tiles and translated to the tile
origin before dispatch. This bounds 64-bit GPU intermediate values. The host
uses a 128-bit reference predicate to reject a tile that would overflow the
GPU contract.

## Perimeters

1. CPU prepares contours and a stable segment ID for each source edge.
2. Vulkan performs tile-local candidate, distance and intersection work.
3. A deterministic scan compacts candidates in source-ID order.
4. The exact polygon boolean/offset result is checked against the host
   reference before it becomes an Arachne wall path.

Adaptive wall-width (Arachne) remains CPU-authoritative until its medial-axis
and path ordering routines have equivalent fixed-point Vulkan kernels.

## Infill

1. The GPU emits fixed-point hatch/gyroid candidates by tile.
2. Candidate segments are clipped against the exact perimeter mask.
3. A stable-ID sort and an exact boundary check generate the final infill IR.
4. G-code formatting preserves this order; only file I/O remains on the CPU.

## Acceptance gate

Every GPU-enabled stage must run a differential test over the same model,
profile, layer range and seed. It is accepted only when its fixed-point wall
and infill IR exactly matches the reference, or when an explicitly reviewed
semantic change has its own golden output. Unsupported geometry, a tile-range
overflow, a missing compute queue, or a missing `shaderInt64` feature causes a
stage-local CPU fallback.
