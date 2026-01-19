#include "CutSurface.hpp"

/// models_input.obj - Check transormation of model to each others
/// projection_center.obj - circle representing center of projection with correct distance
/// {M} .. model index
/// model/model{M}.off - CGAL model created from index_triangle_set
/// model_neg/model{M}.off - CGAL model created for differenciate (multi volume object)
/// shape.off - CGAL model created from shapes
/// constrained/model{M}.off - Visualization of inside and outside triangles
///    Green - not along constrained edge
///    Red - sure that are inside
///    Purple - sure that are outside
/// (only along constrained edge)
/// filled/model{M}.off - flood fill green triangles inside of red area
///                     - Same meaning of color as constrained
/// {N} .. Order of cutted Area of Interestmodel from model surface
/// model_AOIs/{M}/cutAOI{N}.obj - Extracted Area of interest from corefined model
/// model_AOIs/{M}/outline{N}.obj - Outline of Cutted Area
/// {O} .. Order number of patch
/// patches/patch{O}.off
/// result.obj - Merged result its
/// result_contours/{O}.obj - visualization of contours for result patches
//#define DEBUG_OUTPUT_DIR std::string("C:/data/temp/cutSurface/")

using namespace Slic3r;
#include "ExPolygonsIndex.hpp"
#include <boost/next_prior.hpp>
#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <CGAL/Exact_integer.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Cartesian_converter.h>
#include <tbb/parallel_for.h>

// libslic3r
#include "TriangleMesh.hpp" // its_merge
#include "Utils.hpp" // next_highest_power_of_2
#include "ClipperUtils.hpp" // union_ex + offset_ex

namespace priv {

using Project = Emboss::IProjection;
using Project3d = Emboss::IProject3d;

/// <summary>
/// Set true for indices out of area of interest
/// </summary>
/// <param name="skip_indicies">Flag to convert triangle to cgal</param>
/// <param name="its">model</param>
/// <param name="projection">Convert 2d point to pair of 3d points</param>
/// <param name="shapes_bb">2d bounding box define AOI</param>
void set_skip_for_out_of_aoi(std::vector<bool>          &skip_indicies,
                             const indexed_triangle_set &its,
                             const Project              &projection,
                             const BoundingBox          &shapes_bb);

/// <summary>
/// Set true for indicies outward and almost parallel together.
/// Note: internally calculate normals
/// </summary>
/// <param name="skip_indicies">Flag to convert triangle to cgal</param>
/// <param name="its">model</param>
/// <param name="projection">Direction to measure angle</param>
/// <param name="max_angle">Maximal allowed angle between opposit normal and
/// projection direction [in DEG]</param>
void set_skip_by_angle(std::vector<bool>          &skip_indicies,
                       const indexed_triangle_set &its,
                       const Project3d            &projection,
                       double                      max_angle = 89.);

    
using EpicKernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using CutMesh = CGAL::Surface_mesh<EpicKernel::Point_3>;
using CutMeshes = std::vector<CutMesh>;

using VI = CGAL::SM_Vertex_index;
using HI = CGAL::SM_Halfedge_index;
using EI = CGAL::SM_Edge_index;
using FI = CGAL::SM_Face_index;
using P3 = CGAL::Epick::Point_3;

inline Vec3d to_vec3d(const P3 &p) { return Vec3d(p.x(),p.y(),p.z()); }

/// <summary>
/// Convert triangle mesh model to CGAL Surface_mesh
/// Filtrate out opposite triangles
/// Add property map for source face index
/// </summary>
/// <param name="its">Model</param>
/// <param name="skip_indicies">Flags that triangle should be skiped</param>
/// <param name="flip">When true triangle will flip normal</param>
/// <returns>CGAL mesh - half edge mesh</returns>
CutMesh to_cgal(const indexed_triangle_set &its,
                const std::vector<bool>    &skip_indicies,
                bool                        flip = false);

/// <summary>
/// Covert 2d shape (e.g. Glyph) to CGAL model
/// NOTE: internaly create
/// edge_shape_map .. Property map to store conversion from edge to contour
/// face_shape_map .. Property map to store conversion from face to contour
/// </summary>
/// <param name="shapes">2d shapes to project</param>
/// <param name="projection">Define transformation 2d point into 3d</param>
/// <returns>CGAL model of extruded shape</returns>
CutMesh to_cgal(const ExPolygons &shapes, const Project &projection);
// function to check result of projection. 2d int32_t -> 3d double
bool exist_duplicit_vertex(const CutMesh& mesh);


/// <summary>
/// IntersectingElement
///
/// Adress polygon inside of ExPolygon
/// Keep information about source of vertex:
///     - from face (one of 2 possible)
///     - from edge (one of 2 possible)
///
///  V1~~~~~V2
///   | f1 /:
///   |   / :
/// e1|  /e2:
///   | /   :
///   |/ f2 :
///  V1'~~~~V2'
///
/// | .. edge
/// / .. edge
/// : .. foreign edge - neighbor
/// ~ .. no care edge - idealy should not cross model
/// V1,V1' .. projected 2d point to 3d
/// V2,V2' .. projected 2d point to 3d
/// 
/// Vertex indexing
/// V1  .. i (vertex_base + 2x index of point in polygon)
/// V1' .. i + 1
/// V2  .. j = i + 2 || 0 (for last i in polygon)
/// V2' .. j + 1
/// 
/// f1 .. text_face_1 (triangle face made by side of shape contour)
/// f2 .. text_face_2
/// e1 .. text_edge_1 (edge on side of face made by side of shape contour)
/// e2 .. text_edge_2
///
/// </summary>
struct IntersectingElement
{
    // identify source point in shapes
    uint32_t shape_point_index{std::numeric_limits<uint32_t>::max()};

    // store together type, is_first, is_last
    unsigned char attr{std::numeric_limits<unsigned char>::max()};

    // vertex or edge ID, where edge ID is the index of the source point.
    // There are 4 consecutive indices generated for a single contour edge:
    // 0th - 1st text edge (straight)
    // 1th - 1st text face
    // 2nd - 2nd text edge (diagonal)
    // 3th - 2nd text face
    // Type of intersecting element from extruded shape( 3d )
    // NOTE: type must be storable to 3bit -> max value is 7
    enum class Type: unsigned char {
        edge_1 = 0,
        face_1 = 1,
        edge_2 = 2,
        face_2 = 3,
        undefined = 4
    };
        
    IntersectingElement &set_type(Type t)
    {
        attr = static_cast<unsigned char>(
            attr + (int) t - (int) get_type());
        return *this;
    }
    void set_is_first(){ attr += 8; }
    void set_is_last(){ attr += 16; }
    Type get_type() const { return static_cast<Type>(attr % 8);}
    bool is_first() const { return 8 <= attr && attr < 16; }
    bool is_last() const { return attr >= 16; }
};

// stored in model made by shape
using EdgeShapeMap = CutMesh::Property_map<EI, IntersectingElement>;
using FaceShapeMap = CutMesh::Property_map<FI, IntersectingElement>;

// stored in surface source - pointer to EdgeShapeMap | FaceShapeMap
using VertexShapeMap = CutMesh::Property_map<VI, const IntersectingElement *>;

// stored in model made by shape
const std::string edge_shape_map_name = "e:IntersectingElement";
const std::string face_shape_map_name = "f:IntersectingElement";

// stored in surface source
const std::string vert_shape_map_name = "v:IntersectingElement";

/// <summary>
/// Flag for faces in CGAL mesh
/// </summary>
enum class FaceType {
    // face inside of the cutted shape
    inside,
    // face outside of the cutted shape
    outside,
    // face without constrained edge (In or Out)
    not_constrained,

    // Helper flag that inside was processed
    inside_processed
};
using FaceTypeMap = CutMesh::Property_map<FI, FaceType>;
const std::string face_type_map_name = "f:side";

// Conversion one vertex index to another
using CvtVI2VI = CutMesh::Property_map<VI, VI>;
// Each Patch track outline vertex conversion to tource model
const std::string patch_source_name = "v:patch_source";

// For VI that should be reduced, contain VI to use instead of reduced
// Other VI are invalid
using ReductionMap = CvtVI2VI;
const std::string vertex_reduction_map_name = "v:reduction";

// A property map containing the constrained-or-not status of each edge
using EdgeBoolMap = CutMesh::Property_map<EI, bool>;
const std::string is_constrained_edge_name = "e:is_constrained";

/// <summary>
/// Create map to reduce unnecesary triangles,
/// Triangles are made by divided quad to two triangles
/// on side of cutting shape mesh
/// Note: also use from mesh (have to be created)
/// face_type_map .. Type of shape inside / outside
/// vert_shape_map .. Source of outline vertex
/// </summary>
/// <param name="reduction_map">Reduction map from vertex to vertex, 
/// when key == value than no reduction</param>
/// <param name="faces">Faces of one </param> 
/// <param name="mesh">Input object</param>
void create_reduce_map(ReductionMap &reduction_map, const CutMesh &meshes);

// Patch made by Cut area of interest from model
// connected faces(triangles) and outlines(halfEdges) for one surface cut
using CutAOI = std::pair<std::vector<FI>, std::vector<HI>>;
// vector of Cutted Area of interest cutted from one CGAL model
using CutAOIs = std::vector<CutAOI>;
// vector of CutAOIs for each model
using VCutAOIs = std::vector<CutAOIs>;

/// <summary>
/// Create AOIs(area of interest) on model surface
/// </summary>
/// <param name="cgal_model">Input model converted to CGAL
/// NOTE: will be extended by corefine edge </param>
/// <param name="shapes">2d contours</param>
/// <param name="cgal_shape">[const]Model made by shapes
/// NOTE: Can't be definde as const because of corefine function input definition,
/// but it is.</param> 
/// <param name="projection_ratio">Wanted projection distance</param>
/// <param name="s2i">Convert index to shape point from ExPolygons</param>
/// <returns>Patches from model surface</returns>
CutAOIs cut_from_model(CutMesh                &cgal_model,
                       const ExPolygons       &shapes,
                       /*const*/ CutMesh      &cgal_shape,
                       float                   projection_ratio,
                       const ExPolygonsIndices &s2i);

using Loop  = std::vector<VI>;
using Loops = std::vector<Loop>;

/// <summary>
/// Create closed loops of contour vertices created from open half edges
/// </summary>
/// <param name="outlines">Unsorted half edges</param>
/// <param name="mesh">Source mesh for half edges</param>
/// <returns>Closed loops</returns>
Loops create_loops(const std::vector<HI> &outlines, const CutMesh &mesh);

// To track during diff_models,
// what was cutted off, from CutAOI
struct SurfacePatch
{
    // converted cut to CGAL mesh
    // Mesh is reduced.
    // (do not contain divided triangles on contour - created by side Quad)
    CutMesh mesh;
    // CvtVI2VI cvt = mesh.property_map<VI, VI>(patch_source_name);
    // Conversion VI from this patch to source VI(model) is stored in mesh property

    // Outlines - converted CutAOI.second (half edges)
    // to loops (vertex indicies) by function create_loops
    Loops loops;

    // bounding box of mesh
    BoundingBoxf3 bb;

    //// Data needed to find best projection distances
    // index of source model in models
    size_t model_id;
    // index of source CutAOI
    size_t aoi_id;
    // index of shape from ExPolygons
    size_t shape_id = 0;

    // flag that this patch contain whole CutAOI
    bool is_whole_aoi = true;
};
using SurfacePatches = std::vector<SurfacePatch>;

struct ModelCutId
{
    // index of model
    uint32_t model_index;
    // index of cut inside model
    uint32_t cut_index;
};

/// <summary>
/// Keep conversion from VCutAOIs to Index and vice versa
/// Model_index .. contour(or hole) poin from ExPolygons
/// Index       .. continous number
/// </summary>
class ModelCut2index
{
    std::vector<uint32_t> m_offsets;
    // for check range of index
    uint32_t m_count;

public:
    ModelCut2index(const VCutAOIs &cuts);
    uint32_t   calc_index(const ModelCutId &id) const;
    ModelCutId calc_id(uint32_t index) const;
    uint32_t                     get_count() const { return m_count; };
    const std::vector<uint32_t> &get_offsets() const { return m_offsets; }
};

/// <summary>
/// Differenciate other models
/// </summary>
/// <param name="cuts">Patches from meshes</param>
/// <param name="cut_models">Source points for Cutted AOIs
/// NOTE: Create Reduction map as mesh property - clean on end</param>
/// <param name="models">Original models without cut modifications
/// used for differenciation
/// NOTE: Clip function modify Mesh</param> 
/// <param name="projection">Define projection direction</param> 
/// <returns>Cuts differenciate by models - Patch</returns>
SurfacePatches diff_models(VCutAOIs             &cuts,
                           /*const*/ CutMeshes  &cut_models,
                           /*const*/ CutMeshes  &models,
                           const Project3d      &projection);

/// <summary>
/// Checking whether patch is uninterrupted cover of whole expolygon it belongs.
/// </summary>
/// <param name="cutAOI">Part of surface to check</param>
/// <param name="shape">Source shape</param>
/// <param name="mesh">Source of cut</param>
/// <returns>True when cover whole expolygon otherwise false</returns>
bool is_over_whole_expoly(const CutAOI    &cutAOI,
                          const ExPolygon &shape,
                          const CutMesh   &mesh);

/// <summary>
/// Checking whether patch is uninterrupted cover of whole expolygon it belongs.
/// </summary>
/// <param name="patch">Part of surface to check</param>
/// <param name="shape">Source shape</param>
/// <returns>True when cover whole expolygon otherwise false</returns>
bool is_over_whole_expoly(const SurfacePatch &patch,
                          const ExPolygons   &shapes,
                          const VCutAOIs     &cutAOIs,
                          const CutMeshes    &meshes);
/// <summary>
/// Unptoject points from outline loops of patch
/// </summary>
/// <param name="patch">Contain loops and vertices</param>
/// <param name="projection">Know how to project from 3d to 2d</param>
/// <param name="depth_range">Range of unprojected points x .. min, y .. max value</param>
/// <returns>Unprojected points in loops</returns>
Polygons unproject_loops(const SurfacePatch &patch, const Project &projection, Vec2d &depth_range);

/// <summary>
/// Unproject points from loops and create expolygons
/// </summary>
/// <param name="patch">Patch to convert on expolygon</param>
/// <param name="projection">Convert 3d point to 2d</param>
/// <param name="depth_range">Range of unprojected points x .. min, y .. max value</param>
/// <returns>Expolygon represent patch in 2d</returns>
ExPolygon to_expoly(const SurfacePatch &patch, const Project &projection, Vec2d &depth_range);

/// <summary>
/// To select surface near projection distance
/// </summary>
struct ProjectionDistance
{
    // index of source model
    uint32_t model_index = std::numeric_limits<uint32_t>::max();

    // index of CutAOI
    uint32_t aoi_index = std::numeric_limits<uint32_t>::max();

    // index of Patch
    uint32_t patch_index = std::numeric_limits<uint32_t>::max();

    // signed distance to projection 
    float distance = std::numeric_limits<float>::max();    
};
// addresed by ExPolygonsIndices
using ProjectionDistances =  std::vector<ProjectionDistance>;

// each point in shapes has its ProjectionDistances
using VDistances = std::vector<ProjectionDistances>;

/// <summary>
/// Calculate distances for SurfacePatches outline points
/// NOTE:
/// each model has to have "vert_shape_map" .. Know source of new vertices
/// </summary>
/// <param name="patches">Part of surface</param>
/// <param name="models">Vertices position</param>
/// <param name="shapes_mesh">Mesh created by shapes</param>
/// <param name="count_shapes_points">Count of contour points in shapes</param>
/// <param name="projection_ratio">Define best distnace</param>
/// <returns>Projection distances of cutted shape points</returns>
VDistances calc_distances(const SurfacePatches &patches,
                          const CutMeshes      &models,
                          const CutMesh        &shapes_mesh,
                          size_t                count_shapes_points,
                          float                 projection_ratio);

/// <summary>
/// Select distances in similar depth between expolygons
/// </summary>
/// <param name="distances">All distances - Vector distances for each shape point</param>
/// <param name="shapes">Vector of letters</param>
/// <param name="start">Pivot for start projection in 2d</param>
/// <param name="s2i">Convert index to addresss inside of shape</param>
/// <param name="patches">Cutted parts from surface</param>
/// <returns>Closest distance projection indexed by points in shapes(see s2i)</returns>
ProjectionDistances choose_best_distance(
    const VDistances       &distances,
    const ExPolygons       &shapes,
    const Point            &start,
    const ExPolygonsIndices &s2i,
    const SurfacePatches   &patches);

/// <summary>
/// Create mask for patches
/// </summary>
/// <param name="best_distances">For each point selected closest distance</param>
/// <param name="patches">All patches</param>
/// <param name="shapes">Shape to cut</param>
/// <param name="shapes_bb">Bound of shapes</param>
/// <param name="s2i"></param>
/// <param name="cutAOIs"></param>
/// <param name="meshes"></param>
/// <param name="projection"></param>
/// <returns>Mask of used patch</returns>
std::vector<bool> select_patches(const ProjectionDistances &best_distances,
                                 const SurfacePatches      &patches,
                                 const ExPolygons          &shapes,
                                 const BoundingBox         &shapes_bb,
                                 const ExPolygonsIndices   &s2i,
                                 const VCutAOIs            &cutAOIs,
                                 const CutMeshes           &meshes,
                                 const Project             &projection);

/// <summary>
/// Merge two surface cuts together
/// Added surface cut will be consumed
/// </summary>
/// <param name="sc">Surface cut to extend</param>
/// <param name="sc_add">Surface cut to consume</param>
void append(SurfaceCut &sc, SurfaceCut &&sc_add);

/// <summary>
/// Convert patch to indexed_triangle_set
/// </summary>
/// <param name="patch">Part of surface</param>
/// <returns>Converted patch</returns>
SurfaceCut patch2cut(SurfacePatch &patch);

/// <summary>
/// Merge masked patches to one surface cut
/// </summary>
/// <param name="patches">All patches
/// NOTE: Not const because One needs to add property for Convert indices</param>
/// <param name="mask">Mash for using patch</param>
/// <returns>Result surface cut</returns>
SurfaceCut merge_patches(/*const*/ SurfacePatches &patches,
                         const std::vector<bool>  &mask);

#ifdef DEBUG_OUTPUT_DIR
void prepare_dir(const std::string &dir);
void initialize_store(const std::string &dir_to_clear);
/// <summary>
/// Debug purpose store of mesh with colored face by face type
/// </summary>
/// <param name="mesh">Input mesh, could add property color
/// NOTE: Not const because need to [optionaly] append color property map</param>
/// <param name="face_type_map">Color source</param>
/// <param name="file">File to store</param>
void store(const CutMesh &mesh, const FaceTypeMap &face_type_map, const std::string &dir, bool is_filled = false);
void store(const ExPolygons &shapes, const std::string &svg_file);
void store(const CutMesh &mesh, const ReductionMap &reduction_map, const std::string &dir);
void store(const CutAOIs &aois, const CutMesh &mesh, const std::string &dir);
void store(const SurfacePatches &patches, const std::string &dir);
void store(const Vec3f &vertex, const Vec3f &normal, const std::string &file, float size = 2.f);
//void store(const ProjectionDistances &pds, const VCutAOIs &aois, const CutMeshes &meshes, const std::string &file, float width = 0.2f/* [in mm] */);
using Connection = std::pair<size_t, size_t>; using Connections = std::vector<Connection>;
void store(const ExPolygons &shapes, const std::vector<bool> &mask_distances, const Connections &connections, const std::string &file_svg);
void store(const SurfaceCut &cut, const std::string &file, const std::string &contour_dir);
void store(const std::vector<indexed_triangle_set> &models, const std::string &obj_filename);
void store(const std::vector<CutMesh>&models, const std::string &dir);
void store(const Emboss::IProjection &projection, const Point &point_to_project, float projection_ratio, const std::string &obj_filename);
#endif // DEBUG_OUTPUT_DIR
} // namespace privat

#ifdef DEBUG_OUTPUT_DIR
#include "libslic3r/SVG.hpp"
#include <boost/log/trivial.hpp>
#include <filesystem>
#endif // DEBUG_OUTPUT_DIR

SurfaceCut Slic3r::cut_surface(const ExPolygons &shapes,
                               const std::vector<indexed_triangle_set> &models,
                               const Emboss::IProjection &projection,
                               float projection_ratio)
{
    assert(!models.empty());
    assert(!shapes.empty());
    if (models.empty() || shapes.empty() ) return {};

#ifdef DEBUG_OUTPUT_DIR
    priv::initialize_store(DEBUG_OUTPUT_DIR);
    priv::store(models, DEBUG_OUTPUT_DIR + "models_input.obj");
    priv::store(shapes, DEBUG_OUTPUT_DIR + "shapes.svg");
#endif // DEBUG_OUTPUT_DIR

    // for filter out triangles out of bounding box
    BoundingBox shapes_bb = get_extents(shapes);
#ifdef DEBUG_OUTPUT_DIR
    priv::store(projection, shapes_bb.center(), projection_ratio, DEBUG_OUTPUT_DIR + "projection_center.obj");
#endif // DEBUG_OUTPUT_DIR

    // for filttrate opposite triangles and a little more
    const float max_angle = 89.9f;
    priv::CutMeshes cgal_models; // source for patch
    priv::CutMeshes cgal_neg_models; // model used for differenciate patches
    cgal_models.reserve(models.size());
    for (const indexed_triangle_set &its : models) {
        std::vector<bool> skip_indicies(its.indices.size(), {false});
        priv::set_skip_for_out_of_aoi(skip_indicies, its, projection, shapes_bb);

        // create model for differenciate cutted patches
        bool flip = true;
        cgal_neg_models.push_back(priv::to_cgal(its, skip_indicies, flip));
        
        // cut out more than only opposit triangles 
        priv::set_skip_by_angle(skip_indicies, its, projection, max_angle);
        cgal_models.push_back(priv::to_cgal(its, skip_indicies));
    }
#ifdef DEBUG_OUTPUT_DIR
    priv::store(cgal_models, DEBUG_OUTPUT_DIR + "model/");// model[0-N].off
    priv::store(cgal_neg_models, DEBUG_OUTPUT_DIR + "model_neg/"); // model[0-N].off
#endif // DEBUG_OUTPUT_DIR

    priv::CutMesh cgal_shape = priv::to_cgal(shapes, projection);
#ifdef DEBUG_OUTPUT_DIR
    CGAL::IO::write_OFF(DEBUG_OUTPUT_DIR + "shape.off", cgal_shape); // only debug
#endif // DEBUG_OUTPUT_DIR

    // create tool for convert index to shape Point adress and vice versa
    ExPolygonsIndices s2i(shapes);
    priv::VCutAOIs model_cuts;
    // cut shape from each cgal model
    for (priv::CutMesh &cgal_model : cgal_models) { 
        priv::CutAOIs cutAOIs = priv::cut_from_model(
            cgal_model, shapes, cgal_shape, projection_ratio, s2i);
#ifdef DEBUG_OUTPUT_DIR
        size_t index = &cgal_model - &cgal_models.front();
        priv::store(cutAOIs, cgal_model, DEBUG_OUTPUT_DIR + "model_AOIs/" + std::to_string(index) + "/"); // only debug
#endif // DEBUG_OUTPUT_DIR
        model_cuts.push_back(std::move(cutAOIs));
    }

    priv::SurfacePatches patches = priv::diff_models(model_cuts, cgal_models, cgal_neg_models, projection);
#ifdef DEBUG_OUTPUT_DIR
    priv::store(patches, DEBUG_OUTPUT_DIR + "patches/");
#endif // DEBUG_OUTPUT_DIR
    if (patches.empty()) return {};    

    // fix - convert shape_point_id to expolygon index 
    // save 1 param(s2i) from diff_models call
    for (priv::SurfacePatch &patch : patches)
        patch.shape_id = s2i.cvt(patch.shape_id).expolygons_index;

    // calc distance to projection for all outline points of cutAOI(shape)
    // it is used for distiguish the top one
    uint32_t shapes_points = s2i.get_count();
    // for each point collect all projection distances
    priv::VDistances distances = priv::calc_distances(patches, cgal_models, cgal_shape, shapes_points, projection_ratio);
        
    Point start = shapes_bb.center(); // only align center

    // Use only outline points
    // for each point select best projection
    priv::ProjectionDistances best_projection = priv::choose_best_distance(distances, shapes, start, s2i, patches);
    std::vector<bool> use_patch = priv::select_patches(best_projection, patches, shapes, shapes_bb, s2i, model_cuts, cgal_models, projection);
    SurfaceCut result = merge_patches(patches, use_patch);
    //*/

#ifdef DEBUG_OUTPUT_DIR
    priv::store(result, DEBUG_OUTPUT_DIR + "result.obj", DEBUG_OUTPUT_DIR + "result_contours/");
#endif // DEBUG_OUTPUT_DIR
    return result;
}

indexed_triangle_set Slic3r::cut2model(const SurfaceCut         &cut,
                                       const Emboss::IProject3d &projection)
{
    assert(!cut.empty());
    size_t count_vertices = cut.vertices.size() * 2;
    size_t count_indices  = cut.indices.size() * 2;

    // indices from from zig zag
    for (const auto &c : cut.contours) {
        assert(!c.empty());
        count_indices += c.size() * 2;
    }
    
    indexed_triangle_set result;
    result.vertices.reserve(count_vertices);
    result.indices.reserve(count_indices);

    // front
    result.vertices.insert(result.vertices.end(), 
        cut.vertices.begin(), cut.vertices.end());
    result.indices.insert(result.indices.end(), 
        cut.indices.begin(), cut.indices.end());

    // back
    for (const Vec3f &v : cut.vertices) {
        Vec3d vd = v.cast<double>();
        Vec3d vd2 = projection.project(vd);
        result.vertices.push_back(vd2.cast<float>());
    }

    size_t back_offset = cut.vertices.size();
    for (const auto &i : cut.indices) {
        // check range of indices in cut
        assert(i.x() + back_offset < result.vertices.size());
        assert(i.y() + back_offset < result.vertices.size());
        assert(i.z() + back_offset < result.vertices.size());
        assert(i.x() >= 0 && i.x() < cut.vertices.size());
        assert(i.y() >= 0 && i.y() < cut.vertices.size());
        assert(i.z() >= 0 && i.z() < cut.vertices.size());
        // Y and Z is swapped CCW triangles for back side
        result.indices.emplace_back(i.x() + back_offset,
                                    i.z() + back_offset,
                                    i.y() + back_offset);
    }

    // zig zag indices
    for (const auto &contour : cut.contours) {
        size_t prev_front_index = contour.back();
        size_t prev_back_index  = back_offset + prev_front_index;
        for (size_t front_index : contour) {
            assert(front_index < cut.vertices.size());
            size_t back_index  = back_offset + front_index;
            result.indices.emplace_back(front_index, prev_front_index, back_index);
            result.indices.emplace_back(prev_front_index, prev_back_index, back_index);
            prev_front_index = front_index;
            prev_back_index  = back_index;
        }
    }

    assert(count_vertices == result.vertices.size());
    assert(count_indices == result.indices.size());
    return result;
}

// set_skip_for_out_of_aoi helping functions
namespace priv {
// define plane
using PointNormal = std::pair<Vec3d, Vec3d>;
using PointNormals = std::array<PointNormal, 4>;

/// <summary>
/// Check 
/// </summary>
/// <param name="side"></param>
/// <param name="v"></param>
/// <param name="point_normals"></param>
/// <returns></returns>
bool is_out_of(const Vec3d &v, const PointNormal &point_normal);

using IsOnSides = std::vector<std::array<bool, 4>>;
/// <summary>
/// Check if triangle t has all vertices out of any plane
/// </summary>
/// <param name="t">Triangle</param>
/// <param name="is_on_sides">Flag is vertex index out of plane</param>
/// <returns>True when triangle is out of one of plane</returns>
bool is_all_on_one_side(const Vec3i32 &t, const IsOnSides& is_on_sides);

} // namespace priv

bool priv::is_out_of(const Vec3d &v, const PointNormal &point_normal)
{
    const Vec3d& p = point_normal.first;
    const Vec3d& n = point_normal.second;
    double signed_distance = (v - p).dot(n);
    return signed_distance > 1e-5;
};

bool priv::is_all_on_one_side(const Vec3i32 &t, const IsOnSides& is_on_sides) {
    for (size_t side = 0; side < 4; side++) {
        bool result = true;
        for (auto vi : t) {
            if (!is_on_sides[vi][side]) {
                result = false;
                break;
            }
        }
        if (result) return true;
    }
    return false;
}

void priv::set_skip_for_out_of_aoi(std::vector<bool>          &skip_indicies,
                                   const indexed_triangle_set &its,
                                   const Project              &projection,
                                   const BoundingBox          &shapes_bb)
{
    assert(skip_indicies.size() == its.indices.size());
    //   1`*----* 2`
    //    /  2 /|
    // 1 *----* |
    //   |    | * 3`
    //   |    |/
    // 0 *----* 3
    //////////////////
    std::array<std::pair<Vec3d, Vec3d>, 4> bb;
    int index = 0;
    for (Point v :
         {shapes_bb.min, Point{shapes_bb.min.x(), shapes_bb.max.y()},
          shapes_bb.max, Point{shapes_bb.max.x(), shapes_bb.min.y()}})
        bb[index++] = projection.create_front_back(v);

    // define planes to test
    // 0 .. under
    // 1 .. left
    // 2 .. above
    // 3 .. right
    size_t prev_i = 3;
    // plane is defined by point and normal
    PointNormals point_normals;
    for (size_t i = 0; i < 4; i++) {
        const Vec3d &p1 = bb[i].first;
        const Vec3d &p2 = bb[i].second;
        const Vec3d &p3 = bb[prev_i].first;
        prev_i = i;

        Vec3d v1 = p2 - p1;
        v1.normalize();
        Vec3d v2 = p3 - p1;
        v2.normalize();

        Vec3d normal = v2.cross(v1);
        normal.normalize(); 

        point_normals[i] = {p1, normal};
    }

    // check that projection is not left handed
    // Fix for reflected projection
    if (is_out_of(point_normals[2].first, point_normals[0])) {
        // projection is reflected so normals are reflected
        for (auto &pn : point_normals)
            pn.second *= -1;
    }    

    // same meaning as point normal
    IsOnSides is_on_sides(its.vertices.size(), {false,false,false,false});    
    
    // inspect all vertices when it is out of bounding box
    tbb::parallel_for(tbb::blocked_range<size_t>(0, its.vertices.size()),
    [&its, &point_normals, &is_on_sides](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            Vec3d v = its.vertices[i].cast<double>();
            // under + above
            for (int side : {0, 2}) {
                if (is_out_of(v, point_normals[side])) {
                    is_on_sides[i][side] = true;
                    // when it is under it can't be above
                    break;
                }
            }
            // left + right
            for (int side : {1, 3}) {
                if (is_out_of(v, point_normals[side])) {
                    is_on_sides[i][side] = true;
                    // when it is on left side it can't be on right
                    break;
                }        
            }
        }
    }); // END parallel for

    // inspect all triangles, when it is out of bounding box
    tbb::parallel_for(tbb::blocked_range<size_t>(0, its.indices.size()),
    [&its, &is_on_sides, &skip_indicies](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            if (is_all_on_one_side(its.indices[i], is_on_sides)) 
                skip_indicies[i] = true;
        }
    }); // END parallel for
}

indexed_triangle_set Slic3r::its_mask(const indexed_triangle_set &its,
                                      const std::vector<bool>    &mask)
{
    if (its.indices.size() != mask.size()) {
        assert(false);
        return {};
    }

    std::vector<uint32_t> cvt_vetices(its.vertices.size(), {std::numeric_limits<uint32_t>::max()});
    size_t vertices_count = 0;
    size_t faces_count    = 0;
    for (const auto &t : its.indices) {
        size_t index = &t - &its.indices.front();
        if (!mask[index]) continue;
        ++faces_count;
        for (const auto vi : t) {
            uint32_t &cvt = cvt_vetices[vi];
            if (cvt == std::numeric_limits<uint32_t>::max())
                cvt = vertices_count++;
        }
    }
    if (faces_count == 0) return {};
    
    indexed_triangle_set result;
    result.indices.reserve(faces_count);
    result.vertices = std::vector<Vec3f>(vertices_count);
    for (size_t i = 0; i < its.vertices.size(); ++i) {
        uint32_t index = cvt_vetices[i];
        if (index == std::numeric_limits<uint32_t>::max()) continue;
        result.vertices[index] = its.vertices[i];
    }

    for (const stl_triangle_vertex_indices &f : its.indices)
        if (mask[&f - &its.indices.front()])        
            result.indices.push_back(stl_triangle_vertex_indices(
                cvt_vetices[f[0]], cvt_vetices[f[1]], cvt_vetices[f[2]]));
    
    return result;    
}

indexed_triangle_set Slic3r::its_cut_AoI(const indexed_triangle_set &its,
                                         const BoundingBox          &bb,
                                         const Emboss::IProjection &projection)
{
    std::vector<bool> skip_indicies(its.indices.size(), false);
    priv::set_skip_for_out_of_aoi(skip_indicies, its, projection, bb);
    // invert values in vector of bool
    skip_indicies.flip();
    return its_mask(its, skip_indicies);
}

void priv::set_skip_by_angle(std::vector<bool>          &skip_indicies,
                             const indexed_triangle_set &its,
                             const Project3d            &projection,
                             double                      max_angle)
{
    assert(max_angle < 90. && max_angle > 89.);
    assert(skip_indicies.size() == its.indices.size());
    float threshold = static_cast<float>(cos(max_angle / 180. * M_PI));
    for (const stl_triangle_vertex_indices& face : its.indices) {
        size_t index = &face - &its.indices.front();
        if (skip_indicies[index]) continue;
        Vec3f n = its_face_normal(its, face);
        const Vec3f& v = its.vertices[face[0]];
        const Vec3d vd = v.cast<double>();
        // Improve: For Orthogonal Projection it is same for each vertex
        Vec3d projectedd  = projection.project(vd);
        Vec3f projected   = projectedd.cast<float>();
        Vec3f project_dir = projected - v;
        project_dir.normalize();
        float cos_alpha = project_dir.dot(n);
        if (cos_alpha > threshold) continue;
        skip_indicies[index] = true;
    }
}

priv::CutMesh priv::to_cgal(const indexed_triangle_set &its,
                            const std::vector<bool>    &skip_indicies,
                            bool                        flip)
{
    const std::vector<stl_vertex>                  &vertices = its.vertices;
    const std::vector<stl_triangle_vertex_indices> &indices = its.indices;

    std::vector<bool> use_vetices(vertices.size(), {false});

    size_t vertices_count = 0;
    size_t faces_count    = 0;
    size_t edges_count    = 0;

    for (const auto &t : indices) {     
        size_t index = &t - &indices.front();
        if (skip_indicies[index]) continue;        
        ++faces_count;
        size_t count_used_vertices = 0;
        for (const auto vi : t) {
            if (!use_vetices[vi]) {
                ++vertices_count;
                use_vetices[vi] = true;
            } else {
                ++count_used_vertices;
            }
        }
        switch (count_used_vertices) {
        case 3: break; // all edges are already counted
        case 2: edges_count += 2; break;
        case 1:
        case 0: edges_count += 3; break;
        default: assert(false);
        }        
    }
    assert(vertices_count <= vertices.size());
    assert(edges_count <= (indices.size() * 3));
    assert(faces_count <= indices.size());

    CutMesh result;
    result.reserve(vertices_count, edges_count, faces_count);

    std::vector<VI> to_filtrated_vertices_index(vertices.size());
    size_t filtrated_vertices_index = 0;
    for (size_t i = 0; i < vertices.size(); ++i) 
        if (use_vetices[i]) { 
            to_filtrated_vertices_index[i] = VI(filtrated_vertices_index);
            ++filtrated_vertices_index;
        }

    for (const stl_vertex& v : vertices) {
        if (!use_vetices[&v - &vertices.front()]) continue;
        result.add_vertex(CutMesh::Point{v.x(), v.y(), v.z()});
    }

    if (!flip) {
        for (const stl_triangle_vertex_indices &f : indices) {
            if (skip_indicies[&f - &indices.front()]) continue;
            result.add_face(to_filtrated_vertices_index[f[0]],
                            to_filtrated_vertices_index[f[1]],
                            to_filtrated_vertices_index[f[2]]);
        }
    } else {
        for (const stl_triangle_vertex_indices &f : indices) {
            if (skip_indicies[&f - &indices.front()]) continue;
            result.add_face(to_filtrated_vertices_index[f[2]],
                            to_filtrated_vertices_index[f[1]],
                            to_filtrated_vertices_index[f[0]]);
        }
    }
    
    return result;
}

bool priv::exist_duplicit_vertex(const CutMesh &mesh) {
    std::vector<Vec3d> points;
    points.reserve(mesh.vertices().size());
    // copy points
    for (VI vi : mesh.vertices()) { 
        const P3 &p = mesh.point(vi);
        points.emplace_back(p.x(), p.y(), p.z());
    }
    std::sort(points.begin(), points.end(), [](const Vec3d &v1, const Vec3d &v2) {
        return v1.x() < v2.x() || 
              (v1.x() == v2.x() && 
                  (v1.y() < v2.y() || 
                  (v1.y() == v2.y()  && 
                      v1.z() < v2.z())));
    });
    // find first duplicit
    auto it = std::adjacent_find(points.begin(), points.end());
    return it != points.end();
}

priv::CutMesh priv::to_cgal(const ExPolygons  &shapes,
                            const Project     &projection)
{
    if (shapes.empty()) return {};
        
    CutMesh result;
    EdgeShapeMap edge_shape_map = result.add_property_map<EI, IntersectingElement>(edge_shape_map_name).first;
    FaceShapeMap face_shape_map = result.add_property_map<FI, IntersectingElement>(face_shape_map_name).first;

    std::vector<VI> indices;
    auto insert_contour = [&projection, &indices, &result, 
        &edge_shape_map, &face_shape_map]
        (const Polygon &polygon) {
        indices.clear();
        indices.reserve(polygon.points.size() * 2);
        size_t  num_vertices_old = result.number_of_vertices();
        for (const Point &polygon_point : polygon.points) {
            auto [front, back] = projection.create_front_back(polygon_point);
            P3 v_front{front.x(), front.y(), front.z()};
            VI vi1 = result.add_vertex(v_front);
            assert(vi1.idx() == (indices.size() + num_vertices_old));
            indices.push_back(vi1);

            P3 v_back{back.x(), back.y(), back.z()};
            VI vi2 = result.add_vertex(v_back);
            assert(vi2.idx() == (indices.size() + num_vertices_old));
            indices.push_back(vi2);
        }

        auto find_edge = [&result](FI fi, VI from, VI to) {
            HI hi = result.halfedge(fi);
            for (; result.target(hi) != to; hi = result.next(hi));
            assert(result.source(hi) == from);
            assert(result.target(hi) == to);
            return result.edge(hi);
        };

        uint32_t contour_index = static_cast<uint32_t>(num_vertices_old / 2);
        for (int32_t i = 0; i < int32_t(indices.size()); i += 2) {
            bool    is_first  = i == 0;
            bool    is_last   = size_t(i + 2) >= indices.size();
            int32_t j = is_last ? 0 : (i + 2);
            
            FI fi1 = result.add_face(indices[i], indices[j], indices[i + 1]);
            EI ei1 = find_edge(fi1, indices[i + 1], indices[i]);
            EI ei2 = find_edge(fi1, indices[j], indices[i + 1]);
            FI fi2 = result.add_face(indices[j], indices[j + 1], indices[i + 1]);
            IntersectingElement element {contour_index, (unsigned char)IntersectingElement::Type::undefined};
            if (is_first) element.set_is_first();
            if (is_last) element.set_is_last();
            edge_shape_map[ei1] = element.set_type(IntersectingElement::Type::edge_1);
            face_shape_map[fi1] = element.set_type(IntersectingElement::Type::face_1);
            edge_shape_map[ei2] = element.set_type(IntersectingElement::Type::edge_2);
            face_shape_map[fi2] = element.set_type(IntersectingElement::Type::face_2);
            ++contour_index;
        }
    };

    size_t count_point = count_points(shapes);
    result.reserve(result.number_of_vertices() + 2 * count_point,
                   result.number_of_edges() + 4 * count_point,
                   result.number_of_faces() + 2 * count_point);

    // Identify polygon
    for (const ExPolygon &shape : shapes) {
        insert_contour(shape.contour);
        for (const Polygon &hole : shape.holes)
            insert_contour(hole);
    }
    assert(!exist_duplicit_vertex(result));
    return result;
}

priv::ModelCut2index::ModelCut2index(const VCutAOIs &cuts)
{
    // prepare offsets
    m_offsets.reserve(cuts.size());
    uint32_t offset = 0;
    for (const CutAOIs &model_cuts: cuts) {
        m_offsets.push_back(offset);
        offset += model_cuts.size();
    }
    m_count = offset;
}

uint32_t priv::ModelCut2index::calc_index(const ModelCutId &id) const
{
    assert(id.model_index < m_offsets.size());
    uint32_t offset = m_offsets[id.model_index];
    uint32_t res = offset + id.cut_index;
    assert(((id.model_index+1) < m_offsets.size() && res < m_offsets[id.model_index+1]) ||
           ((id.model_index+1) == m_offsets.size() && res < m_count));
    return res;
}

priv::ModelCutId priv::ModelCut2index::calc_id(uint32_t index) const
{
    assert(index < m_count);
    ModelCutId result{0,0};
    // find shape index
    for (size_t model_index = 1; model_index < m_offsets.size(); ++model_index) {
        if (m_offsets[model_index] > index) break;
        result.model_index = model_index;
    }
    result.cut_index = index - m_offsets[result.model_index];
    return result;
}

// cut_from_model help functions
namespace priv {

/// <summary>
/// Track source of intersection 
/// Help for anotate inner and outer faces
/// </summary>
struct Visitor : public CGAL::Polygon_mesh_processing::Corefinement::Default_visitor<CutMesh> {
    Visitor(const CutMesh &object, const CutMesh &shape, EdgeShapeMap edge_shape_map,
            FaceShapeMap face_shape_map, VertexShapeMap vert_shape_map, bool* is_valid) :
        object(object), shape(shape), edge_shape_map(edge_shape_map), face_shape_map(face_shape_map),
        vert_shape_map(vert_shape_map), is_valid(is_valid)
    {}

    const CutMesh &object;
    const CutMesh &shape;

    // Properties of the shape mesh:
    EdgeShapeMap edge_shape_map;
    FaceShapeMap face_shape_map;

    // Properties of the object mesh.
    VertexShapeMap vert_shape_map;

    // check for anomalities
    bool* is_valid;

    // keep source of intersection for each intersection
    // used to copy data into vert_shape_map
    std::vector<const IntersectingElement*> intersections;

    /// <summary>
    /// Called when a new intersection point is detected.
    /// The intersection is detected using a face of tm_f and an edge of tm_e.
    /// Intersecting an edge hh_edge from tm_f with a face h_e of tm_e.
    /// https://doc.cgal.org/latest/Polygon_mesh_processing/classPMPCorefinementVisitor.html#a00ee0ca85db535a48726a92414acda7f
    /// </summary>
    /// <param name="i_id">The id of the intersection point, starting at 0. Ids are consecutive.</param>
    /// <param name="sdim">Dimension of a simplex part of face(h_e) that is intersected by edge(h_f):
    /// 0 for vertex: target(h_e)
    /// 1 for edge: h_e
    /// 2 for the interior of face: face(h_e) </param>
    /// <param name="h_f">
    /// A halfedge from tm_f indicating the simplex intersected: 
    /// if sdim==0 the target of h_f is the intersection point, 
    /// if sdim==1 the edge of h_f contains the intersection point in its interior,
    /// if sdim==2 the face of h_f contains the intersection point in its interior.
    /// @Vojta: Edge of tm_f, see is_target_coplanar & is_source_coplanar whether any vertex of h_f is coplanar with face(h_e).
    /// </param>
    /// <param name="h_e">A halfedge from tm_e
    /// @Vojta: Vertex, halfedge or face of tm_e intersected by h_f, see comment at sdim.
    /// </param>
    /// <param name="tm_f">Mesh containing h_f</param>
    /// <param name="tm_e">Mesh containing h_e</param>
    /// <param name="is_target_coplanar">True if the target of h_e is the intersection point
    /// @Vojta: source(h_f) is coplanar with face(made by h_e).</param>
    /// <param name="is_source_coplanar">True if the source of h_e is the intersection point
    /// @Vojta: target(h_f) is coplanar with face(h_e).</param>
    void intersection_point_detected(std::size_t    i_id,
                                     int            sdim,
                                     HI             h_f,
                                     HI             h_e,
                                     const CutMesh &tm_f,
                                     const CutMesh &tm_e,
                                     bool           is_target_coplanar,
                                     bool           is_source_coplanar);

    /// <summary>
    /// Called when a new vertex is added in tm (either an edge split or a vertex inserted in the interior of a face).
    /// Fill vertex_shape_map by intersections
    /// </summary>
    /// <param name="i_id">Order number of intersection point</param>
    /// <param name="v">New added vertex</param>
    /// <param name="tm">Affected mesh</param>
    void new_vertex_added(std::size_t i_id, VI v, const CutMesh &tm);
};

/// <summary>
/// Distiquish face type for half edge
/// </summary>
/// <param name="hi">Define face</param>
/// <param name="mesh">Mesh to process</param>
/// <param name="shape_mesh">Vertices of mesh made by shapes</param>
/// <param name="vertex_shape_map">Keep information about source of created vertex</param>
/// <param name="shape2index"></param>
/// <param name="shape2index">Convert index to shape point from ExPolygons</param>
/// <returns>Face type defined by hi</returns>
bool is_face_inside(HI                      hi,
                    const CutMesh          &mesh,
                    const CutMesh          &shape_mesh,
                    const VertexShapeMap   &vertex_shape_map,
                    const ExPolygonsIndices &shape2index);

/// <summary>
/// Face with constrained edge are inside/outside by type of intersection
/// Other set to not_constrained(still it could be inside/outside)
/// </summary>
/// <param name="face_type_map">[Output] property map with type of faces</param>
/// <param name="mesh">Mesh to process</param>
/// <param name="vertex_shape_map">Keep information about source of created vertex</param>
/// <param name="ecm">Dynamic Edge Constrained Map of bool</param>
/// <param name="shape_mesh">Vertices of mesh made by shapes</param>
/// <param name="shape2index">Convert index to shape point from ExPolygons</param>
void set_face_type(FaceTypeMap            &face_type_map,
                   const CutMesh          &mesh,
                   const VertexShapeMap   &vertex_shape_map,
                   const EdgeBoolMap          &ecm,
                   const CutMesh          &shape_mesh,
                   const ExPolygonsIndices &shape2index);

/// <summary>
/// Change FaceType from not_constrained to inside
/// For neighbor(or neighbor of neighbor of ...) of inside triangles.
/// Process only not_constrained triangles
/// </summary>
/// <param name="mesh">Corefined mesh</param>
/// <param name="face_type_map">In/Out map with faces type</param>
void flood_fill_inner(const CutMesh &mesh, FaceTypeMap &face_type_map);

/// <summary>
/// Collect connected inside faces
/// Collect outline half edges
/// </summary>
/// <param name="process">Queue of face to process - find connected</param>
/// <param name="faces">[Output] collected Face indices from mesh</param>
/// <param name="outlines">[Output] collected Halfedge indices from mesh</param>
/// <param name="face_type_map">Use flag inside / outside
/// NOTE: Modify in function: inside -> inside_processed</param>
/// <param name="mesh">mesh to process</param>
void collect_surface_data(std::queue<FI>  &process,
                          std::vector<FI> &faces,
                          std::vector<HI> &outlines,
                          FaceTypeMap     &face_type_map,
                          const CutMesh   &mesh);

/// <summary>
/// Create areas from mesh surface
/// </summary>
/// <param name="mesh">Model</param>
/// <param name="shapes">Cutted shapes</param>
/// <param name="face_type_map">Define Triangles of interest.
/// Edge between inside / outside.
/// NOTE: Not const because it need to flag proccessed faces</param>
/// <returns>Areas of interest from mesh</returns>
CutAOIs create_cut_area_of_interests(const CutMesh    &mesh,
                                     const ExPolygons &shapes,
                                     FaceTypeMap      &face_type_map);

} // namespace priv

void priv::Visitor::intersection_point_detected(std::size_t    i_id,
                                                int            sdim,
                                                HI             h_f,
                                                HI             h_e,
                                                const CutMesh &tm_f,
                                                const CutMesh &tm_e,
                                                bool is_target_coplanar,
                                                bool is_source_coplanar)
{
    if (i_id >= intersections.size()) {
        size_t capacity = Slic3r::next_highest_power_of_2(i_id + 1);
        intersections.reserve(capacity);
        intersections.resize(capacity);
    }

    const IntersectingElement *intersection_ptr = nullptr;
    if (&tm_e == &shape) {
        assert(&tm_f == &object);
        switch (sdim) {
        case 1:
            // edge x edge intersection
            intersection_ptr = &edge_shape_map[shape.edge(h_e)];
            break;
        case 2:
            // edge x face intersection
            intersection_ptr = &face_shape_map[shape.face(h_e)];
            break;
        default: assert(false);
        }
        if (is_target_coplanar)
            vert_shape_map[object.source(h_f)] = intersection_ptr;
        if (is_source_coplanar)
            vert_shape_map[object.target(h_f)] = intersection_ptr;
    } else {
        assert(&tm_f == &shape && &tm_e == &object);
        assert(!is_target_coplanar);
        assert(!is_source_coplanar);
        if (is_target_coplanar || is_source_coplanar) 
            *is_valid = false;
        intersection_ptr = &edge_shape_map[shape.edge(h_f)];
        if (sdim == 0) vert_shape_map[object.target(h_e)] = intersection_ptr;
    }

    if (intersection_ptr->shape_point_index == std::numeric_limits<uint32_t>::max()) {
        // there is unexpected intersection
        // Top (or Bottom) shape contour edge (or vertex) intersection
        // Suggest to change projection min/max limits
        *is_valid = false;
    }
    intersections[i_id] = intersection_ptr;
}

void priv::Visitor::new_vertex_added(std::size_t i_id, VI v, const CutMesh &tm)
{
    assert(&tm == &object);
    assert(i_id < intersections.size());
    const IntersectingElement *intersection_ptr = intersections[i_id];
    assert(intersection_ptr != nullptr);
    // intersection was not filled in function intersection_point_detected
    //assert(intersection_ptr->point_index != std::numeric_limits<uint32_t>::max());
    vert_shape_map[v] = intersection_ptr;
}

bool priv::is_face_inside(HI                      hi,
                          const CutMesh          &mesh,
                          const CutMesh          &shape_mesh,
                          const VertexShapeMap   &vertex_shape_map,
                          const ExPolygonsIndices &shape2index)
{
    VI vi_from = mesh.source(hi);
    VI vi_to   = mesh.target(hi);
    // This face has a constrained edge.
    const IntersectingElement &shape_from = *vertex_shape_map[vi_from];
    const IntersectingElement &shape_to   = *vertex_shape_map[vi_to];
    assert(shape_from.shape_point_index != std::numeric_limits<uint32_t>::max());
    assert(shape_from.attr != (unsigned char) IntersectingElement::Type::undefined);
    assert(shape_to.shape_point_index != std::numeric_limits<uint32_t>::max());
    assert(shape_to.attr != (unsigned char) IntersectingElement::Type::undefined);

    // index into contour
    uint32_t                  i_from    = shape_from.shape_point_index;
    uint32_t                  i_to      = shape_to.shape_point_index;
    IntersectingElement::Type type_from = shape_from.get_type();
    IntersectingElement::Type type_to   = shape_to.get_type();
    if (i_from == i_to && type_from == type_to) {
        // intersecting element must be face
        assert(type_from == IntersectingElement::Type::face_1 ||
                type_from == IntersectingElement::Type::face_2);

        // count of vertices is twice as count of point in the contour
        uint32_t i = i_from * 2;
        // j is next contour point in vertices
        uint32_t j = i + 2;
        if (shape_from.is_last()) {
            ExPolygonsIndex point_id = shape2index.cvt(i_from);
            point_id.point_index  = 0;
            j = shape2index.cvt(point_id)*2;
        }

        // opposit point(in triangle face) to edge
        const P3 &p = mesh.point(mesh.target(mesh.next(hi)));

        // abc is source triangle face
        CGAL::Sign abcp = type_from == IntersectingElement::Type::face_1 ?
                        CGAL::orientation(shape_mesh.point(VI(i)),
                                            shape_mesh.point(VI(i + 1)),
                                            shape_mesh.point(VI(j)), p) :
                        // type_from == IntersectingElement::Type::face_2
                        CGAL::orientation(shape_mesh.point(VI(j)),
                                            shape_mesh.point(VI(i + 1)),
                                            shape_mesh.point(VI(j + 1)), p);
        return abcp == CGAL::POSITIVE;
    } else if (i_from < i_to || (i_from == i_to && type_from < type_to)) {
        bool is_last = shape_to.is_last() && shape_from.is_first();
        // check continuity of indicies
        assert(i_from == i_to || is_last || (i_from + 1) == i_to);
        return !is_last;
    } else {
        assert(i_from > i_to || (i_from == i_to && type_from > type_to));
        bool is_last = shape_to.is_first() && shape_from.is_last();
        // check continuity of indicies
        assert(i_from == i_to || is_last || (i_to + 1) == i_from);
        return is_last;
    }

    assert(false);
    return false;
}

void priv::set_face_type(FaceTypeMap            &face_type_map,
                         const CutMesh          &mesh,
                         const VertexShapeMap   &vertex_shape_map,
                         const EdgeBoolMap      &ecm,
                         const CutMesh          &shape_mesh,
                         const ExPolygonsIndices &shape2index)
{
    for (EI ei : mesh.edges()) {
        if (!ecm[ei]) continue;
        HI hi = mesh.halfedge(ei);
        FI fi = mesh.face(hi);
        bool is_inside = is_face_inside(hi, mesh, shape_mesh, vertex_shape_map, shape2index);        
        face_type_map[fi] = is_inside ? FaceType::inside : FaceType::outside;
        HI hi_op = mesh.opposite(hi);
        assert(hi_op.is_valid());
        if (!hi_op.is_valid()) continue;
        FI fi_op = mesh.face(hi_op);
        assert(fi_op.is_valid());
        if (!fi_op.is_valid()) continue;
        face_type_map[fi_op] = (!is_inside) ? FaceType::inside : FaceType::outside;
    }
}

priv::CutAOIs priv::cut_from_model(CutMesh                &cgal_model,
                                   const ExPolygons       &shapes,
                                   CutMesh                &cgal_shape,
                                   float                   projection_ratio,
                                   const ExPolygonsIndices &s2i)
{
    // pointer to edge or face shape_map
    VertexShapeMap vert_shape_map = cgal_model.add_property_map<VI, const IntersectingElement*>(vert_shape_map_name, nullptr).first;
    
    // detect anomalities in visitor.
    bool is_valid = true;
    // NOTE: map are created when convert shapes to cgal model
    const EdgeShapeMap& edge_shape_map = cgal_shape.property_map<EI, IntersectingElement>(edge_shape_map_name).first;
    const FaceShapeMap& face_shape_map = cgal_shape.property_map<FI, IntersectingElement>(face_shape_map_name).first;
    Visitor visitor{cgal_model, cgal_shape, edge_shape_map, face_shape_map, vert_shape_map, &is_valid};

    // a property map containing the constrained-or-not status of each edge
    EdgeBoolMap ecm = cgal_model.add_property_map<EI, bool>(is_constrained_edge_name, false).first;
    const auto &p = CGAL::parameters::visitor(visitor)
                        .edge_is_constrained_map(ecm)
                        .throw_on_self_intersection(false);
    const auto& q = CGAL::parameters::do_not_modify(true);
    CGAL::Polygon_mesh_processing::corefine(cgal_model, cgal_shape, p, q);

    if (!is_valid) return {};

    FaceTypeMap face_type_map = cgal_model.add_property_map<FI, FaceType>(face_type_map_name, FaceType::not_constrained).first;

    // Select inside and outside face in model
    set_face_type(face_type_map, cgal_model, vert_shape_map, ecm, cgal_shape, s2i);
#ifdef DEBUG_OUTPUT_DIR
    store(cgal_model, face_type_map, DEBUG_OUTPUT_DIR + "constrained/"); // only debug
#endif // DEBUG_OUTPUT_DIR
    
    // flood fill the other faces inside the region.
    flood_fill_inner(cgal_model, face_type_map);

#ifdef DEBUG_OUTPUT_DIR
    store(cgal_model, face_type_map, DEBUG_OUTPUT_DIR + "filled/", true); // only debug
#endif // DEBUG_OUTPUT_DIR
        
    // IMPROVE: AOIs area could be created during flood fill
    return create_cut_area_of_interests(cgal_model, shapes, face_type_map);
}

void priv::flood_fill_inner(const CutMesh &mesh,
                            FaceTypeMap   &face_type_map)
{
    std::vector<FI> process;
    // guess count of connected not constrained triangles
    size_t guess_size = 128;
    process.reserve(guess_size);

    // check if neighbor(one of three in triangle) has type inside
    auto has_inside_neighbor = [&mesh, &face_type_map](FI fi) {
        HI hi     = mesh.halfedge(fi);
        HI hi_end = hi;
        auto exist_next = [&hi, &hi_end, &mesh]() -> bool {
            hi = mesh.next(hi);
            return hi != hi_end;
        };
        // loop over 3 half edges of face
        do {
            HI hi_opposite = mesh.opposite(hi);
            // open edge doesn't have opposit half edge
            if (!hi_opposite.is_valid()) continue;
            FI fi_opposite = mesh.face(hi_opposite);
            if (!fi_opposite.is_valid()) continue;
            if (face_type_map[fi_opposite] == FaceType::inside) return true;
        } while (exist_next());
        return false;
    };

    for (FI fi : mesh.faces()) {
        FaceType type = face_type_map[fi];
        if (type != FaceType::not_constrained) continue;
        if (!has_inside_neighbor(fi)) continue;
        assert(process.empty());
        process.push_back(fi); 
        //store(mesh, face_type_map, DEBUG_OUTPUT_DIR + "progress.off");

        while (!process.empty()) {
            FI process_fi = process.back();
            process.pop_back();
            // Do not fill twice
            FaceType& process_type = face_type_map[process_fi];
            if (process_type == FaceType::inside) continue;
            process_type = FaceType::inside;

            // check neighbor triangle
            HI hi = mesh.halfedge(process_fi);
            HI hi_end = hi;
            auto exist_next = [&hi, &hi_end, &mesh]() -> bool {
                hi = mesh.next(hi);
                return hi != hi_end;
            };
            do {
                HI hi_opposite = mesh.opposite(hi);
                // open edge doesn't have opposit half edge
                if (!hi_opposite.is_valid()) continue;                
                FI fi_opposite = mesh.face(hi_opposite);
                if (!fi_opposite.is_valid()) continue;                
                FaceType type_opposite = face_type_map[fi_opposite];
                if (type_opposite == FaceType::not_constrained)
                    process.push_back(fi_opposite);
            } while (exist_next());
        }
    }
}

void priv::collect_surface_data(std::queue<FI>  &process,
                                std::vector<FI> &faces,
                                std::vector<HI> &outlines,
                                FaceTypeMap     &face_type_map,
                                const CutMesh   &mesh)
{
    assert(!process.empty());
    assert(faces.empty());
    assert(outlines.empty());
    while (!process.empty()) {
        FI fi = process.front();
        process.pop();

        FaceType &fi_type = face_type_map[fi];
        // Do not process twice
        if (fi_type == FaceType::inside_processed) continue;
        assert(fi_type == FaceType::inside);
        // flag face as processed
        fi_type = FaceType::inside_processed;
        faces.push_back(fi);

        // check neighbor triangle
        HI hi     = mesh.halfedge(fi);
        HI hi_end = hi;
        do {
            HI hi_opposite = mesh.opposite(hi);
            // open edge doesn't have opposit half edge
            if (!hi_opposite.is_valid()) { 
                outlines.push_back(hi);
                hi = mesh.next(hi);
                continue; 
            }            
            FI fi_opposite = mesh.face(hi_opposite);
            if (!fi_opposite.is_valid()) {
                outlines.push_back(hi);
                hi = mesh.next(hi);
                continue;
            }
            FaceType side = face_type_map[fi_opposite];
            if (side == FaceType::inside) {
                process.emplace(fi_opposite);
            } else if (side == FaceType::outside) {
                // store outlines
                outlines.push_back(hi);
            }
            hi = mesh.next(hi);
        } while (hi != hi_end);
    }
}

void priv::create_reduce_map(ReductionMap &reduction_map, const CutMesh &mesh)
{
    const VertexShapeMap &vert_shape_map = mesh.property_map<VI, const IntersectingElement*>(vert_shape_map_name).first;
    const EdgeBoolMap &ecm = mesh.property_map<EI, bool>(is_constrained_edge_name).first;

    // check if vertex was made by edge_2 which is diagonal of quad
    auto is_reducible_vertex = [&vert_shape_map](VI reduction_from) -> bool {
        const IntersectingElement *ie = vert_shape_map[reduction_from];
        if (ie == nullptr) return false;
        IntersectingElement::Type type = ie->get_type();
        return type == IntersectingElement::Type::edge_2;
    };

    /// <summary>
    /// Append reduction or change existing one.
    /// </summary>
    /// <param name="hi">HalEdge between outside and inside face.
    /// Target vertex will be reduced, source vertex left</param>
    /// [[maybe_unused]] &face_type_map, &is_reducible_vertex are need only in debug
    auto add_reduction = [&] //&reduction_map, &mesh, &face_type_map, &is_reducible_vertex
    (HI hi) {
        VI erase = mesh.target(hi);
        VI left = mesh.source(hi);
        assert(is_reducible_vertex(erase));
        assert(!is_reducible_vertex(left));
        VI &vi = reduction_map[erase];
        // check if it is first add
        if (vi.is_valid())
            return;

        // check that all triangles after reduction has 'erase' and 'left' vertex
        // on same side of opposite line of vertex in triangle
        Vec3d v_erase = to_vec3d(mesh.point(erase));
        Vec3d v_left = to_vec3d(mesh.point(left));
        for (FI fi : mesh.faces_around_target(hi)) {
            if (!fi.is_valid())
                continue;
            // get vertices of rest 
            VI vi_a, vi_b;
            for (VI vi : mesh.vertices_around_face(mesh.halfedge(fi))) {
                if (!vi.is_valid())
                    continue;
                if (vi == erase)
                    continue;
                if (!vi_a.is_valid())
                    vi_a = vi;
                else {
                    assert(!vi_b.is_valid());
                    vi_b = vi;
                }
            } 
            assert(vi_b.is_valid());
            // do not check triangle, which will be removed
            if (vi_a == left || vi_b == left)
                continue;
            
            Vec3d v_a = to_vec3d(mesh.point(vi_a));
            Vec3d v_b = to_vec3d(mesh.point(vi_b));
            // Vectors of triangle edges
            Vec3d v_ab = v_b - v_a;
            Vec3d v_ae = v_erase - v_a;
            Vec3d v_al = v_left - v_a;

            Vec3d n1 = v_ab.cross(v_ae);
            Vec3d n2 = v_ab.cross(v_al);
            // check that normal has same direction
            if (((n1.x() > 0) != (n2.x() > 0)) ||
                ((n1.y() > 0) != (n2.y() > 0)) ||
                ((n1.z() > 0) != (n2.z() > 0)))
                return; // this reduction will create CCW triangle
        }

        reduction_map[erase] = left;
        // I have no better rule than take the first
        // for decide which reduction will be better
        // But it could be use only one of them
    };

    for (EI ei : mesh.edges()) { 
        if (!ecm[ei]) continue;
        HI hi = mesh.halfedge(ei);
        VI vi = mesh.target(hi);
        if (is_reducible_vertex(vi)) add_reduction(hi);
        
        HI hi_op = mesh.opposite(hi);
        VI vi_op = mesh.target(hi_op);
        if (is_reducible_vertex(vi_op)) add_reduction(hi_op);
    }
#ifdef DEBUG_OUTPUT_DIR
    store(mesh, reduction_map, DEBUG_OUTPUT_DIR + "reduces/");
#endif // DEBUG_OUTPUT_DIR
}

priv::CutAOIs priv::create_cut_area_of_interests(const CutMesh    &mesh,
                                                 const ExPolygons &shapes,
                                                 FaceTypeMap &face_type_map)
{
    // IMPROVE: Create better heuristic for count.
    size_t faces_per_cut    = mesh.faces().size() / shapes.size();
    size_t outlines_per_cut = faces_per_cut / 2;
    size_t cuts_per_model   = shapes.size() * 2;

    CutAOIs result;
    result.reserve(cuts_per_model);

    // It is faster to use one queue for all cuts
    std::queue<FI> process;
    for (FI fi : mesh.faces()) {
        if (face_type_map[fi] != FaceType::inside) continue;

        CutAOI cut;
        std::vector<FI> &faces    = cut.first;
        std::vector<HI> &outlines = cut.second;

        // faces for one surface cut
        faces.reserve(faces_per_cut);
        // outline for one surface cut
        outlines.reserve(outlines_per_cut);

        assert(process.empty());
        // Process queue of faces to separate to surface_cut
        process.push(fi);
        collect_surface_data(process, faces, outlines, face_type_map, mesh);
        assert(!faces.empty());
        assert(!outlines.empty());
        result.emplace_back(std::move(cut));
    }
    return result;
}

namespace priv {

/// <summary>
/// Calculate projection distance of point [in mm]
/// </summary>
/// <param name="p">Point to calc distance</param>
/// <param name="pi">Index of point on contour</param>
/// <param name="shapes_mesh">Model of cutting shape</param>
/// <param name="projection_ratio">Ratio for best projection distance</param>
/// <returns>Distance of point from best projection</returns>
float calc_distance(const P3 &p,
                    uint32_t  pi,                    
                    const CutMesh &shapes_mesh,
                    float          projection_ratio);

}

float priv::calc_distance(const P3 &p,
                          uint32_t  pi,                          
                          const CutMesh &shapes_mesh,
                          float          projection_ratio)
{
    // It is known because shapes_mesh is created inside of private space
    VI vi_start(2 * pi);
    VI vi_end(2 * pi + 1);

    // Get range for intersection
    const P3 &start = shapes_mesh.point(vi_start);
    const P3 &end   = shapes_mesh.point(vi_end);

    // find index in vector with biggest difference
    size_t max_i   = 0;
    float  max_val = 0.f;
    for (size_t i = 0; i < 3; i++) {
        float val = start[i] - end[i];
        // abs value
        if (val < 0.f) val *= -1.f;
        if (max_val < val) {
            max_val = val;
            max_i   = i;
        }
    }

    float from_start    = p[max_i] - start[max_i];
    float best_distance = projection_ratio * (end[max_i] - start[max_i]);
    return from_start - best_distance;
}

priv::VDistances priv::calc_distances(const SurfacePatches &patches,
                                      const CutMeshes &models,
                                      const CutMesh &shapes_mesh,
                                      size_t count_shapes_points,
                                      float projection_ratio)
{
    priv::VDistances result(count_shapes_points);
    for (const SurfacePatch &patch : patches) {
        // map is created during intersection by corefine visitor
        const VertexShapeMap &vert_shape_map = 
            models[patch.model_id].property_map<VI, const IntersectingElement *>(vert_shape_map_name).first;
        uint32_t patch_index = &patch - &patches.front();
        // map is created during patch creation / dividing
        const CvtVI2VI& cvt = patch.mesh.property_map<VI, VI>(patch_source_name).first;
        // for each point on outline
        for (const Loop &loop : patch.loops) 
        for (const VI &vi_patch : loop) {
            VI vi_model = cvt[vi_patch];
            if (!vi_model.is_valid()) continue;            
            const IntersectingElement *ie = vert_shape_map[vi_model];
            if (ie == nullptr) continue;
            assert(ie->shape_point_index != std::numeric_limits<uint32_t>::max());
            assert(ie->attr != (unsigned char) IntersectingElement::Type::undefined);
            uint32_t pi = ie->shape_point_index;
            assert(pi <= count_shapes_points);
            std::vector<ProjectionDistance> &pds = result[pi];
            uint32_t model_index = patch.model_id;
            uint32_t  aoi_index = patch.aoi_id;
            //uint32_t  hi_index  = &hi - &patch.outline.front();
            const P3 &p = patch.mesh.point(vi_patch);
            float distance = calc_distance(p, pi, shapes_mesh, projection_ratio);
            pds.push_back({model_index, aoi_index, patch_index, distance});
        }
    }
    return result;
}


#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/Line.hpp"
// functions for choose_best_distance
namespace priv {

// euler square size of vector stored in Point
float calc_size_sq(const Point &p);

// structure to store index and distance together
struct ClosePoint
{
    // index of closest point from another shape
    uint32_t index = std::numeric_limits<uint32_t>::max();
    // squere distance to index
    float dist_sq = std::numeric_limits<float>::max();
};

struct SearchData{
// IMPROVE: float lines are enough
std::vector<Linef> lines;
// convert line index into Shape point index
std::vector<size_t> cvt;
// contain lines from prev point to Point index
AABBTreeIndirect::Tree<2, double> tree;
};

SearchData create_search_data(const ExPolygons &shapes, const std::vector<bool>& mask);
uint32_t get_closest_point_index(const SearchData &sd, size_t line_idx, const Vec2d &hit_point, const ExPolygons &shapes, const ExPolygonsIndices &s2i);

// use AABB Tree Lines to find closest point
uint32_t find_closest_point_index(const Point &p, const ExPolygons &shapes, const ExPolygonsIndices &s2i, const std::vector<bool> &mask);

std::pair<uint32_t, uint32_t> find_closest_point_pair(const ExPolygons &shapes, const std::vector<bool> &done_shapes, const ExPolygonsIndices  &s2i, const std::vector<bool> &mask);

// Search for closest projection to wanted distance
const ProjectionDistance *get_closest_projection(const ProjectionDistances &distance, float wanted_distance);

// fill result around known index inside one polygon
void fill_polygon_distances(const ProjectionDistance &pd, uint32_t index, const ExPolygonsIndex &id, ProjectionDistances & result, const ExPolygon &shape, const VDistances &distances);

// search for closest projection for expolygon
// choose correct cut by source point
void fill_shape_distances(uint32_t start_index, const ProjectionDistance *start_pd, ProjectionDistances &result, const ExPolygonsIndices &s2i, const ExPolygon &shape, const VDistances &distances);

// find close points between finished and unfinished ExPolygons
ClosePoint find_close_point(const Point &p, ProjectionDistances &result, std::vector<bool>& finished_shapes,const ExPolygonsIndices &s2i, const ExPolygons &shapes);

}

float priv::calc_size_sq(const Point &p){    
    // NOTE: p.squaredNorm() can't be use due to overflow max int value
    return (float) p.x() * p.x() + (float) p.y() * p.y();
}

priv::SearchData priv::create_search_data(const ExPolygons        &shapes,
                                          const std::vector<bool> &mask)
{
    // IMPROVE: Use float precission (it is enough)
    SearchData sd;
    sd.lines.reserve(mask.size());
    sd.cvt.reserve(mask.size());
    size_t index     = 0;
    auto   add_lines = [&sd, &index, &mask]
    (const Polygon &poly) {
        Vec2d prev = poly.back().cast<double>();
        bool use_point = mask[index + poly.points.size() - 1];
        for (const Point &p : poly.points) {
            if (!use_point) {
                use_point = mask[index];
                if (use_point) prev = p.cast<double>();
            } else if (!mask[index]) {
                use_point = false;
            } else {
                Vec2d p_d = p.cast<double>();
                sd.lines.emplace_back(prev, p_d);
                sd.cvt.push_back(index);
                prev = p_d;
            }
            ++index;
        }
    };

    for (const ExPolygon &shape : shapes) {
        add_lines(shape.contour);
        for (const Polygon &hole : shape.holes) add_lines(hole);
    }
    sd.tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(sd.lines);
    return sd;
}

uint32_t priv::get_closest_point_index(const SearchData &sd,
                                       size_t            line_idx,
                                       const Vec2d      &hit_point,
                                       const ExPolygons       &shapes,
                                       const ExPolygonsIndices &s2i)
{
    const Linef &line = sd.lines[line_idx];
    Vec2d dir = line.a - line.b;
    Vec2d dir_abs = dir.cwiseAbs();
    // use x coordinate
    int i = (dir_abs.x() > dir_abs.y())? 0 :1;

    bool use_index = abs(line.a[i] - hit_point[i]) >
                     abs(line.b[i] - hit_point[i]);
    size_t point_index = sd.cvt[line_idx];

    // Lambda used only for check result
    [[maybe_unused]] auto is_same = [&s2i, &shapes]
    (const Vec2d &p, size_t i) -> bool {
        auto id = s2i.cvt(i);
        const ExPolygon &shape = shapes[id.expolygons_index];
        const Polygon   &poly  = (id.polygon_index == 0) ?
                                           shape.contour :
                                           shape.holes[id.polygon_index - 1];
        Vec2crd p_ = p.cast<coord_t>();
        return p_ == poly[id.point_index];
    };

    if (use_index) { 
        assert(is_same(line.b, point_index));
        return point_index; 
    }
    auto id = s2i.cvt(point_index);
    if (id.point_index != 0) {
        assert(is_same(line.a, point_index - 1));
        return point_index - 1;
    }
    const ExPolygon &shape = shapes[id.expolygons_index];
    size_t count_polygon_points = (id.polygon_index == 0) ?
        shape.contour.size() :
        shape.holes[id.polygon_index - 1].size();
    size_t prev_point_index = point_index  + (count_polygon_points - 1);
    assert(is_same(line.a, prev_point_index));
    // return previous point index
    return prev_point_index;
}

// use AABB Tree Lines
uint32_t priv::find_closest_point_index(const Point            &p,
                                        const ExPolygons       &shapes,
                                        const ExPolygonsIndices &s2i,
                                        const std::vector<bool> &mask)
{    
    SearchData sd = create_search_data(shapes, mask);
    if (sd.tree.nodes().size() == 0){
        // no lines in expolygon, check whether exist point to start
        double closest_square_distance = INFINITY;
        uint32_t closest_id = -1; 
        for (uint32_t i = 0; i < mask.size(); i++) 
            if (mask[i]){
                ExPolygonsIndex ei = s2i.cvt(i);
                const Point& s_p = ei.is_contour()? 
                    shapes[ei.expolygons_index].contour[ei.point_index]:
                shapes[ei.expolygons_index].holes[ei.hole_index()][ei.point_index];
                double square_distance = (p - s_p).cast<double>().squaredNorm();
                if (closest_id >= mask.size() || 
                    closest_square_distance > square_distance) {
                    closest_id = i;
                    closest_square_distance = square_distance;
                }
            }
        assert(closest_id < mask.size());
        return closest_id;
    }
    size_t line_idx = std::numeric_limits<size_t>::max();
    Vec2d  hit_point;
    Vec2d  p_d = p.cast<double>();
    [[maybe_unused]] double distance_sq =
        AABBTreeLines::squared_distance_to_indexed_lines(
        sd.lines, sd.tree, p_d, line_idx, hit_point);
    assert(distance_sq > 0);
    
    // IMPROVE: one could use line ratio to find closest point
    return get_closest_point_index(sd, line_idx, hit_point, shapes, s2i);
}

std::pair<uint32_t, uint32_t> priv::find_closest_point_pair(
    const ExPolygons        &shapes,
    const std::vector<bool> &done_shapes,
    const ExPolygonsIndices  &s2i,
    const std::vector<bool> &mask)
{
    assert(mask.size() == s2i.get_count());
    assert(done_shapes.size() == shapes.size());
    std::vector<bool> unfinished_mask = mask; // copy

    size_t index = 0;
    for (size_t shape_index = 0; shape_index < shapes.size(); shape_index++) {
        size_t count = count_points(shapes[shape_index]);
        if (done_shapes[shape_index]) { 
            for (size_t i = 0; i < count; ++i, ++index)
                unfinished_mask[index] = false;
        } else {
            index += count;
        }
    }
    assert(index == s2i.get_count());
    SearchData sd = create_search_data(shapes, unfinished_mask);

    struct ClosestPair
    {
        size_t finish_idx          = std::numeric_limits<size_t>::max();
        size_t unfinished_line_idx = std::numeric_limits<size_t>::max();
        Vec2d  hit_point           = Vec2d();
        double distance_sq         = std::numeric_limits<double>::max();
    } cp;

    index = 0;
    for (size_t shape_index = 0; shape_index < shapes.size(); shape_index++) {
        const ExPolygon shape = shapes[shape_index];
        if (!done_shapes[shape_index]) {            
            index += count_points(shape);
            continue;
        }        

        auto search_in_polygon = [&index, &cp, &sd, &mask](const Polygon& polygon) {
            for (size_t i = 0; i < polygon.size(); ++i, ++index) { 
                if (mask[index] == false) continue;
                Vec2d p_d = polygon[i].cast<double>();
                size_t line_idx = std::numeric_limits<size_t>::max();
                Vec2d  hit_point;
                double distance_sq = AABBTreeLines::squared_distance_to_indexed_lines(
                    sd.lines, sd.tree, p_d, line_idx, hit_point, cp.distance_sq);
                if (distance_sq < 0 || 
                    distance_sq >= cp.distance_sq) continue;
                assert(line_idx < sd.lines.size());
                cp.distance_sq         = distance_sq;
                cp.unfinished_line_idx = line_idx;
                cp.hit_point           = hit_point;
                cp.finish_idx          = index;
            }
        };
        search_in_polygon(shape.contour);
        for (const Polygon& hole: shape.holes)
            search_in_polygon(hole);
    }
    assert(index == s2i.get_count());
    // check that exists result
    if (cp.finish_idx == std::numeric_limits<size_t>::max()) {
        return std::make_pair(std::numeric_limits<size_t>::max(),
                              std::numeric_limits<size_t>::max());
    }

    size_t unfinished_idx = get_closest_point_index(sd, cp.unfinished_line_idx, cp.hit_point, shapes, s2i);
    return std::make_pair(cp.finish_idx, unfinished_idx);
}

const priv::ProjectionDistance *priv::get_closest_projection(
    const ProjectionDistances &distance, float wanted_distance)
{
    // minimal distance
    float min_d = std::numeric_limits<float>::max();
    const ProjectionDistance *min_pd = nullptr;
    for (const ProjectionDistance &pd : distance) { 
        float d = std::fabs(pd.distance - wanted_distance);
        // There should be limit for maximal distance
        if (min_d > d) { 
            min_d = d;
            min_pd = &pd;
        }
    }
    return min_pd;
}

void priv::fill_polygon_distances(const ProjectionDistance &pd,
                                  uint32_t                  index,
                                  const ExPolygonsIndex       &id,
                                  ProjectionDistances      &result,
                                  const ExPolygon          &shape,
                                  const VDistances         &distances)
{
    const Points& points = (id.polygon_index == 0) ?
                                            shape.contour.points :
                                            shape.holes[id.polygon_index - 1].points;
    // border of indexes for Polygon
    uint32_t first_index = index - id.point_index;
    uint32_t last_index  = first_index + points.size();        

    uint32_t act_index = index;
    const ProjectionDistance* act_pd = &pd;

    // Copy starting pd to result
    result[act_index] = pd;

    auto exist_next = [&distances, &act_index, &act_pd, &result]
    (uint32_t nxt_index) {
        const ProjectionDistance *nxt_pd = get_closest_projection(distances[nxt_index] ,act_pd->distance);
        // exist next projection distance ?
        if (nxt_pd == nullptr) return false;

        // check no rewrite result
        assert(result[nxt_index].aoi_index == std::numeric_limits<uint32_t>::max());
        // copy founded projection to result
        result[nxt_index] = *nxt_pd; // copy

        // next
        act_index = nxt_index;
        act_pd    = &result[nxt_index];
        return true;
    };

    // last index in circle
    uint32_t finish_index = (index == first_index) ? (last_index - 1) :
                                                        (index - 1);
    // Positive iteration inside polygon
    do {
        uint32_t nxt_index = act_index + 1;
        // close loop of indexes inside of contour
        if (nxt_index == last_index) nxt_index = first_index;
        // check that exist next 
        if (!exist_next(nxt_index)) break;            
    } while (act_index != finish_index);

    // when all results for polygon are set no neccessary to iterate negative
    if (act_index == finish_index) return;

    act_index = index;
    act_pd    = &pd;
    // Negative iteration inside polygon
    do {
        uint32_t nxt_index = (act_index == first_index) ? 
            (last_index-1) : (act_index - 1);
        // When iterate negative it must be split to parts
        // and can't iterate in circle
        assert(nxt_index != index);
        // check that exist next 
        if (!exist_next(nxt_index)) break;
    } while (true);
}

// IMPROVE: when select distance fill in all distances from Patch
void priv::fill_shape_distances(uint32_t                  start_index,
                                const ProjectionDistance *start_pd,
                                ProjectionDistances      &result,
                                const ExPolygonsIndices   &s2i,
                                const ExPolygon         &shape,
                                const VDistances         &distances)
{
    uint32_t expolygons_index = s2i.cvt(start_index).expolygons_index;
    uint32_t first_shape_index = s2i.cvt({expolygons_index, 0, 0});
    do {
        fill_polygon_distances(*start_pd, start_index, s2i.cvt(start_index),result, shape, distances);
        // seaching only inside shape, return index of closed finished point
        auto find_close_finished_point = [&first_shape_index, &shape, &result]
        (const Point &p) -> ClosePoint {
            uint32_t index = first_shape_index;                
            ClosePoint cp;
            auto check_finished_points = [&cp, &result, &index, &p]
            (const Points& pts) { 
                for (const Point &p_ : pts) {
                    // finished point with some distances
                    if (result[index].aoi_index == std::numeric_limits<uint32_t>::max()) {
                        ++index;
                        continue;
                    }
                    float distance = calc_size_sq(p_ - p);
                    if (cp.dist_sq > distance) { 
                        cp.dist_sq = distance;
                        cp.index   = index;
                    }
                    ++index;
                }
            };
            check_finished_points(shape.contour.points);
            for (const Polygon &h : shape.holes)
                check_finished_points(h.points);
            return cp;
        };
                        
        // find next closest pair of points
        // (finished + unfinished) in ExPolygon
        start_index = std::numeric_limits<uint32_t>::max(); // unfinished_index
        uint32_t finished_index = std::numeric_limits<uint32_t>::max();
        float dist_sq = std::numeric_limits<float>::max();

        // first index in shape
        uint32_t index = first_shape_index;
        auto check_unfinished_points = [&index, &result, &distances, &find_close_finished_point, &dist_sq, &start_index, &finished_index]
        (const Points& pts) { 
            for (const Point &p : pts) {
                // try find unfinished
                if (result[index].aoi_index !=
                    std::numeric_limits<uint32_t>::max() ||
                    distances[index].empty()) {
                    ++index;
                    continue;
                }
                ClosePoint cp = find_close_finished_point(p);
                if (dist_sq > cp.dist_sq) { 
                    dist_sq = cp.dist_sq;
                    start_index = index;
                    finished_index = cp.index;
                }
                ++index;
            }
        };
        // for each unfinished points
        check_unfinished_points(shape.contour.points);
        for (const Polygon &h : shape.holes)
            check_unfinished_points(h.points);
    } while (start_index != std::numeric_limits<uint32_t>::max());
}

priv::ClosePoint priv::find_close_point(const Point         &p,
                                        ProjectionDistances &result,
                                        std::vector<bool>   &finished_shapes,
                                        const ExPolygonsIndices &s2i,
                                        const ExPolygons       &shapes)
{
    // result
    ClosePoint cp;
    // for all finished points
    for (uint32_t shape_index = 0; shape_index < shapes.size(); ++shape_index) {
        if (!finished_shapes[shape_index]) continue;
        const ExPolygon &shape = shapes[shape_index];
        uint32_t index = s2i.cvt({shape_index, 0, 0});
        auto find_close_point_in_points = [&p, &cp, &index, &result]
        (const Points &pts){
            for (const Point &p_ : pts) {
                // Exist result (is finished) ?
                if (result[index].aoi_index ==
                    std::numeric_limits<uint32_t>::max()) {
                    ++index;
                    continue;
                }
                float distance_sq = calc_size_sq(p - p_);
                if (cp.dist_sq > distance_sq) { 
                    cp.dist_sq = distance_sq;
                    cp.index = index;
                }
                ++index;
            }
        };
        find_close_point_in_points(shape.contour.points);
        // shape could be inside of another shape's hole
        for (const Polygon& h:shape.holes)
            find_close_point_in_points(h.points);
    }
    return cp;
}

// IMPROVE: when select distance fill in all distances from Patch
priv::ProjectionDistances priv::choose_best_distance(
    const VDistances &distances, const ExPolygons &shapes, const Point &start, const ExPolygonsIndices &s2i, const SurfacePatches &patches)
{
    assert(distances.size() == count_points(shapes));

    // vector of patches for shape
    std::vector<std::vector<uint32_t>> shapes_patches(shapes.size());
    for (const SurfacePatch &patch : patches)
        shapes_patches[patch.shape_id].push_back(&patch-&patches.front());

    // collect one closest projection for each outline point
    ProjectionDistances result(distances.size());

    // store info about finished shapes
    std::vector<bool> finished_shapes(shapes.size(), {false});
    
    // wanted distance from ideal projection
    // Distances are relative to projection distance
    // so first wanted distance is the closest one (ZERO)
    float wanted_distance = 0.f;
    
    std::vector<bool> mask_distances(s2i.get_count(), {true});
    for (const auto &d : distances)
        if (d.empty()) mask_distances[&d - &distances.front()] = false;

    // Select point from shapes(text contour) which is closest to center (all in 2d)
    uint32_t unfinished_index = find_closest_point_index(start, shapes, s2i, mask_distances);
    assert(unfinished_index < s2i.get_count());
    if (unfinished_index >= s2i.get_count())
        // no point to select
        return result;

#ifdef DEBUG_OUTPUT_DIR
    Connections connections;
    connections.reserve(shapes.size());
    connections.emplace_back(unfinished_index, unfinished_index);
#endif // DEBUG_OUTPUT_DIR

    do {
        const ProjectionDistance* pd = get_closest_projection(distances[unfinished_index], wanted_distance);
        // selection of closest_id should proove that pd has value 
        // (functions: get_closest_point_index and find_close_point_in_points)
        assert(pd != nullptr);
        uint32_t expolygons_index = s2i.cvt(unfinished_index).expolygons_index;        
        const ExPolygon &shape = shapes[expolygons_index];
        std::vector<uint32_t> &shape_patches = shapes_patches[expolygons_index];
        if (shape_patches.size() == 1){
            // Speed up, only one patch so copy distance from patch
            uint32_t first_shape_index = s2i.cvt({expolygons_index, 0, 0});
            uint32_t laset_shape_index = first_shape_index + count_points(shape);
            for (uint32_t i = first_shape_index; i < laset_shape_index; ++i) { 
                const ProjectionDistances &pds = distances[i];
                if (pds.empty()) continue;
                // check that index belongs to patch
                assert(pds.front().patch_index == shape_patches.front());
                result[i] = pds.front();
                if (pds.size() == 1) continue;

                float relative_distance = fabs(result[i].distance - pd->distance);
                // patch could contain multiple value for one outline point
                // so choose closest to start point
                for (uint32_t pds_index = 1; pds_index < pds.size(); ++pds_index) {
                    // check that index still belongs to same patch
                    assert(pds[pds_index].patch_index == shape_patches.front());
                    float relative_distance2 = fabs(pds[pds_index].distance - pd->distance);
                    if (relative_distance > relative_distance2) { 
                        relative_distance = relative_distance2;
                        result[i]         = pds[pds_index];
                    }
                }
            }
        } else {
            // multiple patches for expolygon
            // check that exist patch to fill shape
            assert(!shape_patches.empty());
            fill_shape_distances(unfinished_index, pd, result, s2i, shape, distances);
        }

        finished_shapes[expolygons_index] = true;
        // The most close points between finished and unfinished shapes
        auto [finished, unfinished] = find_closest_point_pair(
            shapes, finished_shapes, s2i, mask_distances);

        // detection of end (best doesn't have value)
        if (finished == std::numeric_limits<uint32_t>::max()) break;

        assert(unfinished != std::numeric_limits<uint32_t>::max());
        const ProjectionDistance &closest_pd = result[finished];
        // check that best_cp is finished and has result
        assert(closest_pd.aoi_index != std::numeric_limits<uint32_t>::max());
        wanted_distance = closest_pd.distance;
        unfinished_index = unfinished;

#ifdef DEBUG_OUTPUT_DIR
        connections.emplace_back(finished, unfinished);
#endif // DEBUG_OUTPUT_DIR
    } while (true); //(unfinished_index != std::numeric_limits<uint32_t>::max());    
#ifdef DEBUG_OUTPUT_DIR
    store(shapes, mask_distances, connections, DEBUG_OUTPUT_DIR + "closest_points.svg");
#endif // DEBUG_OUTPUT_DIR
    return result;
}

// functions to help 'diff_model'
namespace priv {
const VI default_vi(std::numeric_limits<uint32_t>::max());

// Keep info about intersection source
struct Source{ HI  hi; int sdim=0;};
using Sources                            = std::vector<Source>;
const std::string vertex_source_map_name = "v:SourceIntersecting";
using VertexSourceMap                    = CutMesh::Property_map<VI, Source>;

/// <summary>
/// Corefine visitor
/// Store intersection source for vertices of constrained edge of tm1
/// Must be used with corefine flag no modification of tm2
/// </summary>
struct IntersectionSources
{
    const CutMesh *patch; // patch
    const CutMesh *model; // const model

    VertexSourceMap vmap;

    // keep sources from call intersection_point_detected
    // until call new_vertex_added
    Sources* sources;

    // count intersections
    void intersection_point_detected(std::size_t    i_id,
                                     int            sdim,
                                     HI             h_f,
                                     HI             h_e,
                                     const CutMesh &tm_f,
                                     const CutMesh &tm_e,
                                     bool           is_target_coplanar,
                                     bool           is_source_coplanar)
    {
        Source source;
        if (&tm_e == model) {
            source = {h_e, sdim};
            // check other CGAL model that is patch
            assert(&tm_f == patch);
            if (is_target_coplanar) {
                assert(sdim == 0);
                vmap[tm_f.source(h_f)] = source;
            }
            if (is_source_coplanar) {
                assert(sdim == 0);
                vmap[tm_f.target(h_f)] = source;
            }

            // clear source to be able check that this intersection source is
            // not used any more
            if (is_source_coplanar || is_target_coplanar) source = {};
        } else {
            source = {h_f, sdim};
            assert(&tm_f == model && &tm_e == patch);
            assert(!is_target_coplanar);
            assert(!is_source_coplanar);
            // if (is_target_coplanar) vmap[tm_e.source(h_e)] = source;
            // if (is_source_coplanar) vmap[tm_e.target(h_e)] = source;
            // if (sdim == 0)
            //     vmap[tm_e.target(h_e)] = source;
        }

        // By documentation i_id is consecutive.
        // check id goes in a row, without skips
        assert(sources->size() == i_id);
        // add source of intersection
        sources->push_back(source);
    }

    /// <summary>
    /// Store VI to intersections by i_id
    /// </summary>
    /// <param name="i_id">Order number of intersection point</param>
    /// <param name="v">New added vertex</param>
    /// <param name="tm">Affected mesh</param>
    void new_vertex_added(std::size_t i_id, VI v, const CutMesh &tm)
    {
        // check that it is first insertation into item of vmap
        assert(!vmap[v].hi.is_valid());
        // check valid addresing into sources
        assert(i_id < sources->size());
        // check that source has value
        assert(sources->at(i_id).hi.is_valid());
        vmap[v] = sources->at(i_id);
    }

    // Not used visitor functions
    void before_subface_creations(FI /* f_old */, CutMesh & /* mesh */) {}
    void after_subface_created(FI /* f_new */, CutMesh & /* mesh */) {}
    void after_subface_creations(CutMesh &) {}
    void before_subface_created(CutMesh &) {}
    void before_edge_split(HI /* h */, CutMesh & /* tm */) {}
    void edge_split(HI /* hnew */, CutMesh & /* tm */) {}
    void after_edge_split() {}
    void add_retriangulation_edge(HI /* h */, CutMesh & /* tm */) {}
};

/// <summary>
/// Create map1 and map2
/// </summary>
/// <param name="map">Convert tm1.face to type</param>
/// <param name="tm1">Corefined mesh</param>
/// <param name="tm2">Source of intersection</param>
/// <param name="ecm1">Identify constrainde edge</param>
/// <param name="sources">Convert tm1.face to type</param>
void create_face_types(FaceTypeMap           &map,
                       const CutMesh         &tm1,
                       const CutMesh         &tm2,
                       const EdgeBoolMap         &ecm,
                       const VertexSourceMap &sources);

/// <summary>
/// Implement 'cut' Minus 'clipper', where clipper is reverse input Volume
/// NOTE: clipper will be modified (corefined by cut) !!!
/// </summary>
/// <param name="cut">differ from</param>
/// <param name="clipper">differ what</param>
/// <returns>True on succes, otherwise FALSE</returns>
bool clip_cut(SurfacePatch &cut, CutMesh clipper);

BoundingBoxf3 bounding_box(const CutAOI &cut, const CutMesh &mesh);
BoundingBoxf3 bounding_box(const CutMesh &mesh);
BoundingBoxf3 bounding_box(const SurfacePatch &ecut);

/// <summary>
/// Create patch
/// </summary>
/// <param name="fis">Define patch faces</param>
/// <param name="mesh">Source of fis
/// NOTE: Need temporary add property map for convert vertices</param>
/// <param name="rmap">Options to reduce vertices from fis.
/// NOTE: Used for skip vertices made by diagonal edge in rectangle of shape side</param>
/// <returns>Patch</returns>
SurfacePatch create_surface_patch(const std::vector<FI> &fis,
                                  /*const*/ CutMesh     &mesh,
                                  const ReductionMap    *rmap = nullptr);

} // namespace priv

void priv::create_face_types(FaceTypeMap           &map,
                             const CutMesh         &tm1,
                             const CutMesh         &tm2,
                             const EdgeBoolMap         &ecm,
                             const VertexSourceMap &sources)
{
    auto get_intersection_source = [&tm2](const Source& s1, const Source& s2)->FI{        
        // when one of sources is face than return it
        FI fi1 = tm2.face(s1.hi);
        if (s1.sdim == 2) return fi1;
        FI fi2 = tm2.face(s2.hi);
        if (s2.sdim == 2) return fi2;
        // both vertices are made by same source triangle
        if (fi1 == fi2) return fi1;

        // when one from sources is edge second one decide side of triangle triangle
        HI hi1_opposit = tm2.opposite(s1.hi);
        FI fi1_opposit;
        if (hi1_opposit.is_valid())
            fi1_opposit = tm2.face(hi1_opposit);
        if (fi2 == fi1_opposit) return fi2;

        HI hi2_opposit = tm2.opposite(s2.hi);
        FI fi2_opposit;
        if (hi2_opposit.is_valid())
            fi2_opposit = tm2.face(hi2_opposit);
        if (fi1 == fi2_opposit) return fi1;
        if (fi1_opposit.is_valid() && fi1_opposit == fi2_opposit)
            return fi1_opposit;

        // when intersection is vertex need loop over neighbor
        for (FI fi_around_hi1 : tm2.faces_around_target(s1.hi)) {
            for (FI fi_around_hi2 : tm2.faces_around_target(s2.hi)) { 
                if (fi_around_hi1 == fi_around_hi2) 
                    return fi_around_hi1;
            }
        }

        // should never rich it
        // Exist case when do not know source triangle for decide side of intersection
        assert(false);
        return FI();
    };

    for (FI fi : tm1.faces()) map[fi] = FaceType::not_constrained;
    for (EI ei1 : tm1.edges()) {
        if (!get(ecm, ei1)) continue;

        // get faces from tm1 (f1a + f1b)
        HI hi1 = tm1.halfedge(ei1);
        assert(hi1.is_valid());
        FI f1a = tm1.face(hi1);
        assert(f1a.is_valid());
        HI hi_op = tm1.opposite(hi1);
        assert(hi_op.is_valid());
        FI f1b = tm1.face(hi_op);
        assert(f1b.is_valid());

        // get faces from tm2 (f2a + f2b)
        VI vi1_source = tm1.source(hi1);
        assert(vi1_source.is_valid());
        VI vi1_target = tm1.target(hi1);
        assert(vi1_target.is_valid());

        const Source &s_s = sources[vi1_source];
        const Source &s_t = sources[vi1_target];
        FI fi2 = get_intersection_source(s_s, s_t);

        // in release solve situation that face was NOT deduced
        if (!fi2.is_valid()) continue;

        HI hi2 = tm2.halfedge(fi2);
        std::array<const P3 *, 3> t;
        size_t ti =0;
        for (VI vi2 : tm2.vertices_around_face(hi2))
            t[ti++] = &tm2.point(vi2);

        // triangle tip from face f1a
        VI vi1a_tip = tm1.target(tm1.next(hi1));
        assert(vi1a_tip.is_valid());
        const P3 &p = tm1.point(vi1a_tip);

        // check if f1a is behinde f2a
        // inside mean it will be used
        // outside will be discarded
        if (CGAL::orientation(*t[0], *t[1], *t[2], p) == CGAL::POSITIVE) {
            map[f1a] = FaceType::inside;
            map[f1b] = FaceType::outside;
        } else {
            map[f1a] = FaceType::outside;
            map[f1b] = FaceType::inside;
        }
    }
}

#include <CGAL/Polygon_mesh_processing/clip.h>
#include <CGAL/Polygon_mesh_processing/corefinement.h>
bool priv::clip_cut(SurfacePatch &cut, CutMesh clipper)
{
    CutMesh& tm = cut.mesh; 
    // create backup for case that there is no intersection
    CutMesh backup_copy = tm; 

    class ExistIntersectionClipVisitor: public CGAL::Polygon_mesh_processing::Corefinement::Default_visitor<CutMesh>
    {
        bool* exist_intersection;
    public:
        ExistIntersectionClipVisitor(bool *exist_intersection): exist_intersection(exist_intersection){}
        void intersection_point_detected(std::size_t, int , HI, HI, const CutMesh&, const CutMesh&, bool, bool)
        { *exist_intersection = true;}
    };
    bool exist_intersection = false;
    ExistIntersectionClipVisitor visitor{&exist_intersection};    
    
    // namep parameters for model tm and function clip
    const auto &np_tm = CGAL::parameters::visitor(visitor)
                            .throw_on_self_intersection(false);
    
    // name parameters for model clipper and function clip
    const auto &np_c = CGAL::parameters::throw_on_self_intersection(false);
    // Can't use 'do_not_modify', when Ture than clipper has to be closed !!
    // .do_not_modify(true);
    // .throw_on_self_intersection(false); is set automaticaly by param 'do_not_modify'
    // .clip_volume(false); is set automaticaly by param 'do_not_modify'
        
    bool suc = CGAL::Polygon_mesh_processing::clip(tm, clipper, np_tm, np_c);

    // true if the output surface mesh is manifold. 
    // If false is returned tm and clipper are only corefined.
    assert(suc); 
    // decide what TODO when can't clip source object !?!
    if (!exist_intersection  || !suc) {
        // TODO: test if cut is fully in or fully out!!
        cut.mesh = backup_copy;
        return false;
    }
    return true;
}

BoundingBoxf3 priv::bounding_box(const CutAOI &cut, const CutMesh &mesh) {
    const P3& p_from_cut = mesh.point(mesh.target(mesh.halfedge(cut.first.front())));
    Vec3d min = to_vec3d(p_from_cut);
    Vec3d max = min;
    for (FI fi : cut.first) { 
        for(VI vi: mesh.vertices_around_face(mesh.halfedge(fi))){
            const P3& p = mesh.point(vi);
            for (size_t i = 0; i < 3; ++i) { 
                if (min[i] > p[i]) min[i] = p[i];
                if (max[i] < p[i]) max[i] = p[i];
            }
        } 
    }
    return BoundingBoxf3(min, max);
}

BoundingBoxf3 priv::bounding_box(const CutMesh &mesh)
{
    Vec3d min = to_vec3d(*mesh.points().begin());
    Vec3d max = min;
    for (VI vi : mesh.vertices()) {
        const P3 &p = mesh.point(vi);
        for (size_t i = 0; i < 3; ++i) {
            if (min[i] > p[i]) min[i] = p[i];
            if (max[i] < p[i]) max[i] = p[i];
        }
    }
    return BoundingBoxf3(min, max);
}

BoundingBoxf3 priv::bounding_box(const SurfacePatch &ecut) {
    return bounding_box(ecut.mesh);
}

priv::SurfacePatch priv::create_surface_patch(const std::vector<FI> &fis,
                                              /* const */ CutMesh   &mesh,
                                              const ReductionMap    *rmap)
{
    auto is_counted = mesh.add_property_map<VI, bool>("v:is_counted").first;
    uint32_t count_vertices = 0;
    if (rmap == nullptr) {
        for (FI fi : fis) 
            for (VI vi : mesh.vertices_around_face(mesh.halfedge(fi))) 
                if (!is_counted[vi]) { 
                    is_counted[vi] = true;
                    ++count_vertices;
                }
    } else {
        for (FI fi : fis)
            for (VI vi : mesh.vertices_around_face(mesh.halfedge(fi))) {
                // Will vertex be reduced?
                if ((*rmap)[vi].is_valid()) continue;
                if (!is_counted[vi]) {
                    is_counted[vi] = true;
                    ++count_vertices;
                }
            }        
    }
    mesh.remove_property_map(is_counted);

    uint32_t count_faces = fis.size();    
    // IMPROVE: Value is greater than neccessary, count edges used twice
    uint32_t count_edges = count_faces*3; 

    CutMesh cm;
    cm.reserve(count_vertices, count_edges, count_faces);

    // vertex conversion function from mesh VI to result VI
    CvtVI2VI mesh2result = mesh.add_property_map<VI,VI>("v:mesh2result").first;

    if (rmap == nullptr) {
        for (FI fi : fis) {
            std::array<VI, 3> t;
            int index = 0;
            for (VI vi : mesh.vertices_around_face(mesh.halfedge(fi))) {
                VI &vi_cvt = mesh2result[vi];
                if (!vi_cvt.is_valid()) {
                    vi_cvt = VI(cm.vertices().size());
                    cm.add_vertex(mesh.point(vi));
                }
                t[index++] = vi_cvt;
            }
            cm.add_face(t[0], t[1], t[2]);
        }
    } else {
        for (FI fi :fis) {
            std::array<VI, 3> t;
            int  index = 0;
            bool exist_reduction = false;
            for (VI vi : mesh.vertices_around_face(mesh.halfedge(fi))) {
                VI vi_r = (*rmap)[vi];
                if (vi_r.is_valid()) { 
                    exist_reduction = true;
                    vi = vi_r;
                }
                VI &vi_cvt = mesh2result[vi];
                if (!vi_cvt.is_valid()) {
                    vi_cvt = VI(cm.vertices().size());
                    cm.add_vertex(mesh.point(vi));
                }
                t[index++] = vi_cvt;
            }

            // prevent add reduced triangle
            if (exist_reduction &&
                (t[0] == t[1] || 
                 t[1] == t[2] ||
                 t[2] == t[0]))
                continue;

            cm.add_face(t[0], t[1], t[2]);
        }
    }
    
    assert(count_vertices == cm.vertices().size());
    assert((rmap == nullptr && count_faces == cm.faces().size()) ||
           (rmap != nullptr && count_faces >= cm.faces().size()));
    assert(count_edges >= cm.edges().size());
    
    // convert VI from this patch to source VI, when exist
    CvtVI2VI cvt = cm.add_property_map<VI, VI>(patch_source_name).first;
    // vi_s .. VertexIndex into mesh (source)
    // vi_d .. new VertexIndex in cm (destination)
    for (VI vi_s : mesh.vertices()) { 
        VI vi_d = mesh2result[vi_s];
        if (!vi_d.is_valid()) continue;
        cvt[vi_d] = vi_s;
    }
    mesh.remove_property_map(mesh2result);
    return {std::move(cm)};
}

// diff_models help functions
namespace priv {

struct SurfacePatchEx
{
    SurfacePatch patch;

    // flag that part will be deleted
    bool full_inside = false;
    // flag that Patch could contain more than one part
    bool just_cliped = false;
};
using SurfacePatchesEx = std::vector<SurfacePatchEx>;


using BBS = std::vector<BoundingBoxf3>;
/// <summary>
/// Create bounding boxes for AOI
/// </summary>
/// <param name="cuts">Cutted AOI from models</param>
/// <param name="cut_models">Source points of cuts</param>
/// <returns>Bounding boxes</returns>
BBS create_bbs(const VCutAOIs &cuts, const CutMeshes &cut_models);

using Primitive = CGAL::AABB_face_graph_triangle_primitive<CutMesh>;
using Traits    = CGAL::AABB_traits<EpicKernel, Primitive>;
using Ray       = EpicKernel::Ray_3;
using Tree      = CGAL::AABB_tree<Traits>;
using Trees     = std::vector<Tree>;
/// <summary>
/// Create AABB trees for check when patch is whole inside of model
/// </summary>
/// <param name="models">Source for trees</param>
/// <returns>trees</returns>
Trees create_trees(const CutMeshes &models);

/// <summary>
/// Check whether bounding box has intersection with model
/// </summary>
/// <param name="bb">Bounding box to check</param>
/// <param name="model_index">Model to check with</param>
/// <param name="bbs">All bounding boxes from VCutAOIs</param>
/// <param name="m2i">Help index into VCutAOIs</param>
/// <returns>True when exist bounding boxes intersection</returns>
bool has_bb_intersection(const BoundingBoxf3  &bb,
                         size_t                model_index,
                         const BBS            &bbs,
                         const ModelCut2index &m2i);

/// <summary>
/// Only for model without intersection
/// Use ray (in projection direction) from a point from patch
/// and count intersections: pair .. outside | odd .. inside
/// </summary>
/// <param name="patch">Patch to check</param>
/// <param name="tree">Model converted to AABB tree</param>
/// <param name="projection">Define direction of projection</param>
/// <returns>True when patch point lay inside of model defined by tree,
/// otherwise FALSE</returns>
bool is_patch_inside_of_model(const SurfacePatch &patch,
                              const Tree         &tree,
                              const Project3d    &projection);

/// <summary>
/// Return some shape point index which identify shape
/// NOTE: Used to find expolygon index
/// </summary>
/// <param name="cut">Used to search source shapes poin</param>
/// <param name="model"></param>
/// <returns>shape point index</returns>
uint32_t get_shape_point_index(const CutAOI &cut, const CutMesh &model);

using PatchNumber = CutMesh::Property_map<FI, size_t>;
/// <summary>
/// Separate triangles singned with number n
/// </summary>
/// <param name="fis">Face indices owned by separate patch</param>
/// <param name="patch">Original patch
/// NOTE: Can't be const. For indexing vetices need temporary add property map</param>
/// <param name="cvt_from">conversion map</param>
/// <returns>Just separated patch</returns>
SurfacePatch separate_patch(const std::vector<FI>   &fis,
                            /* const*/ SurfacePatch &patch,
                            const CvtVI2VI     &cvt_from);

/// <summary>
/// Separate connected triangles into it's own patches
/// new patches are added to back of input patches
/// </summary>
/// <param name="i">index into patches</param>
/// <param name="patches">In/Out Patches</param>
void divide_patch(size_t i, SurfacePatchesEx &patches);

/// <summary>
/// Fill outline in patches by open edges
/// </summary>
/// <param name="patches">Input/Output meshes with open edges</param>
void collect_open_edges(SurfacePatches &patches);

} // namespace priv

std::vector<BoundingBoxf3> priv::create_bbs(const VCutAOIs        &cuts,
                                            const CutMeshes &cut_models)
{
    size_t count = 0;
    for (const CutAOIs &cut : cuts) count += cut.size();

    std::vector<BoundingBoxf3> bbs;
    bbs.reserve(count);
    for (size_t model_index = 0; model_index < cut_models.size(); ++model_index) {
        const CutMesh &cut_model = cut_models[model_index];
        const CutAOIs &cutAOIs   = cuts[model_index];
        for (size_t cut_index = 0; cut_index < cutAOIs.size(); ++cut_index) {
            const CutAOI &cut = cutAOIs[cut_index];
            bbs.push_back(bounding_box(cut, cut_model));
        }
    }
    return bbs;
}


priv::Trees priv::create_trees(const CutMeshes &models) {
    Trees result;
    result.reserve(models.size());
    for (const CutMesh &model : models) {
        Tree tree;
        tree.insert(faces(model).first, faces(model).second, model);
        tree.build();
        result.emplace_back(std::move(tree));
    }
    return result;
}

bool priv::has_bb_intersection(const BoundingBoxf3  &bb,
                               size_t                model_index,
                               const BBS            &bbs,
                               const ModelCut2index &m2i)
{
    const auto&offsets = m2i.get_offsets();
    // for cut index with model_index2
    size_t start = offsets[model_index];
    size_t next  = model_index + 1;
    size_t end   = (next < offsets.size()) ? offsets[next] : m2i.get_count();
    for (size_t bb_index = start; bb_index < end; bb_index++)
        if (bb.intersects(bbs[bb_index])) return true;
    return false;
}

bool priv::is_patch_inside_of_model(const SurfacePatch &patch,
                                    const Tree         &tree,
                                    const Project3d    &projection)
{
    // TODO: Solve model with hole in projection direction !!!
    const P3 &a = patch.mesh.point(VI(0));
    Vec3d a_ = to_vec3d(a);
    Vec3d b_ = projection.project(a_);
    P3 b(b_.x(), b_.y(), b_.z());

    Ray ray_query(a, b);
    size_t count = tree.number_of_intersected_primitives(ray_query);
    bool is_in = (count % 2) == 1;

    // try opposit direction result should be same, otherwise open model is used
    //Vec3f c_ = a_ - (b_ - a_); // opposit direction
    //P3 c(c_.x(), c_.y(), c_.z());
    //Ray ray_query2(a, b);
    //size_t count2 = tree.number_of_intersected_primitives(ray_query2);
    //bool is_in2 = (count2 % 2) == 1;
    assert(((tree.number_of_intersected_primitives(
                    Ray(a, P3(2 * a.x() - b.x(), 
                            2 * a.y() - b.y(),
                            2 * a.z() - b.z()))) %
                2) == 1) == is_in);
    return is_in;        
}

uint32_t priv::get_shape_point_index(const CutAOI &cut, const CutMesh &model)
{
    // map is created during intersection by corefine visitor
    const VertexShapeMap &vert_shape_map = model.property_map<VI, const IntersectingElement *>(vert_shape_map_name).first;
    // for each half edge of outline
    for (HI hi : cut.second) {
        VI vi = model.source(hi);
        const IntersectingElement *ie = vert_shape_map[vi];
        if (ie == nullptr) continue;
        assert(ie->shape_point_index != std::numeric_limits<uint32_t>::max());
        return ie->shape_point_index;
    }
    // can't found any intersecting element in cut
    assert(false);
    return 0;
}

priv::SurfacePatch priv::separate_patch(const std::vector<FI>& fis,
                                        SurfacePatch &patch,
                                        const CvtVI2VI     &cvt_from)
{
    assert(patch.mesh.is_valid());    
    SurfacePatch patch_new = create_surface_patch(fis, patch.mesh);
    patch_new.bb           = bounding_box(patch_new.mesh);
    patch_new.aoi_id       = patch.aoi_id;
    patch_new.model_id     = patch.model_id;
    patch_new.shape_id     = patch.shape_id;
    // fix cvt
    CvtVI2VI cvt = patch_new.mesh.property_map<VI, VI>(patch_source_name).first;
    for (VI &vi : cvt) {
        if (!vi.is_valid()) continue;
        vi = cvt_from[vi];
    }
    return patch_new;
}

void priv::divide_patch(size_t i, SurfacePatchesEx &patches)
{
    SurfacePatchEx &patch_ex = patches[i];
    assert(patch_ex.just_cliped); 
    patch_ex.just_cliped = false;

    SurfacePatch& patch = patch_ex.patch;
    CutMesh& cm = patch.mesh;
    assert(!cm.faces().empty());
    std::string patch_number_name = "f:patch_number";
    CutMesh::Property_map<FI,bool> is_processed = cm.add_property_map<FI, bool>(patch_number_name, false).first;
    
    const CvtVI2VI& cvt_from = patch.mesh.property_map<VI, VI>(patch_source_name).first;

    std::vector<FI> fis;
    fis.reserve(cm.faces().size());

    SurfacePatchesEx new_patches;
    std::vector<FI> queue;
    // IMPROVE: create groups around triangles and than connect groups
    for (FI fi_cm : cm.faces()) {
        if (is_processed[fi_cm]) continue;
        assert(queue.empty());
        queue.push_back(fi_cm);
        if (!fis.empty()) {
            // Be carefull after push to patches,
            // all ref on patch contain non valid values
            SurfacePatchEx patch_ex_n;
            patch_ex_n.patch = separate_patch(fis, patch, cvt_from);
            patch_ex_n.patch.is_whole_aoi = false;
            new_patches.push_back(std::move(patch_ex_n));
            fis.clear();
        }
        // flood fill from triangle fi_cm to surrounding
        do {
            FI fi_q = queue.back();
            queue.pop_back();
            if (is_processed[fi_q]) continue;             
            is_processed[fi_q] = true;
            fis.push_back(fi_q);
            HI hi = cm.halfedge(fi_q);
            for (FI fi : cm.faces_around_face(hi)) {
                // by documentation The face descriptor may be the null face, and it may be several times the same face descriptor.
                if (!fi.is_valid()) continue;
                if (!is_processed[fi]) queue.push_back(fi);
            }
        } while (!queue.empty());
    }
    cm.remove_property_map(is_processed);
    assert(!fis.empty());

    // speed up for only one patch - no dividing (the most common)
    if (new_patches.empty()) {
        patch.bb = bounding_box(cm);
        patch.is_whole_aoi = false;
    } else {
        patch = separate_patch(fis, patch, cvt_from);
        patches.insert(patches.end(), new_patches.begin(), new_patches.end());
    }
}

void priv::collect_open_edges(SurfacePatches &patches) {
    std::vector<HI> open_half_edges;
    for (SurfacePatch &patch : patches) {
        open_half_edges.clear();
        const CutMesh &mesh = patch.mesh;
        for (FI fi : mesh.faces()) {
            HI hi1 = mesh.halfedge(fi);
            assert(hi1.is_valid());
            HI hi2 = mesh.next(hi1);
            assert(hi2.is_valid());
            HI hi3 = mesh.next(hi2);
            assert(hi3.is_valid());
            // Is fi triangle?
            assert(mesh.next(hi3) == hi1);
            for (HI hi : {hi1, hi2, hi3}) {
                HI hi_op = mesh.opposite(hi);
                FI fi_op = mesh.face(hi_op);
                if (!fi_op.is_valid())
                    open_half_edges.push_back(hi);
            }
        }
        patch.loops = create_loops(open_half_edges, mesh);
    }
}

priv::SurfacePatches priv::diff_models(VCutAOIs            &cuts,
                                       /*const*/ CutMeshes &cut_models,
                                       /*const*/ CutMeshes &models,
                                       const Project3d     &projection)
{
    // IMPROVE: when models contain ONE mesh. It is only about convert cuts to patches
    // and reduce unneccessary triangles on contour

    //Convert model_index and cut_index into one index
    priv::ModelCut2index m2i(cuts);

    // create bounding boxes for cuts
    std::vector<BoundingBoxf3> bbs = create_bbs(cuts, cut_models);
    Trees trees(models.size());

    SurfacePatches patches;

    // queue of patches for one AOI (permanent with respect to for loop)
    SurfacePatchesEx aoi_patches;

    //SurfacePatches aoi_patches;
    patches.reserve(m2i.get_count()); // only approximation of count
    size_t index = 0;
    for (size_t model_index = 0; model_index < models.size(); ++model_index) {
        CutAOIs &model_cuts = cuts[model_index];
        CutMesh &cut_model_ = cut_models[model_index];
        const CutMesh &cut_model = cut_model_;
        ReductionMap vertex_reduction_map = cut_model_.add_property_map<VI, VI>(vertex_reduction_map_name).first;
        create_reduce_map(vertex_reduction_map, cut_model);

        for (size_t cut_index = 0; cut_index < model_cuts.size(); ++cut_index, ++index) {
            const CutAOI &cut = model_cuts[cut_index];
            SurfacePatchEx patch_ex;
            SurfacePatch  &patch = patch_ex.patch;
            patch = create_surface_patch(cut.first, cut_model_, &vertex_reduction_map);
            patch.bb = bbs[index];
            patch.aoi_id   = cut_index;
            patch.model_id = model_index;
            patch.shape_id = get_shape_point_index(cut, cut_model);
            patch.is_whole_aoi = true;

            aoi_patches.clear();
            aoi_patches.push_back(patch_ex);
            for (size_t model_index2 = 0; model_index2 < models.size(); ++model_index2) {
                // do not clip source model itself
                if (model_index == model_index2) continue;
                for (SurfacePatchEx &patch_ex : aoi_patches) {
                    SurfacePatch &patch = patch_ex.patch;
                    if (has_bb_intersection(patch.bb, model_index2, bbs, m2i) &&
                        clip_cut(patch, models[model_index2])){
                        patch_ex.just_cliped = true;
                    } else { 
                        // build tree on demand
                        // NOTE: it is possible not neccessary: e.g. one model
                        Tree &tree = trees[model_index2];
                        if (tree.empty()) {
                            const CutMesh &model   = models[model_index2];
                            auto           f_range = faces(model);
                            tree.insert(f_range.first, f_range.second, model);
                            tree.build();
                        }
                        if (is_patch_inside_of_model(patch, tree, projection))
                            patch_ex.full_inside = true;
                    }
                }
                // erase full inside
                for (size_t i = aoi_patches.size(); i != 0; --i) {
                    auto it = aoi_patches.begin() + (i - 1);
                    if (it->full_inside) aoi_patches.erase(it);
                }

                // detection of full AOI inside of model
                if (aoi_patches.empty()) break;

                // divide cliped into parts
                size_t end = aoi_patches.size();
                for (size_t i = 0; i < end; ++i)
                    if (aoi_patches[i].just_cliped)
                        divide_patch(i, aoi_patches);
            }

            if (!aoi_patches.empty()) {
                patches.reserve(patches.size() + aoi_patches.size());
                for (SurfacePatchEx &patch : aoi_patches)
                    patches.push_back(std::move(patch.patch));
                
            }
        }
        cut_model_.remove_property_map(vertex_reduction_map);
    }

    // Also use outline inside of patches(made by non manifold models)
    // IMPROVE: trace outline from AOIs
    collect_open_edges(patches);
    return patches;
}

bool priv::is_over_whole_expoly(const SurfacePatch &patch,
                                const ExPolygons   &shapes,
                                const VCutAOIs     &cutAOIs,
                                const CutMeshes    &meshes)
{
    if (!patch.is_whole_aoi) return false;
    return is_over_whole_expoly(cutAOIs[patch.model_id][patch.aoi_id],
                                shapes[patch.shape_id],
                                meshes[patch.model_id]);
}

bool priv::is_over_whole_expoly(const CutAOI    &cutAOI,
                                const ExPolygon &shape,
                                const CutMesh   &mesh)
{
    // NonInterupted contour is without other point and contain all from shape    
    const VertexShapeMap &vert_shape_map = mesh.property_map<VI, const IntersectingElement*>(vert_shape_map_name).first;
    for (HI hi : cutAOI.second) { 
        const IntersectingElement *ie_s = vert_shape_map[mesh.source(hi)];
        const IntersectingElement *ie_t = vert_shape_map[mesh.target(hi)];
        if (ie_s == nullptr || ie_t == nullptr) 
            return false;

        assert(ie_s->attr != (unsigned char) IntersectingElement::Type::undefined);
        assert(ie_t->attr != (unsigned char) IntersectingElement::Type::undefined);

        // check if it is neighbor indices
        uint32_t i_s = ie_s->shape_point_index;
        uint32_t i_t = ie_t->shape_point_index;
        assert(i_s != std::numeric_limits<uint32_t>::max());
        assert(i_t != std::numeric_limits<uint32_t>::max());
        if (i_s == std::numeric_limits<uint32_t>::max() ||
            i_t == std::numeric_limits<uint32_t>::max())
            return false;

        // made by same index
        if (i_s == i_t) continue;

        // order from source to target
        if (i_s > i_t) { 
            std::swap(i_s, i_t);
            std::swap(ie_s, ie_t);
        }
        // Must be after fix order !!
        bool is_last_polygon_segment = ie_s->is_first() && ie_t->is_last();
        if (is_last_polygon_segment) {
            std::swap(i_s, i_t);
            std::swap(ie_s, ie_t);        
        }

        // Is continous indices
        if (!is_last_polygon_segment &&
            (ie_s->is_last() || (i_s + 1) != i_t))
            return false;
        
        IntersectingElement::Type t_s = ie_s->get_type();
        IntersectingElement::Type t_t = ie_t->get_type();
        if (t_s == IntersectingElement::Type::undefined ||
            t_t == IntersectingElement::Type::undefined)
            return false;

        // next segment must start with edge intersection
        if (t_t != IntersectingElement::Type::edge_1)
            return false;

        // After face1 must be edge2 or face2
        if (t_s == IntersectingElement::Type::face_1)
            return false;        
    }

    // When all open edges are on contour than there is NO holes is shape
    auto is_open = [&mesh](HI hi)->bool {
        HI opposite = mesh.opposite(hi);
        return !mesh.face(opposite).is_valid();
    };

    std::vector<HI> opens; // copy
    opens.reserve(cutAOI.second.size());
    for (HI hi : cutAOI.second) // from lower to bigger
        if (is_open(hi)) opens.push_back(hi);
    std::sort(opens.begin(), opens.end());

    for (FI fi: cutAOI.first) {
        HI face_hi = mesh.halfedge(fi);
        for (HI hi : mesh.halfedges_around_face(face_hi)) {
            if (!is_open(hi)) continue;
            // open edge
            auto lb = std::lower_bound(opens.begin(), opens.end(), hi);
            if (lb == opens.end() || *lb != hi)
                return false; // not in contour
        }
    }
    return true;
}

std::vector<bool> priv::select_patches(const ProjectionDistances &best_distances,
                                       const SurfacePatches      &patches,
                                       const ExPolygons          &shapes,
                                       const BoundingBox         &shapes_bb,
                                       const ExPolygonsIndices   &s2i,
                                       const VCutAOIs            &cutAOIs,
                                       const CutMeshes           &meshes,
                                       const Project             &projection)
{
    // extension to cover numerical mistake made by back projection patch from 3d to 2d
    // Calculated as one percent of average size(width and height)
    Point s = shapes_bb.size();
    const float extend_delta = (s.x() + s.y())/ float(2 * 100);
        
    // vector of patches for shape
    std::vector<std::vector<uint32_t>> used_shapes_patches(shapes.size());    
    std::vector<bool> in_distances(patches.size(), {false});
    for (const ProjectionDistance &d : best_distances) {
        // exist valid projection for shape point?
        if (d.patch_index == std::numeric_limits<uint32_t>::max()) continue;
        if (in_distances[d.patch_index]) continue;
        in_distances[d.patch_index] = true;

        ExPolygonsIndex id = s2i.cvt(&d - &best_distances.front());
        used_shapes_patches[id.expolygons_index].push_back(d.patch_index);
    }

    // vector of patches for shape
    std::vector<std::vector<uint32_t>> shapes_patches(shapes.size());
    for (const SurfacePatch &patch : patches) 
        shapes_patches[patch.shape_id].push_back(&patch - &patches.front());

#ifdef DEBUG_OUTPUT_DIR
    std::string store_dir = DEBUG_OUTPUT_DIR + "select_patches/";
    prepare_dir(store_dir);
#endif // DEBUG_OUTPUT_DIR

    for (size_t shape_index = 0; shape_index < shapes.size(); shape_index++) {
        const ExPolygon &shape = shapes[shape_index];
        std::vector<uint32_t> &used_shape_patches = used_shapes_patches[shape_index];
        if (used_shape_patches.empty()) continue;
        // is used all exist patches?
        if (used_shapes_patches.size() == shapes_patches[shape_index].size()) continue;
        if (used_shape_patches.size() == 1) { 
            uint32_t patch_index = used_shape_patches.front();
            const SurfacePatch &patch = patches[patch_index];
            if (is_over_whole_expoly(patch, shapes, cutAOIs, meshes)) continue;
        }

        // only shapes containing multiple patches
        // or not full filled are back projected (hard processed)

        // intersection of converted patches to 2d
        ExPolygons fill;
        fill.reserve(used_shape_patches.size());

        // Heuristics to predict which patch to be used need average patch depth
        Vec2d used_patches_depth(std::numeric_limits<double>::max(), std::numeric_limits<double>::min());
        for (uint32_t patch_index : used_shape_patches) {
            ExPolygon patch_area = to_expoly(patches[patch_index], projection, used_patches_depth);
            //*/
            ExPolygons patch_areas = offset_ex(patch_area, extend_delta);
            fill.insert(fill.end(), patch_areas.begin(), patch_areas.end());
            /*/
            // without save extension
            fill.push_back(patch_area);
            //*/
        }
        fill = union_ex(fill);

        // not cutted area of expolygon
        ExPolygons rest = diff_ex(ExPolygons{shape}, fill, ApplySafetyOffset::Yes);
#ifdef DEBUG_OUTPUT_DIR
        BoundingBox shape_bb = get_extents(shape);
        SVG svg(store_dir + "shape_" + std::to_string(shape_index) + ".svg", shape_bb);
        svg.draw(fill, "darkgreen");
        svg.draw(rest, "green");
#endif // DEBUG_OUTPUT_DIR

        // already filled by multiple patches
        if (rest.empty()) continue;

        // find patches overlaped rest area
        struct PatchShape{
            uint32_t patch_index;
            ExPolygon shape;
            ExPolygons intersection;
            double depth_range_center_distance; // always positive 
        };
        using PatchShapes = std::vector<PatchShape>;
        PatchShapes patch_shapes;

        double used_patches_depth_center = (used_patches_depth[0] + used_patches_depth[1]) / 2;

        // sort used_patches for faster search
        std::sort(used_shape_patches.begin(), used_shape_patches.end());
        for (uint32_t patch_index : shapes_patches[shape_index]) { 
            // check is patch already used
            auto it = std::lower_bound(used_shape_patches.begin(), used_shape_patches.end(), patch_index);
            if (it != used_shape_patches.end() && *it == patch_index) continue;

            // Heuristics to predict which patch to be used need average patch depth
            Vec2d patche_depth_range(std::numeric_limits<double>::max(), std::numeric_limits<double>::min());
            ExPolygon patch_shape = to_expoly(patches[patch_index], projection, patche_depth_range);
            double depth_center = (patche_depth_range[0] + patche_depth_range[1]) / 2;
            double depth_range_center_distance = std::fabs(used_patches_depth_center - depth_center);

            ExPolygons patch_intersection = intersection_ex(ExPolygons{patch_shape}, rest);
            if (patch_intersection.empty()) continue;

            patch_shapes.push_back({patch_index, patch_shape, patch_intersection, depth_range_center_distance});
        }

        // nothing to add
        if (patch_shapes.empty()) continue;
        // only one solution to add
        if (patch_shapes.size() == 1) {
            used_shape_patches.push_back(patch_shapes.front().patch_index);        
            continue;
        }

        // Idea: Get depth range of used patches and add patches in order by distance to used depth center
        std::sort(patch_shapes.begin(), patch_shapes.end(), [](const PatchShape &a, const PatchShape &b) 
            { return a.depth_range_center_distance < b.depth_range_center_distance; });

#ifdef DEBUG_OUTPUT_DIR
        for (size_t i = patch_shapes.size(); i > 0; --i) { 
            const PatchShape &p = patch_shapes[i - 1];
            int gray_level =  (i * 200) / patch_shapes.size();
            std::stringstream color;
            color << "#" << std::hex << std::setfill('0') << std::setw(2) << gray_level << gray_level << gray_level;
            svg.draw(p.shape, color.str());
            Point text_pos = get_extents(p.shape).center().cast<int>();
            svg.draw_text(text_pos, std::to_string(i-1).c_str(), "orange", std::ceil(shape_bb.size().x() / 20 * 0.000001));
            //svg.draw(p.intersection, color.str());
        }
#endif // DEBUG_OUTPUT_DIR

        for (const PatchShape &patch : patch_shapes) { 
            // Check when exist some place to fill
            ExPolygons patch_intersection = intersection_ex(patch.intersection, rest);
            if (patch_intersection.empty()) continue;

            // Extend for sure
            ExPolygons intersection = offset_ex(patch.intersection, extend_delta);
            rest = diff_ex(rest, intersection, ApplySafetyOffset::Yes);

            used_shape_patches.push_back(patch.patch_index);
            if (rest.empty()) break;
        }

        // QUESTION: How to select which patch to use? How to sort them?
        // Now is used back projection distance from used patches
        // 
        // Idealy by outline depth: (need ray cast into patches)
        // how to calc wanted depth - idealy by depth of outline help to overlap
        // how to calc patch depth - depth in place of outline position
        // Which outline to use between 

    }

    std::vector<bool> result(patches.size(), {false});
    for (const std::vector<uint32_t> &patches: used_shapes_patches)
        for (uint32_t patch_index : patches) { 
            assert(patch_index < result.size());
            // check only onece insertation of patch
            assert(!result[patch_index]);
            result[patch_index] = true;
        }
    return result;
}

priv::Loops priv::create_loops(const std::vector<HI> &outlines, const CutMesh& mesh)
{
    Loops loops;
    Loops unclosed;
    for (HI hi : outlines) {
        VI vi_s = mesh.source(hi);
        VI vi_t = mesh.target(hi);
        Loop *loop_move    = nullptr;
        Loop *loop_connect = nullptr;
        for (std::vector<VI> &cut : unclosed) {
            if (cut.back() != vi_s) continue;
            if (cut.front() == vi_t) {
                // cut closing
                loop_move = &cut;
            } else {
                loop_connect = &cut;
            }
            break;
        }
        if (loop_move != nullptr) {
            // index of closed cut
            size_t index = loop_move - &unclosed.front();
            // move cut to result
            loops.emplace_back(std::move(*loop_move));
            // remove it from unclosed cut
            unclosed.erase(unclosed.begin() + index);
        } else if (loop_connect != nullptr) {
            // try find tail to connect cut
            Loop *loop_tail = nullptr;
            for (Loop &cut : unclosed) {
                if (cut.front() != vi_t) continue;
                loop_tail = &cut;
                break;
            }
            if (loop_tail != nullptr) {
                // index of tail
                size_t index = loop_tail - &unclosed.front();
                // move to connect vector
                loop_connect->insert(loop_connect->end(),
                                    make_move_iterator(loop_tail->begin()),
                                    make_move_iterator(loop_tail->end()));
                // remove tail from unclosed cut
                unclosed.erase(unclosed.begin() + index);
            } else {
                loop_connect->push_back(vi_t);
            }
        } else { // not found
            bool create_cut = true;
            // try to insert to front of cut
            for (Loop &cut : unclosed) {
                if (cut.front() != vi_t) continue;
                cut.insert(cut.begin(), vi_s);
                create_cut = false;
                break;
            }
            if (create_cut)
                unclosed.emplace_back(std::vector{vi_s, vi_t});
        }
    }
    assert(unclosed.empty());
    return loops;
}

Polygons priv::unproject_loops(const SurfacePatch &patch, const Project &projection, Vec2d &depth_range)
{
    assert(!patch.loops.empty());
    if (patch.loops.empty()) return {};

    // NOTE: this method is working only when patch did not contain outward faces
    Polygons polys;
    polys.reserve(patch.loops.size());
    // project conture into 2d space to fillconvert outlines to

    size_t count = 0;
    for (const Loop &l : patch.loops) count += l.size();
    std::vector<float> depths;
    depths.reserve(count);

    Points pts;
    for (const Loop &l : patch.loops) {
        pts.clear();
        pts.reserve(l.size());
        for (VI vi : l) {
            const P3 &p3 = patch.mesh.point(vi);
            Vec3d p = to_vec3d(p3);
            double depth;
            std::optional<Vec2d> p2_opt = projection.unproject(p, &depth);
            if (depth_range[0] > depth) depth_range[0] = depth; // min
            if (depth_range[1] < depth) depth_range[1] = depth; // max
            // Check when appear that skip is enough for poit which can't be unprojected
            // - it could break contour
            assert(p2_opt.has_value());
            if (!p2_opt.has_value()) continue;

            pts.push_back(p2_opt->cast<Point::coord_type>());
            depths.push_back(static_cast<float>(depth));
        }
        // minimal is triangle
        assert(pts.size() >= 3);
        if (pts.size() < 3) continue;

        polys.emplace_back(pts);
    }

    assert(!polys.empty());
    return polys;
}

ExPolygon priv::to_expoly(const SurfacePatch &patch, const Project &projection, Vec2d &depth_range)
{
    Polygons polys = unproject_loops(patch, projection, depth_range);
    // should not be used when no opposit triangle are counted so should not create overlaps
    ClipperLib::PolyFillType fill_type = ClipperLib::PolyFillType::pftEvenOdd;
    ExPolygons expolys = Slic3r::union_ex(polys, fill_type);
    if (expolys.size() == 1)
        return expolys.front();

    // It should be one expolygon
    assert(false);

    if (expolys.empty()) return {};
    // find biggest
    const ExPolygon *biggest = &expolys.front();
    for (size_t index = 1; index < expolys.size(); ++index) {
        const ExPolygon *current = &expolys[index];
        if (biggest->contour.size() < current->contour.size())
            biggest = current;
    }
    return *biggest;
}

SurfaceCut priv::patch2cut(SurfacePatch &patch)
{
    CutMesh &mesh = patch.mesh;

    std::string convert_map_name = "v:convert";
    CutMesh::Property_map<VI, SurfaceCut::Index> convert_map = 
        mesh.add_property_map<VI, SurfaceCut::Index>(convert_map_name).first;

    size_t indices_size  = mesh.faces().size();
    size_t vertices_size = mesh.vertices().size();

    SurfaceCut sc;
    sc.indices.reserve(indices_size);
    sc.vertices.reserve(vertices_size);
    for (VI vi : mesh.vertices()) {
        // vi order is is not sorted
        // assert(vi.idx() == sc.vertices.size());
        // vi is not continous
        // assert(vi.idx() < vertices_size);
        convert_map[vi] = sc.vertices.size();
        const P3 &p = mesh.point(vi);
        sc.vertices.emplace_back(p.x(), p.y(), p.z());
    }

    for (FI fi : mesh.faces()) {
        HI hi = mesh.halfedge(fi);
        assert(mesh.next(hi).is_valid());
        assert(mesh.next(mesh.next(hi)).is_valid());
        // Is fi triangle?
        assert(mesh.next(mesh.next(mesh.next(hi))) == hi);

        // triangle indicies
        Vec3i32 ti;
        size_t i = 0;
        for (VI vi : { mesh.source(hi), 
                       mesh.target(hi), 
                       mesh.target(mesh.next(hi))})
            ti[i++] = convert_map[vi];
        sc.indices.push_back(ti);
    }

    sc.contours.reserve(patch.loops.size());
    for (const Loop &loop : patch.loops) { 
        sc.contours.push_back({});
        std::vector<SurfaceCut::Index> &contour = sc.contours.back();
        contour.reserve(loop.size());
        for (VI vi : loop) contour.push_back(convert_map[vi]);
    }

    // Not neccessary, clean and free memory
    mesh.remove_property_map(convert_map);
    return sc;
}

void priv::append(SurfaceCut &sc, SurfaceCut &&sc_add)
{
    if (sc.empty()) {
        sc = std::move(sc_add);
        return;
    }

    if (!sc_add.contours.empty()) {
        SurfaceCut::Index offset = static_cast<SurfaceCut::Index>(
            sc.vertices.size());
        size_t require = sc.contours.size() + sc_add.contours.size();
        if (sc.contours.capacity() < require) sc.contours.reserve(require);
        for (std::vector<SurfaceCut::Index> &cut : sc_add.contours)
            for (SurfaceCut::Index &i : cut) i += offset;
        Slic3r::append(sc.contours, std::move(sc_add.contours));
    }
    its_merge(sc, std::move(sc_add));
}

SurfaceCut priv::merge_patches(SurfacePatches &patches, const std::vector<bool>& mask)
{
    SurfaceCut result;
    for (SurfacePatch &patch : patches) {
        size_t index = &patch - &patches.front();
        if (!mask[index]) continue;
        append(result, patch2cut(patch));
    }    
    return result;
}

#ifdef DEBUG_OUTPUT_DIR
void priv::prepare_dir(const std::string &dir){
    namespace fs = std::filesystem;
    if (fs::exists(dir)) {
        for (auto &path : fs::directory_iterator(dir)) fs::remove_all(path);
    } else {
        fs::create_directories(dir);
    }
}

namespace priv{
int reduction_order = 0;
int filled_order    = 0;
int constrained_order = 0;
int diff_patch_order  = 0;

} // namespace priv

void priv::initialize_store(const std::string& dir)
{
    // clear previous output
    prepare_dir(dir);
    reduction_order   = 0;
    filled_order      = 0;
    constrained_order = 0;
    diff_patch_order  = 0;
}

void priv::store(const Vec3f       &vertex,
                 const Vec3f       &normal,
                 const std::string &file,
                 float              size)
{
    int    flatten = 20;
    size_t min_i   = 0;
    for (size_t i = 1; i < 3; i++)
        if (normal[min_i] > normal[i]) min_i = i;
    Vec3f up_  = Vec3f::Zero();
    up_[min_i] = 1.f;
    Vec3f side = normal.cross(up_).normalized() * size;
    Vec3f up   = side.cross(normal).normalized() * size;

    indexed_triangle_set its;
    its.vertices.reserve(flatten + 1);
    its.indices.reserve(flatten);

    its.vertices.push_back(vertex);
    its.vertices.push_back(vertex + up);
    size_t max_i = static_cast<size_t>(flatten);
    for (size_t i = 1; i < max_i; i++) {
        float angle = i * 2 * M_PI / flatten;
        Vec3f v     = vertex + sin(angle) * side + cos(angle) * up;
        its.vertices.push_back(v);
        its.indices.emplace_back(0, i, i + 1);
    }
    its.indices.emplace_back(0, flatten, 1);
    its_write_obj(its, file.c_str());
}

void priv::store(const CutMesh &mesh, const FaceTypeMap &face_type_map, const std::string& dir, bool is_filled)
{
    std::string off_file;
    if (is_filled) {
        if (filled_order == 0) prepare_dir(dir);
        off_file = dir + "model" + std::to_string(filled_order++) + ".off";
    }else{
        if (constrained_order == 0) prepare_dir(dir);
        off_file = dir + "model" + std::to_string(constrained_order++) + ".off";
    }

    CutMesh &mesh_ = const_cast<CutMesh &>(mesh);
    auto face_colors = mesh_.add_property_map<priv::FI, CGAL::Color>("f:color").first;    
    for (FI fi : mesh.faces()) { 
        auto &color = face_colors[fi];
        switch (face_type_map[fi]) {
        case FaceType::inside: color = CGAL::Color{100, 250, 100}; break; // light green
        case FaceType::inside_processed: color = CGAL::Color{170, 0, 0}; break; // dark red
        case FaceType::outside: color = CGAL::Color{100, 0, 100}; break; // purple
        case FaceType::not_constrained: color = CGAL::Color{127, 127, 127}; break; // gray
        default: color = CGAL::Color{0, 0, 255}; // blue
        }
    }
    CGAL::IO::write_OFF(off_file, mesh, CGAL::parameters::face_color_map(face_colors));
    mesh_.remove_property_map(face_colors);
}

void priv::store(const ExPolygons &shapes, const std::string &svg_file) { 
    SVG svg(svg_file);
    svg.draw(shapes);
}

void priv::store(const CutMesh &mesh, const ReductionMap &reduction_map, const std::string& dir)
{
    if (reduction_order == 0) prepare_dir(dir);
    std::string off_file = dir + "model" + std::to_string(reduction_order++) + ".off";

    CutMesh &mesh_ = const_cast<CutMesh &>(mesh);
    auto vertex_colors = mesh_.add_property_map<priv::VI, CGAL::Color>("v:color").first;    
    // initialize to gray color
    for (VI vi: mesh.vertices())
        vertex_colors[vi] = CGAL::Color{127, 127, 127};

    for (VI reduction_from : mesh.vertices()) {
        VI reduction_to = reduction_map[reduction_from];
        if (!reduction_to.is_valid()) continue;
        vertex_colors[reduction_from] = CGAL::Color{255, 0, 0};
        vertex_colors[reduction_to] = CGAL::Color{0, 0, 255};
    }
    
    CGAL::IO::write_OFF(off_file, mesh, CGAL::parameters::vertex_color_map(vertex_colors));
    mesh_.remove_property_map(vertex_colors);
}

namespace priv {
indexed_triangle_set create_indexed_triangle_set(const std::vector<FI> &faces,
                                                 const CutMesh         &mesh);
} // namespace priv

indexed_triangle_set priv::create_indexed_triangle_set(
    const std::vector<FI> &faces, const CutMesh &mesh)
{
    std::vector<VI> vertices;
    vertices.reserve(faces.size() * 2);

    indexed_triangle_set its;
    its.indices.reserve(faces.size());
    for (FI fi : faces) {
        HI hi     = mesh.halfedge(fi);
        HI hi_end = hi;

        int   ti = 0;
        Vec3i32 t;

        do {
            VI   vi  = mesh.source(hi);
            auto res = std::find(vertices.begin(), vertices.end(), vi);
            t[ti++]  = res - vertices.begin();
            if (res == vertices.end()) vertices.push_back(vi);
            hi = mesh.next(hi);
        } while (hi != hi_end);

        its.indices.push_back(t);
    }

    its.vertices.reserve(vertices.size());
    for (VI vi : vertices) {
        const auto &p = mesh.point(vi);
        its.vertices.emplace_back(p.x(), p.y(), p.z());
    }
    return its;
}

void priv::store(const CutAOIs &aois, const CutMesh &mesh, const std::string &dir) {
    auto create_outline_its =
        [&mesh](const std::vector<HI> &outlines) -> indexed_triangle_set {
        static const float line_width = 0.1f;
        indexed_triangle_set its;
        its.indices.reserve(2*outlines.size());
        its.vertices.reserve(outlines.size()*4);
        for (HI hi : outlines) { 
            //FI fi = mesh.face(hi);
            VI vi_a = mesh.source(hi);
            VI vi_b = mesh.target(hi);
            VI vi_c = mesh.target(mesh.next(hi));
            P3 p3_a = mesh.point(vi_a);
            P3 p3_b = mesh.point(vi_b);
            P3 p3_c = mesh.point(vi_c);

            Vec3f a(p3_a.x(), p3_a.y(), p3_a.z());
            Vec3f b(p3_b.x(), p3_b.y(), p3_b.z());
            Vec3f c(p3_c.x(), p3_c.y(), p3_c.z());

            Vec3f v1 = b - a; // from a to b
            v1.normalize();
            Vec3f v2 = c - a; // from a to c
            v2.normalize();
            Vec3f norm = v1.cross(v2);
            norm.normalize();
            Vec3f perp_to_edge = norm.cross(v1);
            perp_to_edge.normalize();
            Vec3f dir = -perp_to_edge * line_width;

            size_t ai = its.vertices.size();
            its.vertices.push_back(a);
            size_t bi = its.vertices.size();
            its.vertices.push_back(b);
            size_t ai2 = its.vertices.size();
            its.vertices.push_back(a + dir);
            size_t bi2 = its.vertices.size();
            its.vertices.push_back(b + dir);

            its.indices.push_back(Vec3i32(ai, ai2, bi));
            its.indices.push_back(Vec3i32(ai2, bi2, bi));
        }
        return its;    
    };

    prepare_dir(dir);
    for (const auto &aoi : aois) {
        size_t      index = &aoi - &aois.front();
        std::string file  = dir + "aoi" + std::to_string(index) + ".obj";
        indexed_triangle_set its = create_indexed_triangle_set(aoi.first, mesh);
        its_write_obj(its, file.c_str());

        // exist some outline?
        if (aoi.second.empty()) continue;
        std::string file_outline = dir + "outline" + std::to_string(index) + ".obj";
        indexed_triangle_set outline = create_outline_its(aoi.second);
        its_write_obj(outline, file_outline.c_str());
    }
}

void priv::store(const SurfacePatches &patches, const std::string &dir) {
    prepare_dir(dir);
    for (const priv::SurfacePatch &patch : patches) {
        size_t index = &patch - &patches.front();
        if (patch.mesh.faces().empty()) continue;
        CGAL::IO::write_OFF(dir + "patch" + std::to_string(index) + ".off", patch.mesh);
    }
}
//
//void priv::store(const ProjectionDistances &pds,
//                 const VCutAOIs            &aois,
//                 const CutMeshes           &meshes,
//                 const std::string         &file,
//                 float                      width)
//{
//    // create rectangle for each half edge from projection distances
//    indexed_triangle_set its;
//    its.vertices.reserve(4 * pds.size());
//    its.indices.reserve(2 * pds.size());
//    for (const ProjectionDistance &pd : pds) {
//        if (pd.aoi_index == std::numeric_limits<uint32_t>::max()) continue;
//        HI hi  = aois[pd.model_index][pd.aoi_index].second[pd.hi_index];
//        const CutMesh &mesh = meshes[pd.model_index];
//        VI vi1 = mesh.source(hi);
//        VI vi2 = mesh.target(hi);
//        VI vi3 = mesh.target(mesh.next(hi));
//        const P3 &p1  = mesh.point(vi1);
//        const P3 &p2  = mesh.point(vi2);
//        const P3 &p3  = mesh.point(vi3);
//        Vec3f v1(p1.x(), p1.y(), p1.z());
//        Vec3f v2(p2.x(), p2.y(), p2.z());
//        Vec3f v3(p3.x(), p3.y(), p3.z());
//
//        Vec3f v12 = v2 - v1;
//        v12.normalize();
//        Vec3f v13 = v3 - v1;
//        v13.normalize();
//        Vec3f n = v12.cross(v13);
//        n.normalize();
//        Vec3f side = n.cross(v12);
//        side.normalize();
//        side *= -width;
//        
//        uint32_t i = its.vertices.size();
//        its.vertices.push_back(v1);
//        its.vertices.push_back(v1+side);
//        its.vertices.push_back(v2);
//        its.vertices.push_back(v2+side);
//
//        its.indices.emplace_back(i, i + 1, i + 2);
//        its.indices.emplace_back(i + 2, i + 1, i + 3);
//    }
//    its_write_obj(its, file.c_str());
//}

void priv::store(const ExPolygons &shapes, const std::vector<bool> &mask, const Connections &connections, const std::string &file_svg)
{
    auto bb = get_extents(shapes);
    int  width = get_extents(shapes.front()).size().x() / 70;

    SVG svg(file_svg, bb);
    svg.draw(shapes);
    
    ExPolygonsIndices s2i(shapes);
    auto get_point = [&shapes, &s2i](size_t i)->Point {
        auto id = s2i.cvt(i);
        const ExPolygon &s  = shapes[id.expolygons_index];
        const Polygon   &p  = (id.polygon_index == 0) ?
                                              s.contour :
                                              s.holes[id.polygon_index - 1];
        return p[id.point_index];
    };

    bool is_first = true;
    for (const Connection &c : connections) { 
        if (is_first) {
            is_first = false;
            Point p = get_point(c.first);
            svg.draw(p, "purple", 4 * width);
            continue;
        }
        Point p1 = get_point(c.first);
        Point p2 = get_point(c.second);
        svg.draw(Line(p1, p2), "red", width);
    }

    for (size_t i = 0; i < s2i.get_count(); i++) { 
        Point p = get_point(i);
        svg.draw(p, "black", 2*width);
        if (!mask[i])
            svg.draw(p, "white", width);
    }
    svg.Close();
}

namespace priv {
/// <summary>
/// Create model consist of rectangles for each contour edge
/// </summary>
/// <param name="its"></param>
/// <param name="contour"></param>
/// <returns></returns>
indexed_triangle_set create_contour_its(const indexed_triangle_set& its, const std::vector<unsigned int> &contour);

/// <summary>
/// Getter on triangle tip (third vertex of face)
/// </summary>
/// <param name="vi1">First vertex index</param>
/// <param name="vi2">Second vertex index</param>
/// <param name="its">Source model</param>
/// <returns>Tip Vertex index</returns>
unsigned int get_triangle_tip(unsigned int                vi1,
                              unsigned int                vi2,
                              const indexed_triangle_set &its);
}


unsigned int priv::get_triangle_tip(unsigned int                vi1,
                                    unsigned int                vi2,
                                    const indexed_triangle_set &its)
{
    assert(vi1 < its.vertices.size());
    assert(vi2 < its.vertices.size());
    for (const auto &t : its.indices) {
        unsigned int tvi = std::numeric_limits<unsigned int>::max();
        for (const auto &vi : t) {
            unsigned int vi_ = static_cast<unsigned int>(vi);
            if (vi_ == vi1) continue;
            if (vi_ == vi2) continue;
            if (tvi == std::numeric_limits<unsigned int>::max()) {
                tvi = vi_;
            } else {
                tvi = std::numeric_limits<unsigned int>::max();
                break;
            }
        }
        if (tvi != std::numeric_limits<unsigned int>::max())
            return tvi;
    }
    // triangle with indices vi1 and vi2 doesnt exist
    assert(false);
    return std::numeric_limits<unsigned int>::max();
}

indexed_triangle_set priv::create_contour_its(
    const indexed_triangle_set &its, const std::vector<unsigned int> &contour)
{
    static const float line_width = 0.1f;
    indexed_triangle_set result;
    result.vertices.reserve((contour.size() + 1) * 4);
    result.indices.reserve((contour.size() + 1) * 2);
    unsigned int prev_vi = contour.back();
    for (unsigned int vi : contour) {
        const Vec3f &a = its.vertices[vi];
        const Vec3f &b = its.vertices[prev_vi];
        const Vec3f &c = its.vertices[get_triangle_tip(vi, prev_vi, its)];

        Vec3f v1 = b - a; // from a to b
        v1.normalize();
        Vec3f v2 = c - a; // from a to c
        v2.normalize();
        // triangle normal
        Vec3f norm = v1.cross(v2);
        norm.normalize();
        // perpendiculat to edge lay on triangle 
        Vec3f perp_to_edge = norm.cross(v1);
        perp_to_edge.normalize();

        Vec3f dir = -perp_to_edge * line_width;

        size_t ai = result.vertices.size();
        result.vertices.push_back(a);
        size_t bi = result.vertices.size();
        result.vertices.push_back(b);
        size_t ai2 = result.vertices.size();
        result.vertices.push_back(a + dir);
        size_t bi2 = result.vertices.size();
        result.vertices.push_back(b + dir);

        result.indices.push_back(Vec3i32(ai, bi, ai2));
        result.indices.push_back(Vec3i32(ai2, bi, bi2));
        prev_vi = vi;
    }
    return result;
}

//void priv::store(const SurfaceCuts &cut, const std::string &dir) {
//    prepare_dir(dir);
//    for (const auto &c : cut) {
//        size_t index = &c - &cut.front();
//        std::string file  = dir + "cut" + std::to_string(index) + ".obj";
//        its_write_obj(c, file.c_str());
//        for (const auto& contour : c.contours) {
//            size_t c_index = &contour - &c.contours.front();
//            std::string c_file = dir + "cut" + std::to_string(index) + 
//                "contour" + std::to_string(c_index) + ".obj";
//            indexed_triangle_set c_its = create_contour_its(c, contour);
//            its_write_obj(c_its, c_file.c_str());
//        }
//    }
//}

void priv::store(const SurfaceCut &cut, const std::string &file, const std::string &contour_dir) {
    prepare_dir(contour_dir);
    its_write_obj(cut, file.c_str());
    for (const auto& contour : cut.contours) {
        size_t c_index = &contour - &cut.contours.front();
        std::string c_file  = contour_dir + std::to_string(c_index) + ".obj";
        indexed_triangle_set c_its = create_contour_its(cut, contour);
        its_write_obj(c_its, c_file.c_str());
    }
}

void priv::store(const std::vector<indexed_triangle_set> &models,
                 const std::string                       &obj_filename)
{
    indexed_triangle_set merged_model;
    for (const indexed_triangle_set &model : models)
        its_merge(merged_model, model);
    its_write_obj(merged_model, obj_filename.c_str());
}

void priv::store(const std::vector<priv::CutMesh> &models,
                 const std::string                &dir)
{
    prepare_dir(dir);
    if (models.empty()) return;
    if (models.size() == 1) {
        CGAL::IO::write_OFF(dir + "model.off", models.front());
        return;
    }
    size_t model_index = 0;
    for (const priv::CutMesh& model : models) {
        std::string filename = dir + "model" + std::to_string(model_index++) + ".off";
        CGAL::IO::write_OFF(filename, model);
    }
}

// store projection center
void priv::store(const Emboss::IProjection &projection,
                 const Point               &point_to_project,
                 float                      projection_ratio,
                 const std::string         &obj_filename)
{
    auto [front, back] = projection.create_front_back(point_to_project);
    Vec3d diff         = back - front;
    Vec3d pos          = front + diff * projection_ratio;
    priv::store(pos.cast<float>(), diff.normalized().cast<float>(),
                DEBUG_OUTPUT_DIR + "projection_center.obj"); // only debug
}

#endif // DEBUG_OUTPUT_DIR

bool Slic3r::corefine_test(const std::string &model_path, const std::string &shape_path) {
    priv::CutMesh model, shape;
    if (!CGAL::IO::read_OFF(model_path, model)) return false;
    if (!CGAL::IO::read_OFF(shape_path, shape)) return false;
        
    CGAL::Polygon_mesh_processing::corefine(model, shape);
    return true;
}
