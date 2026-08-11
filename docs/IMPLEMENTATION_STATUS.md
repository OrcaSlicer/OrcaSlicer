# Extreme Slicer implementation status

This is a local development fork of OrcaSlicer. Its purpose is to make two
experiments independently reviewable:

- importing QIDI Studio profile bundles without renaming their files;
- building deterministic CUDA/Vulkan compute paths for expensive geometry work.

It is not yet a production replacement for OrcaSlicer.

## Implemented in this fork

### Compute backend selection

The Preferences dialog now exposes CUDA, Vulkan, and CPU (basic) modes. CUDA
is the default priority when it is compiled and available; Vulkan remains an
independent fallback, and both acceleration checkboxes may be disabled for a
pure CPU run. Batch size and strict-result-validation controls are persisted
as application settings. See `docs/EXTREME_SLICER_COMPUTE.md`.

The CUDA kernel is optional and is only compiled when the CUDA toolkit is
present. Builds without CUDA report that fact in the diagnostics panel rather
than pretending the GPU is active.

### QIDI Studio profile import

`Import Configs` accepts the QIDI Studio profile extensions `.qdscfg` and
`.qdsflmt`. They are ZIP containers, so they use the same validated extraction
and JSON-preset import path as Orca's `.orca_bundle` files.

The importer retains Orca's existing profile validation, version parsing,
overwrite prompt, substitution reporting, and user-preset save path. It does
not silently rename a QIDI profile to `.zip`.

### Vulkan capability and exact-geometry contract

The CMake option below is disabled by default and leaves the normal CPU build
unchanged.

```text
-DSLIC3R_ENABLE_VULKAN_SLICER=ON
```

When enabled, `VulkanSlicerBackend` enumerates devices and only marks a device
as suitable for deterministic tiled geometry when it exposes `shaderInt64`.
The current GPU shader remains a perimeter/infill candidate-stage accelerator;
topology-sensitive confirmation and the authoritative G-code writer remain on
the CPU. Runtime diagnostics identify the active backend and every fallback.

## Accuracy policy

The target representation is signed 64-bit fixed point with 1,000,000 units
per mm. Geometry is translated to a 4 mm tile before a GPU dispatch. A
host-side 128-bit predicate rejects a tile that would violate the GPU range.
Every emitted wall or infill item needs a stable source ID, so output order
does not depend on Vulkan workgroup scheduling.

GPU output may be used only after it exactly matches the fixed-point reference
IR for the same model, profile, layer range, and seed. A missing Vulkan
feature, unsupported geometry, validation mismatch, or tile overflow must
fall back to the current CPU stage.

## Remaining work

1. Import the supplied Q2 `.qdscfg` fixture and catalogue all fields,
   inherited QIDI presets, substitutions, and unsupported keys.
2. Implement a QIDI inheritance resolver for exports that reference QIDI
   system-only parents absent from Orca.
3. Add compiled SPIR-V and a dispatch path for the exact candidate/clip stages.
4. Integrate differential tests for walls, sparse/solid infill, supports, and
   the ordered G-code intermediate representation.
5. Add differential fixtures from a QIDI Studio installation when one is
   available; the current workspace contains no mapped QIDI installation.

## Validation status

No local build or runtime test has run in this workspace because CMake, a C++
compiler, CUDA, and the Vulkan SDK are not installed or discoverable here. The
source paths are guarded so a normal CPU build remains valid; GPU performance
claims still require a toolchain/device validation run.
