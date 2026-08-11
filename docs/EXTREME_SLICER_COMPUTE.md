# Extreme Slicer compute backends

Extreme Slicer exposes three mutually exclusive compute modes:

* **CUDA** (default): uses the exact int64 CUDA kernel when the build contains
  the CUDA toolkit and a device is available. If CUDA cannot initialize, the
  worker may fall through to Vulkan when Vulkan is enabled.
* **Vulkan**: uses the existing shader-int64 path and keeps topology-sensitive
  confirmation on the CPU.
* **CPU (basic)**: disables both GPU dispatch paths and uses the established
  exact CPU implementation.

The two backend checkboxes are independent. Turning both off is a supported
CPU-only configuration. `GPU batch size` defaults to 64, which avoids the
small-batch latency seen on Pascal/GTX 1060 systems while still keeping GPU
occupancy high on larger GPUs. `Strict GPU result validation` recomputes every
result with the wide-integer CPU reference; sampled validation is the normal
performance setting.

## Build options

Vulkan remains optional:

```text
-DSLIC3R_ENABLE_VULKAN_SLICER=ON
```

CUDA is optional and requires a CUDA toolkit with `nvcc` and the runtime:

```text
-DSLIC3R_ENABLE_CUDA_SLICER=ON
```

When CUDA is not compiled, the UI still shows CUDA mode and the diagnostic
panel reports that the backend is unavailable; it never prevents the CPU or
Vulkan paths from running.

## QIDI tool mapping

QIDI profiles may use virtual filament ids for multi-colour jobs. Extreme
Slicer keeps those ids for purge/material bookkeeping but emits the mapped
physical extruder (`T0`, `T1`, …) in QIDI tool-change lines. This prevents a
virtual filament id from selecting a non-existent physical slot. The bundled
QIDI profiles are used as read-only references; no QIDI Studio installation is
modified by the build.
