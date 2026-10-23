# Path Tracer

## Purpose

The interactive GPU path tracer provides physically based progressive rendering in both the 3D model view (Prepare) and the sliced toolpath preview (Preview). It visualizes material optics, realistic lighting, soft contact shadows, surface gloss, and volumetric transmission through translucent 3D printing filaments. When enabled in application preferences (`SETTING_OPENGL_PATHTRACE`), the path tracer renders continuously in the background during camera stillness and converges progressively across frames, providing accurate depth and material perception without stalling interactive navigation.

Quality and performance are controlled by a single preference (`SETTING_OPENGL_PATHTRACE_QUALITY`) with three presets:
- **Low:** 64 max samples, 1 indirect bounce, 4 max transmission bounces, 4 shadow hits, 4.0 ms tile budget, 25–50% dynamic orbit resolution, probe-only SSS.
- **Medium (Default):** 128 max samples, 2 indirect bounces, 8 max transmission bounces, 8 shadow hits, 6.0 ms tile budget, 30–65% dynamic orbit resolution, probe + backlight diffusion SSS.
- **High:** 256 max samples, 2 indirect bounces, 12 max transmission bounces, 8 shadow hits, 8.0 ms tile budget, 35–75% dynamic orbit resolution, full ray-marched backlight transmission SSS.

## Acceleration structure (TLAS / BLAS)

Scene geometry is organized in a two-level bounding volume hierarchy consisting of a top-level acceleration structure (TLAS) over instance bounds and bottom-level acceleration structures (BLAS) for individual meshes or toolpath groups:

- **BLAS (Bottom-Level BVH):** Built asynchronously using a Surface Area Heuristic (SAH) with binned splitting on a background worker thread (`_build_pathtrace_scene_async`). Node data is packed into 32-byte GPU nodes (`PathTraceGpuNode`) flattened in breadth-first order so that the two children of each inner node occupy contiguous array indices (`[child, child + 1]`). BLAS building accepts an atomic cancellation token (`cancel_token`) to abort promptly when the camera or scene geometry changes.
- **TLAS (Top-Level BVH):** Constructed on the main thread over instance world-space axis-aligned bounding boxes (AABBs). In scenes with four or fewer instances, rays test instances directly with minimal overhead; in scenes with more than four instances, a binary top-down bounding volume hierarchy (`TlasBuffer`, binding 4) is traversed logarithmically with a fixed-size traversal stack and distance-ordered child visiting.
- **Identity Transform Fast-Path:** Instances whose transform is an identity matrix (marked via instance metadata) bypass local ray transformation and normal transformation, reducing register pressure and memory bandwidth in both primary and shadow traversal.
- **Deferred Normal Reconstruction:** Triangle intersection records only the closest hit primitive index without evaluating edge cross-products or vertex transformations. Geometry normals and edge-length logic are reconstructed once per ray after traversal completes.
- **Buffer Layout:** Geometry is uploaded to OpenGL Shader Storage Buffer Objects (SSBOs):
  - Binding 0: Node buffer (`NodeBuffer`)
  - Binding 1: Triangle buffer (`TriangleBuffer`)
  - Binding 2: Instance buffer (`InstanceBuffer`) with local-to-world and world-to-local transformations
  - Binding 3: Shading metadata (`ShadingBuffer`) containing linear albedos, layer heights, and extrusion roles
  - Binding 4: Top-level acceleration structure (`TlasBuffer`)

Transformations of objects or plates update only the instance buffer without triggering a BLAS rebuild. Color changes in Prepare or Preview update vertex shading buffers in-place via `glBufferSubData` without rebuilding either hierarchy, triggering triangle buffer uploads only when opacity values change.

## Frame scheduling and interactivity

To preserve high UI responsiveness (≥ 60 FPS) during scene interaction and convergence:
- **Instant Full-Frame Preview Pass:** Sample 0 executes as a low-overhead full-frame pass (`is_interactive_pass = true`), providing instant visual feedback on camera movement without stale tile boundaries.
- **Interleaved Tile Scheduler:** From sample 1 onward, progressive accumulation dispatches fixed 128×128 tiles within a dynamic GPU frame budget (4–8 ms). Tiles are dispatched in a quasi-random Golden-Ratio order ($(\text{cursor} \cdot \text{stride}) \pmod{\text{total\_tiles}}$) that distributes compute load evenly across sky and dense object geometry, preventing latency spikes.
- **Dynamic Orbit Scaling:** During active camera rotation or dragging, resolution dynamically scales to meet a 14 ms target budget (`m_pathtrace_dynamic_orbit_scale`), switching to an orbit fast-path that bounds transmission steps and substitutes analytical wrap-shading for costly secondary SSS and GI bounces.
- **ImGui Decoupling & Scene Caching:** Active ImGui widget adjustments (layer slider, palette controls) suspend compute dispatches and blit the existing display image to maintain maximum UI framerates. Once `max_samples` is reached, `GLCanvas3D::_is_scene_cacheable()` activates `SceneCache`, reducing idle GPU usage to 0%.

## G-code toolpath meshing

In Preview, G-code extrusion toolpaths are extracted into triangle geometry tailored by extrusion role:
- Extrusions use a memory-efficient 4-quad prismatic tube mesh with mitered joints between adjacent segments of identical role on the same layer.
- **Analytical Layer Profile:** In Preview, `layer_profile_mode` automatically modulates surface normals analytically on the 4-quad geometry, rendering smooth cylindrical bead profiles and layer ridges without the memory cost of subdivided meshes.
- Inner perimeters, infill, and solid decks emit compact upper bevels or planar ribbons to keep traversal depth low.
- Seam markers are generated as diamond octahedra exempt from shadow casting and subsurface diffusion.
- Height and move clipping (`z_min`, `z_max`, `move_min`, `move_max`) cull primitives both during ray-box intersection tests and inside leaf intersection routines.

## Physical transmittance and material optics

Surface optics model dielectric boundaries and volumetric media using physical principles:
- **Dielectric Reflection:** Surface reflectance is governed by the Schlick-Fresnel approximation $F = F_0 + (1 - F_0)(1 - \cos\theta)^5$ with plastic base reflectance $F_0 = 0.04$. Specular highlights use a normalized Blinn-Phong lobe with energy conservation, and primary diffuse illumination is scaled by $(1 - F)$. When realistic gloss is enabled, highlights remain clean and untinted across all extrusion paths.
- **Spectral RGB Beer-Lambert Transmittance:** Translucent filaments evaluate channel-specific exponential absorption $T = \text{pow}(T_{\text{ref}}, d / d_{\text{ref}})$ based on reference wall thickness (0.4 mm). Dominant filament pigments transmit more strongly while complementary wavelengths absorb, matching optical color saturation and depth identically across solid meshes in Prepare and extrusion beads in Preview.
- **True Internal Shadows:** Rays traversing beyond the initial translucent boundary evaluate real shadow transmission through interior infill and geometry rather than clamping to ambient values, correctly rendering occluded internal shapes and shadowed back walls.
- **Automatic Subsurface Scattering (SSS):** The SSS mode (Off, Subtle, Medium, Strong) is automatically derived per extruder from filament presets (`filament_type`), opacity, and luminance (`infer_extruder_sss_mode`). Carbon/glass/wood filled or very dark filaments disable SSS, nylon/polypropylene/wax enable strong scattering, PETG/TPU/PC use medium scattering, and PLA/PHA use subtle scattering. Shading parameters pack SSS mode together with surface roughness, driving tangent-disk diffusion and thin-wall forward transmission on the GPU.

## Shadows, GI, and compositing

- **Low-Discrepancy Sampling:** Primary lighting, GI hemispheres, and SSS disks sample a Cranley-Patterson rotated 2D R2 sequence (`sample_ld_2d`) paired with disk-projected key-light sampling (`sample_key_light_dir`), producing clean contact penumbras and smooth gradients without noise clumping.
- **Indirect Color Bleeding (GI):** Secondary cosine-weighted hemisphere rays evaluate combined key, fill, and hemisphere irradiance with distance attenuation and firefly clamping, producing realistic diffuse color bleeding between adjacent objects and onto the build plate.
- **Single-Pass Alpha Compositing:** Progressive samples accumulate in a 32-bit float image (`accumulation_image`), apply highlight-compressing tone mapping and sRGB conversion, and composite over the rasterized print bed in a single pass (`display_image`), preserving bed markings inside colored and ambient shadows.
