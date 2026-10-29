#ifndef slic3r_SheetMetal_hpp_
#define slic3r_SheetMetal_hpp_

#include "libslic3r/Point.hpp"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>

#include <memory>
#include <vector>

// Sheet metal as plain B-rep modelling: a plate of constant thickness, bent along its edges. Every builder returns an
// ordinary solid, so holes, cuts, fillets and the rest of the CAD pipeline keep working on it.
//
// Conventions (ported as behaviour from SolidCraft's documented rules; the code is our own):
//  * a bend is an annulus of inner radius R and outer radius R + t; its virtual sharp is on the OUTER face, so the setback
//    (how far the flat plate is trimmed back from the edge) is (R + t) * tan(angle / 2);
//  * an edge flange's height is measured along its outer face from that virtual sharp;
//  * unfolding maps a bend to a flat strip of length angle * (R + k * t) (the neutral layer, k = K-factor).
namespace Slic3r {
namespace SheetMetal {

struct Rule {
    double t{2.5};      // thickness
    double r{-1};       // inner bend radius; <= 0 means "equal to the thickness"
    double k{0.44};     // K-factor, only used to unfold
    double gap{0.01};   // gap left inside a hem
    double radius() const { return r > 0 ? r : t; }
    double outer() const { return radius() + t; }
};

// Flanges on straight thickness-edges of `body` (each edge is the outer edge of a plate face). All edges of one call are
// trimmed first and their bends added after, so a closed loop of edges gives a tray with open corners.
TopoDS_Shape edge_flanges(const TopoDS_Shape& body, const std::vector<TopoDS_Edge>& edges, const Rule& rule,
                          double height, double angle_deg, bool flip);

// A sheet bent along a polyline: `path` are the world-space vertices of the sheet face that is away from the material, the
// material grows to the right of travel (travel x `normal`), `width` is the extrusion along `normal`.
TopoDS_Shape contour_flange(const std::vector<Vec3d>& path, const Vec3d& normal, double width, const Rule& rule, bool flip);

// Flat hem (180 deg fold) on the end edge of a flange; `length` is measured to the outside of the fold.
TopoDS_Shape flat_hem(const TopoDS_Shape& body, const TopoDS_Edge& edge, const Rule& rule, double length);

// Unfold every bend of `body` into a flat pattern. The sheet is cut into bend sectors (a bend is a pair of coaxial
// cylinders R and R + t) and the flat pieces between them; each bend becomes a strip of angle * (R + k t) and the pieces
// beyond it are rotated and moved back into the plane of the stationary piece (the one with the largest planar face).
// Throws when there is no bend, or the sheet is not one connected piece.
struct Unfolding;
std::shared_ptr<const Unfolding> unfold(const TopoDS_Shape& body, const Rule& rule);
TopoDS_Shape flat_shape(const Unfolding& u);
// Fold `edited_flat` (the flat pattern after cuts made while flat) back. Material removed from a flat piece is carried to
// the same place on the folded piece; material removed from a bend strip is carried over as the matching wedge of the
// bend (exact for cuts whose sides run along and across the bend, bounding-box for anything else). Material ADDED
// while flat is not carried over.
TopoDS_Shape refold(const Unfolding& u, const TopoDS_Shape& edited_flat);

} // namespace SheetMetal
} // namespace Slic3r

#endif
