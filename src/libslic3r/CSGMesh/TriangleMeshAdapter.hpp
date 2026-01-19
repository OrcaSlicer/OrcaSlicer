#ifndef TRIANGLEMESHADAPTER_HPP
#define TRIANGLEMESHADAPTER_HPP

#include "CSGMesh.hpp"

#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace csg {

// Provide default overloads for indexed_triangle_set to be usable as a plain
// CSGPart with an implicit union operation

inline CSGType get_operation(const indexed_triangle_set &part)
{
    return CSGType::Union;
}

inline CSGStackOp get_stack_operation(const indexed_triangle_set &part)
{
    return CSGStackOp::Continue;
}

inline const indexed_triangle_set * get_mesh(const indexed_triangle_set &part)
{
    return &part;
}

inline Transform3f get_transform(const indexed_triangle_set &part)
{
    return Transform3f::Identity();
}

inline CSGType get_operation(const indexed_triangle_set *const part)
{
    return CSGType::Union;
}

inline CSGStackOp get_stack_operation(const indexed_triangle_set *const part)
{
    return CSGStackOp::Continue;
}

inline const indexed_triangle_set * get_mesh(const indexed_triangle_set *const part)
{
    return part;
}

inline Transform3f get_transform(const indexed_triangle_set *const part)
{
    return Transform3f::Identity();
}

inline CSGType get_operation(const TriangleMesh &part)
{
    return CSGType::Union;
}

inline CSGStackOp get_stack_operation(const TriangleMesh &part)
{
    return CSGStackOp::Continue;
}

inline const indexed_triangle_set * get_mesh(const TriangleMesh &part)
{
    return &part.its;
}

inline Transform3f get_transform(const TriangleMesh &part)
{
    return Transform3f::Identity();
}

inline CSGType get_operation(const TriangleMesh * const part)
{
    return CSGType::Union;
}

inline CSGStackOp get_stack_operation(const TriangleMesh * const part)
{
    return CSGStackOp::Continue;
}

inline const indexed_triangle_set * get_mesh(const TriangleMesh * const part)
{
    return &part->its;
}

inline Transform3f get_transform(const TriangleMesh * const part)
{
    return Transform3f::Identity();
}

}} // namespace Slic3r::csg

#endif // TRIANGLEMESHADAPTER_HPP
