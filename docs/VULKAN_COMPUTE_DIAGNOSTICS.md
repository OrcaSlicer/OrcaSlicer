# Vulkan compute diagnostics and tuning

The Vulkan infill path selects the strongest eligible physical device at
runtime. It requires a compute queue and `shaderInt64`, because scanline
intersections are represented as exact signed-integer rational values.

The workgroup size is clamped to the device's reported
`maxComputeWorkGroupInvocations` and `maxComputeWorkGroupSize[0]`. On the first
Vulkan initialization, the slicer creates the supported 64/128/256/512/1024
specialized pipelines, runs a warm-up plus three timestamped trials for each,
and retains the fastest result. GTX 1060 / Pascal begins with 128 as its
expected best size and RTX begins with 256, but neither is hard-coded as the
final choice. Launching one 1024-thread group can leave fewer concurrent groups
resident and is normally slower for this integer-heavy kernel. The runtime
diagnostic reports both the autotuned and hardware-maximum values.

The context keeps host-visible storage buffers, a descriptor set and a command
buffer alive between slices. NVIDIA RTX starts with 65,536 reusable requests;
GTX 1060 starts with 16,384. The buffers grow only when the detected workload
requires it, but are capped at 262,144 requests (about 20 MiB combined input
and output). An oversized single region uses the exact CPU fallback instead of
retaining a multi-gigabyte Vulkan staging allocation for the rest of the app.

## Correctness modes

On initialization, the app dispatches 2,048 signed, reversed-edge and
large-coordinate qualification vectors and compares every result to the CPU
integer reference. Live slices use sampled CPU checks (first, last and every
512th result) by default. The shader's fixed-point arithmetic is otherwise the
source of the rational intersection used by infill and support infill.

Set this environment variable before starting Orca Vulkan Slicer to compare
every live GPU intersection with the CPU reference:

```text
ORCA_VULKAN_SLICER_VALIDATION=strict
```

This audit mode preserves the same G-code behavior but deliberately retains
the CPU multiply/add work, so it is not intended for throughput measurement.

## Capturing a slice diagnostic

Set the following variable before launching the application:

```text
ORCA_VULKAN_SLICER_DIAGNOSTICS=1
```

Each GPU-assisted rectilinear or support-infill batch then writes one log line
with the submitted request count, GPU-accepted results, CPU validation checks,
GPU timestamp time and host synchronization time. The standalone
`orca-vulkan-compute-selftest` also prints device limits and the process
runtime report after it completes.

The current path accelerates exact vertical scanline intersections. Polygon
booleans, Arachne wall generation, topology-sensitive vertex cases and tree
support remain CPU algorithms; a low or high CPU graph alone is therefore not
evidence that the Vulkan path did or did not run.

## In-slice resource panel

While a slice is in progress, the progress notification samples the Orca
process every 500 ms and shows its CPU percentage normalized to all logical
cores. The second line reports only the slicer's Vulkan workload: selected
device, submitted exact-intersection batches and GPU kernel time
(`last/total`). It intentionally does not present Windows' global GPU percent,
because that value includes rendering and other applications and cannot show
whether this slicer's compute pass actually ran.

The slicing worker initializes, autotunes and exact-qualifies Vulkan before
`Print::process()` begins. Therefore the panel shows a selected GPU as
`ready` before the first eligible infill/support workload, or displays the
specific initialization failure instead of an ambiguous "not initialized".
The third line reports process working/private RAM and the bounded Vulkan
host-buffer allocation, so the rest of a large slice or G-code preview cache
is not mistaken for Vulkan memory.

After an FFF slice finishes, Orca asks `tbbmalloc` to release unused
per-thread geometry buffers. This does not discard live G-code/preview data;
it returns only allocator cache pages that otherwise remain committed after a
large slice.
# Performance policy

At slice startup the normal slicer benchmarks the active CPU's fixed-point
intersection throughput against the selected GPU's real Vulkan submission
latency. It then chooses a conservative batch crossover (never below 4,096
candidates) before dispatching exact infill/support intersections. Each
current dispatch waits for its result before final CPU topology processing, so
small batches can be slower than the CPU reference despite using the GPU.

Set `ORCA_VULKAN_SLICER_POLICY=cpu` to keep this stage on CPU or
`ORCA_VULKAN_SLICER_POLICY=gpu` to use the minimum calibrated-safe batch size
for profiling. The default policy is balanced and selects per machine.

## Tree-support contour broad phase

Tree-support topology remains CPU-authoritative: it uses exact Clipper
operations to preserve the same collision decisions and final G-code.  A
Vulkan compute pass now removes work from that CPU path for dense tree-support
jobs.  The shader compares every unique tree branch's line AABB with every
model-contour-edge AABB in parallel (one workgroup per branch).  A reported
``no overlap`` is mathematically conservative, so the CPU can skip the exact
Clipper intersection; a reported candidate is always confirmed by the existing
CPU code.  This design cannot introduce a false negative into support
generation.

The broad phase is dispatched only from 16,384 branch/edge pairs upward, where
the transfer and command-submission cost is justified.  Smaller models stay on
the CPU, and any unavailable or failed Vulkan dispatch falls back to the
unchanged CPU path.  Progress telemetry calls this stage `Tree support contour
broad phase (CPU exact confirmation)` so that waiting for a GPU batch is not
mistaken for GPU execution of the topology mutation itself.

The tree shader is packaged with the normal Vulkan resources.  Development
builds additionally look in the build resource directory, allowing a local
build to run without copying generated SPIR-V files into the source resource
tree.

## Validation snapshot (Windows, 2026-07-25)

The standalone Vulkan self-test passed on an RTX 4080 Laptop GPU (and detected
an Intel Arc adapter as a secondary eligible device):

- 1,024 exact signed-integer vertical-intersection vectors matched the CPU
  reference;
- a 64-branch x 512-contour-edge tree broad-phase batch produced the expected
  conservative candidate mask;
- a Release `OrcaSlicer_app_gui` build completed with no errors.

The link step reports one existing MSVC `LNK4098` runtime-library conflict
warning from the dependency set.  It does not prevent the executable or the
self-test from running, but it remains visible rather than being suppressed.
Throughput measurements should use a dense model: small workloads correctly
choose CPU because synchronization and host/device transfer would otherwise
make Vulkan slower.
