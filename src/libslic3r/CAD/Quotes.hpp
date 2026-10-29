#pragma once

#include <string>
#include <vector>

#include <TopoDS_Shape.hxx>

#include "libslic3r/Point.hpp"

namespace Slic3r {

class CadDocument;

// The dimensions a drawing office would put on a body, read off its shape: the overall size, the diameter of every hole and
// shaft, the radius of every fillet and round. Read from the geometry rather than from the feature list, so it is right for
// an imported solid and for a body three Booleans away from its sketch; `feature` then names the feature that drives a
// quote, when one can be found, so the viewport can reopen it.
namespace Quotes {

enum class Kind {
    Extent,     // the overall size along one axis: `a` and `b` are the corners of the body's box, `axis` the direction
    Diameter,   // a hole or a shaft: a full cylinder
    Radius      // a fillet or a round: part of a cylinder
};

// One place the same quote applies. The ends of the cylinder's axis, and the middle of the arc at each end (what a radius
// line points at); the viewport draws it at the end nearer to the eye.
struct Spot {
    Vec3d c0, c1;
    Vec3d m0, m1;
    Vec3d axis;   // unit, from c0 to c1
};

struct Quote {
    Kind              kind{Kind::Extent};
    double            value{0};   // mm; the length of an extent, or the RADIUS of a cylinder (a Diameter quote reads 2 x value)
    int               axis{0};    // Extent: 0 = X, 1 = Y, 2 = Z
    Vec3d             a{0, 0, 0}, b{0, 0, 0};
    std::vector<Spot> spots;      // Diameter / Radius: every cylinder of this size
    bool              concave{false};
    int               body{-1};
    int               feature{-1};   // the feature that drives the value, or -1: the quote is shown but cannot be edited

    int         count() const { return kind == Kind::Extent ? 1 : int(spots.size()); }
    // "Ø6", "4× Ø6", "R5", "42.5"
    std::string text() const;
};

// Number as a drawing writes it: no trailing zeros, at most two decimals.
std::string format_mm(double v);

// The quotes of one body, the overall size first. Empty for a body with no volume.
std::vector<Quote> compute(const TopoDS_Shape& body, int body_index);

// Names the driving feature of each quote: a Hole of that diameter, a Fillet of that radius.
void link_features(std::vector<Quote>& quotes, const CadDocument& doc);

} // namespace Quotes
} // namespace Slic3r
