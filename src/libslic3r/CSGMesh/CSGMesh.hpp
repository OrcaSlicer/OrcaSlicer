#ifndef CSGMESH_HPP
#define CSGMESH_HPP

#include <libslic3r/AnyPtr.hpp>
#include <admesh/stl.h>

namespace Slic3r { namespace csg {

// A CSGPartT should be an object that can provide at least a mesh + trafo and an
// associated csg operation. A collection of CSGPartT objects can then
// be interpreted as one model and used in various contexts. It can be assembled
// with CGAL or OpenVDB, rendered with OpenCSG or provided to a ray-tracer to
// deal with various parts of it according to the supported CSG types...
//
// A few simple templated interface functions are provided here and a default
// CSGPart class that implements the necessary means to be usable as a
// CSGPartT object.

// Supported CSG operation types
enum class CSGType { Union, Difference, Intersection };

// A CSG part can instruct the processing to push the sub-result until a new
// csg part with a pop instruction appears. This can be used to implement
// parentheses in a CSG expression represented by the collection of csg parts.
// A CSG part can not contain another CSG collection, only a mesh, this is why
// its easier to do this stacking instead of recursion in the data definition.
// CSGStackOp::Continue means no stack operation required.
// When a CSG part contains a Push instruction, it is expected that the CSG
// operation it contains refers to the whole collection spanning to the nearest
// part with a Pop instruction.
// e.g.:
// {
//      CUBE1: { mesh: cube, op: Union, stack op: Continue },
//      CUBE2: { mesh: cube, op: Difference, stack op: Push},
//      CUBE3: { mesh: cube, op: Union, stack op: Pop}
// }
// is a collection of csg parts representing the expression CUBE1 - (CUBE2 + CUBE3)
enum class CSGStackOp { Push, Continue, Pop };

// Get the CSG operation of the part. Can be overriden for any type
template<class CSGPartT> CSGType get_operation(const CSGPartT &part)
{
    return part.operation;
}

// Get the stack operation required by the CSG part.
template<class CSGPartT> CSGStackOp get_stack_operation(const CSGPartT &part)
{
    return part.stack_operation;
}

// Get the mesh for the part. Can be overriden for any type
template<class CSGPartT>
const indexed_triangle_set *get_mesh(const CSGPartT &part)
{
    return part.its_ptr.get();
}

// Get the transformation associated with the mesh inside a CSGPartT object.
// Can be overriden for any type.
template<class CSGPartT>
Transform3f get_transform(const CSGPartT &part)
{
    return part.trafo;
}

// Default implementation
struct CSGPart {
    AnyPtr<const indexed_triangle_set> its_ptr;
    Transform3f trafo;
    CSGType operation;
    CSGStackOp stack_operation;
    std::string name;

    CSGPart(AnyPtr<const indexed_triangle_set> ptr = {},
            CSGType                            op  = CSGType::Union,
            const Transform3f                 &tr  = Transform3f::Identity())
        : its_ptr{std::move(ptr)}
        , operation{op}
        , stack_operation{CSGStackOp::Continue}
        , trafo{tr}
    {}
};

//Prusa
// Check if there are only positive parts (Union) within the collection.
template<class Cont> bool is_all_positive(const Cont &csgmesh)
{
    bool is_all_pos =
        std::all_of(csgmesh.begin(),
                    csgmesh.end(),
                    [](auto &part) {
                        return csg::get_operation(part) == csg::CSGType::Union;
                    });

    return is_all_pos;
}

//Prusa
// Merge all the positive parts of the collection into a single triangle mesh without performing
// any booleans.
template<class Cont>
indexed_triangle_set csgmesh_merge_positive_parts(const Cont &csgmesh)
{
    indexed_triangle_set m;
    for (auto &csgpart : csgmesh) {
        auto op = csg::get_operation(csgpart);
        const indexed_triangle_set * pmesh = csg::get_mesh(csgpart);
        if (pmesh && op == csg::CSGType::Union) {
            indexed_triangle_set mcpy = *pmesh;
            its_transform(mcpy, csg::get_transform(csgpart), true);
            its_merge(m, mcpy);
        }
    }

    return m;
}

}} // namespace Slic3r::csg

#endif // CSGMESH_HPP
