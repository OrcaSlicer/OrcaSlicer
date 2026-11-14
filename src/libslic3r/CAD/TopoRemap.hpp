#pragma once
// Follow a face or edge id across an upstream edit.
//
// Face and edge ids are indices into a body's current topology (TopExp::MapShapes order), so they mean something else
// as soon as an earlier feature is edited: a fillet on "edge 14" fillets whatever is edge 14 afterwards. This finds
// where an entity of the OLD body went in the NEW one by what it is: surface/curve kind, orientation, where it sits in
// the body's bounding box (a stretch keeps that fixed), and what it touches. It answers -1 rather than guess when two
// candidates are as good as each other.
#include <TopoDS_Shape.hxx>

namespace Slic3r { namespace TopoRemap {

// The id in `now` of the face/edge that `old_id` named in `before`. Returns old_id itself when it still names the same
// thing, -1 when it cannot be told (no match, or an ambiguous one).
int remap_face(const TopoDS_Shape& before, const TopoDS_Shape& now, int old_id);
int remap_edge(const TopoDS_Shape& before, const TopoDS_Shape& now, int old_id);

}} // namespace Slic3r::TopoRemap
