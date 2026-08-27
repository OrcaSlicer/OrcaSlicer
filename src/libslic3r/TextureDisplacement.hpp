#ifndef slic3r_TextureDisplacement_hpp_
#define slic3r_TextureDisplacement_hpp_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/array.hpp> // view_project_matrix is a std::array<float, 12>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <array>

#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "TriangleSelector.hpp"

namespace Slic3r {

class ModelVolume;

// Bits of subdivide_mesh_adaptive()'s per-triangle `refine_region` mask. See that function.
static constexpr uint8_t REFINE_PAINTED = 1;
static constexpr uint8_t REFINE_BORDER  = 2;

// Progress/cancellation hook for the (potentially multi-second) bake. Called with a 0..100
// percentage; return false to abort. See build_texture_displacement().
using DisplacementProgressFn = std::function<bool(int)>;

// Maximum number of simultaneous texture-displacement layers a single ModelVolume can hold.
// Each layer owns its own paint mask (ModelVolume::texture_displacement_facet(slot)), so this
// is also the number of independent EnforcerBlockerType selectors kept per volume.
static constexpr size_t TEXTURE_DISPLACEMENT_MAX_LAYERS = 8;

// How a layer's height texture is sampled outside its [0, 1) tile when tiling is enabled. Ignored
// (always clamp) when TextureDisplacementLayer::tile_enabled is false.
enum class TextureTileMethod : int
{
    Repeat = 0,         // wrap around, tile i and tile i+1 are identical (default)
    MirroredRepeat = 1, // wrap around, every other tile is mirrored (no visible seam at tile edges)
};

// How a layer's texture is mapped onto the mesh.
enum class TextureProjectionMethod : int
{
    // Standard *blended* tri-planar projection: the texture is sampled once per world axis (the
    // XY, XZ and YZ planes) and the three samples are blended per vertex, weighted by that
    // vertex's own normal raised to TRIPLANAR_BLEND_SHARPNESS.
    //
    // Earlier versions instead *hard-picked* the single axis most aligned with the normal. That
    // has a real, visible failure mode at any edge where the dominant axis flips: on a +X face the
    // planar coordinate is (y, z), on a -Y face it is (x, z), so at the shared edge u jumps from
    // y_edge to x_edge. On a box centred near the origin those two agree at the (+,+) and (-,-)
    // corners (making them look fine) but differ by the full corner width at the (+,-) and (-,+)
    // corners, which is exactly the "two bad corners, two good ones" seam that was reported.
    // Blending across the transition removes that hard discontinuity by construction.
    Triplanar   = 0,
    // Wrapped around an axis running through the patch's centroid. The axis itself is picked
    // automatically as the world axis *least* aligned with the patch's average normal (since a
    // cylinder's own axis is perpendicular to its outward radial normal) - a reasonable default
    // for roughly cylindrical selections, not a precise fit for arbitrary geometry.
    Cylindrical = 1,
    // Wrapped around the patch's centroid using longitude/latitude - reasonable for roughly
    // spherical/rounded selections, again an approximation rather than an exact geodesic map.
    Spherical   = 2,
    // Real UV unwrap of the painted patch - a proper low-distortion flattening rather than a
    // planar/cylindrical/spherical approximation. The patch is first cut into charts along its
    // sharp edges and each chart is flattened on its own (see compute_patch_unwrap()), so a patch
    // that is not a single developable surface still unwraps sensibly. Falls back to Triplanar for
    // any chart that cannot be flattened at all.
    LSCM        = 3,
    // Flat projection along a fixed direction captured from the 3D camera ("project from view"): the
    // texture is laid onto the painted area as seen from that angle, like a decal projector. Single
    // planar map (no per-face axis switch), so it can smear on faces turned away from the projector -
    // that is inherent to view projection and is the user's call, not a bug. The projector's two
    // in-plane axes live in TextureDisplacementLayer::view_project_right/up.
    ViewProjected = 4,
};

// Dihedral angle (degrees) above which an edge between two painted triangles becomes a chart seam
// - i.e. the unwrap is cut there rather than being forced to flatten across it.
//
// The whole point of this being a threshold rather than "flatten everything as one piece": three
// faces meeting at a cube corner are not developable, so a single-chart solve has to distort them
// badly to lie flat (they splay out into a fan, which is what "it merges all the edges into a
// triangle" describes). Cutting at the 90-degree edges instead lets each face flatten exactly.
// Meanwhile a smoothly curved surface - a subdivided sphere, say - has only small angles between
// neighbouring triangles, stays a single chart, and unwraps as one piece the way it should.
static constexpr float LSCM_DEFAULT_SEAM_ANGLE_DEG = 30.f;

// Exponent the tri-planar blend weights are raised to (see TextureProjectionMethod::Triplanar).
// Higher means a tighter, more "hard-edged" transition between the three axis projections; lower
// means a wider cross-fade. 4 is the usual default: tight enough that a flat face is sampled
// almost purely along its own axis, wide enough that a 90-degree edge has no visible hard seam.
static constexpr float TRIPLANAR_BLEND_SHARPNESS = 4.f;

// How a layer's displacement combines with the displacement accumulated by the layers below it
// (i.e. those in lower slots), evaluated per vertex. Analogous to an image editor's layer blend
// modes, except the quantity being blended is a signed displacement distance in mm rather than a
// pixel value.
//
// Add/Subtract are in mm and need no further explanation. Multiply/Divide are *scaling* operations
// and therefore need a unit convention: they treat the layer's own value as a unitless factor
// relative to 1 mm. That makes `depth_mm` act as a gain - a layer with depth 1 mm and a white
// (1.0) texel multiplies the accumulated relief by exactly 1, i.e. leaves it unchanged - which is
// the behaviour that makes a Multiply layer usable as a mask over the layers beneath it.
enum class TextureBlendMode : int
{
    Add      = 0, // acc + value          (default; several layers pile their relief up together)
    Subtract = 1, // acc - value          (carve this layer's relief out of the layers below)
    Multiply = 2, // acc * (value / 1mm)  (mask/modulate the layers below by this layer)
    Divide   = 3, // acc / (value / 1mm)  (inverse mask; guarded against a zero/near-zero divisor)
};

// Combines one layer's signed displacement `value` (mm) into `accumulated` (mm) per `mode`.
// Shared by the bake/preview path and exposed for tests.
float blend_displacement(float accumulated, float value, TextureBlendMode mode);

// Where one unwrap island (chart) sits in UV space, on top of wherever compute_patch_unwrap() first
// packed it. This is what the UV editor's drag/rotate gestures write to, so a user can lay the
// islands out by hand - move them, rotate them, overlap them - rather than being stuck with the
// automatic packing.
//
// Indexed by chart id, which compute_patch_unwrap() assigns in first-encountered-triangle order. That
// is stable for a given patch and seam angle, but *not* across a change to either: repainting the
// patch, or moving the seam-angle slider, can renumber the charts and so leave a hand-placed island
// applied to a different one. Accepted deliberately - the alternative is a persistent chart identity
// that survives arbitrary re-segmentation, which is a much larger problem than this feature warrants.
struct TextureIsland
{
    Vec2f offset       = Vec2f::Zero(); // in the unwrap's own mm space
    float rotation_deg = 0.f;           // about the island's own centroid
    // About the island's own centroid too. 1 = the size compute_patch_unwrap() gave it, which is
    // already its true surface area in mm - so scaling an island away from 1 deliberately makes its
    // texel density differ from its neighbours'. See average_island_scales().
    float scale = 1.f;

    template<class Archive> void serialize(Archive &ar) { ar(offset, rotation_deg, scale); }
};

// Sets every island's scale to the mean of the current ones (Blender's "Average Islands Scale").
// Only meaningful after islands have been scaled by hand: compute_patch_unwrap() already sizes every
// chart to its true mm area, so a freshly unwrapped patch has uniform texel density to begin with.
void average_island_scales(std::vector<TextureIsland> &islands);

// One texture asset plus its projection/displacement parameters. Several layers may be painted
// onto overlapping areas of the same volume: their displacements are combined per vertex, in slot
// order, each layer folding into the total via its own TextureBlendMode (see
// build_texture_displacement()). This is what "layered/blended" texture displacement means here.
struct TextureDisplacementLayer
{
    // Index into ModelVolume::texture_displacement_facets, assigned once when the layer is
    // created. Not reused for the lifetime of the ModelVolume, so a deleted layer's slot simply
    // becomes unused rather than being handed to a different layer.
    int slot = -1;

    std::string name;
    // Path on the local filesystem the image was loaded from (informational; may be stale or
    // empty, e.g. after loading a .3mf on a different machine).
    std::string path;
    // Path inside the .3mf archive once saved (empty until the project is saved once).
    std::string path_in_3mf;
    // Raw encoded image bytes. Only 8-bit grayscale PNG is understood by decode_height_texture()
    // (libslic3r has no GUI image toolkit available); the GUI converts any imported image to that
    // format before storing it here, so the baking code never needs to depend on wxWidgets.
    std::shared_ptr<std::vector<unsigned char>> image_data;

    float depth_mm     = 0.4f; // maximum displacement along the surface normal, in mm
    float tiling_scale = 10.f; // size of one texture tile, in mm
    float rotation_deg = 0.f;
    Vec2f offset       = Vec2f::Zero();
    bool  invert       = false;

    // The height value that means "don't move this vertex". The sampled height (0..1) has this
    // subtracted before being scaled by depth_mm, so with the default of 0 the surface only ever
    // moves *outwards* (the classic height-map convention), while 0.5 makes mid-grey neutral and
    // lets darker texels cut *into* the surface - an engraved-and-embossed result from one map.
    //
    // Cutting inward is not free: vertices move along their own normals, which converge inside a
    // concave corner and inside a thin wall, so a large depth_mm against a small feature really can
    // fold the surface through itself. There is no cheap way to detect that here (it needs a full
    // self-intersection test on the displaced mesh), so the GUI warns rather than promising safety.
    float midlevel = 0.f;

    // Optional blur applied to the decoded height map before it is sampled, in [0, 1]: 0 is the raw
    // texture, 1 the strongest blur. Softens the relief (rounds hard edges, removes speckle) without
    // needing a pre-blurred source image. Applied in decode_height_texture(), so it feeds the true
    // preview, the UV editor backdrop and the bake identically.
    float smoothing = 0.f;

    // Optional feathering of the displacement toward the edge of the painted patch. When enabled, the
    // displacement is scaled down as a vertex approaches the patch boundary, so the relief blends
    // smoothly into the surrounding surface instead of ending abruptly. `edge_smoothing_amount` in
    // (0, 1] sets how far the fade reaches into the patch: small values only soften a thin band at the
    // very edge, 1 fades the whole patch to nothing (the painted face comes out flat). Off by default.
    bool  edge_smoothing        = false;
    float edge_smoothing_amount = 0.5f;

    // Only used by TextureProjectionMethod::LSCM: when set, a fresh unwrap is laid out as a connected
    // net (adjacent charts unfolded edge-to-edge along a spanning tree, see compute_connected_net())
    // rather than as separately packed islands. On by default. Hand-moving an island overrides its
    // placement until the next re-unwrap.
    bool  auto_connect_islands  = true;

    // When false, the texture is sampled once (clamped to its edge pixels outside [0, 1)) instead
    // of being repeated - useful for a single decal-like placement rather than a repeating tile.
    bool              tile_enabled = true;
    TextureTileMethod tile_method  = TextureTileMethod::Repeat;

    TextureProjectionMethod projection_method = TextureProjectionMethod::Triplanar;
    // Only used by TextureProjectionMethod::LSCM. See LSCM_DEFAULT_SEAM_ANGLE_DEG.
    float                   lscm_seam_angle_deg = LSCM_DEFAULT_SEAM_ANGLE_DEG;
    // Only used by TextureProjectionMethod::LSCM: gap left between islands by the automatic packing,
    // in the unwrap's mm space. Negative means "auto" (a small fraction of the packed size), which is
    // what a patch that has never had the slider touched gets.
    float                   island_padding_mm = -1.f;
    // Only used by TextureProjectionMethod::LSCM: edges the unwrap is forced to cut along, on top of
    // whatever the seam angle already cuts. Each pair is an undirected edge in *mesh vertex index*
    // space (first < second). This is what "mark seam" (manual) and "cut island" (auto) both write to.
    // Mesh-index space, so like the paint masks these are dropped on any topology change.
    std::vector<std::pair<int, int>> lscm_seam_edges;

    // Only used by TextureProjectionMethod::ViewProjected: the projector's in-plane axes, in the
    // volume's *local* space, captured from the camera when the user hits "Project from view". A point
    // projects to Vec2f(dot(pos, right), dot(pos, up)) before the usual tiling/rotation/offset.
    Vec3f view_project_right = Vec3f::UnitX();
    Vec3f view_project_up    = Vec3f::UnitY();

    // Also ViewProjected, and takes precedence over the two axes above when set: an exact *projective*
    // map from a local-space position straight to a texture uv, written by the projection-frame overlay
    // (the semi-transparent window dragged over the 3D view - its border becomes the uv unit square).
    //
    // Row-major 3x4, applied to the homogeneous point p~ = (x, y, z, 1):
    //     uv = ( row0.p~ / row2.p~ , row1.p~ / row2.p~ )
    // The perspective divide is the whole point. view_project_right/up can only express an *affine*
    // projection, which matches an orthographic camera exactly but not a perspective one - under
    // perspective the near end of a part projects larger than the far end, and no pair of axes
    // reproduces that. Folding the camera's full projection*view*model product into one matrix does.
    // Because a point behind the projector has row2.p~ <= 0 and no meaningful uv, sampling must check
    // the sign rather than divide blindly; see project_uv_projective().
    //
    // Note this map already includes placement, so the usual tiling/rotation/offset transform is NOT
    // applied on top of it - the window's own position and size are the placement.
    bool                  view_project_projective = false;
    std::array<float, 12> view_project_matrix{};
    // Only used by TextureProjectionMethod::LSCM: hand placement of the unwrap's islands, indexed by
    // chart id (see TextureIsland). Shorter than the chart count simply means the missing ones are
    // still where the automatic packing put them.
    std::vector<TextureIsland> islands;

    // Only used by TextureProjectionMethod::LSCM: persistent "join" groups, indexed by chart id. Charts
    // that share a group id move together as one in the UV editor - this is what the explicit "Join"
    // command records (over and above placing the child next to its parent). An entry of -1, or an index
    // past the end of the vector, means the chart is its own singleton group (moves alone). Empty means
    // every chart is a singleton. Same chart-renumbering caveat as `islands`: a re-unwrap can reshuffle
    // chart ids, so this is meaningful only against the unwrap it was made on.
    std::vector<int> island_groups;

    // Only used by TextureProjectionMethod::LSCM: manual per-vertex UV edits made in the UV editor's
    // Vertex/Edge select modes. Each pair is (mesh vertex index, its overriding raw-unwrap coordinate in
    // mm) - the *raw* unwrap position, i.e. before the island transform, so the edited vertex still
    // moves and rotates with its island. In compute_lscm_uvs() this replaces the automatic unwrap
    // coordinate for that vertex; in the editor it edits the displayed geometry directly. Keyed in mesh-
    // vertex space like lscm_seam_edges (dropped on a topology change). The raw coordinate is only
    // meaningful against the current unwrap, so a re-unwrap clears these. A mesh vertex shared by several
    // charts (a seam vertex) settles on one, matching compute_lscm_uvs()'s single-UV-per-vertex rule.
    std::vector<std::pair<int, Vec2f>> lscm_uv_overrides;

    // How this layer folds into the displacement accumulated by the layers below it. Ignored for
    // the lowest-slot painted layer, which has nothing beneath it to combine with (the GUI shows
    // it as the "Base" layer and hides the control).
    TextureBlendMode blend_mode = TextureBlendMode::Add;

    bool empty() const { return !image_data || image_data->empty(); }

    template<class Archive> void save(Archive &ar) const
    {
        std::string blob = image_data ? std::string(image_data->begin(), image_data->end()) : std::string();
        ar(slot, name, path, path_in_3mf, blob, depth_mm, tiling_scale, rotation_deg, offset, invert, tile_enabled,
           static_cast<int>(tile_method), static_cast<int>(projection_method), lscm_seam_angle_deg, islands,
           static_cast<int>(blend_mode), midlevel, island_padding_mm, lscm_seam_edges, view_project_right,
           view_project_up, smoothing, edge_smoothing, edge_smoothing_amount, auto_connect_islands, island_groups,
           lscm_uv_overrides, view_project_projective, view_project_matrix);
    }
    template<class Archive> void load(Archive &ar)
    {
        std::string blob;
        int         tile_method_int       = 0;
        int         projection_method_int = 0;
        int         blend_mode_int        = 0;
        ar(slot, name, path, path_in_3mf, blob, depth_mm, tiling_scale, rotation_deg, offset, invert, tile_enabled,
           tile_method_int, projection_method_int, lscm_seam_angle_deg, islands, blend_mode_int, midlevel,
           island_padding_mm, lscm_seam_edges, view_project_right, view_project_up, smoothing, edge_smoothing,
           edge_smoothing_amount, auto_connect_islands, island_groups, lscm_uv_overrides, view_project_projective,
           view_project_matrix);
        image_data        = blob.empty() ? nullptr : std::make_shared<std::vector<unsigned char>>(blob.begin(), blob.end());
        tile_method       = static_cast<TextureTileMethod>(tile_method_int);
        projection_method = static_cast<TextureProjectionMethod>(projection_method_int);
        blend_mode        = static_cast<TextureBlendMode>(blend_mode_int);
    }
};

// Settings that apply to the whole layer stack rather than to one layer, held per ModelVolume next
// to texture_displacement_layers and consumed by build_texture_displacement().
struct TextureDisplacementOptions
{
    // Whether the painted patch's *border* vertices - the ones also used by unpainted triangles -
    // are displaced along with the rest, or pinned flat.
    //
    // Pinning them was originally justified as keeping the patch from tearing away from the
    // surrounding surface. That reasoning no longer applies: since the bake became
    // topology-preserving it only ever *moves* the input's own vertices, so a border vertex is one
    // vertex shared by both regions and moving it simply tilts the unpainted triangles that use it -
    // nothing can come apart. What pinning actually does is clamp the outermost ring of the relief to
    // zero, which on a fully painted face collapses the pattern into a ring of steep ramps right at
    // the edge (the "it doesn't extrude at the border" artifact). Displacing it is the default;
    // pinning is kept for the case where the relief must not spill past the paint at all.
    bool displace_border = true;

    // Optional Laplacian relaxation of the displaced surface, run after all layers have been folded
    // in - a post-process, not a texture filter (TextureDisplacementLayer::smoothing blurs the height
    // map instead, before it is ever sampled). Rounds off the hard steps a bitmap height map leaves
    // behind. Restricted to vertices the displacement actually moved, so the rest of the model keeps
    // its exact geometry. `smooth_strength` in [0, 1] is how far each pass moves a vertex toward the
    // average of its neighbours.
    bool  smooth_enabled    = false;
    float smooth_strength   = 0.3f;
    int   smooth_iterations = 2;

    // Hold the painted patch's outermost ring of vertices out of the smoothing. Those vertices sit
    // next to unpainted ones that are pinned by definition, so relaxing them drags the rim of the
    // relief back down toward the undisplaced surface - the pattern looks half-melted exactly where it
    // meets the edge, however crisp the rest of it is. Excluding them keeps the border extruded at
    // full depth and smooths only the interior. On by default; turn it off to soften the outer edge
    // deliberately (which is a blunter version of the per-layer edge-smoothing falloff).
    bool  smooth_skip_border = true;

    template<class Archive> void serialize(Archive &ar)
    {
        ar(displace_border, smooth_enabled, smooth_strength, smooth_iterations, smooth_skip_border);
    }
};

// Decoded 8-bit grayscale height sample, independent of any GUI/OpenGL texture object so it can
// be evaluated from a background bake Job as well as from GUI-side preview code.
struct DecodedHeightTexture
{
    std::vector<uint8_t> pixels; // row-major, top-to-bottom, one byte per pixel
    int width  = 0;
    int height = 0;

    bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
    // Bilinearly sampled height in [0, 1] at a normalized uv coordinate. When tile_enabled is false,
    // a uv outside [0, 1) samples as 0 - the texture simply is not there, rather than its border
    // row/column being smeared outward forever (which is what clamping the coordinate would do, and
    // was a real reported bug). Callers rely on this to get a hard edge: it is how the projection
    // frame's border becomes the edge of the displacement.
    float sample(const Vec2f &uv, bool tile_enabled = true, TextureTileMethod tile_method = TextureTileMethod::Repeat) const;
};

// Decode a layer's raw image bytes into sampleable grayscale height data. Returns an empty
// DecodedHeightTexture if image_data is empty or is not an 8-bit grayscale PNG.
DecodedHeightTexture decode_height_texture(const TextureDisplacementLayer &layer);

// Raw dominant-axis planar projection of `position` (in mm, not yet scaled/rotated/offset by any
// layer), dropping the axis position that best aligns with `normal`. Exposed on its own (rather
// than only inline inside project_texture_displacement_uv()) so GUI code - the on-canvas
// "adjust texture placement" gizmo - can map a dragged 3D point into the exact same 2D space
// tiling_scale/rotation_deg/offset operate in, without duplicating the axis-selection logic.
Vec2f project_planar(const Vec3f &position, const Vec3f &normal);

// Applies a layer's tiling_scale/rotation_deg/offset to an already-projected planar coordinate
// (in mm, dominant-axis planar, cylindrical, spherical, or CGAL LSCM output - any of them, all
// share this same final step). Exposed separately so build_texture_displacement() can route CGAL
// LSCM's per-patch UV solve through the same scale/rotate/offset controls as every other
// projection method, without going through project_texture_displacement_uv()'s own dispatch
// (which only knows how to compute the *analytic* methods from a single vertex + normal).
// `aspect` is the height map's width / height. It scales the v axis so a non-square image is not
// squeezed into a square tile: `tiling_scale` is the tile's size along u, and the tile is
// `tiling_scale * height / width` mm along v, which keeps texels square. 1 (the default) is the
// square case and leaves the coordinate exactly as it always was.
Vec2f apply_uv_transform(const Vec2f &planar, const TextureDisplacementLayer &layer, float aspect = 1.f);

// Applies a row-major 3x4 projective matrix (see TextureDisplacementLayer::view_project_matrix) to a
// local-space point, writing the resulting texture uv. Returns false - and leaves `uv` untouched -
// when the point lies behind the projector or on its plane (w <= 0), where there is no meaningful uv
// and dividing would produce a mirrored or infinite coordinate. Callers treat that as "no height".
bool project_uv_projective(const std::array<float, 12> &m, const Vec3f &position, Vec2f &uv);

// Sample a layer's height texture at a mesh-local position, honouring the layer's projection
// method, tiling scale, rotation, offset and tiling mode. Returns a height in [0, 1].
//
// This returns a *height* rather than a UV because TextureProjectionMethod::Triplanar is a blend
// of three separate axis projections and therefore takes three texture samples per vertex - there
// is no single UV that represents it. The other methods do map to one UV internally.
//   - `normal` is this specific vertex's own normal; used only by Triplanar (for its blend weights).
//   - `patch_center`/`patch_axis` describe the painted patch as a whole (its centroid, and - for
//     Cylindrical only - the wrap axis); used only by the Cylindrical/Spherical methods.
//   - `lscm_uv`, when non-null, is this vertex's precomputed LSCM coordinate and takes precedence
//     over `layer.projection_method` (LSCM is a single per-patch solve, not a per-vertex formula,
//     so build_texture_displacement() computes it once up front and passes it in here).
// patch_center/patch_axis are cheap to compute once per patch and passed through unchanged for
// every vertex rather than being re-derived per call.
float sample_layer_height(const DecodedHeightTexture &texture, const TextureDisplacementLayer &layer,
                          const Vec3f &position, const Vec3f &normal,
                          const Vec3f &patch_center = Vec3f::Zero(), const Vec3f &patch_axis = Vec3f::UnitZ(),
                          const Vec2f *lscm_uv = nullptr);

// Area-weighted centroid and average normal of a layer's currently painted patch, in mesh-local
// coordinates - the same measurements build_texture_displacement() uses to pick its dominant
// projection axis. Used by the GUI to anchor the on-canvas "adjust texture placement" gizmo to
// wherever the layer is actually painted. Returns false (leaving the outputs untouched) if the
// layer has nothing painted yet.
bool compute_layer_paint_anchor(const indexed_triangle_set                    &base_mesh,
                                 const TriangleSelector::TriangleSplittingData &facet_data,
                                 Vec3f                                         &anchor_pos,
                                 Vec3f                                         &anchor_normal);

// Extracts the currently painted patch from a volume's base mesh + stored facet data - the same
// extraction build_texture_displacement() and compute_layer_paint_anchor() each do internally via
// TriangleSelector::get_facets_strict(ENFORCER). Returns an empty mesh if nothing is painted.
// Exposed so GUI code (the LSCM "UV editor" preview pane) can get the same patch build_texture_
// displacement() would act on, without duplicating the deserialize/get_facets_strict boilerplate.
indexed_triangle_set extract_painted_patch(const indexed_triangle_set                    &base_mesh,
                                            const TriangleSelector::TriangleSplittingData &facet_data);

// A patch flattened into 2D. The patch is first split into charts along edges sharper than
// `seam_angle_deg` (see LSCM_DEFAULT_SEAM_ANGLE_DEG), each chart is flattened independently, the
// charts are scaled to their true mm size and packed side by side.
//
// A vertex sitting on a seam belongs to several charts at once and therefore has a *different* UV
// in each of them, so this cannot be a plain "one UV per patch vertex" array: seam vertices are
// duplicated, once per chart touching them. `indices` is the patch's own triangle list re-indexed
// onto that duplicated vertex set, and `source_vertex` maps each duplicate back to the patch vertex
// it came from.
struct PatchUnwrap
{
    std::vector<Vec2f>                       uvs;           // one per unwrapped vertex, in mm
    std::vector<int>                         source_vertex; // unwrapped vertex -> index into patch.vertices
    std::vector<int>                         vertex_chart;  // unwrapped vertex -> chart (island) id
    std::vector<stl_triangle_vertex_indices> indices;       // patch triangles, re-indexed into `uvs`
    // Per chart, the centroid of its uvs - the point a TextureIsland's rotation turns about.
    std::vector<Vec2f> chart_centroid;
    // Edges belonging to exactly one triangle: the outline of each island. Indices into `uvs`. This
    // is what the UV editor draws highlighted, so the boundaries the seam angle cut are visible.
    std::vector<std::pair<int, int>> boundary_edges;
    int                              chart_count = 0;

    bool empty() const { return indices.empty(); }
};

// Applies an island's hand placement (scale + rotation about its own centroid, then offset) to one
// unwrapped UV. A chart with no entry in `islands` is left exactly where the packing put it.
Vec2f apply_island_transform(const Vec2f &uv, int chart, const PatchUnwrap &unwrap, const std::vector<TextureIsland> &islands);

// The same transform as a 2x3 affine matrix (columns: x basis, y basis, translation), for callers
// that would otherwise apply it to every vertex of an island one at a time. The UV editor renders
// each island through this as a uniform, which is what lets a drag move an island without touching
// its vertex buffer at all.
Eigen::Matrix<float, 2, 3> island_transform_matrix(int chart, const PatchUnwrap &unwrap, const std::vector<TextureIsland> &islands);

// Lays the unwrap's charts out as a connected net: charts that share a mesh edge are unfolded so
// their shared edge coincides (a cube -> its six faces joined along a spanning tree of edges, the rest
// left as free borders). Charts stay separate islands, so their borders still show and any of them can
// still be moved by hand afterwards. A chart whose unfold would overlap one already placed is left
// where the packing put it. Returns one placement per chart. See the gizmo's auto-connect option.
std::vector<TextureIsland> compute_connected_net(const PatchUnwrap &unwrap);

// The placement that unfolds `child` onto `parent` along their shared mesh edge, honouring `parent`'s
// current placement in `islands`. Returns false if the two charts share no edge. Backs the manual
// "Join" command; compute_connected_net() does the same thing across a whole spanning tree.
bool join_chart_placement(const PatchUnwrap &unwrap, const std::vector<TextureIsland> &islands,
                          int child, int parent, TextureIsland &out_child);

// Unwraps `patch` as described above. Charts that are flat (within a degree) are projected onto
// their own tangent plane directly, which is both exact and far cheaper than a solve; only genuinely
// curved charts go through CGAL's LSCM parameterizer (MeshBoolean::cgal::parameterize_lscm()). A
// chart that LSCM cannot flatten at all (it is not a topological disk - closed, or with a hole)
// falls back to that same tangent-plane projection.
//
// `padding_mm` is the gap the packing leaves between islands; negative means auto (see
// TextureDisplacementLayer::island_padding_mm). `seam_edges` are extra edges to cut along regardless
// of angle (manual/auto seams), in the patch's own vertex-index space (which is the mesh's, since the
// patch carries the whole vertex array - see get_facets_strict()).
//
// Results are cached, keyed on the patch's geometry, the seam angle, the padding and the seam edges:
// nothing else about a layer (depth, tiling, rotation, offset, texture, island placement) changes the
// unwrap, so dragging any of those sliders must not pay for a re-solve.
PatchUnwrap compute_patch_unwrap(const indexed_triangle_set &patch, float seam_angle_deg = LSCM_DEFAULT_SEAM_ANGLE_DEG,
                                 float padding_mm = -1.f, const std::vector<std::pair<int, int>> &seam_edges = {});

// One UV per patch vertex, for displacement. Displacement is inherently per-vertex - a vertex has
// exactly one position, so it can only be pushed out by one height - which means a seam vertex has
// to settle on a single one of its charts' UVs (the first, arbitrarily). That is not a compromise
// in the result: the surface stays watertight either way, since neighbouring vertices each move
// along their own normals and nothing depends on the UVs agreeing across the seam. It is only the
// *display* in the UV editor that needs the duplicated-vertex form above.
//
// Returns an empty vector if the patch has no triangles. Takes the whole layer because it applies
// both the layer's seam angle and its hand-placed islands.
std::vector<Vec2f> compute_lscm_uvs(const indexed_triangle_set &patch, const TextureDisplacementLayer &layer);

// One paint mask (as stored by ModelVolume::texture_displacement_facets) per possible layer slot.
using TextureDisplacementFacetsData = std::array<TriangleSelector::TriangleSplittingData, TEXTURE_DISPLACEMENT_MAX_LAYERS>;

// Bake all painted texture-displacement layers into `base_mesh`'s geometry, restricted to the
// painted area(s) only (the rest of the mesh is left untouched). Returns the mesh unchanged if
// nothing is painted or no layer has a usable texture.
//
// **Topology-preserving**: the returned mesh has exactly `base_mesh`'s vertices and triangles, in
// the same order - only the positions of displaced vertices differ. Every layer's paint mask is
// evaluated against `base_mesh` directly, and each vertex accumulates a single signed displacement
// (in mm) that all the layers covering it fold into, in slot order, via their TextureBlendMode.
// The vertex is then moved once, along its base-mesh normal, by that accumulated total.
//
// This replaced an earlier design that instead applied the layers *sequentially*, re-meshing after
// each one and carrying the next layer's paint mask onto the result with
// TriangleSelector::remap_painting(). That was the cause of a real "the second texture is never
// applied" bug: remapping a mask onto a mesh whose vertices had just been displaced out from under
// it routinely produced an empty bitstream, and the layer was then silently skipped. It is also
// what forced the per-layer vertex duplication and the final its_compactify_vertices() pass. The
// accumulate-then-displace formulation has neither problem, is substantially faster (no remap, no
// welding, one pass over the mesh), and - because the output keeps the input's exact vertex
// indexing - lets the GUI overlay a preview on the base mesh without any index translation.
//
// A vertex used by even one *unpainted* triangle of a layer's mask sits on that layer's boundary.
// Whether it moves is TextureDisplacementOptions::displace_border; see that field for why displacing
// it is safe (and the default). Either way the *direction* every vertex moves in is the area-weighted
// normal of the triangles that are painted in at least one layer - not of the whole mesh - so a
// border vertex travels along the painted surface's own normal instead of a blend with whatever
// unpainted geometry meets it there. Without that, the rim of a fully painted face would displace
// along the 45 degrees bisector it shares with the side wall and flare outwards. Interior vertices
// have every incident triangle painted, so for them the two are the same normal.
//
// Takes plain copied data rather than a ModelVolume reference so it is safe to call from a
// background thread (e.g. a bake Job's process() method) on a snapshot captured on the main
// thread, without touching the live Model concurrently with the UI.
//
// Known limitation: this does not attempt to remap texture-displacement paint data across
// topology-changing operations performed outside this gizmo (e.g. ModelObject::split(),
// mesh-boolean ops) the way TriangleSelector::remap_painting() does for the other paint channels.
// Such operations will silently drop any unbaked texture-displacement paint on the affected
// volume. This is an explicit extension point for a later phase, not an oversight.
//
// `progress`, when set, is called from the worker thread with a 0..100 completion percentage as the
// bake proceeds. Returning false from it aborts the run, which then returns an *empty* mesh - never
// a partially displaced one, so a cancelled bake can never be mistaken for a finished result and
// committed. It exists because this is the one call in the feature that can take seconds on a
// subdivided mesh, and without it the progress notification the Job framework puts on screen sits at
// 0% for the whole run and offers no way to close it (its close button only appears at 100%).
indexed_triangle_set build_texture_displacement(const indexed_triangle_set                  &base_mesh,
                                                 const std::vector<TextureDisplacementLayer> &layers,
                                                 const TextureDisplacementFacetsData         &facets_data,
                                                 const TextureDisplacementOptions            &options = {},
                                                 const DisplacementProgressFn                &progress = {});

// Convenience overload for main-thread callers: extracts the mesh/layers/paint data/options from
// `volume` and forwards to the overload above.
indexed_triangle_set build_texture_displacement(const ModelVolume &volume);

// Laplacian relaxation of `mesh` in place, restricted to the vertices flagged in `movable` (sized to
// the mesh's vertex count; anything else is held exactly where it is and still acts as an anchor for
// its neighbours). Each of `iterations` passes moves a movable vertex a `strength` fraction of the
// way to the average of the vertices it shares an edge with, computed from the positions at the
// start of that pass so the result does not depend on vertex order.
//
// Topology-preserving like the bake itself, so it composes with it: this is what "smooth the relief
// after displacing it" runs, and it is also safe to run standalone on an already baked mesh.
// `strength` is clamped to [0, 1]; 0 iterations, an empty/mis-sized `movable`, or an all-false one
// leave the mesh untouched.
// `on_pass`, when set, is called with the 0-based index of each completed pass; returning false stops
// the relaxation there, leaving the passes already done in place.
void smooth_mesh_vertices(indexed_triangle_set &mesh, const std::vector<uint8_t> &movable, float strength,
                          int iterations, const DisplacementProgressFn &on_pass = {});

// Returns a scalar height (in mm - a displacement magnitude) at a surface point, given that point's
// position and interpolated normal. This is what feature-adaptive subdivision samples to decide
// where the displaced surface has *curvature* worth spending triangles on. Called serially from the
// subdivider, so it only needs to be safe on the calling thread.
using HeightFieldSampler = std::function<float(const Vec3f &pos, const Vec3f &normal)>;

// Builds a sampler of the *combined* (all-layers) displacement height in mm at an arbitrary surface
// point, for feature-adaptive subdivision. It mirrors build_texture_displacement()'s per-layer setup
// (decode, patch centroid, cylinder axis, blend order, "lowest layer folds additively") but evaluates
// per point instead of per vertex. Two deliberate simplifications, both erring toward *more* detail
// (safe - over-refinement is never a crack): every sampleable layer is evaluated at every point (no
// per-point paint-mask test, so a point sees all layers' textures, not only the ones painted there),
// and edge-smoothing's boundary falloff is ignored. LSCM layers have no per-point UV and are skipped.
// Returns a null sampler (bool false) when no layer can be sampled - the caller then falls back to
// uniform adaptive subdivision.
HeightFieldSampler make_combined_displacement_sampler(const indexed_triangle_set                  &base_mesh,
                                                      const std::vector<TextureDisplacementLayer> &layers,
                                                      const TextureDisplacementFacetsData         &facets_data);

// Uniformly subdivides `mesh` (every triangle recursively split into 4 via edge midpoints, using a
// shared cache so a midpoint is computed once and reused by both triangles on either side of that
// edge) until every edge is at or below max_edge_length_mm, or max_iterations passes have run,
// whichever comes first (bounding the worst-case triangle-count explosion on a very fine target).
//
// This exists so a low-poly input model can still get fine-grained texture displacement detail -
// build_texture_displacement() can only ever move existing vertices, so a patch with only a
// handful of vertices to begin with cannot show much detail no matter the texture's resolution.
//
// Deliberately whole-mesh and uniform, not limited to a painted patch: subdividing only part of a
// mesh while leaving the rest untouched creates a classic T-junction/cracking problem where the
// denser and sparser regions meet (the finer side has edge midpoints the coarser side doesn't
// know about). Uniform, whole-mesh subdivision has no such seam and stays manifold, at the cost of
// applying everywhere rather than just where texture detail is actually wanted - meant to be run
// once, deliberately, before painting (see the gizmo's "Subdivide model" button), not automatically
// during baking.
indexed_triangle_set subdivide_mesh_uniform(const indexed_triangle_set &mesh, float max_edge_length_mm, int max_iterations = 6);

// Adaptive subdivision by Rivara longest-edge bisection, restricted to a region.
//
// Unlike subdivide_mesh_uniform() this only densifies where asked - `refine_region` (indexed by
// input-triangle index; empty or all-false => no-op) flags the triangles allowed to drive refinement
// - so a small painted patch on a large model does not quadruple the whole model's triangle count.
// It is nonetheless *conformal*: it never leaves a T-junction/crack at the boundary between the
// refined and coarse regions (the trap that made subdivide_mesh_uniform() deliberately whole-mesh).
//
// A triangle wants refining while it is over at least one of these, whichever applies:
//   - length:  its longest edge exceeds `target_edge_length_mm` (a *baseline* - it applies in feature
//              mode too, and is what stops a coarse triangle from being declared flat merely because
//              the four points the chord test samples happened to land at similar heights on a
//              high-frequency texture: the classic aliasing stall);
//   - feature: (only when `sampler` is set and `chord_tolerance_mm > 0`) the *displaced* surface
//              departs from the flat triangle by more than `chord_tolerance_mm`, measured as the max
//              over the three edge midpoints AND the centroid of |sampled displacement - the flat
//              triangle's barycentric interpolation|. Sampling the interior, not just edge midpoints,
//              is what catches a bump that sits inside a triangle. This is a *curvature* test: it is
//              exactly zero on a plane or a linear ramp (barycentric interpolation is exact there, so
//              those stay coarse - the case a gradient criterion would over-refine) and large on a
//              bump/ridge/noise.
// `min_edge_length_mm` is a hard floor under both: no triangle whose longest edge is already at or
// below it is ever refined, which is also what guarantees termination across a sharp texture step
// (where the chord error never falls below the tolerance no matter how fine the mesh gets).
//
// Refinement runs to completion, not for a fixed number of passes: triangles are taken worst-first
// from a max-heap keyed by how many times over its criteria each one is, so a run that hits the
// `max_triangles` budget has spent it on the largest errors rather than wherever a sweep happened to
// reach. The budget is the only bound on a pathological height field; stopping on it leaves a
// perfectly valid, still-conformal mesh.
//
// How it stays crack-free: only "terminal" edges are ever bisected - an edge that is the longest edge
// of *both* triangles sharing it (or a boundary edge that is the longest of its one triangle).
// Bisecting such an edge splits both its triangles 1->2 around the same new midpoint, so a hanging
// node is never created. The edge to split for a triangle that wants refining is found by Rivara
// longest-edge propagation (LEPP): walk to the longest edge of ever-longer-edged neighbours until a
// terminal edge is reached, and bisect that. Edge length strictly increases along the path (ties
// broken by a mesh-vertex key, which both sides of an edge compute identically), so the walk cannot
// cycle, and Rivara's result is that repeating it refines the original triangle in a bounded number
// of bisections - closing the propagation gap a plain per-edge test leaves behind. The transition
// triangles this pulls in just outside the region are the graded band that makes the size change
// conformal; they are a bounded cost paid once, not a per-pass tax.
//
// If `out_source` is non-null it is resized to the output triangle count and out_source[i] receives
// the input triangle that output triangle i descends from (children inherit their parent's index), so
// a caller can carry per-triangle data - e.g. a paint mask - across the topology change without a
// geometric remap.
//
// `refine_region` is a **bitmask** per input triangle, not a plain flag:
//   bit 0 (REFINE_PAINTED) - inside the painted area: refine by the length baseline and, in feature
//                            mode, by the chord-error test.
//   bit 1 (REFINE_BORDER)  - inside the band straddling the paint's edge: refine by
//                            `border_edge_length_mm` alone.
// A value of 1 therefore means exactly what a plain 1 always meant, and 0 still means "never touch
// this triangle except through the conformal closure".
//
// The border band exists because the chord-error test is blind to the one discontinuity the bake
// actually creates. `make_combined_displacement_sampler()` evaluates the height field everywhere,
// with no per-point paint test, so where the paint *stops* it keeps reporting full relief - smooth
// and low-curvature - while the baked surface steps from full displacement to zero. The test sees no
// error there and leaves the transition at whatever density the input had, which is what turns the
// rim of an unpainted island into a ring of large, steeply tilted triangles. Refining that band by
// plain edge length is bounded (it is a thin ring, and a length target always terminates) and needs
// no paint-aware sampler.
//
// `progress`, when given, is called with a 0..100 percentage of the triangle budget spent; returning
// false stops the refinement early. What it hands back then is still a complete, conformal mesh - the
// loop only ever finishes whole bisections - so a caller that wants to discard it has to do so itself.
indexed_triangle_set subdivide_mesh_adaptive(const indexed_triangle_set &mesh,
                                             const std::vector<uint8_t> &refine_region,
                                             float target_edge_length_mm, int max_triangles = 1000000,
                                             std::vector<int> *out_source = nullptr,
                                             const HeightFieldSampler &sampler = nullptr,
                                             float chord_tolerance_mm = 0.f, float min_edge_length_mm = 0.f,
                                             float border_edge_length_mm = 0.f,
                                             const DisplacementProgressFn &progress = nullptr);

// The recipe for getting a mesh ready to receive displacement: even out the triangle density, then
// refine it where the texture bends. Either stage is skipped when its target is <= 0. Pure data, and
// the whole of it, so the preparation can be handed to a background job instead of running on the UI
// thread - see GLGizmoTextureDisplacement::prepare_mesh().
struct TextureDisplacementPrepareParams
{
    // Isotropic remesh (CGAL). The target is clamped against the part's own surface area before it is
    // used, so a value that would produce millions of triangles cannot be asked for by accident.
    float remesh_edge_mm   = 0.f;
    float remesh_sharp_deg = 0.f; // 0 = do not protect sharp edges

    // Adaptive (Rivara) subdivision of the painted area. See subdivide_mesh_adaptive().
    float subdiv_target_mm       = 0.f; // "Max edge": the length baseline, and the only criterion when
                                        // subdiv_feature is off
    float subdiv_detail_mm       = 0.f; // "Detail": chord tolerance, feature mode only
    float subdiv_min_edge_mm     = 0.f; // "Min edge": the floor under both, feature mode only
    float subdiv_border_mm       = 0.f; // "Edge detail": the band straddling the paint's edge, 0 = off
    bool  subdiv_feature         = false; // follow texture curvature, not just edge length
    int   subdiv_added_triangles = 0;     // budget, *added* to the mesh's own count
};

// What a preparation run produced. An empty `mesh` means there was nothing to do and the caller must
// commit nothing - which is not a failure: a mesh that is already even needs no remesh, and one that
// is already fine enough for the texture needs no subdivision.
struct TextureDisplacementPrepareResult
{
    indexed_triangle_set          mesh;
    TextureDisplacementFacetsData masks;
    // The remesh landed but no layer's paint survived being carried onto it. Nothing is committed:
    // everything downstream is driven by that paint, so baking on would bake a flat mesh.
    bool                          paint_lost = false;
};

} // namespace Slic3r

#endif // slic3r_TextureDisplacement_hpp_
