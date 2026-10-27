// Adaptive cubic infill was inspired by the work of @mboerwinkle
// as implemented for Cura.
// https://github.com/Ultimaker/CuraEngine/issues/381
// https://github.com/Ultimaker/CuraEngine/pull/401
//
// Our implementation is more accurate (discretizes a bit less cubes than Cura's)
// by splitting only such cubes which contain a triangle. 
// Our line extraction is time optimal instead of O(n^2) when connecting extracted lines,
// and we also implemented adaptivity for supporting internal overhangs only.

#ifndef slic3r_FillAdaptive_hpp_
#define slic3r_FillAdaptive_hpp_

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "FillBase.hpp"
#include <cstddef>
#include <memory>
#include <utility>
#include <Eigen/Geometry>
#include <vector>
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Line.hpp"

struct indexed_triangle_set;

namespace Slic3r {

class PrintObject;

namespace FillAdaptive
{

struct Octree;
// To keep the definition of Octree opaque, we have to define a custom deleter.
struct OctreeDeleter { void operator()(Octree *p); };
using  OctreePtr = std::unique_ptr<Octree, OctreeDeleter>;

// Orca: One octree per body (see Layer::lslices_separated_component_ids), and one of the whole object
// for objects of a single body or with a body that has none of its own.
struct Octrees
{
    OctreePtr              object;
    std::vector<OctreePtr> bodies;

    // A body without an octree, or body -1, uses the object's, or any body's when the object has none.
    Octree *get(int body) const
    {
        if (body >= 0 && size_t(body) < bodies.size() && bodies[body])
            return bodies[body].get();
        if (object)
            return object.get();
        for (const OctreePtr &octree : bodies)
            if (octree)
                return octree.get();
        return nullptr;
    }
};

// Orca: The octrees of each line spacing the regions of an object fill with.
struct RegionOctrees
{
    std::vector<Octrees> sets;
    // Index into sets for each region, -1 for a region without adaptive or support cubic infill.
    std::vector<int>     region_set;

    const Octrees *region(size_t region_id) const
    {
        return region_id < region_set.size() && region_set[region_id] >= 0 ? &sets[region_set[region_id]] : nullptr;
    }
};

// Line spacing of the adaptive or support cubic infill of each region of the object,
// zero for a region that generates no such infill.
std::vector<double>             adaptive_fill_line_spacing(const PrintObject &print_object);

// Rotation of the octree to stand on one of its corners.
Eigen::Quaterniond              transform_to_world();
// Inverse roation of the above.
Eigen::Quaterniond              transform_to_octree();

FillAdaptive::OctreePtr         build_octree(
    // Mesh is rotated to the coordinate system of the octree.
    const indexed_triangle_set  &triangle_mesh,
    // Overhang triangles extracted from fill surfaces with stInternalBridge type, 
    // rotated to the coordinate system of the octree.
    const std::vector<Vec3d>    &overhang_triangles, 
    coordf_t                     line_spacing, 
    // If true, octree is densified below internal overhangs only.
    bool                         support_overhangs_only);

// Multiline infill: lines of the three families to non-crossing paths d1 apart, ends reaching end_overlap into walls.
Polylines                       multiline_paths(const Lines &lines, double d1, double end_overlap, int sweep, const BoundingBox &cover);

//
// Some of the algorithms used by class FillAdaptive were inspired by
// Cura Engine's class SubDivCube
// https://github.com/Ultimaker/CuraEngine/blob/master/src/infill/SubDivCube.h
//
class Filler : public Slic3r::Fill
{
public:
    ~Filler() override {}

protected:
    Fill* clone() const override { return new Filler(*this); }
	void _fill_surface_single(
	    const FillParams                &params,
	    unsigned int                     thickness_layers,
	    const std::pair<float, Point>   &direction,
	    ExPolygon                        expolygon,
	    Polylines                       &polylines_out) override;
    // Let the G-code export reoder the infill lines.
    //FIXME letting the G-code exporter to reorder infill lines of Adaptive Cubic Infill
    // may not be optimal as the internal infill lines may get extruded before the long infill
    // lines to which the short infill lines are supposed to anchor.
	bool no_sort() const override { return false; }
    bool is_self_crossing() override { return true; }
};

} // namespace FillAdaptive
} // namespace Slic3r

#endif // slic3r_FillAdaptive_hpp_
