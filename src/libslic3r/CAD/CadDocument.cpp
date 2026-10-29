#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/TopoRemap.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/CAD/SheetMetal.hpp"
#include "libslic3r/CAD/SketchConstraints.hpp"
#include "libslic3r/CAD/SketchSolver.hpp"
#include "libslic3r/CAD/SketchImport.hpp"   // transform_regions for imported art

#include <boost/log/trivial.hpp>
#include <Standard_Handle.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <Standard_TypeDef.hxx>
#include <TopAbs_State.hxx>
#include <array>

#include <Standard_Failure.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <Geom_BSplineCurve.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <NCollection_List.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakeFilling.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <functional>
#include <exception>
#include <cstdint>
#include <cereal/cereal.hpp>
#include <cereal/details/helpers.hpp>
#include <gp_Pln.hxx>
#include <NCollection_List.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepLib.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>   // plane_of_face: reject non-planar faces
#include <GeomAbs_SurfaceType.hxx>
#include <GeomAbs_CurveType.hxx>
#include <BRep_Tool.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <GeomAbs_Shape.hxx>        // SurfaceFill: GeomAbs_C0
#include <GC_MakeSegment2d.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopAbs.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Compound.hxx>      // multi-body: compound of bodies for display/compat
#include <BRep_Builder.hxx>
#include <TopAbs_Orientation.hxx>   // outward-normal orientation for face-extrude
#include <TopoDS.hxx>               // TopoDS::Edge for SurfaceFill
#include <mutex>
#include <list>
#include <unordered_map>
#include <chrono>
#include <TopExp_Explorer.hxx>      // is_sheet_shape
#include <ShapeUpgrade_UnifySameDomain.hxx>   // push/pull: merge the faces a pull leaves coplanar
#include <gp_Circ.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>
#include <gp_Trsf.hxx>             // pattern: rigid copy transforms
#include <STEPControl_Writer.hxx>  // STEP export (native B-rep)
#include <IGESControl_Writer.hxx>
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include <STEPControl_StepModelType.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <gp_Ax1.hxx>             // pattern: rotation axis (circular)
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <gp_XY.hxx>
#include <initializer_list>
#include <math.h>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <algorithm>
#include <sstream>

#include <cereal/archives/binary.hpp>
#include <cereal/types/array.hpp>
#include <BRepTools.hxx>
#include <BRepClass_FaceClassifier.hxx>   // faces_made_by: is a point on a face
#include <BRepLProp_SLProps.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <string>
#include <vector>
#include <optional>
#include <utility>
#include "libslic3r/Color.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <cstdlib>
#include <ratio>
#include "libslic3r/CAD/CadFeatureFields.hpp"
#include <GeomAbs_JoinType.hxx>
#include <BRepOffset_Mode.hxx>

class Geom2d_TrimmedCurve;

namespace Slic3r {

// ---- helical-thread construction helpers (file-local) ----------------------

// Helix spine on a cylinder (radius/pitch/height) about `axis`, as a wire.
static TopoDS_Wire make_helix_wire(const gp_Ax3& axis, double radius,
                                   double pitch, double height)
{
    Handle(Geom_CylindricalSurface) cyl = new Geom_CylindricalSurface(axis, radius);
    double turns = (pitch > 1e-6) ? (height / pitch) : 1.0;
    // In the surface (u,v) parametrization u is the angle, v the axial height.
    gp_Pnt2d p0(0.0, 0.0);
    gp_Pnt2d p1(2.0 * M_PI * turns, height);
    Handle(Geom2d_TrimmedCurve) seg = GC_MakeSegment2d(p0, p1);
    TopoDS_Edge e = BRepBuilderAPI_MakeEdge(seg, cyl).Edge();
    BRepLib::BuildCurves3d(e);
    return BRepBuilderAPI_MakeWire(e).Wire();
}

// Helix spine from a CadFeature's helix params. Supports cylindrical (taper==0)
// and conical (taper!=0) surfaces; left_handed flips the winding direction.
// Returns null wire if validation fails (error is written to err).
static TopoDS_Wire make_helix_spine(const CadFeature& f, std::string& err)
{
    err.clear();
    const double R = f.helix_radius, P = f.helix_pitch, H = f.helix_height;
    const double taper = f.helix_taper_deg * M_PI / 180.0;

    if (R <= 0)   { err = _u8L("helix radius must be > 0"); return TopoDS_Wire(); }
    if (P <= 0)   { err = _u8L("helix pitch must be > 0");  return TopoDS_Wire(); }
    if (H < 0)    { err = _u8L("helix height must be >= 0"); return TopoDS_Wire(); }
    if (H == 0)   { err = _u8L("helix height of 0 (flat spiral) is not supported"); return TopoDS_Wire(); }
    const double turns = H / P;
    if (turns > 10000) { err = _u8L("helix turn count exceeds limit (10000)"); return TopoDS_Wire(); }
    if (std::abs(taper) > 1e-12) {
        const double R_top = R + H * std::tan(taper);
        if (R_top <= 0) {
            err = _u8L("helix taper drives radius negative before reaching height");
            return TopoDS_Wire();
        }
    }

    gp_Dir zdir(f.plane.normal.x(), f.plane.normal.y(), f.plane.normal.z());
    gp_Dir xdir(f.plane.x_axis.x(), f.plane.x_axis.y(), f.plane.x_axis.z());
    Vec3d ori = f.plane.origin;
    gp_Pnt o(ori.x(), ori.y(), ori.z());
    gp_Ax2 ax2(o, zdir, xdir);
    gp_Ax3 ax3(o, zdir, xdir);

    TopoDS_Edge e;
    if (std::abs(taper) > 1e-12) {
        Handle(Geom_ConicalSurface) cone = new Geom_ConicalSurface(ax3, taper, R);
        double u1 = f.helix_left_handed ? -2.0 * M_PI * turns : 2.0 * M_PI * turns;
        gp_Pnt2d p0(0.0, 0.0);
        gp_Pnt2d p1(u1, H);
        Handle(Geom2d_TrimmedCurve) seg = GC_MakeSegment2d(p0, p1);
        e = BRepBuilderAPI_MakeEdge(seg, cone).Edge();
    } else {
        Handle(Geom_CylindricalSurface) cyl = new Geom_CylindricalSurface(ax3, R);
        double u1 = f.helix_left_handed ? -2.0 * M_PI * turns : 2.0 * M_PI * turns;
        gp_Pnt2d p0(0.0, 0.0);
        gp_Pnt2d p1(u1, H);
        Handle(Geom2d_TrimmedCurve) seg = GC_MakeSegment2d(p0, p1);
        e = BRepBuilderAPI_MakeEdge(seg, cyl).Edge();
    }
    BRepLib::BuildCurves3d(e);
    return BRepBuilderAPI_MakeWire(e).Wire();
}

// Triangular axial thread profile (a planar face) placed at the helix start
// (origin + radius*xdir). Spans +-pitch/2 axially; apex offset radially by depth.
// Both thread kinds sweep the SAME outward-biting V (base on the cylinder wall,
// apex `depth` into the surrounding material). Only the boolean differs:
//  - external: the V is FUSED to the rod  -> a raised helical ridge.
//  - internal: the V is CUT from the wall -> a sunken helical groove. The cut MUST
//    go outward into the wall to be visible; an inward V (the old behaviour) only
//    sweeps already-empty bore space and removes nothing.
// The V between two radii: its base (the axial edge) at `base_r`, its apex at `apex_r`. The
// base is meant to sit INSIDE the material it is fused to or cut from, the apex out in the space
// the thread reaches into. make_thread_profile below is the legacy layout.
static TopoDS_Wire make_thread_v(const gp_Pnt& origin, const gp_Dir& xdir, const gp_Dir& zdir,
                                 double base_r, double apex_r, double pitch)
{
    gp_Vec vx(xdir), vz(zdir);
    const double half = 0.42 * pitch;   // < pitch/2: adjacent turns must not touch (see below)
    gp_Pnt top (origin.XYZ() + (vx * base_r).XYZ() + (vz * ( half)).XYZ());
    gp_Pnt bot (origin.XYZ() + (vx * base_r).XYZ() + (vz * (-half)).XYZ());
    gp_Pnt apex(origin.XYZ() + (vx * apex_r).XYZ());
    BRepBuilderAPI_MakePolygon poly(top, bot, apex, true);
    return poly.Wire();
}

static TopoDS_Wire make_thread_profile(const gp_Pnt& origin, const gp_Dir& xdir,
                                       const gp_Dir& zdir, double radius,
                                       double pitch, double depth, bool internal)
{
    // `internal` is intentionally unused: the V is the same shape either way, always pointing
    // radially outward from `radius`. What differs is what the caller DOES with the swept solid
    // — an external thread fuses it onto the shaft, an internal one cuts it out of the wall after
    // boring at the minor diameter. Keeping the parameter documents that the caller decided, and
    // stops someone "fixing" the profile to point inward for the internal case, which would make
    // the groove sweep already-empty bore space and cut nothing.
    (void)internal;
    gp_Vec vx(xdir), vz(zdir);
    // Root the V CLEARLY inside the wall (a real overlap, not a 0.05 mm tangency) so the boolean
    // has clean intersections — near-coincident faces are what make OCCT's fuse/cut unstable.
    const double over = std::min(std::max(depth, 0.25), radius * 0.4);
    double inner = radius - over;   // base, well inside the wall (solid overlap)
    double crest = radius + depth;  // apex, `depth` into the surrounding material
    // Axial half-height must be < pitch/2 so ADJACENT helix turns don't collide — a full-pitch
    // profile makes the swept solid self-intersect (invalid -> never renders, or crashes the
    // boolean). 0.42*pitch leaves a clean gap between turns; the V still reads as a thread.
    const double half = 0.42 * pitch;
    gp_Pnt top (origin.XYZ() + (vx * inner).XYZ() + (vz * ( half)).XYZ());
    gp_Pnt bot (origin.XYZ() + (vx * inner).XYZ() + (vz * (-half)).XYZ());
    gp_Pnt apex(origin.XYZ() + (vx * crest).XYZ());
    BRepBuilderAPI_MakePolygon poly(top, bot, apex, true);
    return poly.Wire();   // closed triangle, swept by MakePipeShell with a fixed binormal
}

// ---- expression evaluator (file-local) -------------------------------------
// ponytail: self-contained shunting-yard arithmetic + function set for
// parametric dimension expressions. Extend the function table if needed.

// Built-in names an expression may use besides variables. Units convert a literal to the field's own unit (lengths mm,
// angles degrees): `10 cm` is 100, `2 in` is 50.8, `1 rad` is 57.29...; there is no dimension checking.
static const std::map<std::string, double>& expr_units()
{
    static const std::map<std::string, double> u = {
        {"mm", 1.0}, {"cm", 10.0}, {"m", 1000.0}, {"in", 25.4}, {"ft", 304.8}, {"um", 0.001},
        {"deg", 1.0}, {"rad", 180.0 / M_PI}};
    return u;
}
// Function name -> arity.
static const std::map<std::string, int>& expr_functions()
{
    static const std::map<std::string, int> f = {
        {"sqrt", 1}, {"abs", 1}, {"sin", 1}, {"cos", 1}, {"tan", 1}, {"deg", 1}, {"rad", 1}, {"asin", 1}, {"acos", 1}, {"atan", 1},
        {"floor", 1}, {"ceil", 1}, {"round", 1}, {"trunc", 1}, {"exp", 1}, {"ln", 1}, {"log", 1}, {"log10", 1}, {"sign", 1},
        {"min", 2}, {"max", 2}, {"atan2", 2}, {"mod", 2}, {"pow", 2}, {"hypot", 2}};
    return f;
}
static bool expr_builtin(const std::string& id) { return id == "pi" || expr_functions().count(id) || expr_units().count(id); }

// Evaluate an arithmetic expression to a double. Supports + - * /, parentheses,
// unary minus, decimal literals, and identifiers resolved via `vars`. Functions:
// sqrt, abs, sin, cos, tan, min, max, deg, rad, and the constant `pi`.
// ANGLES ARE DEGREES — every angle field in a feature is, so sin(30) is 0.5. `pi` is the number
// (for 2 * pi * r); to use it as an angle convert it: sin(deg(pi / 2)) = 1. deg() turns radians
// into degrees and rad() degrees into radians.
// Throws std::runtime_error on any parse or lookup error, div-by-zero, bad arity.
static double eval_expr(const std::string& src, const std::map<std::string, double>& vars)
{
    struct Token {
        enum Type { Num, Id, Op, LParen, RParen, Comma };
        Type type;
        std::string val;
        double num{0};
    };
    // precedence: 0=paren/comma, 1=+-, 2=*/, 3=unary-minus, 4=function
    auto prec = [](char op, bool unary) -> int {
        if (unary && op == 'u') return 3;
        if (op == '+' || op == '-') return 1;
        if (op == '*' || op == '/') return 2;
        if (op == '(' || op == ')' || op == ',') return 0;
        return 0;
    };
    auto is_op = [](char c) { return c == '+' || c == '-' || c == '*' || c == '/'; };

    // ---- tokenise ----
    std::vector<Token> tokens;
    size_t i = 0;
    bool prev_was_val = false; // tracked for unary-minus detection
    while (i < src.size()) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
        if (c == '(') { tokens.push_back({Token::LParen, "("}); ++i; prev_was_val = false; continue; }
        if (c == ')') { tokens.push_back({Token::RParen, ")"}); ++i; prev_was_val = true; continue; }
        if (c == ',') { tokens.push_back({Token::Comma, ","}); ++i; prev_was_val = false; continue; }
        if (isdigit(c) || c == '.') {
            size_t start = i;
            while (i < src.size() && (isdigit(src[i]) || src[i] == '.')) ++i;
            Token t{Token::Num, src.substr(start, i - start)};
            // The classic locale: under it_IT "3.6" would parse as 3 with std::stod. A literal that is not entirely a number
            // (`1.2.3`) is an error, never a silent truncation.
            std::istringstream ns(t.val);
            ns.imbue(std::locale::classic());
            ns >> t.num;
            if (ns.fail() || ns.peek() != std::char_traits<char>::eof()) throw std::runtime_error("bad number '" + t.val + "'");
            tokens.push_back(t);
            prev_was_val = true;
            continue;
        }
        if (isalpha(c) || c == '_') {
            size_t start = i;
            while (i < src.size() && (isalnum(src[i]) || src[i] == '_')) ++i;
            std::string id = src.substr(start, i - start);
            size_t nx = i;
            while (nx < src.size() && (src[nx] == ' ' || src[nx] == '\t')) ++nx;
            const bool call = nx < src.size() && src[nx] == '(';
            auto un = expr_units().find(id);
            if (!tokens.empty() && tokens.back().type == Token::Num && prev_was_val && !call && un != expr_units().end()) {
                tokens.push_back({Token::Op, "*"});   // `10 mm` = 10 * 1, `2 in` = 2 * 25.4
                Token f{Token::Num, "", un->second};
                tokens.push_back(f);
                prev_was_val = true;
                continue;
            }
            tokens.push_back({Token::Id, id});
            prev_was_val = true;
            continue;
        }
        if (is_op(c)) {
            // unary minus detection
            if (c == '-' && !prev_was_val) {
                tokens.push_back({Token::Op, "u"});
            } else {
                tokens.push_back({Token::Op, std::string(1, c)});
            }
            ++i;
            prev_was_val = false;
            continue;
        }
        throw std::runtime_error(_u8L("bad character in expression"));
    }

    // ---- shunting-yard: infix -> RPN ----
    std::vector<Token> rpn;
    std::vector<Token> stack;
    auto flush_paren = [&]() {
        while (!stack.empty() && stack.back().type != Token::LParen) {
            rpn.push_back(stack.back()); stack.pop_back();
        }
        if (stack.empty()) throw std::runtime_error(_u8L("mismatched parentheses"));
        stack.pop_back(); // discard '('
#if 0
        // If the '(' belonged to a function call, push the function name
        // (ponytail: we track this via the Id token on top of the op stack
        // BEFORE the '(' was pushed; after flush we check if the previous
        // token was a function-call Id).
#endif
        if (!stack.empty() && stack.back().type == Token::Id) {
            rpn.push_back(stack.back()); stack.pop_back();
        }
    };

    for (size_t j = 0; j < tokens.size(); ++j) {
        const Token& t = tokens[j];
        if (t.type == Token::Num) {
            rpn.push_back(t);
        } else if (t.type == Token::Id) {
            // function call if the next token is '('
            if (j + 1 < tokens.size() && tokens[j + 1].type == Token::LParen) {
                stack.push_back(t);
            } else {
                // identifier — resolve now
                double v;
                if (t.val == "pi") v = M_PI;
                else {
                    auto it = vars.find(t.val);
                    if (it == vars.end())
                        throw std::runtime_error(format(_u8L("unknown identifier: %1%"), t.val));
                    v = it->second;
                }
                rpn.push_back({Token::Num, "", v});
            }
        } else if (t.type == Token::Op) {
            int p = prec(t.val[0], t.val == "u");
            while (!stack.empty()) {
                const Token& top = stack.back();
                if (top.type != Token::Op && top.type != Token::Id) break;
                int tp = prec(top.val[0], top.val == "u");
                if (tp < 4 && p <= tp) {
                    rpn.push_back(top); stack.pop_back();
                } else break;
            }
            stack.push_back(t);
        } else if (t.type == Token::LParen) {
            stack.push_back(t);
        } else if (t.type == Token::RParen) {
            flush_paren();
        } else if (t.type == Token::Comma) {
            while (!stack.empty() && stack.back().type != Token::LParen) {
                rpn.push_back(stack.back()); stack.pop_back();
            }
            // , is a no-op separator — just stay inside the paren
        }
    }
    while (!stack.empty()) {
        if (stack.back().type == Token::LParen || stack.back().type == Token::RParen)
            throw std::runtime_error(_u8L("mismatched parentheses"));
        rpn.push_back(stack.back()); stack.pop_back();
    }

    // ---- evaluate RPN ----
    std::vector<double> vs;
    for (const Token& t : rpn) {
        if (t.type == Token::Num) {
            vs.push_back(t.num);
        } else if (t.type == Token::Op) {
            if (t.val == "u") {
                if (vs.empty()) throw std::runtime_error(_u8L("missing operand for unary minus"));
                vs.back() = -vs.back();
            } else {
                if (vs.size() < 2) throw std::runtime_error(format(_u8L("not enough operands for '%1%'"), t.val));
                double b = vs.back(); vs.pop_back();
                double a = vs.back(); vs.pop_back();
                if (t.val == "+") vs.push_back(a + b);
                else if (t.val == "-") vs.push_back(a - b);
                else if (t.val == "*") vs.push_back(a * b);
                else if (t.val == "/") { if (b == 0) throw std::runtime_error(_u8L("division by zero")); vs.push_back(a / b); }
            }
        } else if (t.type == Token::Id) {
            // function call (already on RPN via shunting-yard)
            auto call_fn = [&](const std::string& name, int arity) {
                if ((int)vs.size() < arity)
                    throw std::runtime_error(format(_u8L("not enough arguments for %1%()"), name));
                if (name == "sqrt") { double a = vs.back(); vs.pop_back(); vs.push_back(std::sqrt(a)); }
                else if (name == "abs") { double a = vs.back(); vs.pop_back(); vs.push_back(std::abs(a)); }
                else if (name == "sin") { double a = vs.back(); vs.pop_back(); vs.push_back(std::sin(a * M_PI / 180.0)); }
                else if (name == "cos") { double a = vs.back(); vs.pop_back(); vs.push_back(std::cos(a * M_PI / 180.0)); }
                else if (name == "tan") { double a = vs.back(); vs.pop_back(); vs.push_back(std::tan(a * M_PI / 180.0)); }
                else if (name == "deg") { double a = vs.back(); vs.pop_back(); vs.push_back(a * 180.0 / M_PI); }
                else if (name == "rad") { double a = vs.back(); vs.pop_back(); vs.push_back(a * M_PI / 180.0); }
                else if (name == "asin") { double a = vs.back(); vs.pop_back(); vs.push_back(std::asin(a) * 180.0 / M_PI); }   // angles are degrees
                else if (name == "acos") { double a = vs.back(); vs.pop_back(); vs.push_back(std::acos(a) * 180.0 / M_PI); }
                else if (name == "atan") { double a = vs.back(); vs.pop_back(); vs.push_back(std::atan(a) * 180.0 / M_PI); }
                else if (name == "floor") { double a = vs.back(); vs.pop_back(); vs.push_back(std::floor(a)); }
                else if (name == "ceil")  { double a = vs.back(); vs.pop_back(); vs.push_back(std::ceil(a)); }
                else if (name == "round") { double a = vs.back(); vs.pop_back(); vs.push_back(std::round(a)); }
                else if (name == "trunc") { double a = vs.back(); vs.pop_back(); vs.push_back(std::trunc(a)); }
                else if (name == "exp")   { double a = vs.back(); vs.pop_back(); vs.push_back(std::exp(a)); }
                else if (name == "ln")    { double a = vs.back(); vs.pop_back(); if (a <= 0) throw std::runtime_error(_u8L("ln of a non-positive number")); vs.push_back(std::log(a)); }
                else if (name == "log" || name == "log10") { double a = vs.back(); vs.pop_back(); if (a <= 0) throw std::runtime_error(_u8L("log of a non-positive number")); vs.push_back(std::log10(a)); }
                else if (name == "sign")  { double a = vs.back(); vs.pop_back(); vs.push_back(a > 0 ? 1.0 : a < 0 ? -1.0 : 0.0); }
                else if (name == "min") { double b = vs.back(); vs.pop_back(); double a = vs.back(); vs.pop_back(); vs.push_back(std::min(a, b)); }
                else if (name == "max") { double b = vs.back(); vs.pop_back(); double a = vs.back(); vs.pop_back(); vs.push_back(std::max(a, b)); }
                else if (name == "atan2") { double b = vs.back(); vs.pop_back(); double a = vs.back(); vs.pop_back(); vs.push_back(std::atan2(a, b) * 180.0 / M_PI); }   // atan2(y, x)
                else if (name == "mod")   { double b = vs.back(); vs.pop_back(); double a = vs.back(); vs.pop_back(); if (b == 0) throw std::runtime_error(_u8L("division by zero")); vs.push_back(std::fmod(a, b)); }
                else if (name == "pow")   { double b = vs.back(); vs.pop_back(); double a = vs.back(); vs.pop_back(); vs.push_back(std::pow(a, b)); }
                else if (name == "hypot") { double b = vs.back(); vs.pop_back(); double a = vs.back(); vs.pop_back(); vs.push_back(std::hypot(a, b)); }
                else throw std::runtime_error(format(_u8L("unknown function: %1%"), name));
            };
            auto fn = expr_functions().find(t.val);
            if (fn == expr_functions().end()) throw std::runtime_error(format(_u8L("unknown function: %1%"), t.val));
            call_fn(t.val, fn->second);
        }
    }
    if (vs.size() != 1) throw std::runtime_error(_u8L("invalid expression"));
    return vs[0];
}

// Topologically evaluate `variables` (name -> expression) into name -> value.
// An expression may reference other variables; resolution is recursive with a
// visiting set. Throws std::runtime_error("variable cycle: ...") on a dependency
// cycle, or propagates eval_expr errors.
static std::map<std::string, double>
evaluate_variables(const std::map<std::string, std::string>& variables)
{
    std::map<std::string, double> done;
    std::set<std::string> visiting;

    std::function<double(const std::string&)> resolve = [&](const std::string& name) -> double {
        auto itd = done.find(name);
        if (itd != done.end()) return itd->second;
        if (visiting.count(name)) throw std::runtime_error(format(_u8L("variable cycle: %1%"), name));
        auto itv = variables.find(name);
        if (itv == variables.end()) throw std::runtime_error(format(_u8L("unknown identifier: %1%"), name));
        visiting.insert(name);
        // pre-resolve every identifier `name` references: scan for identifiers in
        // its expression, resolve them first, then eval with the populated map.
        // ponytail: simplest correct approach — depth-first with visited tracking.
        std::map<std::string, double> scope = done; // start with already-resolved
        // Add any variable name found in the expression to scope (resolve recursively)
        const std::string& expr = itv->second;
        for (size_t i = 0; i < expr.size(); ) {
            char c = expr[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
            if (isalpha(c) || c == '_') {
                size_t start = i;
                while (i < expr.size() && (isalnum(expr[i]) || expr[i] == '_')) ++i;
                std::string id = expr.substr(start, i - start);
                // skip "pi" and function names — they're built-ins, not variables
                if (expr_builtin(id)) continue;   // pi, functions and units are not variables
                if (!variables.count(id)) throw std::runtime_error(format(_u8L("unknown identifier: %1%"), id));
                scope[id] = resolve(id);
                continue;
            }
            ++i;
        }
        double val = eval_expr(expr, scope);
        visiting.erase(name);
        done[name] = val;
        return val;
    };

    for (const auto& [k, _] : variables) resolve(k);
    return done;
}

// The numeric CadFeature fields an expression can drive: the one allow-list behind assign_field, is_bindable_field and
// CadDocument::bindable_fields (which the MCP reports and the docs table is generated from), so they cannot disagree.
// Alphabetical. thread_diameter (written as a radius), pattern_count (an int) and dim:<N> (a sketch dimension) are
// special-cased in assign_field.
static const struct { const char* name; double CadFeature::* member; } kBindable[] = {
    {"bool_tolerance", &CadFeature::bool_tolerance},
    {"cut_offset", &CadFeature::cut_offset},
    {"distance", &CadFeature::distance},
    {"distance2", &CadFeature::distance2},
    {"draft_angle", &CadFeature::draft_angle},
    {"dressup_size", &CadFeature::dressup_size},
    {"height", &CadFeature::height},
    {"helix_height", &CadFeature::helix_height},
    {"helix_pitch", &CadFeature::helix_pitch},
    {"helix_radius", &CadFeature::helix_radius},
    {"helix_taper_deg", &CadFeature::helix_taper_deg},
    {"hole_cbore_diameter", &CadFeature::hole_cbore_diameter},
    {"hole_cbore_depth", &CadFeature::hole_cbore_depth},
    {"hole_csink_angle", &CadFeature::hole_csink_angle},
    {"hole_csink_diameter", &CadFeature::hole_csink_diameter},
    {"hole_depth", &CadFeature::hole_depth},
    {"hole_diameter", &CadFeature::hole_diameter},
    {"hole_x", &CadFeature::hole_x},
    {"hole_y", &CadFeature::hole_y},
    {"import_scale_x", &CadFeature::import_scale_x},
    {"import_scale_y", &CadFeature::import_scale_y},
    {"mate_angle", &CadFeature::mate_angle},
    {"mate_offset", &CadFeature::mate_offset},
    {"pattern_angle", &CadFeature::pattern_angle},
    {"pattern_spacing", &CadFeature::pattern_spacing},
    {"pattern_spacing2", &CadFeature::pattern_spacing2},
    {"plane_angle_tilt", &CadFeature::plane_angle_tilt},
    {"plane_offset", &CadFeature::plane_offset},
    {"plane_u_size", &CadFeature::plane_u_size},
    {"plane_v_size", &CadFeature::plane_v_size},
    {"radius", &CadFeature::radius},
    {"revolve_angle", &CadFeature::revolve_angle},
    {"rib_depth", &CadFeature::rib_depth},
    {"rib_thickness", &CadFeature::rib_thickness},
    {"shell_thickness", &CadFeature::shell_thickness},
    {"sm_angle", &CadFeature::sm_angle},
    {"sm_gap", &CadFeature::sm_gap},
    {"sm_k", &CadFeature::sm_k},
    {"sm_radius", &CadFeature::sm_radius},
    {"sm_thickness", &CadFeature::sm_thickness},
    {"taper_deg", &CadFeature::taper_deg},
    {"thicken_thickness", &CadFeature::thicken_thickness},
    {"thin_in", &CadFeature::thin_in},
    {"thin_out", &CadFeature::thin_out},
    {"thread_depth", &CadFeature::thread_depth},
    {"thread_height", &CadFeature::thread_height},
    {"thread_pitch", &CadFeature::thread_pitch},
    {"thread_radius", &CadFeature::thread_radius},
    {"thread_x", &CadFeature::thread_x},
    {"thread_y", &CadFeature::thread_y},
    {"width", &CadFeature::width},
    {"xf_angle_deg", &CadFeature::xf_angle_deg},
};

// Write `value` into the CadFeature numeric field named `field`. Integer-valued fields are rounded. Throws
// std::runtime_error("unknown parameter: " + field) for anything not in the list.
static void assign_field(CadFeature& f, const std::string& field, double value)
{
    if (field.rfind("dim:", 0) == 0) {   // a sketch dimension: the value of entity_constraints[N]; the sketch is re-solved afterwards
        const int k = std::atoi(field.c_str() + 4);
        if (k < 0 || k >= int(f.entity_constraints.size())) throw std::runtime_error(format(_u8L("no sketch dimension %1%"), field));
        f.entity_constraints[k].value = value;
        return;
    }
    for (const auto& b : kBindable)
        if (field == b.name) { f.*(b.member) = value; return; }
    // The Thread card shows a DIAMETER (the size a thread is named by: M6, 1/4"), so that is
    // the name it offers for binding; thread_radius stays for recipes that already use it.
    if (field == "thread_diameter")   { f.thread_radius = 0.5 * value; return; }
    if (field == "pattern_count")     { f.pattern_count = (int)std::lround(value); return; }
    if (field == "pattern_count2")    { f.pattern_count2 = std::clamp((int)std::lround(value), 1, 500); return; }
    throw std::runtime_error(format(_u8L("unknown parameter: %1%"), field));
}

std::vector<std::string> CadDocument::bindable_fields()
{
    std::vector<std::string> out;
    for (const auto& b : kBindable) out.push_back(b.name);
    out.push_back("pattern_count");
    out.push_back("pattern_count2");
    out.push_back("thread_diameter");
    std::sort(out.begin(), out.end());
    return out;
}

bool CadDocument::produces_body(CadFeatureType t)
{
    switch (t) {
    case CadFeatureType::Sketch: case CadFeatureType::Helix: case CadFeatureType::Plane:
    case CadFeatureType::Axis:   case CadFeatureType::CoordSys: case CadFeatureType::Project:
    case CadFeatureType::Sketch3D: case CadFeatureType::SmRule:
        return false;
    default:
        return true;
    }
}

bool CadDocument::is_bindable_field(const std::string& field)
{
    if (field.rfind("dim:", 0) == 0) return field.size() > 4;
    CadFeature probe;
    try { assign_field(probe, field, 0.0); return true; }
    catch (const std::exception&) { return false; }
}

// ---------------------------------------------------------------------------

// Sample a sketch entity at parameter t in [0,1]. Handles Line, Arc, BSpline (cubic, 4 poles).
// Falls back to a p0->p1 lerp for any other type. // ponytail: covers the entities a guide
// curve is realistically drawn with; extend if needed.
static Vec2d sample_entity_2d(const SketchEntity& e, double t)
{
    t = std::max(0.0, std::min(1.0, t));
    switch (e.type) {
    case SketchEntity::Type::Line:
        return e.p0 + (e.p1 - e.p0) * t;
    case SketchEntity::Type::Arc: {
        double theta = e.start_angle + (e.end_angle - e.start_angle) * t;
        return e.center + e.radius * Vec2d(std::cos(theta), std::sin(theta));
    }
    case SketchEntity::Type::BSpline:
        if (e.ctrl.size() == 4) {
            const double u  = 1.0 - t;
            const double b0 = u * u * u;
            const double b1 = 3.0 * u * u * t;
            const double b2 = 3.0 * u * t * t;
            const double b3 = t * t * t;
            return e.ctrl[0] * b0 + e.ctrl[1] * b1 + e.ctrl[2] * b2 + e.ctrl[3] * b3;
        }
        return e.ctrl.empty() ? e.p0 + (e.p1 - e.p0) * t
                              : e.ctrl.front() + (e.ctrl.back() - e.ctrl.front()) * t;
    default:
        return e.p0 + (e.p1 - e.p0) * t;
    }
}

int CadDocument::add_sketch(SketchShape shape, const SketchPlane& plane,
                            double width, double height, double radius,
                            const std::string& name)
{
    CadFeature f;
    f.type   = CadFeatureType::Sketch;
    f.name   = name;
    f.shape  = shape;
    f.plane  = plane;
    f.width  = width;
    f.height = height;
    f.radius = radius;
    return append_feature(f);
}

int CadDocument::add_sketch_profile(const SketchProfile& profile, const SketchPlane& plane,
                                    const std::string& name)
{
    CadFeature f;
    f.type    = CadFeatureType::Sketch;
    f.name    = name;
    f.plane   = plane;
    f.profile = profile;
    return append_feature(f);
}

int CadDocument::add_sketch_entities(const std::vector<SketchEntity>& entities,
                                     const SketchPlane& plane, const std::string& name,
                                     const std::vector<SketchEntityConstraintDef>& constraints)
{
    CadFeature f;
    f.type               = CadFeatureType::Sketch;
    f.name               = name;
    f.plane              = plane;
    f.entities           = entities;
    f.entity_constraints = constraints;   // driving dimensions, solved by solve_sketch_feature
    return append_feature(f);
}

// Solve Onshape-style constraints on a SketchEntity list (Fase 4.3). All entity
// types participate: Line (P0,P1), Arc (P0,P1,Center), Circle (Center), Point (P0).
// Solved coordinates are written back, with arc angles reflowed from the solved
// center+endpoints. Free function (declared in SketchEngine.hpp) so the in-session
// GUI sketch tool can live-solve the same way committed features do.
bool solve_sketch_entities(std::vector<SketchEntity>& entities,
                           const std::vector<SketchEntityConstraintDef>& constraints)
{
    // Delegated to the vendored SolveSpace solver (SketchSolver / libslvs): full
    // constraint set, real DoF + over-constrained detection.
    SketchSolveNoBad no_bad;   // a yes/no answer: do not search for the conflicting constraints
    return sketch_solve(entities, constraints).ok;
}

#if 0  // legacy hand-rolled Gauss-Newton solver — superseded by libslvs, kept for reference
static bool legacy_solve_sketch_entities(std::vector<SketchEntity>& entities,
                           const std::vector<SketchEntityConstraintDef>& constraints)
{
    if (constraints.empty()) return true;

    SketchConstraints sc;
    // table[entity][role] -> solver point id, or -1 if that role is unregistered.
    std::vector<std::array<int, 3>> table(entities.size(), {-1, -1, -1});
    auto reg = [&](int ei, SketchPointRole role, const Vec2d& p) {
        table[ei][int(role)] = sc.add_point(p.x(), p.y());
    };
    for (size_t i = 0; i < entities.size(); ++i) {
        const SketchEntity& e = entities[i];
        switch (e.type) {
        case SketchEntity::Type::Line:
            reg(int(i), SketchPointRole::P0, e.p0);
            reg(int(i), SketchPointRole::P1, e.p1);
            break;
        case SketchEntity::Type::Arc:
            reg(int(i), SketchPointRole::P0, e.p0);
            reg(int(i), SketchPointRole::P1, e.p1);
            reg(int(i), SketchPointRole::Center, e.center);
            break;
        case SketchEntity::Type::Circle:
            reg(int(i), SketchPointRole::Center, e.center);
            break;
        case SketchEntity::Type::Point:
            reg(int(i), SketchPointRole::P0, e.p0);
            break;
        }
    }

    auto pid = [&](int ei, SketchPointRole role) -> int {
        if (ei < 0 || ei >= int(table.size())) return -1;
        return table[ei][int(role)];
    };

    for (const SketchEntityConstraintDef& c : constraints) {
        switch (c.type) {
        // Point-form: refs A and B name individual entity points.
        case SketchConstraintType::Fix: {
            int a = pid(c.ea, c.ra);
            if (a >= 0) sc.fix_point(a);
            break;
        }
        case SketchConstraintType::Coincident: {
            int a = pid(c.ea, c.ra), b = pid(c.eb, c.rb);
            if (a >= 0 && b >= 0) sc.coincident(a, b);
            break;
        }
        case SketchConstraintType::Horizontal: {
            int a = pid(c.ea, c.ra), b = pid(c.eb, c.rb);
            if (a >= 0 && b >= 0) sc.horizontal(a, b);
            break;
        }
        case SketchConstraintType::Vertical: {
            int a = pid(c.ea, c.ra), b = pid(c.eb, c.rb);
            if (a >= 0 && b >= 0) sc.vertical(a, b);
            break;
        }
        case SketchConstraintType::Distance: {
            int a = pid(c.ea, c.ra), b = pid(c.eb, c.rb);
            if (a >= 0 && b >= 0) sc.distance(a, b, c.value);
            break;
        }
        case SketchConstraintType::LockX: {
            int a = pid(c.ea, c.ra);
            if (a >= 0) sc.lock_x(a, c.value);
            break;
        }
        case SketchConstraintType::LockY: {
            int a = pid(c.ea, c.ra);
            if (a >= 0) sc.lock_y(a, c.value);
            break;
        }
        // Segment-form: ea and eb name whole line segments (their P0->P1).
        case SketchConstraintType::Parallel:
        case SketchConstraintType::Perpendicular:
        case SketchConstraintType::EqualLength: {
            int a0 = pid(c.ea, SketchPointRole::P0), a1 = pid(c.ea, SketchPointRole::P1);
            int b0 = pid(c.eb, SketchPointRole::P0), b1 = pid(c.eb, SketchPointRole::P1);
            if (a0 < 0 || a1 < 0 || b0 < 0 || b1 < 0) break;
            if (c.type == SketchConstraintType::Parallel)      sc.parallel(a0, a1, b0, b1);
            else if (c.type == SketchConstraintType::Perpendicular) sc.perpendicular(a0, a1, b0, b1);
            else                                               sc.equal_length(a0, a1, b0, b1);
            break;
        }
        case SketchConstraintType::Concentric: {
            int a = pid(c.ea, SketchPointRole::Center);
            int b = pid(c.eb, SketchPointRole::Center);
            if (a >= 0 && b >= 0) sc.coincident(a, b);
            break;
        }
        case SketchConstraintType::Midpoint: {
            int m = pid(c.ea, c.ra);
            int a = pid(c.eb, SketchPointRole::P0);
            int b = pid(c.eb, SketchPointRole::P1);
            if (m >= 0 && a >= 0 && b >= 0) sc.midpoint(m, a, b);
            break;
        }
        case SketchConstraintType::Symmetric: {
            int a  = pid(c.ea, c.ra);
            int b  = pid(c.eb, c.rb);
            int x0 = pid(c.ec, SketchPointRole::P0);
            int x1 = pid(c.ec, SketchPointRole::P1);
            if (a >= 0 && b >= 0 && x0 >= 0 && x1 >= 0) sc.symmetric(a, b, x0, x1);
            break;
        }
        case SketchConstraintType::Angle: {
            int a0 = pid(c.ea, SketchPointRole::P0), a1 = pid(c.ea, SketchPointRole::P1);
            int b0 = pid(c.eb, SketchPointRole::P0), b1 = pid(c.eb, SketchPointRole::P1);
            if (a0 >= 0 && a1 >= 0 && b0 >= 0 && b1 >= 0) sc.angle(a0, a1, b0, b1, c.value);
            break;
        }
        case SketchConstraintType::Radius:
        case SketchConstraintType::Diameter:
            // dimensions: applied in the post-solve pass below, not via the solver.
            break;
        case SketchConstraintType::PointOnLine: {
            // Point `ea`/`ra` is held at signed perpendicular distance `value` from
            // line `eb` (value 0 -> on the line). Drives e.g. a circle centre onto a
            // construction axis and keeps it there through later edits.
            int p  = pid(c.ea, c.ra);
            int l0 = pid(c.eb, SketchPointRole::P0), l1 = pid(c.eb, SketchPointRole::P1);
            if (p >= 0 && l0 >= 0 && l1 >= 0) sc.point_line_distance(p, l0, l1, c.value);
            break;
        }
        case SketchConstraintType::Tangent: {
            auto in_range = [&](int e){ return e >= 0 && e < (int)entities.size(); };
            if (!in_range(c.ea) || !in_range(c.eb)) break;
            const SketchEntity& ea_e = entities[c.ea];
            const SketchEntity& eb_e = entities[c.eb];
            auto is_round = [](const SketchEntity& e){
                return e.type == SketchEntity::Type::Circle || e.type == SketchEntity::Type::Arc; };
            if (is_round(ea_e) && eb_e.type == SketchEntity::Type::Line) {
                int cen = pid(c.ea, SketchPointRole::Center);
                int l0 = pid(c.eb, SketchPointRole::P0), l1 = pid(c.eb, SketchPointRole::P1);
                if (cen >= 0 && l0 >= 0 && l1 >= 0) sc.point_line_distance(cen, l0, l1, ea_e.radius);
            } else if (is_round(eb_e) && ea_e.type == SketchEntity::Type::Line) {
                int cen = pid(c.eb, SketchPointRole::Center);
                int l0 = pid(c.ea, SketchPointRole::P0), l1 = pid(c.ea, SketchPointRole::P1);
                if (cen >= 0 && l0 >= 0 && l1 >= 0) sc.point_line_distance(cen, l0, l1, eb_e.radius);
            } else if (is_round(ea_e) && is_round(eb_e)) {
                int c0 = pid(c.ea, SketchPointRole::Center), c1 = pid(c.eb, SketchPointRole::Center);
                if (c0 >= 0 && c1 >= 0) sc.distance(c0, c1, ea_e.radius + eb_e.radius);
            }
            break;
        }
        }
    }

    const bool ok = sc.solve();
    // Write solved coordinates back into the participating entities.
    for (size_t i = 0; i < entities.size(); ++i) {
        SketchEntity& e = entities[i];
        int ip0 = table[i][int(SketchPointRole::P0)];
        int ip1 = table[i][int(SketchPointRole::P1)];
        int ic  = table[i][int(SketchPointRole::Center)];
        if (ip0 >= 0) e.p0     = sc.get_point(ip0);
        if (ip1 >= 0) e.p1     = sc.get_point(ip1);
        if (ic  >= 0) e.center = sc.get_point(ic);

        if (e.type == SketchEntity::Type::Arc && ic >= 0) {
            // Reflow arc angles from solved center + endpoints, preserving the
            // original sweep direction (CCW vs CW).
            const double old_sweep = e.end_angle - e.start_angle;     // signed, original
            double ns = std::atan2(e.p0.y() - e.center.y(), e.p0.x() - e.center.x());
            double ne = std::atan2(e.p1.y() - e.center.y(), e.p1.x() - e.center.x());
            double sweep = ne - ns;
            // Normalize `sweep` into (-2pi, 2pi) then match the sign of old_sweep so
            // the arc keeps turning the same way it did before solving.
            const double TWO_PI = 2.0 * M_PI;
            while (sweep <=  -TWO_PI) sweep += TWO_PI;
            while (sweep >=   TWO_PI) sweep -= TWO_PI;
            if (old_sweep >= 0.0 && sweep < 0.0) sweep += TWO_PI;
            if (old_sweep <  0.0 && sweep > 0.0) sweep -= TWO_PI;
            e.start_angle = ns;
            e.end_angle   = ns + sweep;
            e.radius = 0.5 * ((e.p0 - e.center).norm() + (e.p1 - e.center).norm());
        }
        if (e.type == SketchEntity::Type::Circle && ic >= 0) {
            // p0 mirrors the center for circles; keep them consistent.
            e.p0 = e.center;
        }
    }
    // Apply radius/diameter dimensions directly (radius is not a solver variable).
    for (const auto& c : constraints) {
        if (c.type != SketchConstraintType::Radius &&
            c.type != SketchConstraintType::Diameter) continue;
        if (c.ea < 0 || c.ea >= (int)entities.size()) continue;
        SketchEntity& e = entities[c.ea];
        if (e.type != SketchEntity::Type::Circle && e.type != SketchEntity::Type::Arc) continue;
        const double r = (c.type == SketchConstraintType::Diameter) ? 0.5 * c.value : c.value;
        if (r <= 0.0) continue;
        e.radius = r;
        if (e.type == SketchEntity::Type::Arc) {
            // Rescale endpoints to the new radius around the (solved) center, keeping
            // each endpoint's direction so the reflowed start/end angles stay valid.
            auto rescale = [&](Vec2d& p) {
                Vec2d d = p - e.center;
                const double n = d.norm();
                if (n > 1e-12) p = e.center + (r / n) * d;
            };
            rescale(e.p0);
            rescale(e.p1);
        }
    }
    return ok;
}
#endif  // legacy solver

bool CadDocument::solve_sketch_feature(int index)
{
    if (index < 0 || index >= int(features.size())) return false;
    CadFeature& f = features[index];
    if (f.type != CadFeatureType::Sketch) return false;

    // Onshape-style entity sketches solve against entity endpoints (Fase 4.2).
    if (!f.entities.empty())
        return solve_sketch_entities(f.entities, f.entity_constraints);

    if (f.constraints.empty()) return true;

    // Legacy point-profile path. Every index is checked before the solver sees it: an unset
    // (-1) or stale index made get_point/residuals read m_vars[2*-1]. A constraint type this
    // solver cannot express is a failure, not a silent no-op, and the points are written back
    // only when the solve succeeded — the entity path's rule (SketchSolver.cpp) too.
    const int np = int(f.profile.points.size());
    auto ok_idx = [np](std::initializer_list<int> ids) {
        for (int i : ids) if (i < 0 || i >= np) return false;
        return true;
    };
    SketchConstraints sc;
    for (const Vec2d& p : f.profile.points)
        sc.add_point(p.x(), p.y());

    for (const SketchConstraintDef& c : f.constraints) {
        bool valid = true;
        switch (c.type) {
        case SketchConstraintType::Fix:          if ((valid = ok_idx({c.a}))) sc.fix_point(c.a); break;
        case SketchConstraintType::Coincident:   if ((valid = ok_idx({c.a, c.b}))) sc.coincident(c.a, c.b); break;
        case SketchConstraintType::Horizontal:   if ((valid = ok_idx({c.a, c.b}))) sc.horizontal(c.a, c.b); break;
        case SketchConstraintType::Vertical:     if ((valid = ok_idx({c.a, c.b}))) sc.vertical(c.a, c.b); break;
        case SketchConstraintType::Distance:     if ((valid = ok_idx({c.a, c.b}))) sc.distance(c.a, c.b, c.value); break;
        case SketchConstraintType::LockX:        if ((valid = ok_idx({c.a}))) sc.lock_x(c.a, c.value); break;
        case SketchConstraintType::LockY:        if ((valid = ok_idx({c.a}))) sc.lock_y(c.a, c.value); break;
        case SketchConstraintType::EqualLength:  if ((valid = ok_idx({c.a, c.b, c.c, c.d}))) sc.equal_length(c.a, c.b, c.c, c.d); break;
        case SketchConstraintType::Parallel:     if ((valid = ok_idx({c.a, c.b, c.c, c.d}))) sc.parallel(c.a, c.b, c.c, c.d); break;
        case SketchConstraintType::Perpendicular:if ((valid = ok_idx({c.a, c.b, c.c, c.d}))) sc.perpendicular(c.a, c.b, c.c, c.d); break;
        // The legacy solver implements these too; they used to be dropped without a word.
        case SketchConstraintType::Midpoint:     if ((valid = ok_idx({c.a, c.b, c.c}))) sc.midpoint(c.a, c.b, c.c); break;
        case SketchConstraintType::Symmetric:    if ((valid = ok_idx({c.a, c.b, c.c, c.d}))) sc.symmetric(c.a, c.b, c.c, c.d); break;
        case SketchConstraintType::Angle:        if ((valid = ok_idx({c.a, c.b, c.c, c.d}))) sc.angle(c.a, c.b, c.c, c.d, c.value); break;
        case SketchConstraintType::PointOnLine:  if ((valid = ok_idx({c.a, c.b, c.c}))) sc.point_line_distance(c.a, c.b, c.c, c.value); break;
        default:                                 valid = false; break;   // entity-only types
        }
        if (!valid) return false;
    }

    if (!sc.solve()) return false;
    for (size_t i = 0; i < f.profile.points.size(); ++i)
        f.profile.points[i] = sc.get_point(int(i));
    return true;
}

int CadDocument::add_extrude(int sketch_ref, double distance, bool symmetric,
                             BooleanMode mode, const std::string& name)
{
    CadFeature f;
    f.type       = CadFeatureType::Extrude;
    f.name       = name;
    f.sketch_ref = sketch_ref;
    f.distance   = distance;
    f.symmetric  = symmetric;
    f.mode       = mode;
    return append_feature(f);
}

int CadDocument::add_extrude_entities(const std::vector<SketchEntity>& entities,
                                      const SketchPlane& plane, double distance,
                                      bool symmetric, BooleanMode mode, const std::string& name)
{
    // Self-contained extrude of a single loop: the entity subset lives on the feature
    // itself (sketch_ref = -1), so build_sketch_wire(f) uses f.entities directly. The
    // source sketch stays a separate feature, so its other loops remain selectable.
    CadFeature f;
    f.type       = CadFeatureType::Extrude;
    f.name       = name;
    f.sketch_ref = -1;
    f.entities   = entities;
    f.plane      = plane;
    f.distance   = distance;
    f.symmetric  = symmetric;
    f.mode       = mode;
    return append_feature(f);
}

int CadDocument::add_extrude_face(int src_face, double distance, bool symmetric,
                                  BooleanMode mode, const std::string& name)
{
    CadFeature f;
    f.type             = CadFeatureType::Extrude;
    f.name             = name;
    f.sketch_ref       = -1;
    f.extrude_src_face = src_face;
    f.distance         = distance;
    f.symmetric        = symmetric;
    f.mode             = mode;
    return append_feature(f);
}

int CadDocument::add_fillet(double radius, FaceGroup faces, const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::Fillet;
    f.name         = name;
    f.dressup_size = radius;
    f.face_group   = faces;
    return append_feature(f);
}

int CadDocument::add_fillet(double radius, int edge_id, const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::Fillet;
    f.name         = name;
    f.dressup_size = radius;
    f.dressup_edge = edge_id;
    return append_feature(f);
}

int CadDocument::add_fillet(double radius, const std::vector<int>& edge_ids, const std::string& name)
{
    if (edge_ids.size() == 1) return add_fillet(radius, edge_ids.front(), name);
    CadFeature f;
    f.type          = CadFeatureType::Fillet;
    f.name          = name;
    f.dressup_size  = radius;
    f.dressup_edges = edge_ids;
    f.dressup_edge  = edge_ids.empty() ? -1 : edge_ids.front();
    return append_feature(f);
}

int CadDocument::add_chamfer(double distance, FaceGroup faces, const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::Chamfer;
    f.name         = name;
    f.dressup_size = distance;
    f.face_group   = faces;
    return append_feature(f);
}

int CadDocument::add_chamfer(double distance, int edge_id, const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::Chamfer;
    f.name         = name;
    f.dressup_size = distance;
    f.dressup_edge = edge_id;
    return append_feature(f);
}

int CadDocument::add_chamfer(double distance, const std::vector<int>& edge_ids, const std::string& name)
{
    if (edge_ids.size() == 1) return add_chamfer(distance, edge_ids.front(), name);
    CadFeature f;
    f.type          = CadFeatureType::Chamfer;
    f.name          = name;
    f.dressup_size  = distance;
    f.dressup_edges = edge_ids;
    f.dressup_edge  = edge_ids.empty() ? -1 : edge_ids.front();
    return append_feature(f);
}

int CadDocument::add_hole(double diameter, double depth, bool through,
                          double x, double y, const SketchPlane& plane,
                          const std::string& name)
{
    CadFeature f;
    f.type          = CadFeatureType::Hole;
    f.name          = name;
    f.plane         = plane;
    f.hole_diameter = diameter;
    f.hole_depth    = depth;
    f.hole_through  = through;
    f.hole_x        = x;
    f.hole_y        = y;
    return append_feature(f);
}

static bool is_sheet_feature(CadFeatureType t)
{
    return t == CadFeatureType::SmBase || t == CadFeatureType::SmEdgeFlange || t == CadFeatureType::SmContourFlange
        || t == CadFeatureType::SmHem || t == CadFeatureType::SmUnfold || t == CadFeatureType::SmRefold;
}

static SheetMetal::Rule sheet_rule_of(const CadFeature& f)
{
    SheetMetal::Rule r;
    r.t = f.sm_thickness; r.r = f.sm_radius; r.k = f.sm_k; r.gap = f.sm_gap;
    return r;
}

void CadDocument::bind_sheet_rules()
{
    CadFeature rule;   // defaults = the "Steel (mm)" rule
    for (CadFeature& f : features) {
        if (f.type == CadFeatureType::SmRule) { rule = f; continue; }
        if (!is_sheet_feature(f.type)) continue;
        f.sm_thickness = rule.sm_thickness; f.sm_radius = rule.sm_radius; f.sm_k = rule.sm_k; f.sm_gap = rule.sm_gap;
    }
}

static void sheet_defaults(const std::vector<CadFeature>& features, CadFeature& f)
{
    for (size_t i = features.size(); i-- > 0;)
        if (features[i].type == CadFeatureType::SmRule) {
            f.sm_thickness = features[i].sm_thickness; f.sm_radius = features[i].sm_radius;
            f.sm_k = features[i].sm_k; f.sm_gap = features[i].sm_gap;
            return;
        }
}

int CadDocument::add_sm_rule(double thickness, double bend_radius, double k_factor, double hem_gap, const std::string& name)
{
    CadFeature f;
    f.type = CadFeatureType::SmRule; f.name = name;
    f.sm_thickness = thickness; f.sm_radius = bend_radius; f.sm_k = k_factor; f.sm_gap = hem_gap;
    return append_feature(f);
}

int CadDocument::add_sm_base(int sketch_ref, bool flip, const std::string& name)
{
    CadFeature f;
    f.type = CadFeatureType::SmBase; f.name = name; f.sketch_ref = sketch_ref; f.flip = flip; f.mode = BooleanMode::New;
    sheet_defaults(features, f);
    return append_feature(f);
}

int CadDocument::add_sm_edge_flange(const std::vector<int>& edges, double height, double angle_deg, bool flip, const std::string& name)
{
    CadFeature f;
    f.type = CadFeatureType::SmEdgeFlange; f.name = name; f.dressup_edges = edges; f.distance = height; f.sm_angle = angle_deg; f.flip = flip;
    sheet_defaults(features, f);
    return append_feature(f);
}

int CadDocument::add_sm_contour_flange(int sketch_ref, double width, bool flip, const std::string& name)
{
    CadFeature f;
    f.type = CadFeatureType::SmContourFlange; f.name = name; f.sketch_ref = sketch_ref; f.distance = width; f.flip = flip; f.mode = BooleanMode::New;
    sheet_defaults(features, f);
    return append_feature(f);
}

int CadDocument::add_sm_hem(int edge, double length, const std::string& name)
{
    CadFeature f;
    f.type = CadFeatureType::SmHem; f.name = name; f.dressup_edges = { edge }; f.distance = length;
    sheet_defaults(features, f);
    return append_feature(f);
}

int CadDocument::add_sm_unfold(const std::string& name)
{
    CadFeature f;
    f.type = CadFeatureType::SmUnfold; f.name = name;
    sheet_defaults(features, f);
    return append_feature(f);
}

int CadDocument::add_sm_refold(int unfold_feature, const std::string& name)
{
    CadFeature f;
    f.type = CadFeatureType::SmRefold; f.name = name; f.sm_ref = unfold_feature;
    sheet_defaults(features, f);
    return append_feature(f);
}

// Hole standards: clearance hole, counterbore for a socket head cap screw, countersink for a
// flat head screw. Metric: ISO 273 medium clearance, DIN 974-1 counterbores for ISO 4762 (depth
// = head height + ~0.4 mm), 90° countersinks sized to the ISO 10642 head. Inch: ASME B18.3 /
// B18.6.3 — the counterbore is the head plus 1/32", the countersink is 82°, the angle an inch
// flat head is made with (a 90° seat under an 82° head bears only on its rim).
// Designations are the ones ThreadStandards uses ("1/4-20 UNC"); the bare form without the
// series ("1/4-20") is still accepted, since recipes and scripts were written with it.
struct HoleStdEntry { const char* desig; double clearance; double cbore_d; double cbore_depth;
                      double csink_d; double csink_angle; };
static const HoleStdEntry kHoleStdTable[] = {
    {"M3",           3.4,  6.5,  3.4,  6.7, 90},
    {"M4",           4.5,  8.0,  4.4,  9.0, 90},
    {"M5",           5.5, 10.0,  5.4, 11.3, 90},
    {"M6",           6.6, 11.0,  6.4, 13.4, 90},
    {"M8",           9.0, 15.0,  8.6, 17.9, 90},
    {"M10",         11.0, 18.0, 10.6, 22.4, 90},
    {"#6-32 UNC",    3.7,  6.4,  3.8,  7.1, 82},
    {"#8-32 UNC",    4.5,  7.9,  4.6,  8.4, 82},
    {"#10-24 UNC",   5.1,  9.5,  5.3,  9.8, 82},
    {"1/4-20 UNC",   6.8, 11.1,  6.9, 12.9, 82},
    {"5/16-18 UNC",  8.4, 13.5,  8.6, 16.1, 82},
    {"3/8-16 UNC",  10.1, 15.9, 10.2, 19.4, 82},
};
static const HoleStdEntry* hole_std_lookup(const std::string& desig)
{
    for (const auto& e : kHoleStdTable) {
        const std::string d(e.desig);
        if (d == desig) return &e;
        const size_t sp = d.find(' ');
        if (sp != std::string::npos && d.compare(0, sp, desig) == 0 && desig.size() == sp) return &e;
    }
    return nullptr;
}

int CadDocument::add_hole_styled(double diameter, double depth, bool through,
                                 double x, double y, const SketchPlane& plane, int style,
                                 double cbore_diameter, double cbore_depth,
                                 double csink_diameter, double csink_angle,
                                 const std::string& standard, const std::string& name)
{
    CadFeature f;
    f.type                = CadFeatureType::Hole;
    f.name                = name;
    f.plane               = plane;
    f.hole_diameter       = diameter;
    f.hole_depth          = depth;
    f.hole_through        = through;
    f.hole_x              = x;
    f.hole_y              = y;
    f.hole_style          = style;
    f.hole_cbore_diameter = cbore_diameter;
    f.hole_cbore_depth    = cbore_depth;
    f.hole_csink_diameter = csink_diameter;
    f.hole_csink_angle    = csink_angle;
    f.hole_standard       = standard;
    return append_feature(f);
}

int CadDocument::add_hole_standard(const std::string& designation, int style, bool through,
                                   double depth, double x, double y,
                                   const SketchPlane& plane, const std::string& name)
{
    const HoleStdEntry* e = hole_std_lookup(designation);
    if (e == nullptr)
        throw std::runtime_error(format(_u8L("unknown hole standard \"%1%\""), designation));
    return add_hole_styled(e->clearance, depth, through, x, y, plane, style,
                           e->cbore_d, e->cbore_depth, e->csink_d, e->csink_angle, e->desig, name);
}

int CadDocument::add_thread(double radius, double pitch, double height, double depth,
                            bool internal, double x, double y, const SketchPlane& plane,
                            const std::string& name)
{
    CadFeature f;
    f.type            = CadFeatureType::Thread;
    f.name            = name;
    f.plane           = plane;
    f.thread_radius   = radius;
    f.thread_pitch    = pitch;
    f.thread_height   = height;
    f.thread_depth    = depth;
    f.thread_internal = internal;
    f.thread_x        = x;
    f.thread_y        = y;
    f.thread_major_nominal = true;   // `radius` is the major (nominal) radius
    return append_feature(f);
}

int CadDocument::add_revolve(int sketch_ref, double angle, int axis, bool flip,
                             BooleanMode mode, const std::string& name)
{
    CadFeature f;
    f.type          = CadFeatureType::Revolve;
    f.name          = name;
    f.sketch_ref    = sketch_ref;
    f.revolve_angle = angle;
    f.revolve_axis  = axis;
    f.flip          = flip;
    f.mode          = mode;
    return append_feature(f);
}

int CadDocument::add_revolve_entities(const std::vector<SketchEntity>& entities,
                                      const SketchPlane& plane, double angle, int axis,
                                      bool flip, BooleanMode mode, const std::string& name)
{
    CadFeature f;
    f.type          = CadFeatureType::Revolve;
    f.name          = name;
    f.sketch_ref    = -1;
    f.entities      = entities;
    f.plane         = plane;
    f.revolve_angle = angle;
    f.revolve_axis  = axis;
    f.flip          = flip;
    f.mode          = mode;
    return append_feature(f);
}

int CadDocument::add_sweep(int profile_sketch_ref, int path_sketch_ref, BooleanMode mode,
                           const std::string& name)
{
    CadFeature f;
    f.type           = CadFeatureType::Sweep;
    f.name           = name;
    f.sketch_ref     = profile_sketch_ref;
    f.sweep_path_ref = path_sketch_ref;
    f.mode           = mode;
    return append_feature(f);
}

int CadDocument::add_loft(const std::vector<int>& profile_refs, bool ruled, BooleanMode mode,
                          const std::string& name)
{
    CadFeature f;
    f.type              = CadFeatureType::Loft;
    f.name              = name;
    f.loft_profile_refs = profile_refs;
    f.loft_ruled        = ruled;
    f.mode              = mode;
    return append_feature(f);
}

int CadDocument::add_pattern(bool circular, int count, double spacing, int dir,
                             double angle_deg, int target_body, const std::string& name)
{
    CadFeature f;
    f.type             = CadFeatureType::Pattern;
    f.name             = name;
    f.pattern_circular = circular;
    f.pattern_count    = count;
    f.pattern_spacing  = spacing;
    f.pattern_dir      = dir;
    f.pattern_angle    = angle_deg;
    f.pattern_inclusive = true;
    f.target_body      = target_body;
    // The pattern's plane — its linear axes and the circular pattern's pivot — defaults to the
    // XY plane through the MODELING origin (the bed centre), like every other default plane in
    // the tab. Left at SketchPlane::XY() it pivoted about the bed's corner.
    f.plane            = SketchPlane::XY();
    f.plane.origin    += modeling_origin;
    return append_feature(f);
}

int CadDocument::add_pattern_on_curve(int count, int curve_sketch, int curve_entity,
                                       int target, const std::string& name)
{
    CadFeature f;
    f.type                 = CadFeatureType::Pattern;
    f.name                 = name;
    f.pattern_count        = count;
    f.pattern_curve_sketch = curve_sketch;
    f.pattern_curve_entity = curve_entity;
    f.target_body          = target;
    f.pattern_circular     = false;
    return append_feature(f);
}

int CadDocument::add_shell(double thickness, int face, int target_body, const std::string& name)
{
    CadFeature f;
    f.type            = CadFeatureType::Shell;
    f.name            = name;
    f.shell_thickness = thickness;
    f.shell_face      = face;
    f.target_body     = target_body;
    return append_feature(f);
}

int CadDocument::add_rib(int sketch_ref, int entity, double thickness, double depth,
                          int target_body, const std::string& name)
{
    CadFeature f;
    f.type           = CadFeatureType::Rib;
    f.name           = name;
    f.rib_sketch_ref = sketch_ref;
    f.rib_entity     = entity;
    f.rib_thickness  = thickness;
    f.rib_depth      = depth;
    f.target_body    = target_body;
    return append_feature(f);
}

int CadDocument::add_delete_face(int target_body, const std::vector<int>& faces,
                                  const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::DeleteFace;
    f.name         = name;
    f.target_body  = target_body;
    f.delete_faces = faces;
    return append_feature(f);
}

int CadDocument::add_draft(double angle, int face, int target_body, const std::string& name)
{
    CadFeature f;
    f.type        = CadFeatureType::Draft;
    f.name        = name;
    f.draft_angle = angle;
    f.draft_face  = face;
    f.target_body = target_body;
    return append_feature(f);
}

int CadDocument::add_boolean(BooleanMode op, int target_body, int tool_body, bool keep_tool,
                             double tolerance, int target_face, int tool_face,
                             const std::string& name)
{
    CadFeature f;
    f.type             = CadFeatureType::Boolean;
    f.name             = name;
    f.mode             = op;
    f.target_body      = target_body;
    f.bool_tool_body   = tool_body;
    f.bool_keep_tool   = keep_tool;
    f.bool_tolerance   = tolerance;
    f.bool_target_face = target_face;
    f.bool_tool_face   = tool_face;
    return append_feature(f);
}

int CadDocument::add_cut(const SketchPlane& plane, double offset, bool flip,
                         bool keep_upper, bool keep_lower, int target_body,
                         const std::string& name)
{
    CadFeature f;
    f.type           = CadFeatureType::Cut;
    f.name           = name;
    f.plane          = plane;
    f.cut_offset     = offset;
    f.cut_flip       = flip;
    f.cut_keep_upper = keep_upper;
    f.cut_keep_lower = keep_lower;
    f.target_body    = target_body;
    return append_feature(f);
}

int CadDocument::add_split_by_face(int target_body, int face_body, int face,
                                    bool keep_upper, bool keep_lower, const std::string& name)
{
    CadFeature f;
    f.type           = CadFeatureType::Cut;
    f.name           = name;
    f.target_body    = target_body;
    f.cut_face_body  = face_body;
    f.cut_face       = face;
    f.cut_keep_upper = keep_upper;
    f.cut_keep_lower = keep_lower;
    return append_feature(f);
}

int CadDocument::add_mirror(const SketchPlane& plane, int target_body, BooleanMode mode,
                            const std::string& name)
{
    CadFeature f;
    f.type                 = CadFeatureType::Mirror;
    f.name                 = name;
    f.plane                = plane;
    f.target_body          = target_body;
    f.mode                 = mode;
    return append_feature(f);
}

int CadDocument::add_transform(int target_body, const Vec3d& translate, const Vec3d& axis,
                               const Vec3d& pivot, double angle_deg, bool copy,
                               const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::Transform;
    f.name         = name;
    f.target_body  = target_body;
    f.xf_translate = translate;
    f.xf_axis      = axis;
    f.xf_pivot     = pivot;
    f.xf_angle_deg = angle_deg;
    f.xf_copy      = copy;
    return append_feature(f);
}

int CadDocument::add_thicken(int target_body, int face, double thickness, bool flip,
                              const std::string& name)
{
    CadFeature f;
    f.type             = CadFeatureType::Thicken;
    f.name             = name;
    f.target_body      = target_body;
    f.thicken_face     = face;
    f.thicken_thickness = thickness;
    f.thicken_flip     = flip;
    return append_feature(f);
}

int CadDocument::add_thicken_surface(int target_body, double thickness, bool flip,
                                      const std::string& name)
{
    CadFeature f;
    f.type              = CadFeatureType::ThickenSurface;
    f.name              = name;
    f.target_body       = target_body;
    f.thicken_thickness = thickness;
    f.thicken_flip      = flip;
    f.mode              = BooleanMode::New;
    return append_feature(f);
}

int CadDocument::add_surface_offset(int target_body, double offset, const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::SurfaceOffset;
    f.name         = name;
    f.target_body  = target_body;
    f.plane_offset = offset;
    f.mode         = BooleanMode::New;
    return append_feature(f);
}

static void project_edges_to_entities(const std::vector<TopoDS_Edge>& edges,
                                      const SketchPlane& plane,
                                      bool construction,
                                      std::vector<SketchEntity>& out);

int CadDocument::add_project_edges(int source_body, const std::vector<int>& edge_ids, int face,
                                   const SketchPlane& plane, const std::string& name)
{
    CadFeature f;
    f.type                = CadFeatureType::Project;
    f.name                = name;
    f.project_source_body = source_body;
    f.project_edges       = edge_ids;
    f.project_face        = face;
    f.plane               = plane;
    return append_feature(f);
}

int CadDocument::add_use_sketch(int source_sketch, const std::vector<int>& entities,
                                const SketchPlane& plane, const std::string& name)
{
    CadFeature f;
    f.type                  = CadFeatureType::Project;
    f.name                  = name;
    f.project_from_sketch   = true;
    f.project_source_sketch = source_sketch;
    f.project_sketch_entities = entities;
    f.plane                 = plane;
    return append_feature(f);
}

std::vector<SketchEntity> CadDocument::in_plane_edges(const SketchPlane& plane, double tol) const
{
    std::vector<TopoDS_Edge> flat;
    for (const auto& b : bodies) {
        if (b.shape.IsNull()) continue;
        for (const TopoDS_Edge& e : GeometryEngine::edges_of(b.shape)) {
            bool in = true;
            for (const Vec3d& p : GeometryEngine::sample_edge_world(e))
                if (std::abs((p - plane.origin).dot(plane.normal)) > tol) { in = false; break; }
            if (in) flat.push_back(e);
        }
    }
    std::vector<SketchEntity> out;
    project_edges_to_entities(flat, plane, /*construction=*/true, out);
    return out;
}

int CadDocument::project_edges_into_sketch(int sketch_feature, int source_body,
                                           const std::vector<int>& edge_ids, int face)
{
    if (sketch_feature < 0 || sketch_feature >= int(features.size())) return -1;
    CadFeature& sk = features[sketch_feature];
    if (sk.type != CadFeatureType::Sketch && sk.type != CadFeatureType::Project) return -1;
    if (source_body < 0 || source_body >= int(bodies.size())
        || bodies[source_body].shape.IsNull()) return -1;
    const TopoDS_Shape& shape = bodies[source_body].shape;

    std::vector<TopoDS_Edge> edges;
    if (!edge_ids.empty()) {
        for (int id : edge_ids) {
            TopoDS_Edge e = GeometryEngine::edge_by_index(shape, id);
            if (e.IsNull()) return -1;
            edges.push_back(e);
        }
    } else if (face >= 0) {
        TopoDS_Face fc = GeometryEngine::face_by_index(shape, face);
        if (fc.IsNull()) return -1;
        edges = GeometryEngine::edges_of_face(fc);
    } else {
        edges = GeometryEngine::edges_of(shape);
    }
    if (edges.empty()) return -1;

    const size_t before = sk.entities.size();
    project_edges_to_entities(edges, sk.plane, /*construction=*/true, sk.entities);
    return int(sk.entities.size() - before);
}

int CadDocument::add_bridge(int sketch_ref, int ent_a, int end_a, int ent_b, int end_b,
                             const std::string& name)
{
    (void)name;
    if (sketch_ref < 0 || sketch_ref >= int(features.size())
        || features[sketch_ref].type != CadFeatureType::Sketch)
        throw std::runtime_error("bridge: sketch_ref must refer to a Sketch feature");
    auto& ents = features[sketch_ref].entities;
    if (ent_a < 0 || ent_a >= int(ents.size()) || ent_b < 0 || ent_b >= int(ents.size()))
        throw std::runtime_error("bridge: entity index out of range in sketch");
    SketchEntity br = SketchEngine::make_bridge(ents[ent_a], end_a, ents[ent_b], end_b);
    ents.push_back(br);
    return int(ents.size()) - 1;
}

int CadDocument::add_plane(int base, double offset, double angle_tilt, int axis,
                           const std::string& name)
{
    CadFeature f;
    f.type             = CadFeatureType::Plane;
    f.name             = name;
    f.plane_base       = base;
    f.plane_offset     = offset;
    f.plane_angle_tilt = angle_tilt;
    f.plane_axis       = axis;
    return append_feature(f);
}

int CadDocument::add_axis(AxisType axis_type_, const std::string& name)
{
    CadFeature f;
    f.type      = CadFeatureType::Axis;
    f.name      = name;
    f.axis_type = axis_type_;
    return append_feature(f);
}

int CadDocument::add_coordsys(CoordSysType type, const Vec3d& point, const std::string& name)
{
    CadFeature f;
    f.type           = CadFeatureType::CoordSys;
    f.name           = name;
    f.coordsys_type  = type;
    f.coordsys_point = point;
    return append_feature(f);
}

int CadDocument::add_mate(int kind, int cs_a, int cs_b, double offset, double angle_deg, bool flip,
                           const std::string& name)
{
    CadFeature f;
    f.type       = CadFeatureType::Mate;
    f.name       = name;
    f.mate_kind  = kind;
    f.mate_cs_a  = cs_a;
    f.mate_cs_b  = cs_b;
    f.mate_offset = offset;
    f.mate_angle  = angle_deg;
    f.mate_flip   = flip;
    return append_feature(f);
}

std::vector<CadDocument::MateOption> CadDocument::mate_options(int cs_a, int cs_b) const
{
    std::vector<MateOption> out(5);
    for (int k = 0; k < 5; ++k) out[k].kind = k;

    auto is_connector = [&](int idx) -> bool {
        return idx >= 0 && idx < int(features.size()) &&
               features[idx].type == CadFeatureType::CoordSys &&
               features[idx].enabled;
    };
    if (!is_connector(cs_a) || !is_connector(cs_b)) {
        const char side = is_connector(cs_a) ? 'B' : 'A';
        const std::string reason = format(_u8L("connector %1% is not a coordinate system"), side);
        for (auto& o : out) { o.viable = false; o.reason = reason; }
        return out;
    }
    if (cs_a == cs_b) {
        for (auto& o : out) { o.viable = false; o.reason = _u8L("a mate needs two different connectors"); }
        return out;
    }

    // B is the connector on the body that MOVES, so B must belong to one; A is the fixed
    // reference and needs no body. Without this every kind was offered on a Point(world)
    // connector and the mate only failed at RECOMPUTE, with "mate_cs_b has no associated body" —
    // one step too late, after the feature already existed in the tree. The same two conditions
    // the apply path throws on are checked here, so the offer and the kernel cannot disagree.
    const int body_b = features[cs_b].coordsys_body;
    if (body_b < 0) {
        for (auto& o : out) {
            o.viable = false;
            o.reason = _u8L("connector B is not attached to a body — a mate moves B's body");
        }
        return out;
    }
    if (body_b >= int(bodies.size()) || bodies[body_b].shape.IsNull()) {
        for (auto& o : out) {
            o.viable = false;
            o.reason = _u8L("connector B's body no longer exists");
        }
        return out;
    }

    const int ka = features[cs_a].coordsys_face_kind;
    const int kb = features[cs_b].coordsys_face_kind;

    // Planar: needs a flat face at both ends.
    const bool plan_bad_a = ka >= 0 && ka != GeomAbs_Plane;
    const bool plan_bad_b = kb >= 0 && kb != GeomAbs_Plane;
    if (plan_bad_a || plan_bad_b) {
        out[1].viable = false;
        if (plan_bad_a && plan_bad_b)
            out[1].reason = _u8L("needs a flat face at both ends — both connectors are on curved faces");
        else
            out[1].reason = format(_u8L("needs a flat face at both ends — connector %1% is on a curved face"),
                                   plan_bad_a ? 'A' : 'B');
    }

    // Revolute and Cylindrical: need a cylindrical face at both ends.
    for (int k : {2, 4}) {
        const bool ax_bad_a = ka >= 0 && ka != GeomAbs_Cylinder;
        const bool ax_bad_b = kb >= 0 && kb != GeomAbs_Cylinder;
        if (!ax_bad_a && !ax_bad_b) continue;
        out[k].viable = false;
        if (ax_bad_a && ax_bad_b)
            out[k].reason = (ka == GeomAbs_Plane && kb == GeomAbs_Plane)
                ? _u8L("needs a cylindrical face at both ends — both connectors are on flat faces")
                : _u8L("needs a cylindrical face at both ends — both connectors are on non-cylindrical faces");
        else
            out[k].reason = format((ax_bad_a ? ka : kb) == GeomAbs_Plane
                                       ? _u8L("needs a cylindrical face at both ends — connector %1% is on a flat face")
                                       : _u8L("needs a cylindrical face at both ends — connector %1% is on a curved face"),
                                   ax_bad_a ? 'A' : 'B');
    }

    return out;
}

int CadDocument::add_sketch3d(const std::vector<Vec3d>& points, bool smooth, bool closed, const std::string& name)
{
    CadFeature f;
    f.type       = CadFeatureType::Sketch3D;
    f.name       = name;
    f.sk3_points = points;
    f.sk3_smooth = smooth;
    f.sk3_closed = closed;
    return append_feature(f);
}

TopoDS_Wire CadDocument::build_sketch3d_wire(const CadFeature& f, std::string& err)
{
    err.clear();
    // Drop repeated points: a zero-length leg is not an edge, and OCCT's polygon builder fails on it.
    std::vector<gp_Pnt> pts;
    for (const Vec3d& p : f.sk3_points) {
        const gp_Pnt g(p.x(), p.y(), p.z());
        if (pts.empty() || pts.back().Distance(g) > 1e-6) pts.push_back(g);
    }
    if (f.sk3_closed && pts.size() > 2 && pts.front().Distance(pts.back()) < 1e-6) pts.pop_back();
    if (pts.size() < 2) { err = _u8L("a 3D sketch needs at least two distinct points"); return TopoDS_Wire(); }
    try {
        if (f.sk3_smooth && pts.size() >= 3) {
            Handle(TColgp_HArray1OfPnt) arr = new TColgp_HArray1OfPnt(1, int(pts.size()));
            for (int i = 0; i < int(pts.size()); ++i) arr->SetValue(i + 1, pts[i]);
            GeomAPI_Interpolate interp(arr, f.sk3_closed ? Standard_True : Standard_False, 1e-6);
            interp.Perform();
            if (!interp.IsDone()) { err = _u8L("the 3D sketch spline could not be interpolated"); return TopoDS_Wire(); }
            TopoDS_Edge e = BRepBuilderAPI_MakeEdge(interp.Curve()).Edge();
            return BRepBuilderAPI_MakeWire(e).Wire();
        }
        BRepBuilderAPI_MakePolygon poly;
        for (const gp_Pnt& p : pts) poly.Add(p);
        if (f.sk3_closed && pts.size() >= 3) poly.Close();
        if (!poly.IsDone()) { err = _u8L("the 3D sketch polyline could not be built"); return TopoDS_Wire(); }
        return poly.Wire();
    } catch (const Standard_Failure& ex) {
        err = std::string(_u8L("the 3D sketch could not be built: ")) + (*ex.GetMessageString() ? ex.GetMessageString() : "OCCT failure");
        return TopoDS_Wire();
    }
}

int CadDocument::add_helix(const SketchPlane& plane, double radius, double pitch, double height,
                           bool left_handed, double taper_deg, const std::string& name)
{
    CadFeature f;
    f.type              = CadFeatureType::Helix;
    f.name              = name;
    f.plane             = plane;
    f.helix_radius      = radius;
    f.helix_pitch       = pitch;
    f.helix_height      = height;
    f.helix_left_handed = left_handed;
    f.helix_taper_deg   = taper_deg;
    return append_feature(f);
}

TopoDS_Wire CadDocument::build_helix_wire(const CadFeature& f, std::string& err) const
{
    return make_helix_spine(f, err);
}

// Derive a SketchPlane: shift `base` along its normal by `offset`, then tilt
// `angle_deg` about the base's X (axis 0) or Y (axis 1) axis (Rodrigues rotation).
static SketchPlane offset_angle_plane(const SketchPlane& base, double offset,
                                      double angle_deg, int axis)
{
    SketchPlane p;
    p.origin = base.origin + base.normal * offset;
    Vec3d n = base.normal, x = base.x_axis, y = base.y_axis;
    if (std::abs(angle_deg) > 1e-9) {
        const double a = angle_deg * M_PI / 180.0;
        const Vec3d  k = (axis == 1) ? base.y_axis : base.x_axis; // unit rotation axis
        auto rot = [&](const Vec3d& v) -> Vec3d {   // -> Vec3d forces eval (no dangling Eigen expr)
            return v * std::cos(a) + k.cross(v) * std::sin(a)
                   + k * (k.dot(v)) * (1.0 - std::cos(a));
        };
        n = rot(n);
        if (axis == 1) x = rot(x);   // tilt about Y rotates X + normal, Y fixed
        else           y = rot(y);   // tilt about X rotates Y + normal, X fixed
    }
    p.normal = n.normalized();
    p.x_axis = x.normalized();
    p.y_axis = y.normalized();
    return p;
}

// Build a full orthonormal frame from a normal + origin.
static SketchPlane frame_from(const Vec3d& origin, const Vec3d& normal)
{
    Vec3d n = normal.normalized();
    Vec3d ref = (std::abs(n.z()) < 0.9) ? Vec3d(0, 0, 1) : Vec3d(1, 0, 0);
    Vec3d x = ref.cross(n);
    if (x.squaredNorm() < 1e-12) x = Vec3d(1, 0, 0);
    x.normalize();
    Vec3d y = n.cross(x).normalized();
    SketchPlane p;
    p.origin = origin;
    p.normal = n;
    p.x_axis = x;
    p.y_axis = y;
    return p;
}

bool CadDocument::plane_of_face(int body_idx, int face_idx, SketchPlane& out) const
{
    if (face_idx < 0 || body_idx < 0 || body_idx >= int(bodies.size())) return false;
    const TopoDS_Face f = GeometryEngine::face_by_index(bodies[body_idx].shape, face_idx);
    if (f.IsNull()) return false;
    // Planar only. face_normal_world evaluates the normal at the mid parameter, which on a cylinder
    // or a fillet is a tangent plane at one arbitrary point — usable for a datum offset, wrong as a
    // sketch plane. Refuse rather than sketch somewhere the user did not point at.
    BRepAdaptor_Surface surf(f);
    if (surf.GetType() != GeomAbs_Plane) return false;
    out = frame_from(GeometryEngine::face_centroid_world(f), GeometryEngine::face_normal_world(f));
    return true;
}

SketchPlane CadDocument::carried_plane(int fi) const
{
    if (fi < 0 || fi >= int(features.size())) return SketchPlane();
    const CadFeature& f = features[fi];
    SketchPlane p = f.plane;
    if (!attached_follow_moves || f.attach_body < 0) return p;
    for (int j = fi + 1; j < int(features.size()); ++j) {
        if (rollback_index >= 0 && j > rollback_index) break;
        const CadFeature& t = features[j];
        if (!t.enabled || t.type != CadFeatureType::Transform || t.xf_copy) continue;
        const int tb = t.target_body == -1 ? int(bodies.size()) - 1 : t.target_body;
        if (tb != f.attach_body) continue;
        // the same motion apply_transform gives the body: rotate about the pivot first, then translate
        Eigen::Matrix3d R = Eigen::Matrix3d::Identity();
        if (std::abs(t.xf_angle_deg) > 1e-12 && t.xf_axis.norm() > 1e-9)
            R = Eigen::AngleAxisd(t.xf_angle_deg * M_PI / 180.0, t.xf_axis.normalized()).toRotationMatrix();
        p.origin = R * (p.origin - t.xf_pivot) + t.xf_pivot + t.xf_translate;
        p.normal = R * p.normal; p.x_axis = R * p.x_axis; p.y_axis = R * p.y_axis;
    }
    return p;
}

std::vector<std::pair<std::string, SketchPlane>> CadDocument::resolve_datum_planes() const
{
    std::vector<std::pair<std::string, SketchPlane>> out;
    for (const CadFeature& f : features) {
        if (f.type != CadFeatureType::Plane || !f.enabled) continue;

        // Resolve base reference plane. The default XY/XZ/YZ planes pass through the modeling
        // origin (bed centre); datum bases (>=3) are already in world coords from earlier passes.
        SketchPlane base;
        bool base_missing = false;
        if      (f.plane_base == 1) { base = SketchPlane::XZ(); base.origin += modeling_origin; }
        else if (f.plane_base == 2) { base = SketchPlane::YZ(); base.origin += modeling_origin; }
        else if (f.plane_base >= 3) {
            const int di = f.plane_base - 3;
            if (di < int(out.size())) base = out[di].second;
            else {
                // The datum it was built on is gone or hidden. Fall back to XY through the
                // modeling origin like the other defaults, and SAY so in the plane's name —
                // this list is what the GUI shows — instead of quietly becoming world XY.
                base = SketchPlane::XY(); base.origin += modeling_origin;
                base_missing = true;
            }
        }
        else                        { base = SketchPlane::XY(); base.origin += modeling_origin; }

        // --- Resolve refs from bodies ---
        auto resolve_face = [&](int body_idx, int face_idx) -> TopoDS_Face {
            if (face_idx < 0 || body_idx < 0 || body_idx >= int(bodies.size()))
                return TopoDS_Face();
            return GeometryEngine::face_by_index(bodies[body_idx].shape, face_idx);
        };
        auto resolve_edge = [&](int body_idx, int edge_idx,
                                 Vec3d& p0, Vec3d& dir) -> bool {
            if (edge_idx < 0 || body_idx < 0 || body_idx >= int(bodies.size()))
                return false;
            TopoDS_Edge e = GeometryEngine::edge_by_index(bodies[body_idx].shape, edge_idx);
            if (e.IsNull()) return false;
            auto pts = GeometryEngine::sample_edge_world(e);
            if (pts.size() < 2) return false;
            p0 = pts.front();
            dir = (pts.back() - pts.front()).normalized();
            return true;
        };

        // Face A
        TopoDS_Face faceA = resolve_face(f.plane_face_body, f.plane_face);
        SketchPlane faceA_plane;
        bool has_faceA = false;
        if (!faceA.IsNull()) {
            faceA_plane = frame_from(
                GeometryEngine::face_centroid_world(faceA),
                GeometryEngine::face_normal_world(faceA));
            has_faceA = true;
        }

        // Face B
        TopoDS_Face faceB = resolve_face(f.plane_face2_body, f.plane_face2);
        SketchPlane faceB_plane;
        bool has_faceB = false;
        if (!faceB.IsNull()) {
            faceB_plane = frame_from(
                GeometryEngine::face_centroid_world(faceB),
                GeometryEngine::face_normal_world(faceB));
            has_faceB = true;
        }

        // Edge A
        Vec3d eA_p0, eA_dir;
        bool has_edgeA = resolve_edge(f.plane_edge_body, f.plane_edge, eA_p0, eA_dir);

        // Edge B
        Vec3d eB_p0, eB_dir;
        bool has_edgeB = resolve_edge(f.plane_edge2_body, f.plane_edge2, eB_p0, eB_dir);

        // --- Dispatch on plane_type ---
        auto fallback_offset = [&]() {
            return offset_angle_plane(base, f.plane_offset, f.plane_angle_tilt, f.plane_axis);
        };

        SketchPlane result;
        switch (f.plane_type) {

        case PlaneType::Offset: {
            // From a picked face: pure offset along its normal. From a base/datum plane:
            // offset + the legacy tilt-about-axis (keeps the old Offset/Tilt controls live).
            if (has_faceA)
                result = frame_from(faceA_plane.origin + faceA_plane.normal * f.plane_offset,
                                    faceA_plane.normal);
            else
                result = offset_angle_plane(base, f.plane_offset, f.plane_angle_tilt, f.plane_axis);
            break;
        }

        case PlaneType::Coincident: {
            result = has_faceA ? faceA_plane : base;
            break;
        }

        case PlaneType::Angle: {
            if (!has_edgeA) { result = fallback_offset(); break; }
            const SketchPlane& ref = has_faceA ? faceA_plane : base;
            Vec3d n0 = ref.normal - eA_dir * ref.normal.dot(eA_dir);
            if (n0.squaredNorm() < 1e-12) {
                Vec3d perp = (std::abs(eA_dir.z()) < 0.9) ? Vec3d(0, 0, 1) : Vec3d(1, 0, 0);
                n0 = perp - eA_dir * perp.dot(eA_dir);
            }
            n0.normalize();
            const double a = f.plane_angle_tilt * M_PI / 180.0;
            Vec3d n_rot = n0 * std::cos(a) + eA_dir.cross(n0) * std::sin(a)
                          + eA_dir * (eA_dir.dot(n0)) * (1.0 - std::cos(a));
            result = frame_from(eA_p0, n_rot);
            break;
        }

        case PlaneType::Midplane: {
            if (!has_faceA || !has_faceB) { result = fallback_offset(); break; }
            Vec3d origin = 0.5 * (faceA_plane.origin + faceB_plane.origin);
            Vec3d nB = (faceA_plane.normal.dot(faceB_plane.normal) >= 0)
                           ? faceB_plane.normal : -faceB_plane.normal;
            Vec3d normal = (faceA_plane.normal + nB).normalized();
            result = frame_from(origin, normal);
            break;
        }

        case PlaneType::Tangent: {
            if (!has_faceA) { result = fallback_offset(); break; }
            GeometryEngine::CylinderFace cyl = GeometryEngine::cylinder_of_face(faceA);
            if (!cyl.ok) { result = fallback_offset(); break; }
            Vec3d refdir = (std::abs(cyl.axis.z()) < 0.9) ? Vec3d(0, 0, 1) : Vec3d(1, 0, 0);
            refdir = refdir - cyl.axis * refdir.dot(cyl.axis);
            refdir.normalize();
            const double theta = f.plane_angle_tilt * M_PI / 180.0;
            Vec3d r = refdir * std::cos(theta) + cyl.axis.cross(refdir) * std::sin(theta);
            Vec3d touch = cyl.base + r * cyl.radius;
            result = frame_from(touch, r);
            break;
        }

        case PlaneType::TwoEdges: {
            if (!has_edgeA) { result = fallback_offset(); break; }
            if (!has_edgeB) { result = fallback_offset(); break; }
            Vec3d cross = eA_dir.cross(eB_dir);
            if (cross.squaredNorm() > 1e-12) {
                result = frame_from(eA_p0, cross.normalized());
            } else {
                Vec3d v = eA_dir.cross(eB_p0 - eA_p0);
                if (v.squaredNorm() > 1e-12) {
                    result = frame_from(eA_p0, v.normalized());
                } else {
                    result = fallback_offset();
                }
            }
            break;
        }

        }

        // TRN Datum plane entry in the reference plane list. %1% is the plane name.
        out.emplace_back(base_missing ? format(_u8L("%1% (base plane missing)"), f.name) : f.name, result);
    }
    return out;
}

// Resolved datum axes in feature order. Origin + unit direction computed from
// construction params; axis_err is non-empty when construction fails (no crash).
std::vector<CadDocument::DatumAxis> CadDocument::resolve_datum_axes() const
{
    std::vector<DatumAxis> out;
    // For PlaneIntersection we need the already-resolved datum planes.
    std::vector<std::pair<std::string, SketchPlane>> datum_planes = resolve_datum_planes();

    auto resolve_face = [&](int body_idx, int face_idx) -> TopoDS_Face {
        if (face_idx < 0 || body_idx < 0 || body_idx >= int(bodies.size()))
            return TopoDS_Face();
        return GeometryEngine::face_by_index(bodies[body_idx].shape, face_idx);
    };
    auto resolve_edge = [&](int body_idx, int edge_idx,
                             Vec3d& p0, Vec3d& dir) -> bool {
        if (edge_idx < 0 || body_idx < 0 || body_idx >= int(bodies.size()))
            return false;
        TopoDS_Edge e = GeometryEngine::edge_by_index(bodies[body_idx].shape, edge_idx);
        if (e.IsNull()) return false;
        auto pts = GeometryEngine::sample_edge_world(e);
        if (pts.size() < 2) return false;
        p0 = pts.front();
        dir = (pts.back() - pts.front()).normalized();
        return true;
    };

    for (const CadFeature& f : features) {
        if (f.type != CadFeatureType::Axis || !f.enabled) continue;

        DatumAxis da;
        da.name = f.name;
        switch (f.axis_type) {
        case AxisType::TwoPoints: {
            Vec3d dir = f.axis_p2 - f.axis_p1;
            if (dir.squaredNorm() < 1e-18) { da.error = _u8L("two points are coincident"); break; }
            da.origin    = f.axis_p1;
            da.direction = dir.normalized();
            break;
        }
        case AxisType::FaceNormal: {
            TopoDS_Face fc = resolve_face(f.axis_body, f.axis_face);
            if (fc.IsNull()) { da.error = _u8L("face not found"); break; }
            da.origin    = GeometryEngine::face_centroid_world(fc);
            da.direction = GeometryEngine::face_normal_world(fc);
            break;
        }
        case AxisType::CylinderCenterline: {
            TopoDS_Face fc = resolve_face(f.axis_body, f.axis_face);
            if (fc.IsNull()) { da.error = _u8L("face not found"); break; }
            GeometryEngine::CylinderFace cyl = GeometryEngine::cylinder_of_face(fc);
            if (!cyl.ok) { da.error = _u8L("face is not a cylinder"); break; }
            da.origin    = cyl.base;
            da.direction = cyl.axis;
            break;
        }
        case AxisType::AlongEdge: {
            Vec3d p0, dir;
            if (!resolve_edge(f.axis_body, f.axis_edge, p0, dir)) {
                da.error = _u8L("edge not found"); break;
            }
            da.origin    = p0;
            da.direction = dir;
            break;
        }
        case AxisType::PlaneIntersection: {
            // Same reference encoding as CadFeature::plane_base, because the GUI fills these
            // two fields from populate_plane_choices() — whose rows are XY/XZ/YZ followed by
            // the datum planes — and stores the row verbatim. Indexing datum_planes[ref]
            // directly, as this did, is off by three: picking XY resolved to datum plane 0 and
            // picking the first datum ran off the end with "plane ref not found", so the
            // PlaneIntersection axis type could not work at all. Two base planes are also a
            // perfectly ordinary way to define an axis (XY x XZ = the X axis), so they must
            // resolve rather than be rejected as out of scope.
            auto base_plane = [&](int ref, Vec3d& origin, Vec3d& normal) -> bool {
                SketchPlane p;
                if      (ref == 0) { p = SketchPlane::XY(); p.origin += modeling_origin; }
                else if (ref == 1) { p = SketchPlane::XZ(); p.origin += modeling_origin; }
                else if (ref == 2) { p = SketchPlane::YZ(); p.origin += modeling_origin; }
                else if (ref >= 3 && ref - 3 < int(datum_planes.size()))
                    p = datum_planes[ref - 3].second;      // datums are already world-space
                else
                    return false;
                origin = p.origin;
                normal = p.normal;
                return true;
            };
            bool ok0 = base_plane(f.axis_plane_a, da.origin, da.direction); // direction reused as normal0
            Vec3d origin1, normal1;
            bool ok1 = base_plane(f.axis_plane_b, origin1, normal1);
            if (!ok0 || !ok1) { da.error = _u8L("plane ref not found"); break; }
            // Direction = cross product of the two plane normals.
            Vec3d dir = da.direction.cross(normal1); // da.direction was normal0
            if (dir.squaredNorm() < 1e-18) {
                da.error = _u8L("planes are parallel (no intersection)"); break;
            }
            dir.normalize();
            // Find a point on the intersection line: closest points between two planes.
            // Project origin of plane A onto the intersection line.
            Vec3d n0 = da.direction;   // normal of plane A (stored temporarily)
            Vec3d n1 = normal1;
            Vec3d p0 = da.origin;
            Vec3d p1 = origin1;
            // Solve for point on line of intersection using vector formula.
            double d0 = n0.dot(p0);
            double d1 = n1.dot(p1);
            double n0n1 = n0.dot(n1);
            double det = 1.0 - n0n1 * n0n1;
            if (std::abs(det) < 1e-18) { da.error = _u8L("planes are parallel (no intersection)"); break; }
            double t0 = (d0 - d1 * n0n1) / det;
            double t1 = (d1 - d0 * n0n1) / det;
            da.origin    = n0 * t0 + n1 * t1;
            da.direction = dir;
            break;
        }
        }
        out.push_back(da);
    }
    return out;
}

CadDocument::DatumCoordSys CadDocument::datum_frame(const std::vector<CadBody>& bodies, const CadFeature& f)
{
    CadDocument::DatumCoordSys ds;
    ds.name = f.name;

    auto resolve_face = [&](int body_idx, int face_idx) -> TopoDS_Face {
        if (face_idx < 0 || body_idx < 0 || body_idx >= int(bodies.size()))
            return TopoDS_Face();
        return GeometryEngine::face_by_index(bodies[body_idx].shape, face_idx);
    };
    auto resolve_edge = [&](int body_idx, int edge_idx,
                             Vec3d& p0, Vec3d& dir) -> bool {
        if (edge_idx < 0 || body_idx < 0 || body_idx >= int(bodies.size()))
            return false;
        TopoDS_Edge e = GeometryEngine::edge_by_index(bodies[body_idx].shape, edge_idx);
        if (e.IsNull()) return false;
        auto pts = GeometryEngine::sample_edge_world(e);
        if (pts.size() < 2) return false;
        p0  = pts.front();
        dir = (pts.back() - pts.front()).normalized();
        return true;
    };

    switch (f.coordsys_type) {
    case CoordSysType::PointWorld: {
        ds.origin = f.coordsys_point;
        ds.x      = Vec3d(1, 0, 0);
        ds.y      = Vec3d(0, 1, 0);
        break;
    }
    case CoordSysType::FaceAndDirection: {
        TopoDS_Face fc = resolve_face(f.coordsys_body, f.coordsys_face);
        Vec3d p0, edge_dir;
        bool have_edge = resolve_edge(f.coordsys_body, f.coordsys_edge, p0, edge_dir);
        if (fc.IsNull()) { ds.error = _u8L("face not found"); break; }
        ds.origin = GeometryEngine::face_centroid_world(fc);
        Vec3d Z = GeometryEngine::face_normal_world(fc);
        // Tentative X: an explicit edge reference wins. Failing that, derive X from the
        // face's OWN first usable edge, so the frame rotates with the body. Reading it from
        // coordsys_x_hint — a world constant — meant a face-only connector could not encode
        // spin about its own normal: Z followed the body, X did not, so Fastened and Slider
        // claimed to fix an orientation the frame could not see. The hint survives only as a
        // last resort, for faces that offer no usable direction (a full circular edge has
        // coincident endpoints, and a cylinder's seam projects to nothing in-plane).
        Vec3d X_tent = Vec3d::Zero();
        if (have_edge) {
            X_tent = edge_dir;
        } else {
            for (const TopoDS_Edge& fe : GeometryEngine::edges_of_face(fc)) {
                auto pts = GeometryEngine::sample_edge_world(fe);
                if (pts.size() < 2) continue;
                Vec3d d = pts.back() - pts.front();
                if (d.squaredNorm() < 1e-18) continue;      // closed edge: endpoints coincide
                Vec3d in_plane = d - Z * Z.dot(d);          // drop any out-of-plane component
                if (in_plane.squaredNorm() < 1e-18) continue;
                X_tent = in_plane;
                break;
            }
            if (X_tent.squaredNorm() < 1e-18) X_tent = f.coordsys_x_hint;
        }
        if (X_tent.squaredNorm() < 1e-18) { ds.error = _u8L("zero-length direction"); break; }
        X_tent.normalize();
        // Gram-Schmidt: ensure orthonormal, right-handed frame.
        // Y = Z x X_tent,  X = Y x Z  (this makes X perpendicular to Z, not X_tent)
        Vec3d Y = Z.cross(X_tent);
        if (Y.squaredNorm() < 1e-12) {
            // Edge/hint is parallel to Z -> X is degenerate; fall back to world X/Y orthonormalised.
            Vec3d ref = (std::abs(Z.z()) < 0.9) ? Vec3d(0, 0, 1) : Vec3d(1, 0, 0);
            Y = Z.cross(ref);
            if (Y.squaredNorm() < 1e-12) Y = Z.cross(Vec3d(0, 1, 0));
        }
        Y.normalize();
        ds.x = Y.cross(Z).normalized();
        ds.y = Y;
        break;
    }
    }
    return ds;
}

std::vector<CadDocument::DatumCoordSys> CadDocument::resolve_datum_coordsys() const
{
    std::vector<DatumCoordSys> out;
    for (const CadFeature& f : features) {
        if (f.type != CadFeatureType::CoordSys || !f.enabled) continue;
        out.push_back(datum_frame(bodies, f));
    }
    return out;
}

void CadDocument::clear()
{
    features.clear();
    body = TopoDS_Shape();
    bodies.clear();                // multibody result — must clear too (else solids linger)
    display_mesh = TriangleMesh{};
    display_body_meshes.clear();
    display_tri_face.clear();
    display_tri_body.clear();
    origin_from_recipe = false;
    auto_close_loops   = true;
    // Variables are document state like the features: left behind, the previous project's
    // variables were written into the next project's recipe.
    variables.clear();
    error.clear();
    mate_conflicts.clear();
    ++topo_generation;       // every face/edge id handed out before this is stale now
    // A cleared document is a fresh start with no history.
    m_undo.clear();
    m_redo.clear();
}

void CadDocument::checkpoint()
{
    m_bar = rollback_index;   // an ADD still honours the bar (append_feature); everything else acts on the full history
    rollback_index = -1;
    m_undo.push_back(Snapshot{features, variables});   // snapshot the pre-mutation recipe
    m_redo.clear();                                    // any new action invalidates the redo branch
    if (m_undo.size() > k_undo_cap)
        m_undo.erase(m_undo.begin());
}

void CadDocument::abandon_checkpoint()
{
    if (!m_undo.empty())
        m_undo.pop_back();
}

bool CadDocument::undo()
{
    if (m_undo.empty())
        return false;
    rollback_index = -1;
    m_redo.push_back(Snapshot{std::move(features), std::move(variables)});   // current state becomes redoable
    features  = std::move(m_undo.back().features);
    variables = std::move(m_undo.back().variables);
    m_undo.pop_back();
    recompute();   // benign-empty (only a sketch / empty doc) is a valid undo target
    return true;
}

bool CadDocument::redo()
{
    if (m_redo.empty())
        return false;
    rollback_index = -1;
    m_undo.push_back(Snapshot{std::move(features), std::move(variables)});
    features  = std::move(m_redo.back().features);
    variables = std::move(m_redo.back().variables);
    m_redo.pop_back();
    recompute();
    return true;
}


// A feature's body index: -1 means "the last body", anything else must exist. An index past
// the end used to fall back to the last body in silence, so a feature whose body had gone away
// went on to work on a different one.
static int pick_body(int ref, int nb)
{
    if (ref == -1) return nb - 1;
    if (ref < 0 || ref >= nb)
        throw std::runtime_error(format(_u8L("a feature works on body %1%, which does not exist at that point in the history"),
                                        ref + 1));
    return ref;
}

// ---- body and datum-plane references across a change of history ---------------------------
//
// A feature names a body by its INDEX and a datum plane by its ORDINAL (3 + N = the Nth enabled
// Plane feature). Both are positions, and a position only means something against the history
// it was taken in: delete, reorder or hide a feature that makes a body or a datum plane and every
// later reference silently lands on a different one. The fix-up for features[] indices
// (for_each_feature_ref) never covered these two bases.

// Every field holding a BODY index, and which list it indexes: `true` = the bodies as they stand
// while the feature is applied (recompute's running list), `false` = the finished list (datums
// are resolved against it afterwards).
template<class Visit>
static void for_each_body_ref(CadFeature& f, Visit&& visit)
{
    visit(f.target_body,         true);
    visit(f.bool_tool_body,      true);
    visit(f.cut_face_body,       true);
    visit(f.project_source_body, true);
    visit(f.coordsys_body,       false);
    visit(f.axis_body,           false);
    visit(f.plane_face_body,     false);
    visit(f.plane_face2_body,    false);
    visit(f.plane_edge_body,     false);
    visit(f.plane_edge2_body,    false);
}
static constexpr int kBodyRefCount = 10;
// A body reference whose body was made by a feature that has since been deleted or hidden.
static constexpr int kBodyGone = -1000;

// Identity of bodies[i]: the feature that made it, and which of that feature's bodies it is.
static CadFeature::BodyId body_id_of(const std::vector<CadBody>& v, int i)
{
    CadFeature::BodyId id;
    if (i < 0 || i >= int(v.size())) return id;
    id.src = v[i].source_feature;
    for (int j = 0; j < i; ++j)
        if (v[j].source_feature == id.src) ++id.ord;
    return id;
}
static int find_body(const std::vector<CadBody>& v, const CadFeature::BodyId& id)
{
    int ord = 0;
    for (int i = 0; i < int(v.size()); ++i)
        if (v[i].source_feature == id.src && ord++ == id.ord) return i;
    return -1;
}

// Re-point `f`'s body fields of one kind (`running`) from the identities recorded last time,
// then record the identities they resolve to now. Throws when a pending reference cannot be
// found: the body it named is gone, and working on some other body instead is the bug this
// exists to prevent.
static void settle_body_refs(CadFeature& f, const std::vector<CadBody>& v, bool running)
{
    if (f.body_ref_ids.size() != size_t(kBodyRefCount)) f.body_ref_ids.assign(kBodyRefCount, {});
    int k = 0;
    for_each_body_ref(f, [&](int& ref, bool kind) {
        CadFeature::BodyId& id = f.body_ref_ids[k++];
        if (kind != running) return;
        if (ref == kBodyGone)
            throw std::runtime_error(format(_u8L("\"%1%\" works on a body made by a feature that was deleted or hidden"), f.name));
        if (f.body_refs_pending && id.src >= 0) {
            const int i = find_body(v, id);
            if (i < 0)
                throw std::runtime_error(format(_u8L("\"%1%\" works on a body that does not exist at that point in the history"), f.name));
            ref = i;
        }
        id = ref >= 0 ? body_id_of(v, ref) : CadFeature::BodyId{};
    });
}

// Datum-plane ordinals: plane_base, axis_plane_a, axis_plane_b hold 3 + N.
template<class Visit>
static void for_each_datum_ref(CadFeature& f, Visit&& visit)
{
    visit(f.plane_base);
    visit(f.axis_plane_a);
    visit(f.axis_plane_b);
}

// Prepare every reference for a change of history in which feature `i` ends up at
// `map_feature(i)` (-1 = removed) and enabled as `enabled_after(i)`. Runs on the OLD history and
// writes into `features` in the old order; the caller then performs the change.
static void stage_history_change(std::vector<CadFeature>& features,
                                 const std::function<int(int)>& map_feature,
                                 const std::function<bool(int)>& enabled_after)
{
    const int n = int(features.size());
    auto alive = [&](int i) { return i >= 0 && i < n && map_feature(i) >= 0 && enabled_after(i); };

    // Datum planes: ordinal -> old feature index, then -> new ordinal.
    std::vector<int> planes_before;
    for (int i = 0; i < n; ++i)
        if (features[i].type == CadFeatureType::Plane && features[i].enabled) planes_before.push_back(i);
    std::vector<std::pair<int, int>> after;   // (new index, old index) of planes enabled after
    for (int i = 0; i < n; ++i)
        if (features[i].type == CadFeatureType::Plane && alive(i)) after.emplace_back(map_feature(i), i);
    std::sort(after.begin(), after.end());
    auto new_ordinal = [&](int old_feature) {
        for (int k = 0; k < int(after.size()); ++k)
            if (after[k].second == old_feature) return k;
        return -1;
    };

    for (int j = 0; j < n; ++j) {
        if (map_feature(j) < 0) continue;
        CadFeature& f = features[j];
        for_each_datum_ref(f, [&](int& ref) {
            if (ref < 3) return;
            const int N = ref - 3;
            const int oldf = (N < int(planes_before.size())) ? planes_before[N] : -1;
            const int nn = oldf >= 0 ? new_ordinal(oldf) : -1;
            // A plane that is gone or hidden stays pointed PAST the end, where resolution
            // reports it, rather than quietly sliding onto the next plane.
            ref = nn >= 0 ? 3 + nn : 3 + int(after.size()) + 1000;
        });
        if (f.body_ref_ids.size() != size_t(kBodyRefCount)) continue;   // never resolved: nothing to follow
        // A feature hidden after the change is not replayed, so a hidden source is no reason to
        // drop its body: it keeps following it and re-points when shown. Shown while the source
        // is still hidden, it is staged as replayed and loses the body then. Only deleting the
        // source drops a hidden feature's body.
        const bool replayed = enabled_after(j);
        int k = 0;
        for_each_body_ref(f, [&](int& ref, bool) {
            CadFeature::BodyId& id = f.body_ref_ids[k++];
            if (ref < 0 || id.src < 0) return;
            if (replayed ? !alive(id.src) : map_feature(id.src) < 0) { ref = kBodyGone; id = {}; return; }
            id.src = map_feature(id.src);
        });
        f.body_refs_pending = true;
    }
}

// Re-run recompute(); if it fails for a GENUINE geometry error, restore `snapshot`
// and recompute that instead, so a rejected edit leaves the document exactly as it
// was. recompute() also returns false for the BENIGN case where the edit simply
// leaves no solid-producing feature (empty document, or only a sketch) — that is a
// valid result of a deletion, not a failure, so we accept it with an empty body.
static bool commit_or_rollback(CadDocument& doc, std::vector<CadFeature>& snapshot)
{
    if (doc.recompute())
        return true;

    bool has_solid_feature = false;
    for (const auto& f : doc.features)
        if (f.enabled && CadDocument::produces_body(f.type)) { has_solid_feature = true; break; }
    if (!has_solid_feature) {
        doc.bodies.clear();
        doc.body         = TopoDS_Shape();
        doc.display_mesh = TriangleMesh{};
        doc.display_body_meshes.clear();
        doc.display_tri_face.clear();
        doc.display_tri_body.clear();
        doc.error.clear();
        return true;
    }

    std::string fail_err = doc.error;   // why the attempted edit failed
    doc.features.swap(snapshot);
    doc.recompute();                    // restore the previous good body (clears error)
    doc.error = fail_err.empty() ? _u8L("feature is used by a later feature")
                                 : fail_err;
    return false;
}

// Visit every field of `f` that holds an index into features[].
//
// Deliberately NOT dispatched on f.type. A type switch is what this replaced, and it was
// wrong in the way type switches go wrong: it listed Extrude and Mate, and every feature
// type added afterwards — Revolve, Sweep, Loft, Rib, Pattern-on-curve, the Surface* family —
// silently inherited a remap that skipped its references. Visiting the FIELDS instead means
// a new type is covered the moment it reuses one of them, and reaching a field its type
// never reads costs nothing: unset refs are -1 and every visitor here ignores those.
//
// The list is exactly the fields documented as "index into features[]" / "feature index" in
// CadDocument.hpp. Not included, because they are a different basis and need their own pass:
// plane_base / axis_plane_a / axis_plane_b encode `3 + N` = the Nth DATUM PLANE, an ordinal
// into resolve_datum_planes(), not into features[]. Everything named *_body / *_face / *_edge
// is a body index or a global topology id and must never be remapped here.
template<class Visit>
static void for_each_feature_ref(CadFeature& f, Visit&& visit)
{
    // The list of ref fields lives with the recipe's field groups (CadFeatureFields.hpp): v.ref() there.
    CadRefVisitor<Visit> rv{visit};
    cad_fields_of_all_groups(rv, f);
}

int CadDocument::paste_features(const std::vector<std::pair<int, CadFeature>>& items)
{
    std::map<int, int> was_now;
    int first = -1;
    for (const auto& it : items) {
        CadFeature f = it.second;
        for_each_feature_ref(f, [&](int& ref) { auto m = was_now.find(ref); if (m != was_now.end()) ref = m->second; });
        f.name += " copy";
        const int now = append_feature(f);
        was_now[it.first] = now;
        if (first < 0) first = now;
    }
    return first;
}

int CadDocument::append_feature(const CadFeature& f0)
{
    CadFeature f = f0;
    const int bar = rollback_index >= 0 ? rollback_index : m_bar;
    int pos = int(features.size());
    if (bar >= 0 && bar + 1 < pos) {
        // a feature may only read what lies above it: if it points at one below the bar it goes to the end instead
        bool ok = true;
        for_each_feature_ref(f, [&](int& ref) { if (ref > bar) ok = false; });
        if (ok) {
            pos = bar + 1;
            for (CadFeature& g : features)
                for_each_feature_ref(g, [&](int& ref) { if (ref >= pos) ++ref; });
            if (error_feature >= pos) ++error_feature;
            features.insert(features.begin() + pos, f);
            rollback_index = pos;
            last_added = pos;
            return pos;
        }
    }
    features.push_back(f);
    last_added = int(features.size()) - 1;
    return last_added;
}

bool CadDocument::remove_feature(int index)
{
    if (index < 0 || index >= int(features.size()))
        return false;

    // A sketch that a Use / Project feature reads cannot be deleted from under it: refused with the
    // reason, rather than letting the dependent derive nothing (or keep stale geometry) in silence.
    for (int j = 0; j < int(features.size()); ++j)
        if (features[j].project_from_sketch && features[j].project_source_sketch == index) {
            error = _u8L("This sketch is the source of ") + features[j].name
                  + _u8L(" (Use). Delete that first.");
            return false;
        }

    std::vector<CadFeature> snapshot = features;

    // Deleting a Sketch cascades to every feature that consumes it AS ITS PROFILE — the
    // reason is the original one ("a dangling Extrude would have no wire"), and it applies
    // unchanged to Revolve, Sweep, Rib and the Surface* family, which all read the profile
    // through sketch_ref, plus the sweep spine and the rib line. Testing the FIELD rather
    // than the type is what makes that true without a list to keep up to date.
    //
    // pattern_curve_sketch and loft_profile_refs are deliberately NOT cascaded: a Pattern or
    // a Loft that loses one of several inputs is degraded, not meaningless, so those refs go
    // to -1 in the remap below and the feature survives. A lone Sketch is harmless either way.
    std::vector<int> remove{index};
    if (features[index].type == CadFeatureType::Sketch) {
        for (int j = 0; j < int(features.size()); ++j) {
            const CadFeature& c = features[j];
            if (c.sketch_ref == index || c.sweep_path_ref == index || c.rib_sketch_ref == index)
                remove.push_back(j);
        }
    }
    std::sort(remove.begin(), remove.end());
    remove.erase(std::unique(remove.begin(), remove.end()), remove.end());

    stage_history_change(features,
        [&remove](int i) {
            if (std::binary_search(remove.begin(), remove.end(), i)) return -1;
            int shift = 0;
            for (int r : remove) if (r < i) ++shift;
            return i - shift;
        },
        [this](int i) { return features[i].enabled; });

    // Erase high-to-low so earlier indices stay valid.
    for (auto it = remove.rbegin(); it != remove.rend(); ++it)
        features.erase(features.begin() + *it);

    // Remap surviving feature references through the deletions: subtract the count of
    // removed indices that sat before it; orphaned refs (target removed) -> -1.
    //
    // This must cover EVERY field holding a feature index, not just sketch_ref. A mate's two
    // connectors are such indices, and leaving them behind slid them onto whatever features
    // landed on those slots — silently, because recompute() only rejects out-of-range and
    // non-CoordSys targets, and a shifted index usually lands on the assembly's other CoordSys.
    auto remap = [&remove](int& ref) {
        if (ref < 0)
            return;
        if (std::binary_search(remove.begin(), remove.end(), ref)) {
            ref = -1;
        } else {
            int shift = 0;
            for (int r : remove)
                if (r < ref) ++shift;
            ref -= shift;
        }
    };
    for (auto& f : features)
        for_each_feature_ref(f, remap);

    return commit_or_rollback(*this, snapshot);
}

bool CadDocument::move_feature(int index, int delta)
{
    if (index < 0 || index >= int(features.size()))
        return false;
    int target = index + delta;
    if (target < 0 || target >= int(features.size()))
        return true; // clamped at the ends — no-op, not a failure

    std::vector<CadFeature> snapshot = features;
    stage_history_change(features,
        [index, target](int i) { return i == index ? target : i == target ? index : i; },
        [this](int i) { return features[i].enabled; });
    std::swap(features[index], features[target]);

    // The two slots traded places: fix every feature reference that pointed at either —
    // a mate's connectors as well as sketch_ref, for the reason given in remove_feature().
    auto swap_ref = [index, target](int& ref) {
        if (ref == index)       ref = target;
        else if (ref == target) ref = index;
    };
    for (auto& f : features)
        for_each_feature_ref(f, swap_ref);

    // History runs top to bottom: a feature may only consume what comes BEFORE it. Moving a
    // consumer above its sketch (or a sketch below its consumer) would make it read whatever the
    // input held the last time round — or, for a Project, nothing. Refuse the move and say so.
    for (int fi = 0; fi < int(features.size()); ++fi) {
        CadFeature& f = features[fi];
        std::string bad;
        auto check = [&](int& ref) { if (ref >= fi && bad.empty()) bad = features[ref].name; };
        check(f.sketch_ref); check(f.sweep_path_ref); check(f.pattern_curve_sketch); check(f.rib_sketch_ref);
        for (int& r : f.loft_profile_refs) check(r);
        for (int& r : f.pattern_features) check(r);
        if (!bad.empty()) {
            // TRN Feature reorder error. %1% is the moved feature, %2% the feature it depends on.
            error = format(_u8L("\"%1%\" uses \"%2%\", so it cannot come before it"), f.name, bad);
            features.swap(snapshot);
            return false;
        }
    }

    return commit_or_rollback(*this, snapshot);
}

bool CadDocument::set_feature_enabled(int index, bool enabled)
{
    if (index < 0 || index >= int(features.size())) return false;
    if (features[index].enabled == enabled) return true;
    std::vector<CadFeature> snapshot = features;
    stage_history_change(features, [](int i) { return i; },
        [this, index, enabled](int i) { return i == index ? enabled : features[i].enabled; });
    features[index].enabled = enabled;
    return commit_or_rollback(*this, snapshot);
}

bool CadDocument::replace_feature(int index, const CadFeature& edited)
{
    if (index < 0 || index >= int(features.size()))
        return false;

    std::vector<CadFeature> snapshot = features;

    // Preserve identity (name) and the structural link (sketch_ref) from the
    // original; only the user-editable parameters come from `edited`.
    CadFeature f      = edited;
    f.name            = features[index].name;
    f.type            = features[index].type;
    if (f.type == CadFeatureType::Extrude)
        f.sketch_ref  = features[index].sketch_ref;
    features[index]   = f;

    return commit_or_rollback(*this, snapshot);
}

bool CadDocument::replace_sketch_extrude(int sketch_idx, int extrude_idx,
                                         const CadFeature& edited)
{
    if (sketch_idx  < 0 || sketch_idx  >= int(features.size())) return false;
    if (extrude_idx < 0 || extrude_idx >= int(features.size())) return false;

    std::vector<CadFeature> snapshot = features;

    // A box in the tree is two linked features: the Sketch consumes the profile
    // params (shape/plane/width/height/radius), the Extrude consumes the solid
    // params (distance/symmetric/mode). `edited` carries all of them; split it
    // back into the two slots, preserving each slot's name/type and the link.
    CadFeature& sk = features[sketch_idx];
    sk.shape  = edited.shape;
    sk.plane  = edited.plane;
    sk.width  = edited.width;
    sk.height = edited.height;
    sk.radius = edited.radius;

    CadFeature& ex = features[extrude_idx];
    ex.distance  = edited.distance;
    ex.symmetric = edited.symmetric;
    ex.mode      = edited.mode;

    return commit_or_rollback(*this, snapshot);
}

// "the sketch is open" is not actionable; WHERE it is open is. Shared by both throws so the
// two ways of reaching an open profile (Revolve via build_sketch_wire, Extrude via
// build_sketch_face) report it identically.
static std::string open_loop_message(const CadFeature& sketch,
                                     const std::string& head = _u8L("sketch entities do not form a closed loop"))
{
    std::string msg = head;
    const std::vector<Vec2d> open = Slic3r::sketch_open_ends(sketch.entities, sketch.plane);
    if (!open.empty()) {
        const size_t shown = std::min<size_t>(open.size(), 3);
        for (size_t i = 0; i < shown; ++i)
            msg += "\n  " + format(_u8L("Open at (%.3f, %.3f)"), open[i].x(), open[i].y());
        if (open.size() > shown)
            // TRN Follows a list of open sketch ends. %1% is how many more were not listed.
            msg += "\n  " + format(_u8L("Open ends not shown: %1%"), open.size() - shown);
    }
    return msg;
}

// World axis of a Revolve / Surface Revolve whose profile is `sk`: the Line of that sketch named
// by revolve_axis_entity, else the sketch plane's X or Y axis through its origin.
static gp_Ax1 revolve_axis_of(const CadFeature& f, const CadFeature& sk)
{
    if (f.revolve_axis_entity >= 0) {
        if (f.revolve_axis_entity >= int(sk.entities.size())
            || sk.entities[f.revolve_axis_entity].type != SketchEntity::Type::Line)
            throw std::runtime_error(_u8L("revolve: the axis line is no longer in the sketch — pick the axis again"));
        const SketchEntity& e = sk.entities[f.revolve_axis_entity];
        const Vec3d a = sk.plane.to_world(e.p0), b = sk.plane.to_world(e.p1);
        if ((b - a).norm() < 1e-9)
            throw std::runtime_error(_u8L("revolve: the axis line has no length"));
        const Vec3d d = (b - a).normalized();
        return gp_Ax1(gp_Pnt(a.x(), a.y(), a.z()), gp_Dir(d.x(), d.y(), d.z()));
    }
    const Vec3d& adir = (f.revolve_axis == 1) ? sk.plane.y_axis : sk.plane.x_axis;
    return gp_Ax1(gp_Pnt(sk.plane.origin.x(), sk.plane.origin.y(), sk.plane.origin.z()),
                  gp_Dir(adir.x(), adir.y(), adir.z()));
}

std::vector<std::vector<int>> CadDocument::sketch_closed_loops(const CadFeature& sketch)
{
    std::vector<std::vector<int>> loops;
    SketchEngine::entities_to_wires(sketch.entities, sketch.plane, true, &loops);
    return loops;
}

TopoDS_Wire CadDocument::loft_profile_wire(const CadFeature& f, size_t ri, bool closed_only) const
{
    const CadFeature& sk = features[size_t(f.loft_profile_refs[ri])];
    if (ri >= f.loft_profile_loops.size() || f.loft_profile_loops[ri].empty()) return build_sketch_wire(sk, closed_only);
    CadFeature sub = sk;
    sub.entities.clear();
    sub.entity_constraints.clear();
    for (int id : f.loft_profile_loops[ri]) {
        if (id < 0 || id >= int(sk.entities.size()))
            throw std::runtime_error(format(_u8L("loft: the loop chosen in %1% is no longer in the sketch — pick it again"), sk.name));
        sub.entities.push_back(sk.entities[size_t(id)]);
    }
    return build_sketch_wire(sub, closed_only);
}

TopoDS_Wire CadDocument::build_sketch_wire(const CadFeature& sketch, bool closed_only) const
{
    if (!sketch.entities.empty()) {
        TopoDS_Wire w = SketchEngine::entities_to_wire(sketch.entities, sketch.plane, closed_only);
        if (!w.IsNull()) return w;
        // An entity sketch that yields no wire is an ERROR, not a cue to fall through. The
        // legacy tail of this function ends in a default rectangle built from width/height,
        // which for an entity sketch are whatever they happened to be initialised to — so a
        // sketch entities_to_wire cannot handle (a circle coexisting with a line, two circles:
        // 88v) used to extrude into a box the user never drew, silently. Failing here
        // costs the caller an error message; falling through cost them wrong geometry that
        // looked deliberate. The legacy profile/shape paths below are still reached by sketches
        // that legitimately carry no entities at all.
        const size_t loops = SketchEngine::entities_to_wires(sketch.entities, sketch.plane, closed_only).size();
        if (loops > 1)
            throw std::runtime_error(format(_u8L("%1% holds %2% separate closed shapes — a profile must be exactly one closed shape "
                                                 "(delete or make construction the others)"), sketch.name, int(loops)));
        throw std::runtime_error(
            open_loop_message(sketch,
                format(_u8L("%1% does not form one closed shape — a closed entity (circle/ellipse) combined with other "
                            "entities is not supported yet"), sketch.name)));
    }
    if (!sketch.profile.points.empty()) {
        SketchProfile prof = sketch.profile;
        prof.closed = true;               // extrude needs a closed wire
        TopoDS_Wire w = prof.to_occt_wire(sketch.plane);
        if (w.IsNull()) throw std::runtime_error(_u8L("sketch profile wire failed"));
        return w;
    }
    if (sketch.shape == SketchShape::Circle) {
        gp_Pnt o(sketch.plane.origin.x(), sketch.plane.origin.y(), sketch.plane.origin.z());
        gp_Dir n(sketch.plane.normal.x(), sketch.plane.normal.y(), sketch.plane.normal.z());
        gp_Circ circ(gp_Ax2(o, n), sketch.radius);
        TopoDS_Edge e = BRepBuilderAPI_MakeEdge(circ).Edge();
        BRepBuilderAPI_MakeWire wm(e);
        if (!wm.IsDone()) throw std::runtime_error(_u8L("circle wire failed"));
        return wm.Wire();
    }
    // Rectangle centered on the plane origin
    SketchProfile prof;
    double hw = sketch.width * 0.5, hh = sketch.height * 0.5;
    prof.points.push_back(Vec2d(-hw, -hh));
    prof.points.push_back(Vec2d( hw, -hh));
    prof.points.push_back(Vec2d( hw,  hh));
    prof.points.push_back(Vec2d(-hw,  hh));
    prof.closed = true;
    return prof.to_occt_wire(sketch.plane);
}

int CadDocument::body_touching_sketch(int sketch_ref) const
{
    if (sketch_ref < 0 || sketch_ref >= int(features.size())
        || features[sketch_ref].type != CadFeatureType::Sketch)
        return -1;
    // A query that only picks a default: any failure answers "touches nothing".
    try {
        const TopoDS_Face face = build_sketch_face(features[sketch_ref]);
        if (face.IsNull()) return -1;
        for (int i = int(bodies.size()) - 1; i >= 0; --i) {   // newest first: the likeliest target
            const TopoDS_Shape& b = bodies[i].shape;
            if (b.IsNull() || is_sheet_shape(b)) continue;
            BRepExtrema_DistShapeShape d(face, b);
            if (d.IsDone() && d.Value() <= 1e-4)
                return i;
        }
    } catch (const Standard_Failure&) {   // before std::exception: on OCCT >= 8 it derives from it
    } catch (const std::exception&) {
    }
    return -1;
}

TopoDS_Face CadDocument::build_sketch_face(const CadFeature& sketch) const
{
    if (!sketch.entities.empty()) {
        const std::vector<TopoDS_Wire> loops = SketchEngine::entities_to_wires(sketch.entities, sketch.plane, true);
        if (loops.empty())
            // Same courtesy as build_sketch_wire: name WHERE the sketch is open. Extrude
            // reaches its failure through here, not through build_sketch_wire, so without
            // this the most common way to hit an open profile is also the least informative.
            throw std::runtime_error(open_loop_message(sketch));
        return SketchEngine::wires_to_face(loops, sketch.plane);
    }
    return BRepBuilderAPI_MakeFace(build_sketch_wire(sketch, true)).Face();
}

// A combine that could not do what its mode asks (Join that does not touch, Cut that eats the whole
// body, Intersect with no overlap, an OCCT failure). Carries the TOOL so route_feature can keep the
// user's shape as a NEW body and say so, instead of throwing their work away.
struct CombineFailed : std::runtime_error {
    TopoDS_Shape tool;
    CombineFailed(const std::string& m, const TopoDS_Shape& t) : std::runtime_error(m), tool(t) {}
};

static int solid_count(const TopoDS_Shape& s)
{
    int n = 0;
    if (s.IsNull()) return 0;
    for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next()) ++n;
    return n;
}

// Merge faces and edges that lie on the same surface. A pull leaves the old side faces and the new
// walls coplanar and split; without this a later fillet or shell sees a seam that is not there.
static TopoDS_Shape unify_same_domain(const TopoDS_Shape& s)
{
    try {
        ShapeUpgrade_UnifySameDomain u(s, /*edges*/ true, /*faces*/ true, /*concat bsplines*/ true);
        u.Build();
        const TopoDS_Shape out = u.Shape();
        return out.IsNull() ? s : out;
    } catch (...) {
        return s;   // a shape OCCT cannot unify is still a valid shape
    }
}

// The one place Extrude / Revolve / Sweep / Loft combine their tool with the body (Onshape's New /
// Add / Remove / Intersect). Four copies of this used to drift independently.
static void combine_into(BooleanMode mode, TopoDS_Shape& result, bool& have_body, const TopoDS_Shape& tool)
{
    if (!have_body || mode == BooleanMode::New) {
        result = tool;
        have_body = true;
        return;
    }
    if (mode == BooleanMode::Add) {
        BRepAlgoAPI_Fuse fuse(result, tool);
        if (!fuse.IsDone() || fuse.Shape().IsNull())
            throw CombineFailed(_u8L("Join failed"), tool);
        // Disjoint solids fuse into one compound that still has both: that is not a join.
        const int nb = solid_count(result), nt = solid_count(tool), nf = solid_count(fuse.Shape());
        if (nb >= 1 && nt >= 1 && nf >= nb + nt)
            throw CombineFailed(_u8L("the new shape does not touch the body, so it cannot be joined"), tool);
        result = fuse.Shape();
    } else if (mode == BooleanMode::Cut) {
        BRepAlgoAPI_Cut cut(result, tool);
        if (!cut.IsDone() || cut.Shape().IsNull())
            throw CombineFailed(_u8L("Cut failed"), tool);
        if (solid_count(cut.Shape()) == 0)
            throw CombineFailed(_u8L("the cut would remove the whole body"), tool);
        result = cut.Shape();
    } else if (mode == BooleanMode::Intersect) {
        BRepAlgoAPI_Common common(result, tool);
        if (!common.IsDone() || common.Shape().IsNull())
            throw CombineFailed(_u8L("Intersect failed"), tool);
        if (solid_count(common.Shape()) == 0)
            throw CombineFailed(_u8L("the shapes do not overlap, so there is nothing to intersect"), tool);
        result = common.Shape();
    }
}

// The open chain of straight lines of a sketch, as world points from one free end to the other.
static std::vector<Vec3d> sketch_line_chain(const CadFeature& sk)
{
    std::vector<const SketchEntity*> lines;
    for (const SketchEntity& e : sk.entities) {
        if (e.construction) continue;
        if (e.type != SketchEntity::Type::Line) throw std::runtime_error(_u8L("contour flange: the sketch may only hold straight lines"));
        lines.push_back(&e);
    }
    if (lines.empty()) throw std::runtime_error(_u8L("contour flange: the sketch has no lines"));
    auto same = [](const Vec2d& a, const Vec2d& b) { return (a - b).norm() < 1e-6; };
    std::vector<Vec2d> pts{ lines[0]->p0, lines[0]->p1 };
    std::vector<char> used(lines.size(), 0);
    used[0] = 1;
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (used[i]) continue;
            const Vec2d &a = lines[i]->p0, &b = lines[i]->p1;
            if      (same(a, pts.back()))  pts.push_back(b);
            else if (same(b, pts.back()))  pts.push_back(a);
            else if (same(b, pts.front())) pts.insert(pts.begin(), a);
            else if (same(a, pts.front())) pts.insert(pts.begin(), b);
            else continue;
            used[i] = 1; grew = true;
        }
    }
    for (char u : used) if (!u) throw std::runtime_error(_u8L("contour flange: the lines do not form one chain"));
    if (same(pts.front(), pts.back())) throw std::runtime_error(_u8L("contour flange: the contour must be open"));
    std::vector<Vec3d> out;
    for (const Vec2d& p : pts) out.push_back(sk.plane.to_world(p));
    return out;
}

void CadDocument::apply_feature(TopoDS_Shape& result, bool& have_body,
                                const TopoDS_Shape& context, const CadFeature& f) const
{
    switch (f.type) {
    case CadFeatureType::SmRule:
        return;   // sets the rule; bind_sheet_rules has already copied it into the features that use it
    case CadFeatureType::SmBase: {
        CadFeature e = f;   // a base flange is an extrude of the profile by the rule thickness
        e.type = CadFeatureType::Extrude; e.mode = BooleanMode::New; e.distance = f.sm_thickness;
        apply_feature(result, have_body, context, e);
        return;
    }
    case CadFeatureType::SmEdgeFlange: {
        if (!have_body) throw std::runtime_error(_u8L("a flange needs a sheet body"));
        std::vector<TopoDS_Edge> es;
        for (int id : f.dressup_edge_ids()) {
            TopoDS_Edge e = GeometryEngine::edge_by_index(result, id);
            if (e.IsNull()) throw std::runtime_error(_u8L("flange: invalid edge id"));
            es.push_back(e);
        }
        result = SheetMetal::edge_flanges(result, es, sheet_rule_of(f), f.distance, f.sm_angle, f.flip);
        break;
    }
    case CadFeatureType::SmContourFlange: {
        if (f.sketch_ref < 0 || f.sketch_ref >= int(features.size()) || features[f.sketch_ref].type != CadFeatureType::Sketch)
            throw std::runtime_error(_u8L("contour flange needs a sketch"));
        const CadFeature& sk = features[f.sketch_ref];
        result = SheetMetal::contour_flange(sketch_line_chain(sk), sk.plane.normal, f.distance, sheet_rule_of(f), f.flip);
        have_body = true;
        break;
    }
    case CadFeatureType::SmHem: {
        if (!have_body) throw std::runtime_error(_u8L("a hem needs a sheet body"));
        const std::vector<int> ids = f.dressup_edge_ids();
        const TopoDS_Edge e = ids.empty() ? TopoDS_Edge() : GeometryEngine::edge_by_index(result, ids[0]);
        if (e.IsNull()) throw std::runtime_error(_u8L("hem: invalid edge id"));
        result = SheetMetal::flat_hem(result, e, sheet_rule_of(f), f.distance);
        break;
    }
    case CadFeatureType::SmUnfold:
    case CadFeatureType::SmRefold:
        throw std::runtime_error(_u8L("unfold / refold are not available yet"));
    case CadFeatureType::Sketch:
        return; // sketches carry no solid; consumed by an extrude
    case CadFeatureType::Project:
        return; // edges-to-sketch: consumed downstream, no solid body
    case CadFeatureType::Helix:
        return; // helical curve; consumed by Sweep as a path (like Sketch)
    case CadFeatureType::Sketch3D:
        return; // free 3D curve; consumed by Sweep as a path
    case CadFeatureType::Boolean:
        return; // body-body boolean is handled in route_feature/apply_boolean, never here
    case CadFeatureType::Import:
        // Imported B-rep (STEP): rigid data carried on the feature, not built from
        // parameters — adopt it as the new body (New-path: result starts empty).
        result    = f.imported_solid;
        have_body = !result.IsNull();
        return;
    case CadFeatureType::Extrude: {
        const bool sym = (f.extrude_end == ExtrudeEnd::Symmetric);
        const double signed_d = f.flip ? -f.distance : f.distance;
        TopoDS_Shape tool;
        if (f.extrude_src_face >= 0) {
            // The source face is read from `context` (the owner body), which for a New
            // face-extrude is the source solid while `result` is the empty new body.
            if (context.IsNull()) throw std::runtime_error(_u8L("face-extrude needs a body"));
            TopoDS_Face srcf = GeometryEngine::face_by_index(context, f.extrude_src_face);
            if (srcf.IsNull()) throw std::runtime_error("face-extrude: invalid face id");
            SketchPlane fpl = SketchPlane::from_face(srcf);
            // from_face takes the surface's geometric normal and IGNORES the topological
            // face orientation, so for a REVERSED face (e.g. the top cap of an extruded
            // prism) it points INTO the solid -> a default push would fuse to nothing.
            // Orient it outward so push/pull grows away from the material (Onshape default);
            // the Flip checkbox (signed_d) still lets the user drive it inward for a cut.
            if (srcf.Orientation() == TopAbs_REVERSED) fpl.normal = -fpl.normal;
            tool = SketchEngine::make_extrude_face(srcf, fpl, signed_d, sym);
        } else {
            // Use the referenced sketch when sketch_ref is a valid Sketch index,
            // otherwise fall back to f's own inline sketch params (this makes a
            // single self-contained candidate previewable).
            const CadFeature& sk0 = (f.sketch_ref >= 0 && f.sketch_ref < int(features.size())
                                    && (features[f.sketch_ref].type == CadFeatureType::Sketch
                                        || features[f.sketch_ref].type == CadFeatureType::Project))
                                   ? features[f.sketch_ref] : f;
            // Only some of the sketch's loops: a private copy of the sketch holding just those entities.
            CadFeature sk_subset;
            if (!f.extrude_entities.empty()) {
                sk_subset = sk0;
                sk_subset.entities.clear();
                sk_subset.entity_constraints.clear();
                for (int id : f.extrude_entities) {
                    if (id < 0 || id >= int(sk0.entities.size())) throw std::runtime_error(_u8L("extrude: a chosen sketch entity does not exist"));
                    sk_subset.entities.push_back(sk0.entities[size_t(id)]);
                }
            }
            const CadFeature& sk = f.extrude_entities.empty() ? sk0 : sk_subset;
            // Imported rigid art (Text/SVG) extrudes via the faces-with-holes path
            // (with its placement transform applied); otherwise build the sketch's planar
            // region (outer loop + holes) from entities/profile/shape.
            tool = !sk.imported_regions.empty()
                ? SketchEngine::make_extrude_regions(
                      transform_regions(sk.imported_regions, sk.import_offset,
                                        sk.import_scale_x, sk.import_scale_y),
                      sk.plane,
                      f.extrude_end == ExtrudeEnd::ThroughAll ? 1e5 : signed_d,
                      f.extrude_end == ExtrudeEnd::ThroughAll ? true : (sym || f.extrude_end == ExtrudeEnd::TwoSided))
                : [&]() {
                    auto one_region = [&](TopoDS_Face profile) -> TopoDS_Shape {
                      if (f.thin_in > 0 || f.thin_out > 0) {
                          // A wall around the outline: (outline grown by thin_out) minus (outline shrunk by thin_in), as a face.
                          if (std::abs(f.taper_deg) > 1e-6) throw std::runtime_error(_u8L("a thin extrude cannot be tapered"));
                          const TopoDS_Wire outline = build_sketch_wire(sk, true);
                          auto grown = [&](double d) -> TopoDS_Face {
                              if (std::abs(d) < 1e-9) { BRepBuilderAPI_MakeFace fm(outline); return fm.Face(); }
                              BRepOffsetAPI_MakeOffset mko(outline, GeomAbs_Intersection);
                              mko.Perform(d);
                              if (!mko.IsDone() || mko.Shape().IsNull()) throw std::runtime_error(_u8L("thin extrude: the wall does not fit around this outline"));
                              TopoDS_Wire w;
                              if (mko.Shape().ShapeType() == TopAbs_WIRE) w = TopoDS::Wire(mko.Shape());
                              else for (TopExp_Explorer ex(mko.Shape(), TopAbs_WIRE); ex.More(); ex.Next()) { w = TopoDS::Wire(ex.Current()); break; }
                              if (w.IsNull()) throw std::runtime_error(_u8L("thin extrude: the wall does not fit around this outline"));
                              BRepBuilderAPI_MakeFace fm(w);
                              if (!fm.IsDone()) throw std::runtime_error(_u8L("thin extrude: the wall does not fit around this outline"));
                              return fm.Face();
                          };
                          if (!outline.Closed()) throw std::runtime_error(_u8L("a thin extrude needs a closed outline"));
                          BRepAlgoAPI_Cut ring(grown(f.thin_out), grown(-f.thin_in));
                          if (!ring.IsDone()) throw std::runtime_error(_u8L("thin extrude failed"));
                          TopoDS_Face rf;
                          for (TopExp_Explorer ex(ring.Shape(), TopAbs_FACE); ex.More(); ex.Next()) { rf = TopoDS::Face(ex.Current()); break; }
                          if (rf.IsNull()) throw std::runtime_error(_u8L("thin extrude: the wall is empty (it is thicker than the outline)"));
                          profile = rf;
                      }
                      // A tapered extrude offsets the profile; a holed profile would have to
                      // offset its inner loops the opposite way, which is not implemented yet.
                      auto taper = [&](double L) -> TopoDS_Shape {
                          // Count the loops off the face we ALREADY built, rather than rebuilding
                          // every wire from the entities a second time to ask how many there are.
                          // It is also the more honest test: what matters is whether the profile
                          // being extruded has holes, not what the entity list could produce.
                          int nloops = 0;
                          for (TopExp_Explorer ex(profile, TopAbs_WIRE); ex.More(); ex.Next()) ++nloops;
                          if (nloops > 1)
                              throw std::runtime_error(_u8L("tapered extrude of a sketch with holes is not supported yet"));
                          return SketchEngine::make_extrude_taper(build_sketch_wire(sk, true), sk.plane, L, f.taper_deg);
                      };
                      TopoDS_Shape t;
                      switch (f.extrude_end) {
                          case ExtrudeEnd::Blind:
                              t = (std::abs(f.taper_deg) > 1e-6)
                                  ? taper(signed_d)
                                  : SketchEngine::make_extrude(profile, sk.plane, signed_d, false);
                              break;
                          case ExtrudeEnd::Symmetric:  t = SketchEngine::make_extrude(profile, sk.plane, f.distance, true); break;
                          case ExtrudeEnd::TwoSided:   t = SketchEngine::make_extrude_two_sided(profile, sk.plane, f.distance, f.distance2); break;
                          case ExtrudeEnd::ThroughAll: t = SketchEngine::make_extrude(profile, sk.plane, 1.0e5, true); break;
                          case ExtrudeEnd::UpToFace: {
                              const TopoDS_Face tgt = GeometryEngine::face_by_index(context, f.up_to_face);
                              double L = signed_d;
                              if (!tgt.IsNull()) {
                                  const Vec3d c = GeometryEngine::face_centroid_world(tgt);
                                  L = (c - sk.plane.origin).dot(sk.plane.normal);
                              }
                              t = (std::abs(f.taper_deg) > 1e-6)
                                  ? taper(L)
                                  : SketchEngine::make_extrude(profile, sk.plane, L, false);
                              break;
                          }
                          case ExtrudeEnd::UpToVertex: {
                              const double L = (f.up_to_point - sk.plane.origin).dot(sk.plane.normal);
                              t = SketchEngine::make_extrude(profile, sk.plane, L, false);
                              break;
                          }
                          default: t = SketchEngine::make_extrude(profile, sk.plane, signed_d, false); break;
                      }
                      return t;
                    };
                    if (f.extrude_entities.empty() && SketchEngine::arrangement_needed(sk.entities)) {
                        // curves that meet in T-junctions: every region of the subdivision, joined into one solid
                        TopoDS_Shape acc;
                        for (const TopoDS_Face& region : SketchEngine::arrangement_faces(sk.entities, sk.plane)) {
                            const TopoDS_Shape part = one_region(region);
                            if (acc.IsNull()) { acc = part; continue; }
                            BRepAlgoAPI_Fuse fu(acc, part);
                            if (!fu.IsDone() || fu.Shape().IsNull()) throw std::runtime_error(_u8L("the regions of the sketch could not be joined"));
                            acc = fu.Shape();
                        }
                        return unify_same_domain(acc);
                    }
                    if (f.extrude_entities.empty() && !sk.entities.empty()) {
                        // Several islands in one sketch (two separate outlines): one prism per region, together in one body.
                        // A single region (holes included) stays on the plain path below.
                        const std::vector<TopoDS_Wire> loops = SketchEngine::entities_to_wires(sk.entities, sk.plane, true);
                        if (loops.size() > 1) {
                            const std::vector<TopoDS_Face> regions = SketchEngine::wires_to_faces(loops, sk.plane);
                            if (regions.size() > 1) {
                                TopoDS_Compound all;
                                BRep_Builder bb;
                                bb.MakeCompound(all);
                                for (const TopoDS_Face& region : regions) {
                                    const TopoDS_Shape part = one_region(region);
                                    for (TopExp_Explorer ex(part, TopAbs_SOLID); ex.More(); ex.Next()) bb.Add(all, ex.Current());
                                }
                                return TopoDS_Shape(all);
                            }
                        }
                    }
                    if (f.extrude_entities.empty()) return one_region(build_sketch_face(sk));
                    if (SketchEngine::arrangement_needed(sk.entities)) {   // the chosen curves meet in T-junctions: see above
                        TopoDS_Shape acc;
                        for (const TopoDS_Face& region : SketchEngine::arrangement_faces(sk.entities, sk.plane)) {
                            const TopoDS_Shape part = one_region(region);
                            if (acc.IsNull()) { acc = part; continue; }
                            BRepAlgoAPI_Fuse fu(acc, part);
                            if (!fu.IsDone() || fu.Shape().IsNull()) throw std::runtime_error(_u8L("the regions of the sketch could not be joined"));
                            acc = fu.Shape();
                        }
                        return unify_same_domain(acc);
                    }
                    const std::vector<TopoDS_Wire> loops = SketchEngine::entities_to_wires(sk.entities, sk.plane, true);
                    if (loops.empty()) throw std::runtime_error(open_loop_message(sk));
                    TopoDS_Compound all;
                    BRep_Builder bb;
                    bb.MakeCompound(all);
                    for (const TopoDS_Face& region : SketchEngine::wires_to_faces(loops, sk.plane)) {
                        const TopoDS_Shape part = one_region(region);
                        for (TopExp_Explorer ex(part, TopAbs_SOLID); ex.More(); ex.Next()) bb.Add(all, ex.Current());
                    }
                    return TopoDS_Shape(all);
                  }();
        }
        // New / first-of-a-body => result becomes the tool (route_feature sends New extrudes
        // here with an empty result, so a face-extrude New builds a fresh body from the source
        // face in `context` without touching it). Add/Cut/Intersect boolean into `result`.
        // Push/pull of a picked face: the SIGN of the distance picks the operation, as in Fusion,
        // Shapr3D and Plasticity. Out along the normal joins (V + A*d); in cuts (V - A*d). The tool
        // for a negative distance is already the prism going into the material.
        BooleanMode eff = f.mode;
        if (f.extrude_src_face >= 0 && signed_d < 0 && eff == BooleanMode::Add) eff = BooleanMode::Cut;
        combine_into(eff, result, have_body, tool);
        if (f.extrude_src_face >= 0 && eff != BooleanMode::New && have_body && !result.IsNull())
            result = unify_same_domain(result);
        break;
    }
    case CadFeatureType::Revolve: {
        // Resolve the profile sketch like Extrude: referenced Sketch when valid,
        // else this feature's own inline entities/profile (self-contained candidate).
        const CadFeature& sk = (f.sketch_ref >= 0 && f.sketch_ref < int(features.size())
                                && (features[f.sketch_ref].type == CadFeatureType::Sketch
                                    || features[f.sketch_ref].type == CadFeatureType::Project))
                               ? features[f.sketch_ref] : f;
        TopoDS_Wire wire = build_sketch_wire(sk, true);
        const double ang = f.flip ? -f.revolve_angle : f.revolve_angle;
        TopoDS_Shape tool = SketchEngine::make_revolve(wire, revolve_axis_of(f, sk), ang);
        combine_into(f.mode, result, have_body, tool);
        break;
    }
    case CadFeatureType::SurfaceExtrude: {
        if (f.sketch_ref < 0 || f.sketch_ref >= int(features.size()))
            throw std::runtime_error("surface-extrude: bad sketch ref");
        const CadFeature& sk = features[f.sketch_ref];
        if (sk.type != CadFeatureType::Sketch && sk.type != CadFeatureType::Project)
            throw std::runtime_error("surface-extrude: ref is not a sketch");
        TopoDS_Wire wire = build_sketch_wire(sk);
        if (wire.IsNull()) throw std::runtime_error(_u8L("surface-extrude: empty profile"));
        gp_Dir nrm(sk.plane.normal.x(), sk.plane.normal.y(), sk.plane.normal.z());
        try {
            result = SketchEngine::make_prism(wire, gp_Vec(nrm.XYZ() * f.distance));
        } catch (const std::exception& e) {
            throw std::runtime_error(format(_u8L("surface-extrude: %1%"), *e.what() ? e.what() : _u8L("prism failed")));
        }
        have_body = true;
        break;
    }
    case CadFeatureType::SurfaceRevolve: {
        if (f.sketch_ref < 0 || f.sketch_ref >= int(features.size()))
            throw std::runtime_error("surface-revolve: bad sketch ref");
        const CadFeature& sk = features[f.sketch_ref];
        if (sk.type != CadFeatureType::Sketch && sk.type != CadFeatureType::Project)
            throw std::runtime_error("surface-revolve: ref is not a sketch");
        TopoDS_Wire wire = build_sketch_wire(sk);
        if (wire.IsNull()) throw std::runtime_error(_u8L("surface-revolve: empty profile"));
        gp_Ax1 axis = revolve_axis_of(f, sk);
        // Same angle rules as the solid Revolve (flip reverses it, and a negative sweep is a
        // positive one about the reversed axis — MakeRevol wants (0, 2 pi]); this surface
        // version used to ignore both.
        double ang = (f.flip ? -f.revolve_angle : f.revolve_angle) * M_PI / 180.0;
        if (ang < 0) { axis.Reverse(); ang = -ang; }
        BRepPrimAPI_MakeRevol rev(wire, axis, ang, false);
        if (!rev.IsDone()) throw std::runtime_error(_u8L("surface-revolve: revolve failed"));
        result = rev.Shape(); have_body = true;
        break;
    }
    case CadFeatureType::SurfaceLoft: {
        std::vector<TopoDS_Wire> profiles;
        for (size_t ri = 0; ri < f.loft_profile_refs.size(); ++ri) {
            const int ref = f.loft_profile_refs[ri];
            if (ref < 0 || ref >= int(features.size())
                || (features[ref].type != CadFeatureType::Sketch
                    && features[ref].type != CadFeatureType::Project))
                continue;
            profiles.push_back(loft_profile_wire(f, ri, false));
        }
        if (profiles.size() < 2)
            throw std::runtime_error(_u8L("surface-loft needs 2+ valid profile sketches"));
        TopoDS_Shape skin = SketchEngine::make_loft_surface(profiles, f.loft_ruled);
        if (skin.IsNull()) throw std::runtime_error(_u8L("surface-loft: loft failed"));
        result = skin; have_body = true;
        break;
    }
    case CadFeatureType::SurfaceFill: {
        if (f.sketch_ref < 0 || f.sketch_ref >= int(features.size()))
            throw std::runtime_error("surface-fill: bad sketch ref");
        const CadFeature& sk = features[f.sketch_ref];
        if (sk.type != CadFeatureType::Sketch && sk.type != CadFeatureType::Project)
            throw std::runtime_error("surface-fill: ref is not a sketch");
        TopoDS_Wire wire = build_sketch_wire(sk);
        if (wire.IsNull()) throw std::runtime_error(_u8L("surface-fill: empty boundary"));
        BRepOffsetAPI_MakeFilling fill;
        int nedges = 0;
        for (TopExp_Explorer ex(wire, TopAbs_EDGE); ex.More(); ex.Next()) {
            fill.Add(TopoDS::Edge(ex.Current()), GeomAbs_C0);
            ++nedges;
        }
        if (nedges == 0) throw std::runtime_error(_u8L("surface-fill: boundary has no edges"));
        fill.Build();
        if (!fill.IsDone()) throw std::runtime_error(_u8L("surface-fill: fill failed"));
        TopoDS_Shape face = fill.Shape();
        if (face.IsNull()) throw std::runtime_error(_u8L("surface-fill: produced no geometry"));
        result = face; have_body = true;
        break;
    }
    case CadFeatureType::Sweep: {
        const CadFeature& sk = (f.sketch_ref >= 0 && f.sketch_ref < int(features.size())
                                && (features[f.sketch_ref].type == CadFeatureType::Sketch
                                    || features[f.sketch_ref].type == CadFeatureType::Project))
                               ? features[f.sketch_ref] : f;
        if (f.sweep_path_ref < 0 || f.sweep_path_ref >= int(features.size()))
            throw std::runtime_error(_u8L("sweep needs a valid path reference"));
        const CadFeature& path_feat = features[f.sweep_path_ref];
        TopoDS_Wire path;
        if (path_feat.type == CadFeatureType::Helix) {
            std::string helix_err;
            path = make_helix_spine(path_feat, helix_err);
            if (path.IsNull()) throw std::runtime_error(format(_u8L("helix path: %1%"), helix_err));
        } else if (path_feat.type == CadFeatureType::Sketch) {
            path = build_sketch_wire(path_feat);
        } else if (path_feat.type == CadFeatureType::Sketch3D) {
            std::string e3;
            path = build_sketch3d_wire(path_feat, e3);
            if (path.IsNull()) throw std::runtime_error(format(_u8L("3D sketch path: %1%"), e3));
        } else {
            throw std::runtime_error(_u8L("sweep path must be a sketch, 3D sketch or helix"));
        }
        TopoDS_Wire profile = build_sketch_wire(sk, true);
        TopoDS_Shape tool   = SketchEngine::make_sweep(profile, path);
        combine_into(f.mode, result, have_body, tool);
        break;
    }
    case CadFeatureType::Loft: {
        // Loft through 2+ closed profile Sketches, in recipe order. Each profile builds
        // a wire via build_sketch_wire (so it keeps its own plane); make_loft skins them.
        std::vector<TopoDS_Wire> profiles;
        std::optional<gp_Pnt> apex_first, apex_last;   // a section that is a lone point (a sketch holding only one point) ends the loft in a tip
        const size_t n_refs = f.loft_profile_refs.size();
        for (size_t ri = 0; ri < n_refs; ++ri) {
            const int ref = f.loft_profile_refs[ri];
            if (ref < 0 || ref >= int(features.size())
                || (features[ref].type != CadFeatureType::Sketch
                    && features[ref].type != CadFeatureType::Project))
                continue;
            const CadFeature& sk = features[ref];
            if (sk.type == CadFeatureType::Sketch && sk.entities.size() == 1 && sk.entities[0].type == SketchEntity::Type::Point) {
                if (ri != 0 && ri + 1 != n_refs) throw std::runtime_error(_u8L("loft: a point can only be the first or the last section"));
                const Vec3d w = sk.plane.to_world(sk.entities[0].p0);
                (ri == 0 ? apex_first : apex_last) = gp_Pnt(w.x(), w.y(), w.z());
                continue;
            }
            profiles.push_back(loft_profile_wire(f, ri, true));
        }
        const bool tip = apex_first || apex_last;
        if (profiles.size() < (tip ? 1u : 2u))
            throw std::runtime_error(_u8L("loft needs 2+ valid profile sketches"));
        TopoDS_Shape tool = tip ? SketchEngine::make_loft_to_point(profiles, f.loft_ruled, apex_first ? &*apex_first : nullptr, apex_last ? &*apex_last : nullptr)
                                : SketchEngine::make_loft(profiles, f.loft_ruled, f.loft_start_cond, f.loft_end_cond);
        combine_into(f.mode, result, have_body, tool);
        break;
    }
    case CadFeatureType::Pattern: {
        // Pattern along a curve: when pattern_curve_sketch >= 0 this mode takes
        // precedence over linear/circular. Copies are placed at equal-parameter points
        // along the referenced sketch entity, translated by (P_i - P_0).
        if (f.pattern_curve_sketch >= 0) {
            if (f.pattern_curve_sketch >= (int)features.size())
                throw std::runtime_error("pattern-on-curve: bad sketch ref");
            const CadFeature& gs = features[f.pattern_curve_sketch];
            if (gs.type != CadFeatureType::Sketch)
                throw std::runtime_error("pattern-on-curve: ref is not a sketch");
            if (f.pattern_curve_entity < 0 || f.pattern_curve_entity >= (int)gs.entities.size())
                throw std::runtime_error("pattern-on-curve: bad entity");
            if (!have_body) throw std::runtime_error(_u8L("pattern needs a body"));
            const SketchEntity& gc = gs.entities[f.pattern_curve_entity];
            const int n = std::max(1, f.pattern_count);
            if (n > 500) throw std::runtime_error(_u8L("pattern: at most 500 instances"));
            const TopoDS_Shape seed = result;
            Vec3d p0 = gs.plane.to_world(sample_entity_2d(gc, 0.0));
            for (int i = 1; i < n; ++i) {
                double t = double(i) / double(n - 1 > 0 ? n - 1 : 1);
                Vec3d pi = gs.plane.to_world(sample_entity_2d(gc, t));
                Vec3d d  = pi - p0;
                gp_Trsf trsf;
                trsf.SetTranslation(gp_Vec(d.x(), d.y(), d.z()));
                TopoDS_Shape copy = BRepBuilderAPI_Transform(seed, trsf, true).Shape();
                BRepAlgoAPI_Fuse fuse(result, copy);
                if (!fuse.IsDone()) throw std::runtime_error(_u8L("pattern fuse failed"));
                result = fuse.Shape();
            }
            break;
        }
        // Replicate the target body. Each copy is a rigid gp_Trsf of the seed, all
        // fused into one body. Linear: i*spacing along plane axis pattern_dir
        // (0=X,1=Y). Circular: i*(angle/count) about the plane normal through the
        // plane origin (so a seed offset from the origin orbits the axis).
        if (!have_body) throw std::runtime_error(_u8L("pattern needs a body"));
        const int n = std::max(1, f.pattern_count);
        if (n > 500) throw std::runtime_error(_u8L("pattern: at most 500 instances"));
        const TopoDS_Shape seed = result;
        for (int i = 1; i < n; ++i) {
            gp_Trsf trsf;
            if (f.pattern_circular) {
                Vec3d  o = f.plane.to_world(Vec2d(0, 0));
                gp_Ax1 ax(gp_Pnt(o.x(), o.y(), o.z()),
                          gp_Dir(f.plane.normal.x(), f.plane.normal.y(), f.plane.normal.z()));
                const bool full = std::abs(std::abs(f.pattern_angle) - 360.0) < 1e-9;
                const double div = (f.pattern_inclusive && !full && n > 1) ? double(n - 1) : double(n);
                const double step = (f.pattern_angle * M_PI / 180.0) / div;
                trsf.SetRotation(ax, step * i);
            } else {
                const Vec3d& d = (f.pattern_dir == 1) ? f.plane.y_axis : f.plane.x_axis;
                trsf.SetTranslation(gp_Vec(d.x() * f.pattern_spacing * i,
                                           d.y() * f.pattern_spacing * i,
                                           d.z() * f.pattern_spacing * i));
            }
            TopoDS_Shape copy = BRepBuilderAPI_Transform(seed, trsf, true).Shape();
            BRepAlgoAPI_Fuse fuse(result, copy);
            if (!fuse.IsDone()) throw std::runtime_error(_u8L("pattern fuse failed"));
            result = fuse.Shape();
        }
        break;
    }
    case CadFeatureType::Rib: {
        if (!have_body) throw std::runtime_error(_u8L("rib needs a body"));
        if (f.rib_sketch_ref < 0 || f.rib_sketch_ref >= (int)features.size())
            throw std::runtime_error("rib: bad sketch ref");
        const CadFeature& sk = features[f.rib_sketch_ref];
        // Project counts as sketch-like here exactly as it does for Extrude, SurfaceExtrude
        // and SurfaceRevolve: it carries plane + Line entities, which is all a rib reads.
        // Ribbing along a projected body edge is otherwise unreachable.
        if (sk.type != CadFeatureType::Sketch && sk.type != CadFeatureType::Project)
            throw std::runtime_error("rib: ref is not a sketch");
        if (f.rib_entity < 0 || f.rib_entity >= (int)sk.entities.size())
            throw std::runtime_error("rib: bad entity");
        const SketchEntity& ln = sk.entities[f.rib_entity];
        if (ln.type != SketchEntity::Type::Line)
            throw std::runtime_error(_u8L("rib: entity must be a line")); // ponytail: line-only for now
        // Thin rectangle centred on the line, in the sketch plane: offset both endpoints by
        // +/-thickness/2 along the in-plane perpendicular of the line direction.
        const SketchPlane& pl = sk.plane;
        Vec2d a = ln.p0, b = ln.p1;
        Vec2d dir = (b - a); double L = dir.norm();
        if (L < 1e-9) throw std::runtime_error(_u8L("rib: degenerate line"));
        dir /= L;
        Vec2d perp(-dir.y(), dir.x());
        double h = f.rib_thickness * 0.5;
        Vec2d q0 = a + perp*h, q1 = b + perp*h, q2 = b - perp*h, q3 = a - perp*h;
        auto w3 = [&](const Vec2d& p){ Vec3d w = pl.to_world(p); return gp_Pnt(w.x(),w.y(),w.z()); };
        BRepBuilderAPI_MakePolygon poly(w3(q0), w3(q1), w3(q2), w3(q3), true);
        if (!poly.IsDone()) throw std::runtime_error(_u8L("rib: profile failed"));
        TopoDS_Shape wall = SketchEngine::make_extrude(poly.Wire(), pl, f.rib_depth, false, 0.0);
        if (wall.IsNull()) throw std::runtime_error(_u8L("rib: extrude failed"));
        BRepAlgoAPI_Fuse fuse(result, wall);
        if (!fuse.IsDone()) throw std::runtime_error(_u8L("rib: fuse failed"));
        result = fuse.Shape();
        break;
    }
    case CadFeatureType::Fillet:
        if (!have_body) throw std::runtime_error(_u8L("fillet needs a body"));
        if (f.dressup_edge >= 0 || !f.dressup_edges.empty())
        {
            GeometryEngine::FilletLaw law;
            law.r2 = f.dressup_size2; law.chord = f.dressup_chord; law.has_start = f.dressup_has_start; law.start = f.dressup_start;
            law.radii = f.dressup_radii; law.knots = f.dressup_knots; law.fit_taper = f.dressup_fit_taper;
            result = GeometryEngine::apply_fillet(result, f.dressup_size, f.dressup_edge_ids(), law);
        }
        else
            result = GeometryEngine::apply_fillet(result, f.dressup_size, f.face_group);
        break;
    case CadFeatureType::Chamfer:
        if (!have_body) throw std::runtime_error(_u8L("chamfer needs a body"));
        if (f.dressup_edge >= 0 || !f.dressup_edges.empty())
            result = GeometryEngine::apply_chamfer(result, f.dressup_size, f.dressup_edge_ids());
        else
            result = GeometryEngine::apply_chamfer(result, f.dressup_size, f.face_group);
        break;
    case CadFeatureType::Hole: {
        if (!have_body) throw std::runtime_error(_u8L("hole needs a body"));
        // Circle wire centered at the positioned point on the plane
        Vec3d c = f.plane.to_world(Vec2d(f.hole_x, f.hole_y));
        gp_Pnt o(c.x(), c.y(), c.z());
        gp_Dir n(f.plane.normal.x(), f.plane.normal.y(), f.plane.normal.z());
        gp_Circ circ(gp_Ax2(o, n), f.hole_diameter * 0.5);
        TopoDS_Edge e = BRepBuilderAPI_MakeEdge(circ).Edge();
        BRepBuilderAPI_MakeWire wm(e);
        if (!wm.IsDone()) throw std::runtime_error(_u8L("hole wire failed"));
        // Through = symmetric huge cut (passes fully through any body);
        // Blind = +normal extrude of hole_depth into the body.
        TopoDS_Shape tool = f.hole_through
            ? SketchEngine::make_extrude(wm.Wire(), f.plane, 1.0e5, true, 0.0)
            : SketchEngine::make_extrude(wm.Wire(), f.plane, f.hole_depth, false, 0.0);
        BRepAlgoAPI_Cut cut(result, tool);
        if (!cut.IsDone()) throw std::runtime_error(_u8L("hole cut failed"));
        result = cut.Shape();

        // Drill point: a cone continuing the blind bore past its shoulder (apex h = r / tan(tip / 2) further in)
        if (!f.hole_through && f.hole_tip_angle > 1.0 && f.hole_tip_angle < 179.0) {
            const double r = f.hole_diameter * 0.5;
            const double h = r / std::tan(f.hole_tip_angle * 0.5 * M_PI / 180.0);
            const gp_Pnt base(o.X() + n.X() * f.hole_depth, o.Y() + n.Y() * f.hole_depth, o.Z() + n.Z() * f.hole_depth);
            TopoDS_Shape tip = BRepPrimAPI_MakeCone(gp_Ax2(base, n), r, 0.0, h).Shape();
            BRepAlgoAPI_Cut tc(result, tip);
            if (tc.IsDone() && !tc.Shape().IsNull()) result = tc.Shape();
        }

        // Enlarge the entry for a screw head (style 1 = counterbore, 2 = countersink).
        if (f.hole_style == 1 && f.hole_cbore_diameter > f.hole_diameter
                              && f.hole_cbore_depth > 1e-6) {
            gp_Ax2 cbax(o, n);
            TopoDS_Shape cb = BRepPrimAPI_MakeCylinder(cbax, f.hole_cbore_diameter * 0.5,
                                                       f.hole_cbore_depth).Shape();
            BRepAlgoAPI_Cut cbc(result, cb);
            if (cbc.IsDone() && !cbc.Shape().IsNull()) result = cbc.Shape();
        } else if (f.hole_style == 2 && f.hole_csink_diameter > f.hole_diameter) {
            const double Rmaj = f.hole_csink_diameter * 0.5;
            const double Rmin = f.hole_diameter * 0.5;
            const double half = f.hole_csink_angle * 0.5 * M_PI / 180.0;
            const double h = (Rmaj - Rmin) / std::tan(half);
            if (h > 1e-6) {
                gp_Ax2 csax(o, n);
                TopoDS_Shape cs = BRepPrimAPI_MakeCone(csax, Rmaj, Rmin, h).Shape();
                BRepAlgoAPI_Cut csc(result, cs);
                if (csc.IsDone() && !csc.Shape().IsNull()) result = csc.Shape();
            }
        }
        break;
    }
    case CadFeatureType::Thread: {
        // Reject degenerate parameters that make OCCT's helical sweep / boolean unstable (a tiny
        // pitch, depth >= half-pitch, an enormous turn count, depth eating the whole wall), and
        // SAY which: a thread that quietly did nothing read, for an internal one, as "the tool
        // does not intersect the body", and for an external one as nothing at all.
        {
            const double R = f.thread_radius, P = f.thread_pitch, H = f.thread_height, D = f.thread_depth;
            // ISO external thread depth is ~0.61*P, so allow up to 0.7*P (0.49 wrongly rejected
            // every real thread -> nothing rendered). Still bound it well under a full pitch.
            // R >= 0.5 (not >): M1, the smallest size in the standards table, is exactly 0.5.
            const char* why = R < 0.5                   ? L("thread: the diameter must be at least 1 mm")
                            : P <= 0.1                  ? L("thread: the pitch must be more than 0.1 mm")
                            : (D <= 1e-3 || D >= 0.7 * P) ? L("thread: the depth must be between 0 and 0.7 x the pitch")
                            : D >= 0.45 * R             ? L("thread: the depth is too large for this diameter")
                            : H <= 0.5 * P              ? L("thread: the length must be more than half a pitch")
                            : H / P >= 400.0            ? L("thread: too many turns (length / pitch must stay under 400)")
                                                        : nullptr;
            // Legacy recipes keep their old outcome (the body left as it was) so a project that
            // opened before still opens; every thread made now reports the reason instead.
            if (why != nullptr) {
                if (f.thread_major_nominal) throw std::runtime_error(Slic3r::I18N::translate(why));
                break;
            }
        }
        // Axis at the positioned point on the plane; +normal = thread rise.
        Vec3d c3 = f.plane.to_world(Vec2d(f.thread_x, f.thread_y));
        gp_Pnt  c(c3.x(), c3.y(), c3.z());
        gp_Dir  zdir(f.plane.normal.x(), f.plane.normal.y(), f.plane.normal.z());
        gp_Dir  xdir(f.plane.x_axis.x(), f.plane.x_axis.y(), f.plane.x_axis.z());
        gp_Ax3  ax3(c, zdir, xdir);
        gp_Ax2  ax2(c, zdir, xdir);
        const double R = f.thread_radius, D = f.thread_depth;
        // Root the V CLEARLY inside the material (a real overlap, not a tangency) so the boolean
        // has clean intersections — near-coincident faces are what make OCCT's fuse/cut unstable.
        const double over = std::min(std::max(D, 0.25), R * 0.4);

        // Sweep a V profile along the helix (guarded — never fatal). `helix_r` is the radius the
        // helix runs at: the profile is placed relative to it.
        auto sweep = [&](double helix_r, const TopoDS_Wire& prof, TopoDS_Shape& out) {
            try {
                TopoDS_Wire spine = make_helix_wire(ax3, helix_r, f.thread_pitch, f.thread_height);
                // MakePipeShell with a FIXED BINORMAL = cylinder axis keeps the V-profile's
                // orientation constant along the helix (axial edge always parallel to the axis).
                // The plain MakePipe used a Frenet frame that TWISTED the profile around the helix.
                BRepOffsetAPI_MakePipeShell pipe(spine);
                pipe.SetMode(zdir);
                pipe.Add(prof);
                pipe.Build();
                if (pipe.IsDone() && pipe.MakeSolid()) out = pipe.Shape();
            } catch (const Standard_Failure&) {   // on OCCT >= 8 it derives from std::exception: first
            } catch (const std::exception&) {
            }
            return !out.IsNull();
        };
        auto cut_from = [](TopoDS_Shape& target, const TopoDS_Shape& tool) {
            BRepAlgoAPI_Cut cut(target, tool);
            if (cut.IsDone() && !cut.Shape().IsNull()) target = cut.Shape();
        };

        if (f.thread_major_nominal) {
            if (f.thread_internal) {
                // Tapped hole: bore to the minor radius R - D, then cut the groove out to the
                // major radius R. The bore sits 10 um inside the minor radius so that, threading
                // an existing tap-drill hole, it does not coincide with that hole's wall.
                if (!have_body) throw std::runtime_error(_u8L("internal thread needs a body"));
                const double bore_r = R - D - 0.01;
                cut_from(result, BRepPrimAPI_MakeCylinder(ax2, bore_r, f.thread_height).Shape());
                TopoDS_Shape groove;
                if (sweep(R, make_thread_v(c, xdir, zdir, R - D - over, R, f.thread_pitch), groove))
                    cut_from(result, groove);
            } else {
                // External thread: the crests are at R. A groove is cut INTO the rod down to
                // R - D; with no body yet, the rod is made first.
                if (!have_body || result.IsNull()) {
                    result    = BRepPrimAPI_MakeCylinder(ax2, R, f.thread_height).Shape();
                    have_body = true;
                }
                TopoDS_Shape groove;
                if (sweep(R, make_thread_v(c, xdir, zdir, R + over, R - D, f.thread_pitch), groove))
                    cut_from(result, groove);
            }
            break;
        }

        // ---- legacy recipes (thread_major_nominal == false): built exactly as they always were.
        TopoDS_Shape ridge;
        const bool have_ridge = sweep(R, make_thread_profile(c, xdir, zdir, f.thread_radius,
                                                             f.thread_pitch, f.thread_depth,
                                                             f.thread_internal), ridge);
        if (f.thread_internal) {
            if (!have_body) throw std::runtime_error(_u8L("internal thread needs a body"));
            // Tapped bore: cut a clean pocket at radius - depth, then carve the helical groove
            // outward into its wall.
            const double bore_r = std::max(0.5, f.thread_radius - f.thread_depth);
            TopoDS_Shape bore = BRepPrimAPI_MakeCylinder(ax2, bore_r, f.thread_height).Shape();
            try {
                BRepAlgoAPI_Cut cut_bore(result, bore);
                if (cut_bore.IsDone() && !cut_bore.Shape().IsNull())
                    result = cut_bore.Shape();
            } catch (const std::exception&) { /* keep existing bore */ }
            if (have_ridge) cut_from(result, ridge);
        } else {
            // External thread: FUSE the helical ridge onto the existing body, or onto a
            // standalone rod when there is none.
            if (have_body && !result.IsNull()) {
                if (have_ridge) {
                    BRepAlgoAPI_Fuse fuse(result, ridge);
                    if (fuse.IsDone() && !fuse.Shape().IsNull()) result = fuse.Shape();
                }
            } else {
                TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(ax2, f.thread_radius,
                                                            f.thread_height).Shape();
                if (have_ridge) {
                    BRepAlgoAPI_Fuse fuse(rod, ridge);
                    if (fuse.IsDone()) rod = fuse.Shape();
                }
                result = rod;
                have_body = true;
            }
        }
        break;
    }
    case CadFeatureType::Shell: {
        if (!have_body) throw std::runtime_error(_u8L("shell needs a body"));
        // Hollow the body to a wall thickness; the picked face (if any) is removed so the
        // shell is open there. MakeThickSolidByJoin with a NEGATIVE offset shells inward.
        NCollection_List<TopoDS_Shape> remove;
        if (f.shell_face >= 0) {
            TopoDS_Face fc = GeometryEngine::face_by_index(result, f.shell_face);
            if (!fc.IsNull()) remove.Append(fc);
        }
        for (int id : f.shell_faces) {
            TopoDS_Face fc = GeometryEngine::face_by_index(result, id);
            if (fc.IsNull()) throw std::runtime_error(_u8L("shell: a face to open was not found"));
            if (id != f.shell_face) remove.Append(fc);
        }
        // Sharp (intersection) joins first: with the default arc joins the cavity gets rounded corners where the
        // body is concave, and the shell comes out 0.2-0.4 % light against the exact offset. Arc joins stay as the
        // fallback for bodies the intersection join cannot offset.
        TopoDS_Shape shelled;
        // (Only with an open face: a closed hollow keeps the arc joins it always had.)
        for (GeomAbs_JoinType join : {remove.IsEmpty() ? GeomAbs_Arc : GeomAbs_Intersection, GeomAbs_Arc}) {
            BRepOffsetAPI_MakeThickSolid mts;
            mts.MakeThickSolidByJoin(result, remove, -std::abs(f.shell_thickness), 1.0e-3, BRepOffset_Skin, false, false, join);
            mts.Build();
            if (mts.IsDone() && !mts.Shape().IsNull()) { shelled = mts.Shape(); break; }
        }
        if (shelled.IsNull()) throw std::runtime_error(_u8L("shell failed"));
        result = shelled;
        break;
    }
    case CadFeatureType::Draft: {
        if (!have_body) throw std::runtime_error(_u8L("draft needs a body"));
        if (f.draft_face < 0 && f.draft_faces.empty()) throw std::runtime_error(_u8L("draft needs a picked face"));
        // Neutral plane = horizontal plane through the body's bbox bottom, pull direction +Z.
        // The face pivots about the line where it meets the neutral plane and tilts by the angle.
        // ponytail: neutral plane / pull direction fixed to world up; pick-based neutral plane
        // deferred (same as the datum-plane pick types, dgv).
        Bnd_Box bb; BRepBndLib::Add(result, bb);
        double xmin, ymin, zmin, xmax, ymax, zmax;
        bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        gp_Dir pull(0, 0, 1);
        gp_Pln neutral(gp_Pnt(0, 0, zmin), pull);
        if (f.draft_has_neutral) {
            if (f.draft_pull.norm() < 1e-9) throw std::runtime_error(_u8L("draft: the pull direction is zero"));
            const Vec3d pn = f.draft_pull.normalized();
            pull    = gp_Dir(pn.x(), pn.y(), pn.z());
            neutral = gp_Pln(gp_Pnt(f.draft_neutral.x(), f.draft_neutral.y(), f.draft_neutral.z()), pull);
        }
        BRepOffsetAPI_DraftAngle draft(result);
        std::vector<int> ids = f.draft_faces;
        if (f.draft_face >= 0 && std::find(ids.begin(), ids.end(), f.draft_face) == ids.end()) ids.insert(ids.begin(), f.draft_face);
        for (int id : ids) {
            TopoDS_Face fc = GeometryEngine::face_by_index(result, id);
            if (fc.IsNull()) throw std::runtime_error(_u8L("draft: face not found"));
            draft.Add(fc, pull, f.draft_angle * M_PI / 180.0, neutral);
            if (!draft.AddDone())
                throw std::runtime_error(_u8L("draft: face cannot be drafted (is it parallel to the base?)"));
        }
        draft.Build();
        if (!draft.IsDone()) throw std::runtime_error(_u8L("draft failed"));
        result = draft.Shape();
        if (result.IsNull()) throw std::runtime_error(_u8L("draft produced no geometry"));
        break;
    }
    case CadFeatureType::DeleteFace: {
        if (!have_body) throw std::runtime_error(_u8L("delete_face needs a body"));
        if (f.delete_faces.empty()) throw std::runtime_error(_u8L("delete_face needs at least one face"));
        BRepAlgoAPI_Defeaturing df;
        df.SetShape(result);
        for (int fi : f.delete_faces) {
            TopoDS_Face fc = GeometryEngine::face_by_index(result, fi);
            if (fc.IsNull()) throw std::runtime_error(_u8L("delete_face: face not found"));
            df.AddFaceToRemove(fc);
        }
        df.Build();
        if (!df.IsDone()) throw std::runtime_error(_u8L("delete_face failed"));
        result = df.Shape();
        if (result.IsNull()) throw std::runtime_error(_u8L("delete_face produced no geometry"));
        break;
    }
    }
}

// Compound of all body shapes (1 body => that body verbatim, so single-body display and
// global face/edge ids are byte-identical to the pre-multi-body behaviour).
static TopoDS_Shape compound_of(const std::vector<CadBody>& bodies)
{
    if (bodies.size() == 1) return bodies[0].shape;
    TopoDS_Compound comp;
    BRep_Builder bld;
    bld.MakeCompound(comp);
    for (const CadBody& b : bodies)
        if (!b.shape.IsNull()) bld.Add(comp, b.shape);
    return comp;
}

// Tessellate every body separately and concatenate into one mesh, recording per-triangle
// (body index, face id WITHIN that body). Single-body => byte-identical to tessellate(body).
static TriangleMesh tessellate_bodies(const std::vector<CadBody>& bodies,
                                      std::vector<int>& tri_face, std::vector<int>& tri_body,
                                      std::vector<TriangleMesh>& body_meshes,
                                      double lin, double ang)
{
    tri_face.clear();
    tri_body.clear();
    body_meshes.clear();
    indexed_triangle_set merged;
    for (int bi = 0; bi < int(bodies.size()); ++bi) {
        std::vector<int> tf;
        TriangleMesh bm = SketchEngine::tessellate(bodies[bi].shape, tf, lin, ang);
        const indexed_triangle_set& its = bm.its;
        const int voff = int(merged.vertices.size());
        for (const auto& v : its.vertices) merged.vertices.push_back(v);
        for (const auto& t : its.indices)
            merged.indices.emplace_back(t[0] + voff, t[1] + voff, t[2] + voff);
        for (int fid : tf) { tri_face.push_back(fid); tri_body.push_back(bi); }
        body_meshes.push_back(std::move(bm));   // per-body mesh kept for distinct GLVolume colors
    }
    return TriangleMesh(merged);
}

void CadDocument::apply_boolean(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb   = int(bodies.size());
    const int tgt  = pick_body(f.target_body, nb);
    const int tool = f.bool_tool_body == -1 ? -1 : pick_body(f.bool_tool_body, nb);
    // Same rule as Cut, Mirror and Transform: an operation that cannot run says why, rather
    // than leaving the bodies as they were and reporting success.
    if (tgt < 0)     throw std::runtime_error(_u8L("boolean: no target body"));
    if (tool < 0)    throw std::runtime_error(_u8L("boolean: no tool body — pick a second body"));
    if (tgt == tool) throw std::runtime_error(_u8L("boolean: the target and the tool are the same body"));
    const TopoDS_Shape A = bodies[tgt].shape;          // target survives
    const TopoDS_Shape B = bodies[tool].shape;         // tool, consumed unless kept
    if (A.IsNull() || B.IsNull()) throw std::runtime_error(_u8L("boolean: a body has no geometry"));

    NCollection_List<TopoDS_Shape> args, tools;
    args.Append(A);
    tools.Append(B);
    auto run = [&](BRepAlgoAPI_BooleanOperation& bop) -> TopoDS_Shape {
        bop.SetArguments(args);
        bop.SetTools(tools);
        if (f.bool_tolerance > 0.0) bop.SetFuzzyValue(f.bool_tolerance);   // OCCT fuzzy: merge near-coincident faces
        bop.Build();
        if (!bop.IsDone()) throw std::runtime_error(_u8L("boolean operation failed"));
        return bop.Shape();
    };
    TopoDS_Shape result;
    switch (f.mode) {
    case BooleanMode::Add:       { BRepAlgoAPI_Fuse   op; result = run(op); break; }   // union
    case BooleanMode::Cut:       { BRepAlgoAPI_Cut    op; result = run(op); break; }   // target - tool
    case BooleanMode::Intersect: { BRepAlgoAPI_Common op; result = run(op); break; }   // overlap
    default: throw std::runtime_error("boolean: choose Union, Subtract or Intersect");   // New means nothing between two bodies
    }
    if (result.IsNull()) throw std::runtime_error(_u8L("boolean produced an empty shape"));

    bodies[tgt].shape = result;
    if (!f.bool_keep_tool) bodies.erase(bodies.begin() + tool);   // consume the tool body
}

void CadDocument::apply_cut(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb  = int(bodies.size());
    if (nb == 0) throw std::runtime_error(_u8L("cut: no target body"));
    const int tgt = pick_body(f.target_body, nb);
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error(_u8L("cut: no target body"));

    if (!f.cut_keep_upper && !f.cut_keep_lower)
        throw std::runtime_error(_u8L("cut keeps nothing"));

    SketchPlane cp;
    if (f.cut_face >= 0) {
        const int fb = f.cut_face_body == -1 ? tgt : pick_body(f.cut_face_body, nb);
        if (bodies[fb].shape.IsNull()) throw std::runtime_error(_u8L("cut: face body is empty"));
        TopoDS_Face fc = GeometryEngine::face_by_index(bodies[fb].shape, f.cut_face);
        if (fc.IsNull()) throw std::runtime_error(_u8L("cut: face not found"));
        cp = SketchPlane::from_face(fc);
    } else {
        cp = f.plane;
    }
    cp.origin += cp.normal * f.cut_offset;
    if (f.cut_flip) cp.normal = -cp.normal;

    // Build a large square wire in the cut plane, centered at plane origin.
    const double L = 1.0e5;
    Vec3d x = cp.x_axis * L;
    Vec3d y = cp.y_axis * L;
    Vec3d o = cp.origin;
    auto p = [&](double sx, double sy) {
        Vec3d v = o + x * sx + y * sy;
        return gp_Pnt(v.x(), v.y(), v.z());
    };
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(p( 1,  1));
    poly.Add(p( 1, -1));
    poly.Add(p(-1, -1));
    poly.Add(p(-1,  1));
    poly.Close();
    if (!poly.IsDone()) throw std::runtime_error(_u8L("cut: failed to build cut wire"));
    TopoDS_Wire wire = poly.Wire();

    TopoDS_Shape upper_piece, lower_piece;
    const TopoDS_Shape& target = bodies[tgt].shape;

    if (f.cut_keep_upper) {
        TopoDS_Shape upper_tool = SketchEngine::make_extrude(wire, cp, L, false, 0.0);
        BRepAlgoAPI_Common common(target, upper_tool);
        if (!common.IsDone()) throw std::runtime_error(_u8L("cut operation failed"));
        upper_piece = common.Shape();
    }

    if (f.cut_keep_lower) {
        SketchPlane lp = cp;
        lp.normal = -lp.normal;
        TopoDS_Shape lower_tool = SketchEngine::make_extrude(wire, lp, L, false, 0.0);
        BRepAlgoAPI_Common common(target, lower_tool);
        if (!common.IsDone()) throw std::runtime_error(_u8L("cut operation failed"));
        lower_piece = common.Shape();
    }

    if (f.cut_keep_upper && f.cut_keep_lower) {
        bodies[tgt].shape = upper_piece;
        bodies.insert(bodies.begin() + tgt + 1,
                      CadBody{ lower_piece, bodies[tgt].name + " (2)" });
    } else if (f.cut_keep_upper) {
        bodies[tgt].shape = upper_piece;
    } else {
        bodies[tgt].shape = lower_piece;
    }
}

void CadDocument::apply_mirror(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb  = int(bodies.size());
    if (nb == 0) throw std::runtime_error(_u8L("mirror: no target body"));
    const int tgt = pick_body(f.target_body, nb);
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error(_u8L("mirror: no target body"));

    const TopoDS_Shape& src = bodies[tgt].shape;

    gp_Trsf trsf;
    trsf.SetMirror(gp_Ax2(gp_Pnt(f.plane.origin.x(), f.plane.origin.y(), f.plane.origin.z()),
                          gp_Dir(f.plane.normal.x(), f.plane.normal.y(), f.plane.normal.z())));
    BRepBuilderAPI_Transform xform(src, trsf, true /*copy*/);
    if (!xform.IsDone()) throw std::runtime_error(_u8L("mirror: transform failed"));
    TopoDS_Shape mirrored = xform.Shape();

    // A mirror reverses orientation — verify the result has positive volume.
    {
        GProp_GProps props;
        BRepGProp::VolumeProperties(mirrored, props);
        if (props.Mass() <= 0.0) {
            // Flip orientation to get a valid forward solid.
            mirrored.Reverse();
            BRepGProp::VolumeProperties(mirrored, props);
            if (props.Mass() <= 0.0)
                throw std::runtime_error(_u8L("mirror: result has zero or negative volume"));
        }
    }

    switch (f.mode) {
    case BooleanMode::Add: {
        BRepAlgoAPI_Fuse fuse(src, mirrored);
        if (!fuse.IsDone()) throw std::runtime_error(_u8L("mirror fuse failed"));
        bodies[tgt].shape = fuse.Shape();
        break;
    }
    case BooleanMode::New: {
        if (!f.mirror_keep_original)
            bodies.erase(bodies.begin() + tgt);   // replace: the mirrored copy takes the source slot
        bodies.push_back({mirrored, f.name.empty() ? _u8L("Mirror") : f.name});
        break;
    }
    default:
        throw std::runtime_error("mirror: mode must be New or Add");
    }
}

void CadDocument::apply_transform(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb  = int(bodies.size());
    if (nb == 0) throw std::runtime_error(_u8L("transform: no target body"));
    const int tgt = pick_body(f.target_body, nb);
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error(_u8L("transform: no target body"));

    gp_Trsf rot;
    if (std::abs(f.xf_angle_deg) > 1e-12) {
        if (f.xf_axis.norm() < 1e-9)
            throw std::runtime_error(_u8L("transform: rotation axis is degenerate"));
        rot.SetRotation(gp_Ax1(gp_Pnt(f.xf_pivot.x(), f.xf_pivot.y(), f.xf_pivot.z()),
                               gp_Dir(f.xf_axis.x(), f.xf_axis.y(), f.xf_axis.z())),
                        f.xf_angle_deg * M_PI / 180.0);
    }
    gp_Trsf tr;
    tr.SetTranslation(gp_Vec(f.xf_translate.x(), f.xf_translate.y(), f.xf_translate.z()));
    const gp_Trsf trsf = tr * rot;          // rotate first, then translate

    BRepBuilderAPI_Transform xform(bodies[tgt].shape, trsf, true /*copy*/);
    if (!xform.IsDone()) throw std::runtime_error(_u8L("transform: failed"));
    TopoDS_Shape moved = xform.Shape();

    if (f.xf_copy)
        bodies.push_back({moved, f.name.empty() ? _u8L("Transform") : f.name});
    else
        bodies[tgt].shape = moved;
}

void CadDocument::apply_thicken(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb = int(bodies.size());
    if (nb == 0) throw std::runtime_error(_u8L("thicken: no target body"));
    const int tgt = pick_body(f.target_body, nb);
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error(_u8L("thicken: no target body"));

    TopoDS_Face fc = GeometryEngine::face_by_index(bodies[tgt].shape, f.thicken_face);
    if (fc.IsNull()) throw std::runtime_error(_u8L("thicken: face not found"));
    if (std::abs(f.thicken_thickness) < 1e-9) throw std::runtime_error(_u8L("thicken: thickness is zero"));

    TopoDS_Shell shell;
    BRep_Builder bb;
    bb.MakeShell(shell);
    bb.Add(shell, fc);

    const double off = f.thicken_flip ? -std::abs(f.thicken_thickness)
                                      :  std::abs(f.thicken_thickness);
    BRepOffsetAPI_MakeThickSolid mts;
    mts.MakeThickSolidBySimple(shell, off);
    mts.Build();
    if (!mts.IsDone()) throw std::runtime_error(_u8L("thicken: failed"));
    TopoDS_Shape solid = mts.Shape();
    if (solid.IsNull()) throw std::runtime_error(_u8L("thicken: produced no geometry"));

    // MakeThickSolidBySimple may produce a reversed solid. Ensure positive volume.
    {
        GProp_GProps props;
        BRepGProp::VolumeProperties(solid, props);
        if (props.Mass() < 0.0) solid.Reverse();
    }

    bodies.push_back({solid, f.name.empty() ? _u8L("Thicken") : f.name});
}

void CadDocument::apply_thicken_surface(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb = int(bodies.size());
    if (nb == 0) throw std::runtime_error(_u8L("thicken-surface: no target body"));
    const int tgt = pick_body(f.target_body, nb);
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error(_u8L("thicken-surface: no target body"));
    if (!is_sheet_shape(bodies[tgt].shape)) throw std::runtime_error(_u8L("thicken-surface: target is not a sheet body"));
    if (std::abs(f.thicken_thickness) < 1e-9) throw std::runtime_error(_u8L("thicken-surface: thickness is zero"));

    const double off = f.thicken_flip ? -std::abs(f.thicken_thickness)
                                      :  std::abs(f.thicken_thickness);
    const TopoDS_Shape& sheet = bodies[tgt].shape;

    // MakeThickSolidBySimple offsets each face along its own normal and sews the result; it never
    // extends neighbours to meet, so at every edge where two faces join at an angle the corner
    // material is simply absent. A flat sheet is therefore exact while a 4-walled box is not
    // (measured: 29648 against the 44000 that (60^2-50^2)*40 requires).
    //
    // ByJoin is the call that mitres those corners, and it is what the Shell feature already uses
    // successfully a few hundred lines up — but it CANNOT be handed an open sheet. In OCCT,
    // BRepOffset_MakeOffset::MakeThickSolid builds the solid only inside `if (!myFaces.IsEmpty())`
    // (BRepOffset_MakeOffset.cxx:1115): with no closing faces it stops after the offset shell and
    // returns that. So it reports IsDone() and a non-null shape containing no TopAbs_SOLID at all
    // — which is exactly how two earlier attempts failed, and why no parameter could have fixed it.
    //
    // So close the sheet first, then use the working call: cap the free rims, sew into a closed
    // shell, make a solid, and hollow it inward passing the caps as the faces to remove. The caps
    // come back off, leaving the wall.
    // A SINGLE face has no edge shared with a neighbour, so there is no corner to mitre and
    // BySimple is already exact on it (flat 60x60 by 5 -> 18000.000). It also has a free
    // boundary, so "does it have free wires" is NOT the question to ask here — capping a lone
    // face with its own rim sews a zero-thickness shell and the offset then measures 6000.
    int n_faces = 0;
    for (TopExp_Explorer fe(sheet, TopAbs_FACE); fe.More(); fe.Next()) ++n_faces;

    NCollection_List<TopoDS_Shape> caps;
    if (n_faces > 1) {
        ShapeAnalysis_FreeBounds fb(sheet);
        for (TopExp_Explorer we(fb.GetClosedWires(), TopAbs_WIRE); we.More(); we.Next()) {
            BRepBuilderAPI_MakeFace mk(TopoDS::Wire(we.Current()));
            if (!mk.IsDone())
                throw std::runtime_error(_u8L("thicken-surface: cannot cap the sheet rim "
                                              "(is it planar?)"));
            caps.Append(mk.Face());
        }
    }

    TopoDS_Shape solid;
    if (caps.IsEmpty()) {
        BRepOffsetAPI_MakeThickSolid mts;
        mts.MakeThickSolidBySimple(sheet, off);
        mts.Build();
        if (!mts.IsDone()) throw std::runtime_error(_u8L("thicken-surface: failed"));
        solid = mts.Shape();
    } else {
        BRepBuilderAPI_Sewing sewer(1.0e-3);
        sewer.Add(sheet);
        for (NCollection_List<TopoDS_Shape>::Iterator it(caps); it.More(); it.Next())
            sewer.Add(it.Value());
        sewer.Perform();

        TopoDS_Shell closed_shell;
        for (TopExp_Explorer se(sewer.SewedShape(), TopAbs_SHELL); se.More(); se.Next()) {
            if (!closed_shell.IsNull())
                throw std::runtime_error(_u8L("thicken-surface: the capped sheet split into "
                                              "more than one shell"));
            closed_shell = TopoDS::Shell(se.Current());
        }
        if (closed_shell.IsNull() || !BRep_Tool::IsClosed(closed_shell))
            throw std::runtime_error(_u8L("thicken-surface: the capped sheet is not closed"));

        // A shell sewn from an extruded sheet carries no guarantee that its faces point outward,
        // and BRepBuilderAPI_MakeSolid does not fix that. Offsetting an inside-out solid sends the
        // wall the wrong way: measured bbox 70x70x40 for an inward offset on a 60x60 box, with a
        // volume larger than its own bounding box because the result then overlaps itself. A
        // negative mass IS the inverted orientation, so use it as the test.
        TopoDS_Solid capped = BRepBuilderAPI_MakeSolid(closed_shell).Solid();
        {
            GProp_GProps vp;
            BRepGProp::VolumeProperties(capped, vp);
            if (vp.Mass() < 0.0) capped.Reverse();
        }

        BRepOffsetAPI_MakeThickSolid mts;
        mts.MakeThickSolidByJoin(capped, caps, -std::abs(off), 1.0e-3);
        mts.Build();
        if (!mts.IsDone()) throw std::runtime_error(_u8L("thicken-surface: failed"));
        solid = mts.Shape();
    }
    if (solid.IsNull()) throw std::runtime_error(_u8L("thicken-surface: produced no geometry"));
    // IsDone() is NOT a success test here — the failed attempts had IsDone() true and no solid.
    if (!TopExp_Explorer(solid, TopAbs_SOLID).More())
        throw std::runtime_error(_u8L("thicken-surface: produced a shell, not a solid"));

    {
        GProp_GProps props;
        BRepGProp::VolumeProperties(solid, props);
        if (props.Mass() < 0.0) solid.Reverse();
    }

    bodies.push_back({solid, f.name.empty() ? _u8L("Thicken Surface") : f.name});
}

void CadDocument::apply_surface_offset(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb = int(bodies.size());
    if (nb == 0) throw std::runtime_error(_u8L("surface-offset: no target body"));
    const int tgt = pick_body(f.target_body, nb);
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error(_u8L("surface-offset: no target body"));
    if (!is_sheet_shape(bodies[tgt].shape)) throw std::runtime_error(_u8L("surface-offset: target is not a sheet body"));
    const double d = f.plane_offset;
    if (std::abs(d) < 1e-9) throw std::runtime_error(_u8L("surface-offset: zero offset"));

    BRepOffsetAPI_MakeOffsetShape mos;
    mos.PerformBySimple(bodies[tgt].shape, d);
    if (!mos.IsDone()) throw std::runtime_error(_u8L("surface-offset: failed"));
    TopoDS_Shape off_shape = mos.Shape();
    if (off_shape.IsNull()) throw std::runtime_error(_u8L("surface-offset: produced no geometry"));

    bodies.push_back({off_shape, f.name.empty() ? _u8L("Surface Offset") : f.name});
}

// Project `edges` of a shape onto `plane`, appending the resulting 2D entities to `out`.
// `construction` marks them as guides rather than built geometry. This is the body of the
// Project feature's conversion loop, factored out so a sketch can borrow the same geometry.
static void project_edges_to_entities(const std::vector<TopoDS_Edge>& edges,
                                      const SketchPlane& plane,
                                      bool construction,
                                      std::vector<SketchEntity>& out)
{
    auto to2d = [&](const gp_Pnt& p) -> Vec2d {
        Vec3d d(p.X() - plane.origin.x(), p.Y() - plane.origin.y(), p.Z() - plane.origin.z());
        return Vec2d(d.dot(plane.x_axis), d.dot(plane.y_axis));
    };

    // A segment whose endpoints coincide after projection carries no geometry: that is what
    // an edge perpendicular to the target plane becomes. Emitting it as a zero-length line
    // would poison the sketch downstream, so drop it here.
    auto push_line = [&](const Vec2d& a, const Vec2d& b) {
        if ((b - a).norm() < 1e-7) return;
        SketchEntity se; se.type = SketchEntity::Type::Line;
        se.p0 = a; se.p1 = b;
        se.construction = construction;
        out.push_back(se);
    };

    for (const TopoDS_Edge& e : edges) {
        BRepAdaptor_Curve ac(e);
        const GeomAbs_CurveType ct = ac.GetType();
        if (ct == GeomAbs_Line) {
            gp_Pnt a = ac.Value(ac.FirstParameter());
            gp_Pnt b = ac.Value(ac.LastParameter());
            push_line(to2d(a), to2d(b));
        } else if (ct == GeomAbs_Circle) {
            gp_Circ c = ac.Circle();
            gp_Dir cn = c.Axis().Direction();
            Vec3d cnv(cn.X(), cn.Y(), cn.Z());
            const double par = std::abs(cnv.dot(plane.normal));
            const bool full = BRep_Tool::IsClosed(e) ||
                std::abs((ac.LastParameter() - ac.FirstParameter()) - 2.0 * M_PI) < 1e-6;
            if (par > 0.999) {
                Vec2d ctr = to2d(c.Location());
                if (full) {
                    SketchEntity se; se.type = SketchEntity::Type::Circle;
                    se.center = ctr; se.radius = c.Radius();
                    se.construction = construction;
                    out.push_back(se);
                } else {
                    gp_Pnt a = ac.Value(ac.FirstParameter());
                    gp_Pnt b = ac.Value(ac.LastParameter());
                    Vec2d a2 = to2d(a), b2 = to2d(b);
                    SketchEntity se; se.type = SketchEntity::Type::Arc;
                    se.center = ctr; se.radius = c.Radius();
                    se.p0 = a2; se.p1 = b2;
                    se.start_angle = std::atan2(a2.y() - ctr.y(), a2.x() - ctr.x());
                    se.end_angle   = std::atan2(b2.y() - ctr.y(), b2.x() - ctr.x());
                    se.construction = construction;
                    out.push_back(se);
                }
                continue;
            }
            std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(e);
            for (size_t i = 1; i < pts.size(); ++i)
                push_line(to2d(gp_Pnt(pts[i-1].x(), pts[i-1].y(), pts[i-1].z())),
                          to2d(gp_Pnt(pts[i].x(),   pts[i].y(),   pts[i].z())));
        } else {
            std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(e);
            for (size_t i = 1; i < pts.size(); ++i)
                push_line(to2d(gp_Pnt(pts[i-1].x(), pts[i-1].y(), pts[i-1].z())),
                          to2d(gp_Pnt(pts[i].x(),   pts[i].y(),   pts[i].z())));
        }
    }
}

// Orthogonal projection of another sketch's entities onto this feature's plane. Lines, points and
// spline poles project exactly onto any plane; circles and arcs only onto a parallel one (they
// would become ellipses), and ellipses are not carried yet. Anything it cannot carry is an Error,
// never a quietly dropped entity.
void CadDocument::apply_project_from_sketch(CadFeature& f, size_t fi) const
{
    f.entities.clear();
    const int src = f.project_source_sketch;
    if (src < 0 || src >= int(fi) || src >= int(features.size()))
        throw std::runtime_error(_u8L("use: the source sketch is missing"));
    const CadFeature& s = features[src];
    if ((s.type != CadFeatureType::Sketch && s.type != CadFeatureType::Project) || !s.enabled)
        throw std::runtime_error(_u8L("use: the source is not an enabled sketch"));
    if (s.entities.empty())
        throw std::runtime_error(_u8L("use: the source sketch has no entities"));

    const SketchPlane& sp = s.plane;
    const SketchPlane& dp = f.plane;
    auto frame_normal = [](const SketchPlane& p) { return p.x_axis.cross(p.y_axis).normalized(); };
    const double facing = frame_normal(sp).dot(frame_normal(dp));
    const bool   parallel = std::abs(facing) > 1.0 - 1e-9;
    auto to_dst = [&](const Vec2d& uv) {
        const Vec3d w = sp.to_world(uv) - dp.origin;
        return Vec2d(w.dot(dp.x_axis), w.dot(dp.y_axis));
    };

    std::vector<int> pick = f.project_sketch_entities;
    if (pick.empty())
        for (int i = 0; i < int(s.entities.size()); ++i) pick.push_back(i);
    for (int idx : pick) {
        if (idx < 0 || idx >= int(s.entities.size()))
            throw std::runtime_error(_u8L("use: a source entity no longer exists"));
        SketchEntity e = s.entities[idx];
        switch (e.type) {
        case SketchEntity::Type::Line:
            e.p0 = to_dst(e.p0); e.p1 = to_dst(e.p1);
            if ((e.p1 - e.p0).norm() < 1e-7) continue;   // perpendicular to the target: no geometry
            break;
        case SketchEntity::Type::Point:
            e.p0 = to_dst(e.p0); break;
        case SketchEntity::Type::BSpline:
            for (Vec2d& c : e.ctrl) c = to_dst(c);
            e.p0 = to_dst(e.p0); e.p1 = to_dst(e.p1);
            break;
        case SketchEntity::Type::Circle:
        case SketchEntity::Type::Arc: {
            if (!parallel)
                throw std::runtime_error(_u8L("use: a circle or arc cannot be projected onto a plane that is not parallel to its sketch"));
            const Vec2d c = to_dst(e.center);
            if (e.type == SketchEntity::Type::Arc) {
                Vec2d a = to_dst(e.p0), b = to_dst(e.p1);
                if (facing < 0) std::swap(a, b);          // a mirrored frame reverses the sweep
                e.p0 = a; e.p1 = b;
                e.start_angle = std::atan2(a.y() - c.y(), a.x() - c.x());
                e.end_angle   = std::atan2(b.y() - c.y(), b.x() - c.x());
            } else {
                e.p0 = c;
            }
            e.center = c;
            break;
        }
        default:
            throw std::runtime_error(_u8L("use: ellipses cannot be projected yet"));
        }
        f.entities.push_back(e);
    }
    if (f.entities.empty()) throw std::runtime_error(_u8L("use: produced no entities"));
}

void CadDocument::apply_project(const std::vector<CadBody>& bodies, CadFeature& f, size_t fi) const
{
    if (f.project_from_sketch) { apply_project_from_sketch(f, fi); return; }
    f.entities.clear();
    const int nb = int(bodies.size());
    if (nb == 0) throw std::runtime_error(_u8L("project: no source body"));
    const int src = pick_body(f.project_source_body, nb);
    if (src < 0 || bodies[src].shape.IsNull()) throw std::runtime_error(_u8L("project: source body is empty"));
    const TopoDS_Shape& shape = bodies[src].shape;

    std::vector<TopoDS_Edge> edges;
    if (!f.project_edges.empty()) {
        for (int id : f.project_edges) {
            TopoDS_Edge e = GeometryEngine::edge_by_index(shape, id);
            if (e.IsNull()) throw std::runtime_error(_u8L("project: edge not found"));
            edges.push_back(e);
        }
    } else if (f.project_face >= 0) {
        TopoDS_Face fc = GeometryEngine::face_by_index(shape, f.project_face);
        if (fc.IsNull()) throw std::runtime_error(_u8L("project: face not found"));
        edges = GeometryEngine::edges_of_face(fc);
    } else {
        // No face and no explicit selection means "all edges" — the state the Project card
        // starts in, and its label says so. Edges perpendicular to the target plane collapse
        // to a point when projected; they are dropped below rather than emitted as
        // zero-length lines.
        edges = GeometryEngine::edges_of(shape);
    }
    if (edges.empty()) throw std::runtime_error(_u8L("project: no edges to project"));

    project_edges_to_entities(edges, f.plane, /*construction=*/false, f.entities);

    if (f.entities.empty()) throw std::runtime_error(_u8L("project: produced no entities"));
}

void CadDocument::detect_mate_conflicts()
{
    mate_conflicts.clear();

    const int n = int(features.size());
    std::map<int, int> first_driver; // body -> feature index of first mate that drives it
    std::map<int, std::vector<int>> graph; // dst -> list of src (body indices)

    for (int fi = 0; fi < n; ++fi) {
        const CadFeature& f = features[fi];
        if (f.type != CadFeatureType::Mate || !f.enabled) continue;

        if (f.mate_cs_a < 0 || f.mate_cs_a >= n) continue;
        if (f.mate_cs_b < 0 || f.mate_cs_b >= n) continue;
        const CadFeature& fa = features[f.mate_cs_a];
        const CadFeature& fb = features[f.mate_cs_b];
        if (fa.type != CadFeatureType::CoordSys || !fa.enabled) continue;
        if (fb.type != CadFeatureType::CoordSys || !fb.enabled) continue;

        const int src = fa.coordsys_body;
        const int dst = fb.coordsys_body;
        if (dst < 0) continue; // apply_mate already reports this one

        // (a) duplicate target: a second mate driving the same body
        auto it = first_driver.find(dst);
        if (it != first_driver.end()) {
            int first_fi = it->second;
            std::string first_name = features[first_fi].name.empty() ? _u8L("Mate") : features[first_fi].name;
            std::string this_name = f.name.empty() ? _u8L("Mate") : f.name;
            mate_conflicts.push_back({fi,
                format(_u8L("Body %1% is already positioned by \"%2%\" (feature %3%), so \"%4%\" overrides it"),
                       dst + 1, first_name, first_fi + 1, this_name)});
        } else {
            first_driver[dst] = fi;
        }

        // (b) self-mate or cycle detection
        if (src >= 0 && dst >= 0) {
            if (src == dst) {
                mate_conflicts.push_back({fi,
                    format(_u8L("This mate positions Body %1% against itself"), dst + 1)});
            } else {
                graph[dst].push_back(src);
            }
        }
    }

    // DFS cycle detection on the graph built above.
    // ponytail: iterative DFS avoids recursion depth issues on long chains.
    // Colour: 0 = unvisited, 1 = in-progress (grey), 2 = done (black).
    std::map<int, int> colour;
    struct Frame { int node; size_t next; };
    std::vector<Frame> stack;

    for (const auto& [start, _] : graph) {
        if (colour[start] == 2) continue;
        stack.clear();
        stack.push_back({start, 0});
        colour[start] = 1;
        while (!stack.empty()) {
            Frame& top = stack.back();
            auto git = graph.find(top.node);
            if (git == graph.end() || top.next >= git->second.size()) {
                colour[top.node] = 2;
                stack.pop_back();
                continue;
            }
            int child = git->second[top.next++];
            if (colour[child] == 1) {
                // Back edge found — find the mate whose (dst==top.node, src==child).
                for (int fi = 0; fi < n; ++fi) {
                    const CadFeature& f = features[fi];
                    if (f.type != CadFeatureType::Mate || !f.enabled) continue;
                    if (f.mate_cs_a < 0 || f.mate_cs_a >= n) continue;
                    if (f.mate_cs_b < 0 || f.mate_cs_b >= n) continue;
                    const CadFeature& fa2 = features[f.mate_cs_a];
                    const CadFeature& fb2 = features[f.mate_cs_b];
                    if (fa2.type != CadFeatureType::CoordSys || !fa2.enabled) continue;
                    if (fb2.type != CadFeatureType::CoordSys || !fb2.enabled) continue;
                    int sd = fb2.coordsys_body;
                    int ss = fa2.coordsys_body;
                    if (sd == top.node && ss == child) {
                        mate_conflicts.push_back({fi,
                            // Worded for ANY cycle length: "leads back" is true transitively,
                            // where "depends back on" would be a lie for a 3+ body chain.
                            format(_u8L("Mate chain is circular: Body %1% depends on Body %2%, which leads "
                                        "back to Body %1% — the result depends on feature order"),
                                   top.node + 1, child + 1)});
                        break;
                    }
                }
                continue;
            }
            if (colour[child] == 0) {
                colour[child] = 1;
                stack.push_back({child, 0});
            }
        }
    }

    // Deliberately NOT in scope: computing the numeric disagreement between two mates
    // ("Mate3 puts it at X=10, Mate7 at X=15"). That needs speculative per-mate evaluation.
}

void CadDocument::apply_mate(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nc = int(features.size());
    if (f.mate_cs_a < 0 || f.mate_cs_a >= nc)
        throw std::runtime_error("mate: mate_cs_a out of range");
    if (f.mate_cs_b < 0 || f.mate_cs_b >= nc)
        throw std::runtime_error("mate: mate_cs_b out of range");

    const CadFeature& fa = features[f.mate_cs_a];
    const CadFeature& fb = features[f.mate_cs_b];
    if (fa.type != CadFeatureType::CoordSys || !fa.enabled)
        throw std::runtime_error("mate: mate_cs_a is not a valid CoordSys feature");
    if (fb.type != CadFeatureType::CoordSys || !fb.enabled)
        throw std::runtime_error("mate: mate_cs_b is not a valid CoordSys feature");

    CadDocument::DatumCoordSys A = datum_frame(bodies, fa);
    CadDocument::DatumCoordSys B = datum_frame(bodies, fb);
    if (!A.error.empty()) throw std::runtime_error(format(_u8L("mate: %1%"), A.error));
    if (!B.error.empty()) throw std::runtime_error(format(_u8L("mate: %1%"), B.error));

    const int tgt_body = fb.coordsys_body;
    if (tgt_body < 0)
        throw std::runtime_error("mate: mate_cs_b has no associated body");
    if (tgt_body >= int(bodies.size()) || bodies[tgt_body].shape.IsNull())
        throw std::runtime_error("mate: target body out of range or null");

    Vec3d xA(A.x), yA(A.y), oA(A.origin);
    Vec3d zA = xA.cross(yA).normalized();
    Vec3d xB(B.x), yB(B.y), oB(B.origin);
    Vec3d zB = xB.cross(yB).normalized();

    auto make_4x4 = [&](const Vec3d& x, const Vec3d& y, const Vec3d& z, const Vec3d& o) {
        gp_Trsf T;
        T.SetValues(x.x(), y.x(), z.x(), o.x(),
                    x.y(), y.y(), z.y(), o.y(),
                    x.z(), y.z(), z.z(), o.z());
        return T;
    };
    gp_Trsf M_A = make_4x4(xA, yA, zA, oA);
    gp_Trsf M_B = make_4x4(xB, yB, zB, oB);

    gp_Trsf F;
    if (f.mate_flip) {
        // Rx(pi): flip y→-y, z→-z
        F.SetValues(1,  0,  0, 0,
                    0, -1,  0, 0,
                    0,  0, -1, 0);
    }

    if (f.mate_kind < 0 || f.mate_kind > 4)
        throw std::runtime_error("mate: unknown mate type " + std::to_string(f.mate_kind));
    gp_Trsf T;
    if (f.mate_kind == 0) {
        // Fastened: T = M_A * Rz(mate_angle) * Tz(mate_offset) * F * M_B^-1
        gp_Trsf Rz;
        Rz.SetRotation(gp_Ax1(gp_Pnt(0,0,0), gp_Dir(0,0,1)), f.mate_angle * M_PI / 180.0);
        gp_Trsf Tz;
        Tz.SetTranslation(gp_Vec(0, 0, f.mate_offset));
        gp_Trsf M_B_inv = M_B.Inverted();
        T = M_A * Rz * Tz * F * M_B_inv;
    } else {
        Vec3d z_target = f.mate_flip ? -zA : zA;

        // Minimum-rotation helper: compute R that rotates zB onto z_target
        // about an axis through oB. Reused by Planar / Revolute / Cylindrical.
        auto make_z_align = [&](const Vec3d& zsrc, const Vec3d& zdst) -> gp_Trsf {
            double ddot = zsrc.dot(zdst);
            Vec3d rot_axis;
            double rot_angle = 0;
            if (ddot <= -0.9999) {
                Vec3d ref = (std::abs(zsrc.z()) < 0.9) ? Vec3d(0, 0, 1) : Vec3d(1, 0, 0);
                rot_axis = zsrc.cross(ref).normalized();
                rot_angle = M_PI;
            } else {
                rot_axis = zsrc.cross(zdst);
                if (rot_axis.squaredNorm() > 1e-18) {
                    rot_axis.normalize();
                    rot_angle = std::acos(std::max(-1.0, std::min(1.0, ddot)));
                }
            }
            gp_Trsf R;
            if (rot_angle > 1e-12) {
                R.SetRotation(gp_Ax1(gp_Pnt(oB.x(), oB.y(), oB.z()),
                                     gp_Dir(rot_axis.x(), rot_axis.y(), rot_axis.z())),
                              rot_angle);
            }
            return R;
        };

        if (f.mate_kind == 1 || f.mate_kind == 2 || f.mate_kind == 4) {
            // Planar (1) / Revolute (2) / Cylindrical (4):
            // all share the same minimum-rotation z-alignment.
            gp_Trsf R_align = make_z_align(zB, z_target);

            gp_Trsf Rz_about_target;
            if (std::abs(f.mate_angle) > 1e-12) {
                Rz_about_target.SetRotation(
                    gp_Ax1(gp_Pnt(oB.x(), oB.y(), oB.z()),
                           gp_Dir(z_target.x(), z_target.y(), z_target.z())),
                    f.mate_angle * M_PI / 180.0);
            }
            gp_Trsf R = Rz_about_target * R_align;

            // Translation: depends on which DOFs are constrained
            Vec3d trans;
            if (f.mate_kind == 1) {
                // Planar: normal distance becomes mate_offset
                double d = (oB - oA).dot(zA);
                trans = zA * (f.mate_offset - d);
            } else if (f.mate_kind == 2) {
                // Revolute: full position on the axis line
                trans = (oA + zA * f.mate_offset) - oB;
            } else {
                // Cylindrical (4): fix perpendicular, preserve axial
                double axial = (oB - oA).dot(zA);
                trans = (oA + zA * (axial + f.mate_offset)) - oB;
            }

            T.SetValues(1, 0, 0, trans.x(),
                        0, 1, 0, trans.y(),
                        0, 0, 1, trans.z());
            T = T * R;
        } else {
            // Slider (mate_kind == 3): full orientation alignment,
            // fix perpendicular position, preserve axial translation.
            Vec3d x_target = xA;
            Vec3d y_target = f.mate_flip ? -yA : yA;

            // R = target * B^T  (both bases orthonormal)
            double r11 = x_target.x() * xB.x() + y_target.x() * yB.x() + z_target.x() * zB.x();
            double r12 = x_target.x() * xB.y() + y_target.x() * yB.y() + z_target.x() * zB.y();
            double r13 = x_target.x() * xB.z() + y_target.x() * yB.z() + z_target.x() * zB.z();
            double r21 = x_target.y() * xB.x() + y_target.y() * yB.x() + z_target.y() * zB.x();
            double r22 = x_target.y() * xB.y() + y_target.y() * yB.y() + z_target.y() * zB.y();
            double r23 = x_target.y() * xB.z() + y_target.y() * yB.z() + z_target.y() * zB.z();
            double r31 = x_target.z() * xB.x() + y_target.z() * yB.x() + z_target.z() * zB.x();
            double r32 = x_target.z() * xB.y() + y_target.z() * yB.y() + z_target.z() * zB.y();
            double r33 = x_target.z() * xB.z() + y_target.z() * yB.z() + z_target.z() * zB.z();

            // Build rotation about oB: R_full * p = R * (p - oB) + oB
            double tx = oB.x() - (r11 * oB.x() + r12 * oB.y() + r13 * oB.z());
            double ty = oB.y() - (r21 * oB.x() + r22 * oB.y() + r23 * oB.z());
            double tz = oB.z() - (r31 * oB.x() + r32 * oB.y() + r33 * oB.z());
            gp_Trsf R_full;
            R_full.SetValues(r11, r12, r13, tx,
                             r21, r22, r23, ty,
                             r31, r32, r33, tz);

            double axial = (oB - oA).dot(zA);
            Vec3d trans = (oA + zA * (axial + f.mate_offset)) - oB;

            T.SetValues(1, 0, 0, trans.x(),
                        0, 1, 0, trans.y(),
                        0, 0, 1, trans.z());
            T = T * R_full;
        }
    }

    BRepBuilderAPI_Transform xform(bodies[tgt_body].shape, T, true /*copy*/);
    if (!xform.IsDone()) throw std::runtime_error(_u8L("mate: transform failed"));
    bodies[tgt_body].shape = xform.Shape();
}

// Volume of a shape, 0 for anything that isn't a solid we can measure. Used to catch a
// subtraction that removed nothing (daf).
static double solid_volume(const TopoDS_Shape& s)
{
    if (s.IsNull()) return 0.0;
    GProp_GProps props;
    BRepGProp::VolumeProperties(s, props);
    return std::abs(props.Mass());
}

// Repeat what earlier features did to a body. The features' effect is measured, not re-run: `removed` is the material the
// first..last listed feature took away (state before minus state after), `added` the material they put on. Each instance moves
// both by a rigid transform; one cut and one fuse then apply them all, so overlapping instances merge instead of fighting.
void CadDocument::apply_feature_pattern(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    // During a recompute the state before THIS feature was just recorded (it is the last entry); in a preview the candidate is not
    // recorded yet and the bodies passed in ARE that state - valid only while the recorded states cover every existing feature.
    const int n_rec = int(m_pre_states.size());
    if (!m_in_replay && n_rec != int(features.size()))
        throw std::runtime_error(_u8L("pattern: rebuild the model first"));
    const int me = m_in_replay ? n_rec - 1 : n_rec;   // this feature's own index
    int a = f.pattern_features.front(), b = a;
    for (int r : f.pattern_features) { a = std::min(a, r); b = std::max(b, r); }
    if (a < 0 || b >= me)
        throw std::runtime_error(_u8L("pattern: it can only repeat features that come before it"));
    auto state_before = [&](int i) -> const std::vector<CadBody>& { return i < n_rec ? m_pre_states[size_t(i)] : bodies; };
    const std::vector<CadBody>& before = state_before(a);
    const std::vector<CadBody>& after  = state_before(b + 1);
    const int t = pick_body(features[a].target_body, int(before.size()));
    if (t < 0 || t >= int(after.size()) || t >= int(bodies.size()) || before.size() != after.size())
        throw std::runtime_error(_u8L("pattern: the repeated features must change one existing body (not create or remove bodies)"));
    const int n1 = std::max(1, f.pattern_count), n2 = std::max(1, f.pattern_count2);
    const bool circ = f.pattern_circular;
    if (long(n1) * (circ ? 1 : n2) > 500) throw std::runtime_error(_u8L("pattern: at most 500 instances"));

    auto solid_count = [](const TopoDS_Shape& s) { int n = 0; for (TopExp_Explorer e(s, TopAbs_SOLID); e.More(); e.Next()) ++n; return n; };
    auto diff = [&](const TopoDS_Shape& x, const TopoDS_Shape& y) -> TopoDS_Shape {
        BRepAlgoAPI_Cut c(x, y);
        if (!c.IsDone()) throw std::runtime_error(_u8L("pattern: could not measure what the features changed"));
        return solid_count(c.Shape()) > 0 ? c.Shape() : TopoDS_Shape();
    };
    // What to take away and what to put on, per instance. Repeated cuts use their own TOOL solids, not the measured loss: where a tool
    // overlapped material that was already gone (an earlier pattern's hole), the loss is smaller than the tool, and a rotated or
    // shifted copy would then miss material it should remove. Anything else is measured.
    std::vector<TopoDS_Shape> cut_pieces, add_pieces;
    bool all_cuts = true;
    for (int r : f.pattern_features) {
        const CadFeature& rf = features[r];
        all_cuts = all_cuts && (rf.type == CadFeatureType::Extrude || rf.type == CadFeatureType::Revolve) && rf.mode == BooleanMode::Cut;
    }
    if (all_cuts) {
        for (int r : f.pattern_features) {
            CadFeature tool_f = features[r];
            tool_f.mode = BooleanMode::New;   // the same cutter, built as a solid of its own against the body it met
            std::vector<CadBody> tmp = m_pre_states[r];
            const size_t n0 = tmp.size();
            route_feature(tmp, tool_f);
            if (tmp.size() <= n0 || tmp.back().shape.IsNull()) throw std::runtime_error(_u8L("pattern: could not rebuild the cutter of a repeated feature"));
            cut_pieces.push_back(tmp.back().shape);
        }
    } else {
        const TopoDS_Shape removed = diff(before[t].shape, after[t].shape);
        const TopoDS_Shape added   = diff(after[t].shape, before[t].shape);
        if (!removed.IsNull()) cut_pieces.push_back(removed);
        if (!added.IsNull())   add_pieces.push_back(added);
    }
    if (cut_pieces.empty() && add_pieces.empty()) throw std::runtime_error(_u8L("pattern: the repeated features changed nothing"));

    std::vector<gp_Trsf> trsfs;
    auto unit = [](const Vec3d& v) { if (v.norm() < 1e-9) throw std::runtime_error(_u8L("pattern: the direction is zero")); return Vec3d(v.normalized()); };
    if (f.pattern_mirror) {
        const Vec3d nrm = unit(f.pattern_axis);
        gp_Trsf tr;
        tr.SetMirror(gp_Ax2(gp_Pnt(f.pattern_origin.x(), f.pattern_origin.y(), f.pattern_origin.z()), gp_Dir(nrm.x(), nrm.y(), nrm.z())));
        trsfs.push_back(tr);
    } else if (f.pattern_curve_sketch >= 0) {
        if (f.pattern_curve_sketch >= int(features.size()) || features[f.pattern_curve_sketch].type != CadFeatureType::Sketch)
            throw std::runtime_error(_u8L("pattern: the path is not a sketch"));
        const CadFeature& gs = features[f.pattern_curve_sketch];
        if (f.pattern_curve_entity < 0 || f.pattern_curve_entity >= int(gs.entities.size()))
            throw std::runtime_error(_u8L("pattern: the path curve does not exist"));
        const SketchEntity& gc = gs.entities[size_t(f.pattern_curve_entity)];
        // arc length -> point, by a fine polyline over the curve's own parameter
        constexpr int N = 4000;
        std::vector<Vec3d> pts(N + 1);
        std::vector<double> len(N + 1, 0.0);
        for (int i = 0; i <= N; ++i) {
            pts[size_t(i)] = gs.plane.to_world(sample_entity_2d(gc, double(i) / N));
            if (i > 0) len[size_t(i)] = len[size_t(i - 1)] + (pts[size_t(i)] - pts[size_t(i - 1)]).norm();
        }
        if (n1 > 1 && f.pattern_spacing * (n1 - 1) > len[N] * (1 + 1e-6))
            throw std::runtime_error(_u8L("pattern: the path is shorter than the instances need"));
        for (int k = 1; k < n1; ++k) {
            const double s = std::min(f.pattern_spacing * k, len[N]);
            int i = int(std::lower_bound(len.begin(), len.end(), s) - len.begin());
            i = std::clamp(i, 1, N);
            const double u = (len[size_t(i)] - len[size_t(i - 1)]) > 1e-12 ? (s - len[size_t(i - 1)]) / (len[size_t(i)] - len[size_t(i - 1)]) : 0.0;
            const Vec3d d = pts[size_t(i - 1)] + (pts[size_t(i)] - pts[size_t(i - 1)]) * u - pts[0];
            gp_Trsf tr; tr.SetTranslation(gp_Vec(d.x(), d.y(), d.z()));
            trsfs.push_back(tr);
        }
    } else if (circ) {
        const Vec3d ax = unit(f.pattern_axis);
        const gp_Ax1 axis(gp_Pnt(f.pattern_origin.x(), f.pattern_origin.y(), f.pattern_origin.z()), gp_Dir(ax.x(), ax.y(), ax.z()));
        const bool full = std::abs(std::abs(f.pattern_angle) - 360.0) < 1e-9;
        const double div = (!full && n1 > 1) ? double(n1 - 1) : double(n1);
        for (int k = 1; k < n1; ++k) { gp_Trsf tr; tr.SetRotation(axis, (f.pattern_angle * M_PI / 180.0) / div * k); trsfs.push_back(tr); }
    } else {
        const Vec3d d1 = unit(f.pattern_axis), d2 = n2 > 1 ? unit(f.pattern_axis2) : Vec3d(0, 0, 0);
        for (int j = 0; j < n2; ++j)
            for (int i = 0; i < n1; ++i) {
                if (i == 0 && j == 0) continue;
                const Vec3d d = d1 * (i * f.pattern_spacing) + d2 * (j * f.pattern_spacing2);
                gp_Trsf tr; tr.SetTranslation(gp_Vec(d.x(), d.y(), d.z()));
                trsfs.push_back(tr);
            }
    }

    TopoDS_Shape result = bodies[t].shape;
    auto run = [&](bool cut, const std::vector<TopoDS_Shape>& pieces) {
        if (pieces.empty()) return;
        NCollection_List<TopoDS_Shape> args, tools;
        args.Append(result);
        for (const TopoDS_Shape& piece : pieces)
            for (const gp_Trsf& tr : trsfs) tools.Append(BRepBuilderAPI_Transform(piece, tr, true).Shape());
        std::unique_ptr<BRepAlgoAPI_BooleanOperation> op;
        if (cut) op = std::make_unique<BRepAlgoAPI_Cut>(); else op = std::make_unique<BRepAlgoAPI_Fuse>();
        op->SetArguments(args);
        op->SetTools(tools);
        op->Build();
        if (!op->IsDone() || op->HasErrors()) throw std::runtime_error(_u8L("pattern: combining the instances failed"));
        result = op->Shape();
    };
    run(true, cut_pieces);
    run(false, add_pieces);
    if (result.IsNull() || solid_count(result) == 0) throw std::runtime_error(_u8L("pattern: the instances left no solid"));
    bodies[t].shape = result;
}

void CadDocument::route_feature(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    if (f.type == CadFeatureType::Plane)   return;   // datum plane: not part of the body pipeline
    if (f.type == CadFeatureType::Axis)    return;   // datum axis
    if (f.type == CadFeatureType::CoordSys) return; // datum coordinate system
    if (f.type == CadFeatureType::Helix)   return;   // helical curve; consumed by Sweep
    if (f.type == CadFeatureType::Sketch3D) return;  // free 3D curve; consumed by Sweep
    if (f.type == CadFeatureType::Boolean) { apply_boolean(bodies, f); return; }   // body-body op
    if (f.type == CadFeatureType::Cut)     { apply_cut(bodies, f);     return; }   // plane-split body
    if (f.type == CadFeatureType::Mirror)  { apply_mirror(bodies, f);  return; }   // mirror body about plane
    if (f.type == CadFeatureType::Pattern && !f.pattern_features.empty()) { apply_feature_pattern(bodies, f); return; }
    if (f.type == CadFeatureType::Transform) { apply_transform(bodies, f); return; }   // move/rotate body
    if (f.type == CadFeatureType::Mate)     { apply_mate(bodies, f);     return; }   // assembly mate
    if (f.type == CadFeatureType::Thicken) { apply_thicken(bodies, f); return; }   // face -> plate
    if (f.type == CadFeatureType::ThickenSurface) { apply_thicken_surface(bodies, f); return; }
    if (f.type == CadFeatureType::SurfaceOffset) { apply_surface_offset(bodies, f); return; }
    if (f.type == CadFeatureType::Project) return;   // sketch-like: consumed downstream, no body
    if (f.type == CadFeatureType::SmRule)  return;   // rule: no geometry of its own
    // Resolve the target body: explicit target_body when valid, else the last body.
    const int t = pick_body(f.target_body, int(bodies.size()));
    const TopoDS_Shape context = (t >= 0) ? bodies[t].shape : TopoDS_Shape();
    if (f.type == CadFeatureType::SmUnfold || f.type == CadFeatureType::SmRefold) {
        if (t < 0) throw std::runtime_error(_u8L("unfold needs a sheet body"));
        const SheetMetal::Rule rule = sheet_rule_of(f);
        if (f.type == CadFeatureType::SmUnfold) {
            bodies[size_t(t)].shape = SheetMetal::flat_shape(*SheetMetal::unfold(bodies[size_t(t)].shape, rule));
        } else {
            // the folded sheet is rebuilt from the state the Unfold started from, so a cached replay needs no side data
            // (m_pre_states also holds the last recompute, which is what a card's preview works from)
            if (f.sm_ref < 0 || f.sm_ref >= int(m_pre_states.size()) || t >= int(m_pre_states[size_t(f.sm_ref)].size()))
                throw std::runtime_error(_u8L("refold: the unfold it folds back is not available"));
            const auto un = SheetMetal::unfold(m_pre_states[size_t(f.sm_ref)][size_t(t)].shape, rule);
            bodies[size_t(t)].shape = SheetMetal::refold(*un, bodies[size_t(t)].shape);
        }
        return;
    }
    // A New extrude (or the very first solid feature) starts a fresh body; everything else
    // mutates the target body in place.
    const bool starts_new = bodies.empty()
        || f.type == CadFeatureType::Import   // an imported solid is always its own base body
        || f.type == CadFeatureType::SurfaceExtrude || f.type == CadFeatureType::SurfaceRevolve
        || f.type == CadFeatureType::ThickenSurface || f.type == CadFeatureType::SurfaceOffset
        || f.type == CadFeatureType::SurfaceLoft || f.type == CadFeatureType::SurfaceFill
        || f.type == CadFeatureType::SmBase || f.type == CadFeatureType::SmContourFlange
        || ((f.type == CadFeatureType::Extrude || f.type == CadFeatureType::Revolve
             || f.type == CadFeatureType::Sweep || f.type == CadFeatureType::Loft)
            && f.mode == BooleanMode::New);

    if (starts_new) {
        TopoDS_Shape result;            // empty -> apply_feature fills it (New path)
        bool have_body = false;
        apply_feature(result, have_body, context, f);
        if (have_body && !result.IsNull())
            bodies.push_back({ result, f.name.empty() ? _u8L("Body") : f.name });
    } else {
        if (t < 0) throw std::runtime_error(_u8L("feature needs a body"));
        TopoDS_Shape result = bodies[t].shape;   // shallow handle; apply_feature mutates it
        bool have_body = true;
        // A subtraction whose tool misses the target is a legal boolean that removes nothing, so
        // OCCT reports IsDone() and the feature lands in the recipe reporting success. A caller —
        // an agent especially — then has no signal at all that the hole it asked for was never
        // drilled: same body, same volume, ok:true. Measure the volume across the op and refuse
        // the no-op. Only for removals: every other feature type may legitimately leave the volume
        // alone (a Transform certainly does). daf.
        const bool removes = f.type == CadFeatureType::Hole
                          || f.type == CadFeatureType::Thread
                          || ((f.type == CadFeatureType::Extrude || f.type == CadFeatureType::Revolve
                               || f.type == CadFeatureType::Sweep  || f.type == CadFeatureType::Loft)
                              && f.mode == BooleanMode::Cut);
        const double before = removes ? solid_volume(result) : 0.0;
        try {
            apply_feature(result, have_body, context, f);
        } catch (const CombineFailed& cf) {
            // The mode could not be honoured. Keep the user's shape as a NEW body, and say why:
            // a failed combine must never silently lose the profile they just built.
            bodies.push_back({ cf.tool, f.name.empty() ? _u8L("Body") : f.name });
            m_pending_warnings.push_back(format(_u8L("%1% — made a new body instead"), cf.what()));
            return;
        }
        if (removes && before > 0.0) {
            const double after = solid_volume(result);
            // Relative tolerance: a cut that shaves a numerically invisible sliver is a miss too,
            // and an absolute epsilon would be wrong across the mm-to-metre range of real parts.
            if (after >= before - 1e-9 * std::max(1.0, before))
                throw std::runtime_error(
                    f.type == CadFeatureType::Hole ?
                        _u8L("hole removed no material — the tool does not intersect the target body"
                             " (coordinates are in the sketch plane's frame, not world)") :
                    f.type == CadFeatureType::Thread ?
                        _u8L("thread removed no material — the tool does not intersect the target body"
                             " (coordinates are in the sketch plane's frame, not world)") :
                        _u8L("cut removed no material — the tool does not intersect the target body"
                             " (coordinates are in the sketch plane's frame, not world)"));
        }
        // A removal that cuts the body in two leaves a compound of two solids. Keep them as separate bodies, as the
        // other CAD packages do ("result: multiple bodies"); the first stays in place, the rest follow it.
        if (removes) {
            std::vector<TopoDS_Shape> solids;
            for (TopExp_Explorer ex(result, TopAbs_SOLID); ex.More(); ex.Next()) solids.push_back(ex.Current());
            if (solids.size() > 1) {
                const std::string base = bodies[t].name;
                bodies[t].shape = solids[0];
                for (size_t k = 1; k < solids.size(); ++k)
                    bodies.push_back({ solids[k], base + " " + std::to_string(k + 1) });
                return;
            }
        }
        bodies[t].shape = result;
    }
}

void CadDocument::bind_expressions()
{
    std::map<std::string, double> varvals = evaluate_variables(variables);
    for (CadFeature& f : features) {
        bool dims = false;
        for (const auto& [field, e] : f.expr) {
            const double v = eval_expr(e, varvals);
            if (field.rfind("dim:", 0) == 0) {
                // Only a dimension whose value MOVED needs the sketch solved again: the entities already satisfy the others, and a solve
                // per sketch on every rebuild was a third of a cache-restored recompute.
                const int k = std::atoi(field.c_str() + 4);
                dims = dims || k < 0 || k >= int(f.entity_constraints.size()) || f.entity_constraints[size_t(k)].value != v;
            }
            assign_field(f, field, v);
        }
        if (dims) {   // new dimension values: solve the sketch from where it is
            std::vector<SketchEntity> ents = f.entities;
            if (!solve_sketch_entities(ents, f.entity_constraints))
                throw std::runtime_error(format(_u8L("the dimension values conflict in sketch \"%1%\""), f.name));
            f.entities = std::move(ents);
        }
    }
    bind_sheet_rules();
}

// ---- variable housekeeping ---------------------------------------------------------------------------------------
static bool valid_variable_name(const std::string& n)
{
    if (n.empty() || !(std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_')) return false;
    for (char ch : n) if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) return false;
    return !expr_builtin(n);
}

// `expr` with every identifier token equal to `from` replaced by `to`.
static std::string rename_identifier(const std::string& expr, const std::string& from, const std::string& to, bool& changed)
{
    std::string out;
    for (size_t i = 0; i < expr.size();) {
        const char ch = expr[i];
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            size_t j = i;
            while (j < expr.size() && (std::isalnum(static_cast<unsigned char>(expr[j])) || expr[j] == '_')) ++j;
            const std::string id = expr.substr(i, j - i);
            if (id == from) { out += to; changed = true; } else out += id;
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.') {   // a number, so `2e` or `10mm` is not an identifier
            size_t j = i;
            while (j < expr.size() && (std::isdigit(static_cast<unsigned char>(expr[j])) || expr[j] == '.')) ++j;
            out += expr.substr(i, j - i);
            i = j;
        } else { out += ch; ++i; }
    }
    return out;
}

bool CadDocument::rename_variable(const std::string& from, const std::string& to, std::string& err)
{
    err.clear();
    if (!variables.count(from)) { err = format(_u8L("no variable named \"%1%\""), from); return false; }
    if (!valid_variable_name(to)) { err = format(_u8L("\"%1%\" is not a valid variable name (letters, digits and _, not starting with a digit, not a built-in)"), to); return false; }
    if (from != to && variables.count(to)) { err = format(_u8L("a variable named \"%1%\" already exists"), to); return false; }
    bool unused = false;
    std::map<std::string, std::string> vs;
    for (const auto& [k, v] : variables) vs[k == from ? to : k] = rename_identifier(v, from, to, unused);
    variables = std::move(vs);
    for (CadFeature& f : features)
        for (auto& [field, e] : f.expr) e = rename_identifier(e, from, to, unused);
    return true;
}

static std::string csv_quote(const std::string& s)
{
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string q = "\"";
    for (char ch : s) { if (ch == '"') q += '"'; q += ch; }
    return q + "\"";
}

std::string CadDocument::variables_csv() const
{
    std::string out = "name,expression\n";
    for (const auto& [k, v] : variables) out += csv_quote(k) + "," + csv_quote(v) + "\n";
    return out;
}

int CadDocument::variables_from_csv(const std::string& csv, std::string& err)
{
    err.clear();
    // one row = two fields; a quoted field may hold commas, doubled quotes and newlines
    std::vector<std::vector<std::string>> rows(1);
    std::string cur; bool quoted = false, any = false;
    auto end_field = [&]() { rows.back().push_back(cur); cur.clear(); any = false; };
    for (size_t i = 0; i < csv.size(); ++i) {
        const char ch = csv[i];
        if (quoted) { if (ch == '"') { if (i + 1 < csv.size() && csv[i + 1] == '"') { cur += '"'; ++i; } else quoted = false; } else cur += ch; }
        else if (ch == '"') { quoted = true; any = true; }
        else if (ch == ',') end_field();
        else if (ch == '\n' || ch == '\r') { if (ch == '\r' && i + 1 < csv.size() && csv[i + 1] == '\n') ++i; if (!cur.empty() || any || !rows.back().empty()) { end_field(); rows.emplace_back(); } }
        else { cur += ch; any = true; }
    }
    if (quoted) { err = _u8L("unterminated quote in the CSV"); return 0; }
    if (!cur.empty() || any || !rows.back().empty()) end_field();
    if (rows.back().empty()) rows.pop_back();
    int taken = 0;
    std::map<std::string, std::string> next = variables;
    for (size_t r = 0; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (r == 0 && row.size() >= 1 && row[0] == "name") continue;   // header
        if (row.size() != 2) { err = format(_u8L("row %1%: expected \"name,expression\""), int(r) + 1); return 0; }
        if (!valid_variable_name(row[0])) { err = format(_u8L("row %1%: \"%2%\" is not a valid variable name"), int(r) + 1, row[0]); return 0; }
        next[row[0]] = row[1];
        ++taken;
    }
    try { evaluate_variables(next); }   // every expression must evaluate, with no cycles, before any of it is kept
    catch (const std::exception& e) { err = e.what(); return 0; }
    variables = std::move(next);
    return taken;
}

void CadDocument::replay_feature(size_t fi, std::vector<CadBody>& built)
{
    CadFeature& f = features[fi];
    if (!f.enabled) return;
    if (f.type == CadFeatureType::Sketch) return; // consumed by an extrude
    if (f.type == CadFeatureType::Helix)  return; // consumed by Sweep as a path
    if (f.type == CadFeatureType::Sketch3D) {     // no solid, but a curve that cannot be built is an Error now,
        std::string e3;                           // not a surprise when a Sweep later reads it
        if (build_sketch3d_wire(f, e3).IsNull()) throw std::runtime_error(e3);
        return;
    }
    if (f.type == CadFeatureType::SmRule)  return; // rule only
    if (f.type == CadFeatureType::Plane)   return; // datum: no solid, derived on demand
    if (f.type == CadFeatureType::Axis)    return; // datum axis
    if (f.type == CadFeatureType::CoordSys) return; // datum coordinate system
    settle_body_refs(f, built, true);
    m_pending_warnings.clear();
    if (f.type == CadFeatureType::Project) { apply_project(built, f, fi); }
    else                                   { route_feature(built, f); }
    for (const std::string& w : m_pending_warnings) feature_warnings.emplace_back(int(fi), w);
    m_pending_warnings.clear();
    // Record which feature made each body. "Still unset?" is the whole rule, and it is
    // sufficient because of an invariant worth stating: NO feature ever replaces a whole
    // CadBody. Every in-place op writes only `.shape` (boolean, cut, mirror-fuse,
    // transform, dress-up — checked, all 8 sites), so an existing body keeps the stamp it
    // was born with; a consumed body is erased outright, taking its stamp with it; and
    // the only bodies still at -1 here are the ones THIS feature just pushed. That also
    // means a feature type added later needs no change here, as long as it keeps to the
    // same invariant.
    for (CadBody& b : built)
        if (b.source_feature < 0)
            b.source_feature = int(fi);
}

// ---- replay cache ------------------------------------------------------------------------------------------------
struct CadDocument::ReplayCache {
    struct Entry { std::vector<CadBody> built; CadFeature after; std::vector<std::string> warnings; };
    std::mutex mu;
    std::unordered_map<uint64_t, Entry> map;
    std::list<uint64_t> lru;   // front = most recent
    static constexpr size_t kMax = 512;
    bool get(uint64_t k, Entry& out) {
        std::lock_guard<std::mutex> g(mu);
        auto it = map.find(k);
        if (it == map.end()) return false;
        lru.remove(k); lru.push_front(k);
        out = it->second;
        return true;
    }
    void put(uint64_t k, Entry e) {
        std::lock_guard<std::mutex> g(mu);
        if (map.emplace(k, std::move(e)).second) lru.push_front(k);
        while (map.size() > kMax) { map.erase(lru.back()); lru.pop_back(); }
    }
};

static uint64_t fnv_chain(uint64_t h, const void* data, size_t n)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// Features that never touch the body list: replayed every time (cheap) but not worth a cache entry.
static bool replay_is_trivial(const CadFeature& f)
{
    return !f.enabled || f.type == CadFeatureType::Sketch || f.type == CadFeatureType::Helix || f.type == CadFeatureType::Sketch3D
        || f.type == CadFeatureType::Plane || f.type == CadFeatureType::Axis || f.type == CadFeatureType::CoordSys
        || f.type == CadFeatureType::SmRule;
}

// Carry every face/edge id of `f` from the bodies it was written against (`was`) to the rebuilt ones (`now`). `target` is
// the body the feature works on; a field that names another body (a cut face, a project source, a datum's body) is
// followed on that one. Returns how many it could not place; those keep their id. Every id-valued field of CadFeature
// is listed here: one added without a line below silently points elsewhere after the first upstream edit (see the
// "every id field is followed" test in test_caddocument.cpp).
static int remap_feature_refs(CadFeature& f, const std::vector<CadBody>& was, const std::vector<CadBody>& now, int target)
{
    int lost = 0;
    auto usable = [&](int body) { return body >= 0 && body < int(was.size()) && body < int(now.size()) && !was[body].shape.IsSame(now[body].shape); };
    auto face = [&](int body, int& id) { if (id < 0 || !usable(body)) return; const int n = TopoRemap::remap_face(was[body].shape, now[body].shape, id); if (n < 0) ++lost; else id = n; };
    auto edge = [&](int body, int& id) { if (id < 0 || !usable(body)) return; const int n = TopoRemap::remap_edge(was[body].shape, now[body].shape, id); if (n < 0) ++lost; else id = n; };
    auto last = [&](int ref) { return ref == -1 ? int(now.size()) - 1 : ref; };   // pick_body's rule
    const int t = target;
    face(t, f.up_to_face); face(t, f.extrude_src_face); face(t, f.shell_face); face(t, f.draft_face);
    for (int& id : f.shell_faces) face(t, id);
    for (int& id : f.draft_faces) face(t, id);
    for (int& id : f.delete_faces) face(t, id);
    edge(t, f.dressup_edge);
    for (int& id : f.dressup_edges) edge(t, id);
    face(t, f.thicken_face);
    face(t, f.bool_target_face); face(last(f.bool_tool_body), f.bool_tool_face);
    face(f.cut_face_body == -1 ? t : f.cut_face_body, f.cut_face);
    const int src = last(f.project_source_body);
    for (int& id : f.project_edges) edge(src, id);
    face(src, f.project_face);
    // datum planes, axes and coordinate systems: their bodies are explicit (-1 = no reference)
    face(f.plane_face_body, f.plane_face);   face(f.plane_face2_body, f.plane_face2);
    edge(f.plane_edge_body, f.plane_edge);   edge(f.plane_edge2_body, f.plane_edge2);
    face(f.axis_body, f.axis_face);          edge(f.axis_body, f.axis_edge);
    face(f.coordsys_body, f.coordsys_face);  edge(f.coordsys_body, f.coordsys_edge);
    return lost;
}

bool CadDocument::recompute()
{
    // Where a rebuild spends its time beyond the features themselves: logged when it took 20 ms or more.
    struct Prof {
        using clock = std::chrono::steady_clock;
        clock::time_point t0 = clock::now();
        double remap_ms = 0; int remaps = 0;
        double bind_ms = 0, hash_ms = 0, cache_ms = 0, loop_ms = 0;
        static double since(clock::time_point t) { return std::chrono::duration<double, std::milli>(clock::now() - t).count(); }
        ~Prof() {
            const double ms = since(t0);
            if (ms >= 20)
                BOOST_LOG_TRIVIAL(info) << "CadDocument::recompute " << ms << " ms: bind " << bind_ms << ", hashing " << hash_ms << ", cache " << cache_ms
                                        << ", loop " << loop_ms << ", after the loop " << (ms - loop_ms - bind_ms) << ", following face/edge ids " << remap_ms
                                        << " in " << remaps << " features";
        }
    } prof;
    m_bar = -1;
    error.clear();
    feature_warnings.clear();
    set_sketch_auto_close(auto_close_loops);   // this document's weld rule, not the last one's
    detect_mate_conflicts();
    std::vector<CadBody> built;
    // Did any feature in this document even ASK for a solid? A document made only of sketches
    // and datums has nothing to build, and that is a legitimate state — it is every document
    // between drawing the first profile and extruding it. Reporting it as a failure is what
    // made a sketch-only design unsaveable AND unopenable: DesignPanel::recompute_guarded syncs
    // the 3MF recipe only "on success", so nothing was written, and deserialize_recipe ends with
    // `return recompute()`, so a project that did carry a recipe was refused on load with
    // "Could not restore the CAD model" while its features sat correctly in the list. mtav.
    // (Only the replayed prefix counts: rolled back to a sketch, "no solid yet" is the right answer.)
    bool any_solid_feature = false;
    for (size_t i = 0; i < features.size() && (rollback_index < 0 || int(i) <= rollback_index); ++i)
        if (features[i].enabled && produces_body(features[i].type)) { any_solid_feature = true; break; }
    error_feature = -1;
    size_t replaying = 0;
    try {
        { const auto tb = Prof::clock::now(); bind_expressions(); prof.bind_ms = Prof::since(tb); }
        const auto t_loop = Prof::clock::now();
        replay_stats = ReplayStats{};
        replay_stats.feature_ms.assign(features.size(), 0.0);
        if (!m_replay_cache) m_replay_cache = std::make_shared<ReplayCache>();
        uint64_t fp = 1469598103934665603ull;   // chain seed: everything outside the features that changes a build
        { const uint32_t v = ORCA_CAD_RECIPE_VERSION; fp = fnv_chain(fp, &v, sizeof v); }
        fp = fnv_chain(fp, modeling_origin.data(), 3 * sizeof(double));
        fp = fnv_chain(fp, &auto_close_loops, sizeof auto_close_loops);
        std::map<uint64_t, std::vector<CadBody>> seen_now;
        auto own_hash = [&prof](const CadFeature& f) {
            const auto th = Prof::clock::now();
            std::ostringstream o; { cereal::BinaryOutputArchive a(o); cad_save_feature(a, f); }
            const std::string s = o.str();
            const uint64_t h = fnv_chain(1469598103934665603ull, s.data(), s.size());
            prof.hash_ms += Prof::since(th);
            return h;
        };
        // A sketch or hole drawn on a face (a thread on a cylinder) follows that face: find where the face went, and move
        // the plane onto it.
        auto attach_update = [&](size_t fi, auto was) {
            CadFeature& f = features[fi];
            const std::string what = f.type == CadFeatureType::Sketch ? _u8L("the sketch") : f.type == CadFeatureType::Hole ? _u8L("the hole") : _u8L("the thread");
            auto warn = [&](const std::string& why) { feature_warnings.emplace_back(int(fi), what + why + _u8L("; it stays where it is")); };
            const int tb = f.attach_body;
            if (tb < 0 || tb >= int(built.size())) { warn(_u8L("'s body no longer exists at this point")); return; }
            int face = f.attach_face;
            if (was != m_seen_by_feature.end() && tb < int(was->second.size()) && !was->second[tb].shape.IsSame(built[tb].shape)) {
                face = TopoRemap::remap_face(was->second[tb].shape, built[tb].shape, face);
                if (face < 0) { warn(_u8L(" lost the face it was drawn on")); return; }
            }
            const TopoDS_Face fc = GeometryEngine::face_by_index(built[tb].shape, face);
            if (f.type == CadFeatureType::Thread) {   // the axis of the cylinder it was cut on
                const GeometryEngine::CylinderFace cf = fc.IsNull() ? GeometryEngine::CylinderFace() : GeometryEngine::cylinder_of_face(fc);
                if (!cf.ok) { warn(_u8L("'s cylinder is gone")); return; }
                SketchPlane np = f.plane;
                np.origin = cf.base;
                np.normal = cf.axis.dot(f.plane.normal) < 0 ? Vec3d(-cf.axis) : cf.axis;
                const Vec3d ref = std::abs(np.normal.z()) < 0.9 ? Vec3d(0, 0, 1) : Vec3d(1, 0, 0);
                np.x_axis = ref.cross(np.normal).normalized();
                np.y_axis = np.normal.cross(np.x_axis).normalized();
                if ((np.origin - f.plane.origin).norm() > 1e-9 || (np.normal - f.plane.normal).norm() > 1e-9 || (np.x_axis - f.plane.x_axis).norm() > 1e-9) f.plane = np;
                f.attach_face = face;
                return;
            }
            if (fc.IsNull() || BRepAdaptor_Surface(fc).GetType() != GeomAbs_Plane) { warn(_u8L("'s face is no longer planar")); return; }
            Vec3d n = GeometryEngine::face_normal_world(fc);
            if (n.norm() < 1e-9) return;
            n.normalize();
            const Vec3d p = GeometryEngine::face_centroid_world(fc);
            // The face may only have MOVED (its body was moved or turned): same size, same outline. What is on it then goes with
            // it, whole, as one rigid motion; a face that changed shape (a stretch) keeps the plane's place in its own plane.
            SketchPlane base = f.plane;
            if (attached_follow_moves && was != m_seen_by_feature.end() && tb < int(was->second.size()) && !was->second[tb].shape.IsSame(built[tb].shape)) {
                const TopoDS_Face old_fc = GeometryEngine::face_by_index(was->second[tb].shape, f.attach_face);
                if (!old_fc.IsNull() && BRepAdaptor_Surface(old_fc).GetType() == GeomAbs_Plane) {
                    GProp_GProps g0, g1; BRepGProp::SurfaceProperties(old_fc, g0); BRepGProp::SurfaceProperties(fc, g1);
                    Vec3d n0 = GeometryEngine::face_normal_world(old_fc);
                    if (n0.norm() > 1e-9 && std::abs(g0.Mass() - g1.Mass()) <= 1e-6 * std::max(g0.Mass(), 1.0)
                        && GeometryEngine::edges_of_face(old_fc).size() == GeometryEngine::edges_of_face(fc).size()) {
                        n0.normalize();
                        const Eigen::Quaterniond rot = Eigen::Quaterniond::FromTwoVectors(n0, n);
                        const Vec3d shift = p - rot * GeometryEngine::face_centroid_world(old_fc);
                        base.origin = rot * f.plane.origin + shift;
                        base.normal = rot * f.plane.normal;
                        base.x_axis = rot * f.plane.x_axis;
                        base.y_axis = rot * f.plane.y_axis;
                    }
                }
            }
            if (n.dot(base.normal) < 0) n = -n;                                      // the side the sketch was on
            SketchPlane np = base;
            np.normal = n;
            np.origin = base.origin - n * ((base.origin - p).dot(n));                // slide along the normal onto the face
            Vec3d x = base.x_axis - n * base.x_axis.dot(n);
            if (x.norm() < 1e-9) {
                x = std::abs(n.x()) < 0.9 ? Vec3d(1, 0, 0) : Vec3d(0, 1, 0);
                x -= n * x.dot(n);
            }
            x.normalize();
            np.x_axis = x;
            const bool right = base.x_axis.cross(base.y_axis).dot(base.normal) > 0;   // keep the frame's handedness
            np.y_axis = right ? Vec3d(n.cross(x)) : Vec3d(x.cross(n));
            const bool same = (np.origin - f.plane.origin).norm() < 1e-9 && (np.normal - f.plane.normal).norm() < 1e-9 && (np.x_axis - f.plane.x_axis).norm() < 1e-9;
            if (!same) f.plane = np;
            f.attach_face = face;
        };
        // A sketch on a datum plane lies on that plane as it is NOW (its offset may be bound to a variable).
        auto datum_update = [&](size_t fi) {
            CadFeature& f = features[fi];
            const int pi = f.attach_datum;
            if (pi < 0 || pi >= int(features.size()) || features[pi].type != CadFeatureType::Plane || !features[pi].enabled) {
                feature_warnings.emplace_back(int(fi), _u8L("the datum plane this sketch was drawn on is gone; the sketch stays where it is"));
                return;
            }
            size_t k = 0;
            for (int j = 0; j < pi; ++j)
                if (features[j].type == CadFeatureType::Plane && features[j].enabled) ++k;
            const auto datums = resolve_datum_planes();
            if (k < datums.size()) f.plane = datums[k].second;
        };
        m_pre_states.clear();
        struct ReplayFlag { bool& f; explicit ReplayFlag(bool& x) : f(x) { f = true; } ~ReplayFlag() { f = false; } } replay_flag(m_in_replay);
        for (size_t fi = 0; fi < features.size(); ++fi) {
            if (rollback_index >= 0 && int(fi) > rollback_index) break;   // rolled back to rollback_index
            replaying = fi;
            m_pre_states.push_back(built);   // the state this feature starts from (feature patterns look back at it)
            if (features[fi].enabled && features[fi].type == CadFeatureType::Sketch && features[fi].attach_datum >= 0) datum_update(fi);
            if (!incremental) { replay_feature(fi, built); ++replay_stats.replayed; continue; }
            const bool solid_feature = !replay_is_trivial(features[fi]);
            const bool on_face = features[fi].enabled && features[fi].attach_face >= 0
                              && (features[fi].type == CadFeatureType::Sketch || features[fi].type == CadFeatureType::Hole || features[fi].type == CadFeatureType::Thread);
            // What this feature saw last time, found under its bytes as they are now: before the face follow and the id remap
            // below change them.
            auto was = m_seen_by_feature.end();
            if ((solid_feature || on_face) && remap_refs && !m_seen_by_feature.empty()) was = m_seen_by_feature.find(own_hash(features[fi]));
            if (on_face) {
                attach_update(fi, was);
                if (!solid_feature) seen_now[own_hash(features[fi])] = built;
            }
            if (solid_feature && was != m_seen_by_feature.end()) {
                // An upstream edit rebuilt the body this feature's ids were written against: follow them.
                // not pick_body(): the reference may still be waiting to be re-pointed, and pick_body throws on a stale one
                const int tb = features[fi].target_body;
                const int t = tb == -1 ? int(built.size()) - 1 : tb;
                if (t >= 0) {
                    const auto tr = std::chrono::steady_clock::now();
                    const int lost = remap_feature_refs(features[fi], was->second, built, t);
                    prof.remap_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tr).count(); ++prof.remaps;
                    if (lost > 0)
                        feature_warnings.emplace_back(int(fi), format(_u8L("%1% face/edge reference(s) could not be followed through the edit and may point elsewhere"), lost));
                }
            }
            std::vector<CadBody> before;
            if (solid_feature) before = built;   // what this feature is about to see, recorded under the feature as it ends up
            auto note = [&]() { if (solid_feature) seen_now[own_hash(features[fi])] = std::move(before); };
            {   // chain: this feature's bytes (fields already bound to the variables) on top of everything before it
                const auto th = Prof::clock::now();
                std::ostringstream fos;
                { cereal::BinaryOutputArchive fa(fos); fa(features[fi]); }
                const std::string fb = fos.str();
                fp = fnv_chain(fp, fb.data(), fb.size());
                prof.hash_ms += Prof::since(th);
            }
            if (replay_is_trivial(features[fi])) { replay_feature(fi, built); continue; }
            ReplayCache::Entry hit;
            const auto tc = Prof::clock::now();
            if (m_replay_cache->get(fp, hit)) {
                built = std::move(hit.built);
                features[fi] = std::move(hit.after);
                for (const std::string& w : hit.warnings) feature_warnings.emplace_back(int(fi), w);
                ++replay_stats.restored;
                note();
                prof.cache_ms += Prof::since(tc);
                continue;
            }
            const size_t nw = feature_warnings.size();
            const auto t0 = std::chrono::steady_clock::now();
            replay_feature(fi, built);
            replay_stats.feature_ms[fi] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++replay_stats.replayed;
            ReplayCache::Entry e{built, features[fi], {}};
            for (size_t k = nw; k < feature_warnings.size(); ++k) e.warnings.push_back(feature_warnings[k].second);
            m_replay_cache->put(fp, std::move(e));
            note();
        }
        if (incremental) m_seen_by_feature = std::move(seen_now);
        prof.loop_ms = Prof::since(t_loop);
    } catch (const std::exception& e) {
        error_feature = int(replaying);
        error = *e.what() ? e.what() : _u8L("OCCT operation failed");
        return false;
    } catch (...) {
        error_feature = int(replaying);
        error = _u8L("unknown geometry error");
        return false;
    }
    if (built.empty() && any_solid_feature) { error = _u8L("no solid-producing features"); return false; }

    // A feature that leaves a body with a null shape must fail loudly. Until this existed,
    // recompute() returned true and the document kept advertising the body: describe_scene
    // counted it, error was empty, and only mass_properties on that specific body revealed
    // anything was wrong. Returning false hands the caller its normal rollback path, so the
    // operation that destroyed the body is undone rather than committed.
    for (size_t i = 0; i < built.size(); ++i) {
        if (!built[i].shape.IsNull()) continue;
        const int src = built[i].source_feature;
        // source_feature is -1 for a body no feature claims. "feature 0" would be a lie, and
        // this message exists precisely to be trusted about which feature to look at.
        const std::string fname =
            (src < 0 || src >= int(features.size()))
                ? _u8L("an unidentified feature")
                : (features[src].name.empty() ? format(_u8L("feature %1%"), src + 1)
                                              : features[src].name);
        error = format(_u8L("body %1% was destroyed by %2% (the operation produced an empty shape)"), i + 1, fname);
        return false;
    }

    // recompute() replaces the bodies vector wholesale, which would drop any per-body
    // colour override (Color tool) and the name the user gave a body. They are carried forward
    // by the body's IDENTITY — the feature that made it and which of its bodies it is — not by
    // its index: a Boolean consuming its tool, a Mirror replacing its source or a Cut splitting
    // one body into two all shift every later index, and by index the colour and the name
    // moved to a different body.
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (!bodies[i].has_color && !bodies[i].has_user_name) continue;
        const int j = find_body(built, body_id_of(bodies, int(i)));
        if (j < 0) continue;
        if (bodies[i].has_color) {
            built[j].has_color = true;
            built[j].color     = bodies[i].color;
        }
        if (bodies[i].has_user_name) {
            built[j].has_user_name = true;
            built[j].user_name     = bodies[i].user_name;
        }
    }
    // Datum planes, axes and coordinate systems name faces and edges of the FINISHED body list: follow them from the list
    // the last rebuild made to this one, before the old one is gone.
    if (remap_refs && rollback_index < 0 && !bodies.empty()) {
        for (int fi = 0; fi < int(features.size()); ++fi) {
            CadFeature& f = features[fi];
            if (!f.enabled || (f.type != CadFeatureType::Plane && f.type != CadFeatureType::Axis && f.type != CadFeatureType::CoordSys)) continue;
            const int lost = remap_feature_refs(f, bodies, built, -1);
            if (lost > 0)
                feature_warnings.emplace_back(fi, format(_u8L("%1% face/edge reference(s) could not be followed through the edit and may point elsewhere"), lost));
        }
    }
    bodies = std::move(built);
    // Datum references index the FINISHED body list; settle them against it, and close every
    // pending re-point now that both kinds have been through settle_body_refs.
    try {
        for (CadFeature& f : features) {
            if (!f.enabled) continue;   // a hidden feature keeps its pending re-point for when it returns
            settle_body_refs(f, bodies, false);
            f.body_refs_pending = false;
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    // The face and edge maps have just been rebuilt, so every global id handed out before this
    // point now means something else. Bump here rather than in each mutator: this is the single
    // line where the topology is actually replaced, so it cannot be forgotten by a new feature
    // type the way a per-mutator bump would be.
    ++topo_generation;

    // A sketch on a datum plane built from a face was seated, in the loop above, on the plane the PREVIOUS bodies gave: a plane
    // from a face is only known once the bodies are. They are final now, and the datum ids have followed them: if a sketch's plane
    // is not where its datum is, seat it there and rebuild once more (the second pass finds it in place, so it ends).
    if (!m_datum_redo) {
        const auto datums = resolve_datum_planes();
        bool moved = false;
        for (int fi = 0; fi < int(features.size()) && !moved; ++fi) {
            const CadFeature& f = features[fi];
            const int pi = f.attach_datum;
            if (!f.enabled || f.type != CadFeatureType::Sketch || pi < 0 || pi >= int(features.size())
                || features[pi].type != CadFeatureType::Plane || !features[pi].enabled)
                continue;
            size_t k = 0;
            for (int j = 0; j < pi; ++j)
                if (features[j].type == CadFeatureType::Plane && features[j].enabled) ++k;
            if (k >= datums.size()) continue;
            const SketchPlane& np = datums[k].second;
            moved = (np.origin - f.plane.origin).norm() > 1e-9 || (np.normal - f.plane.normal).norm() > 1e-9 || (np.x_axis - f.plane.x_axis).norm() > 1e-9;
        }
        if (moved) {
            struct Pass { bool& b; explicit Pass(bool& x) : b(x) { b = true; } ~Pass() { b = false; } } pass(m_datum_redo);
            return recompute();
        }
    }

    // Face-drift fingerprint for FaceAndDirection CoordSys connectors. This runs AFTER the
    // bodies are final and APPENDS to mate_conflicts (detect_mate_conflicts() cleared it at
    // the top of recompute() and must not run again here). A mismatch is a WARNING, not an
    // error: a legitimate Draft on a mated face renumbers nothing but a real renumber after a
    // dress-up silently points the connector at a different face, and that must not abort.
    for (int fi = 0; fi < int(features.size()); ++fi) {
        CadFeature& f = features[fi];
        if (!f.enabled) continue;
        if (f.type != CadFeatureType::CoordSys) continue;
        if (f.coordsys_type != CoordSysType::FaceAndDirection) continue;
        if (f.coordsys_body < 0 || f.coordsys_body >= int(bodies.size())) continue;
        TopoDS_Face face = GeometryEngine::face_by_index(bodies[f.coordsys_body].shape, f.coordsys_face);
        if (face.IsNull()) continue; // "face not found" is already reported by datum_frame()
        const int kind  = int(BRepAdaptor_Surface(face).GetType());
        const int edges = int(GeometryEngine::edges_of_face(face).size());
        if (f.coordsys_face_kind < 0) {
            // No fingerprint yet (old recipe, or a connector never resolved): adopt the state
            // the user last saw. Self-heals old recipes on first load, and gives both existing
            // writers the fingerprint for free.
            f.coordsys_face_kind  = kind;
            f.coordsys_face_edges = edges;
            continue;
        }
        if (f.coordsys_face_kind != kind || f.coordsys_face_edges != edges) {
            mate_conflicts.emplace_back(int(fi),
                format(_u8L("Connector \"%1%\" may have moved to a different face (an upstream edit "
                            "renumbered this body's faces)"), f.name));
        }
    }

    body = compound_of(bodies);
    const auto mesh_t0 = std::chrono::steady_clock::now();
    display_mesh = tessellate_bodies(bodies, display_tri_face, display_tri_body,
                                     display_body_meshes,
                                     linear_deflection, angular_deflection);
    replay_stats.mesh_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mesh_t0).count();
    if (display_mesh.its.indices.empty() && any_solid_feature) {
        // Empty only because there are no bodies to tessellate is the same legitimate state as
        // above: a sketch-only document has nothing to draw as a solid, and that is not a fault.
        error = _u8L("tessellation produced an empty mesh");
        return false;
    }
    return true;
}

bool CadDocument::preview(const CadFeature& candidate, TriangleMesh& out_mesh,
                          std::vector<TriangleMesh>& out_body_meshes, std::string& err) const
{
    err.clear();
    out_body_meshes.clear();
    std::vector<CadBody> tmp = bodies;     // start from the current committed bodies
    try {
        route_feature(tmp, candidate);     // candidate may append a new body or mutate one
    } catch (const std::exception& e) {
        err = *e.what() ? e.what() : _u8L("OCCT operation failed");
        return false;
    } catch (...) {
        err = _u8L("unknown geometry error");
        return false;
    }
    if (tmp.empty()) {
        err = _u8L("preview produced no geometry");
        return false;
    }
    // Tessellate per body (same path as recompute) so the GUI can re-apply its display-only
    // per-body Move transforms to the ghost; out_mesh is the merged whole.
    std::vector<int> tf, tb;
    out_mesh = tessellate_bodies(tmp, tf, tb, out_body_meshes,
                                 std::max(linear_deflection, preview_linear_deflection), angular_deflection);
    if (out_mesh.its.indices.empty()) {
        err = _u8L("preview produced an empty mesh");
        return false;
    }
    return true;
}

bool CadDocument::preview(const CadFeature& candidate, TriangleMesh& out_mesh, std::string& err) const
{
    std::vector<TriangleMesh> ignore;
    return preview(candidate, out_mesh, ignore, err);
}

namespace {

// A face of a replayed body and what the boundary test needs from it. The projector is built on
// first use: few probes pass the box test, and building one on a freeform surface samples the
// whole surface.
struct FaceProbe {
    TopoDS_Face          face;
    Handle(Geom_Surface) surface;
    Bnd_Box              box;
    mutable std::unique_ptr<GeomAPI_ProjectPointOnSurf> proj;
};

// How close a point must be to lie on a face: a face rebuilt on the same surface is within
// 1e-7 mm of it, a new face nowhere near.
constexpr double kOnFaceTol = 1e-4;

std::vector<FaceProbe> face_probes(const std::vector<CadBody>& bodies)
{
    std::vector<FaceProbe> out;
    for (const CadBody& b : bodies)
        for (TopExp_Explorer e(b.shape, TopAbs_FACE); e.More(); e.Next()) {
            FaceProbe p;
            p.face    = TopoDS::Face(e.Current());
            p.surface = BRep_Tool::Surface(p.face);
            if (p.surface.IsNull()) continue;
            BRepBndLib::Add(p.face, p.box);
            p.box.Enlarge(kOnFaceTol);
            out.push_back(std::move(p));
        }
    return out;
}

bool outward_normal(const TopoDS_Face& face, double u, double v, gp_Dir& n)
{
    BRepAdaptor_Surface surf(face);
    BRepLProp_SLProps props(surf, u, v, 1, Precision::Confusion());
    if (!props.IsNormalDefined()) return false;
    n = props.Normal();
    if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
    return true;
}

// A point inside `face` and its outward normal there: the UV centroid of the largest triangle of
// the face's triangulation, which lies inside the trimmed face even where the centre of its UV
// bounds falls in a hole. Without a triangulation, that centre, if it is inside.
bool interior_point(const TopoDS_Face& face, gp_Pnt& p, gp_Dir& n)
{
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    double u = 0., v = 0., best = -1.;
    if (!tri.IsNull() && tri->HasUVNodes())
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int a, b, c;
            tri->Triangle(i).Get(a, b, c);
            const gp_Pnt pa = tri->Node(a);
            const double area = gp_Vec(pa, tri->Node(b)).Crossed(gp_Vec(pa, tri->Node(c))).SquareMagnitude();
            if (area <= best) continue;
            best = area;
            const gp_XY uv = (tri->UVNode(a).XY() + tri->UVNode(b).XY() + tri->UVNode(c).XY()) / 3.;
            u = uv.X();
            v = uv.Y();
        }
    if (best < 0.) {
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        u = 0.5 * (u0 + u1);
        v = 0.5 * (v0 + v1);
        if (BRepClass_FaceClassifier(face, gp_Pnt2d(u, v), Precision::Confusion()).State() != TopAbs_IN)
            return false;
    }
    p = BRepAdaptor_Surface(face).Value(u, v);
    return outward_normal(face, u, v, n);
}

// Does `p` lie on one of `probes`, on a face turned the same way as `n`? The facing tells apart
// coincident faces of different bodies: a block stacked on a base has its bottom in the base's
// top face, turned the other way.
bool on_boundary(const std::vector<FaceProbe>& probes, const gp_Pnt& p, const gp_Dir& n)
{
    for (const FaceProbe& fp : probes) {
        if (fp.box.IsOut(p)) continue;
        try {
            if (!fp.proj) {
                // The setup GeomAPI_ProjectPointOnSurf(p, surface) repeats for every point, done once.
                double u0, u1, v0, v1;
                fp.surface->Bounds(u0, u1, v0, v1);
                fp.proj = std::make_unique<GeomAPI_ProjectPointOnSurf>();
                fp.proj->Init(fp.surface, u0, u1, v0, v1, Precision::Confusion());
            }
            GeomAPI_ProjectPointOnSurf& proj = *fp.proj;
            proj.Perform(p);
            if (proj.NbPoints() == 0 || proj.LowerDistance() > kOnFaceTol) continue;
            double u, v;
            proj.LowerDistanceParameters(u, v);
            if (BRepClass_FaceClassifier(fp.face, gp_Pnt2d(u, v), kOnFaceTol).State() == TopAbs_OUT) continue;
            gp_Dir m;
            if (outward_normal(fp.face, u, v, m) && m.Dot(n) > 0.) return true;
        } catch (const Standard_Failure&) {
            // A surface the projection cannot handle says nothing about the point.
        }
    }
    return false;
}

} // namespace

std::vector<std::pair<int, int>> CadDocument::faces_made_by(int index) const
{
    std::vector<std::pair<int, int>> out;
    if (index < 0 || index >= int(features.size()) || !features[index].enabled
        || !produces_body(features[index].type) || bodies.empty())
        return out;

    // Replay up to the feature, then the feature alone on a copy, so a body it left alone is the
    // same shape on both sides. On a scratch document, as a replay settles references in the
    // features it runs; it reads only the features, the variables and the process-wide weld rule.
    CadDocument tmp;
    tmp.features  = features;
    tmp.variables = variables;
    std::vector<CadBody> before, after;
    try {
        set_sketch_auto_close(auto_close_loops);
        tmp.bind_expressions();
        for (size_t fi = 0; fi < size_t(index); ++fi)
            tmp.replay_feature(fi, before);
        after = before;
        tmp.replay_feature(size_t(index), after);
    } catch (...) {   // OCCT's Standard_Failure as well as std::exception
        return out;
    }

    // The bodies the step made or replaced. A face on any other body was on the boundary before
    // the step too, so only these are probed after it.
    std::vector<int>     changed;
    std::vector<CadBody> changed_bodies;
    for (int j = 0; j < int(after.size()); ++j)
        if (std::none_of(before.begin(), before.end(), [&](const CadBody& w) { return w.shape.IsSame(after[j].shape); })) {
            changed.push_back(j);
            changed_bodies.push_back(after[j]);
        }
    const std::vector<FaceProbe> was = face_probes(before), now = face_probes(changed_bodies);
    for (int b = 0; b < int(bodies.size()); ++b) {
        int id = 0;
        for (TopExp_Explorer e(bodies[b].shape, TopAbs_FACE); e.More(); e.Next(), ++id) {
            gp_Pnt p;
            gp_Dir n;
            try {
                if (interior_point(TopoDS::Face(e.Current()), p, n) && on_boundary(now, p, n) && !on_boundary(was, p, n))
                    out.emplace_back(b, id);
            } catch (const Standard_Failure&) {
                // A face that cannot be sampled is left out, not the whole answer.
            }
        }
    }
    if (!out.empty())
        return out;

    // No face of its own: answer with every face of each body the step changed, found again in
    // the finished model by the id a body keeps across the rest of the history.
    for (int j : changed) {
        const int b = find_body(bodies, body_id_of(after, j));
        if (b < 0) continue;
        const int n = GeometryEngine::face_count(bodies[b].shape);
        for (int id = 0; id < n; ++id)
            out.emplace_back(b, id);
    }
    return out;
}

std::string brep_to_string(const TopoDS_Shape& s)
{
    if (s.IsNull()) return {};
    std::ostringstream oss;
    BRepTools::Write(s, oss);
    return oss.str();
}

TopoDS_Shape brep_from_string(const std::string& d)
{
    if (d.empty()) return {};
    std::istringstream iss(d);
    TopoDS_Shape s;
    BRep_Builder b;
    BRepTools::Read(s, iss, b);
    return s;
}

std::string CadDocument::serialize_recipe() const
{
    std::ostringstream oss;
    {
        cereal::BinaryOutputArchive ar(oss);
        uint32_t v = ORCA_CAD_RECIPE_VERSION;
        ar(v);
        uint32_t n = static_cast<uint32_t>(features.size());
        ar(n);
        for (const CadFeature& f : features) {
            std::ostringstream fos;
            {
                cereal::BinaryOutputArchive fa(fos);
                cad_save_feature(fa, f);
            }
            std::string fb = fos.str();
            uint32_t len = static_cast<uint32_t>(fb.size());
            ar(len);
            ar(cereal::binary_data(fb.data(), fb.size()));
        }
        std::ostringstream vos;
        {
            cereal::BinaryOutputArchive va(vos);
            va(variables);
        }
        std::string vb = vos.str();
        uint32_t vlen = static_cast<uint32_t>(vb.size());
        ar(vlen);
        ar(cereal::binary_data(vb.data(), vb.size()));
        // BODY NAMES, appended after the variables block. Bodies are not serialised — they are
        // recomputed — so a name the user gave one has nowhere else to live, and without this it
        // would survive a recompute (see the carry-over in recompute()) but not a save.
        //
        // APPENDED RATHER THAN VERSION-BUMPED, on purpose: a build that predates this block
        // reads features and variables, returns, and never looks at the trailing bytes, so its
        // projects still open here AND this build's projects still open there. A version bump
        // would have made every project written today unreadable by yesterday's build for the
        // sake of one optional field. Written as (index, name) pairs so an unnamed body costs
        // nothing.
        // A map, not a vector of pairs: cereal's map support is already included here and its
        // pair support is not, and one more include for one more field is not worth it.
        std::map<uint32_t, std::string> named;
        for (uint32_t i = 0; i < bodies.size(); ++i)
            if (bodies[i].has_user_name && !bodies[i].user_name.empty())
                named[i] = bodies[i].user_name;
        std::ostringstream bos;
        {
            cereal::BinaryOutputArchive ba(bos);
            ba(named);
        }
        std::string bb = bos.str();
        uint32_t blen = static_cast<uint32_t>(bb.size());
        ar(blen);
        ar(cereal::binary_data(bb.data(), bb.size()));
        // DOCUMENT EXTRAS, appended after the body names on the same terms (older builds stop
        // reading before it): the modeling origin, and the body colours. A colour set with the
        // Color tool used to live only until the project was closed.
        std::map<uint32_t, std::array<float, 4>> colours;
        for (uint32_t i = 0; i < bodies.size(); ++i)
            if (bodies[i].has_color)
                colours[i] = { bodies[i].color.r(), bodies[i].color.g(), bodies[i].color.b(), bodies[i].color.a() };
        std::ostringstream xos;
        {
            cereal::BinaryOutputArchive xa(xos);
            xa(modeling_origin.x(), modeling_origin.y(), modeling_origin.z(), colours, auto_close_loops);
        }
        std::string xb = xos.str();
        uint32_t xlen = static_cast<uint32_t>(xb.size());
        ar(xlen);
        ar(cereal::binary_data(xb.data(), xb.size()));
        // HISTORY MARKER, appended on the same terms: where the user rolled the timeline back to (-1 = full history), so a
        // project reopens as it was left. Older builds stop reading before this block.
        std::ostringstream mos;
        {
            cereal::BinaryOutputArchive ma(mos);
            ma(int32_t(rollback_index));
        }
        std::string mb = mos.str();
        uint32_t mlen = static_cast<uint32_t>(mb.size());
        ar(mlen);
        ar(cereal::binary_data(mb.data(), mb.size()));
    }
    return oss.str();
}

bool CadDocument::deserialize_recipe(const std::string& blob)
{
    error.clear();
    try {
        std::istringstream iss(blob);
        cereal::BinaryInputArchive ar(iss);
        uint32_t v;
        ar(v);
        if (v > ORCA_CAD_RECIPE_VERSION) {
            error = format(_u8L("saved with a newer version of the Design tab (format v%1%, this build reads up to v%2%)"),
                           v, ORCA_CAD_RECIPE_VERSION);
            return false;
        }
        // The OUTER framing has been byte-identical since v5 (v6 advanced the stamp without touching
        // the bytes; v7 changed only what is inside each feature's length-prefixed stream: field groups
        // instead of one positional list), so every version from 5 up is read below. A future bump that
        // changes the outer framing must exclude itself there.
        static_assert(ORCA_CAD_RECIPE_VERSION <= 7,
                      "recipe version bumped: confirm the new version still uses the v5 outer framing");
        if (v >= 5) {
            // Framed path: every feature is a length-prefixed self-contained cereal stream, so
            // the same four lines handle BOTH directions of mismatch. Older file, newer build:
            // the sub-stream ends early, fa(f) throws, and the fields already assigned are kept
            // (cereal assigns sequentially, so a mid-list throw leaves the earlier fields set)
            // while the rest default. Newer file, older build: the sub-stream has MORE bytes
            // than this build knows how to read; it reads what it knows and simply stops, and
            // the outer stream is unaffected because the length prefix was consumed in full.
            features.clear();
            uint32_t count;
            ar(count);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t len;
                ar(len);
                std::string buf(len, '\0');
                if (len > 0)
                    ar(cereal::binary_data(&buf[0], len));   // consume EXACTLY len bytes from the outer stream
                CadFeature f;
                try {
                    std::istringstream fs(buf);
                    cereal::BinaryInputArchive fa(fs);
                    if (v >= 7) cad_load_feature(fa, f);
                    else        fa(f);   // v5/v6: the old positional list, CadFeature::load
                } catch (...) {
                    // An OLDER file: the blob ran out before this build's field list did. Everything read
                    // so far is kept and the remaining fields stay at their defaults. Deliberately NOT an
                    // error — a field a project predates is not a corrupt project.
                }
                features.push_back(f);
            }
            variables.clear();
            uint32_t vlen;
            ar(vlen);
            std::string vbuf(vlen, '\0');
            if (vlen > 0)
                ar(cereal::binary_data(&vbuf[0], vlen));
            try {
                std::istringstream vs(vbuf);
                cereal::BinaryInputArchive va(vs);
                va(variables);
            } catch (...) {
                // Variables blob predates this build: keep what was read, default the rest.
            }
            // Body names, if this project carries them. A project written before the block
            // simply ends here, so the read throws and there are no names — not an error.
            std::map<uint32_t, std::string> named;
            try {
                uint32_t blen;
                ar(blen);
                std::string bbuf(blen, '\0');
                if (blen > 0)
                    ar(cereal::binary_data(&bbuf[0], blen));
                std::istringstream bs(bbuf);
                cereal::BinaryInputArchive ba(bs);
                ba(named);
            } catch (...) {
                named.clear();
            }
            // Document extras (origin, colours), when the project has them. The origin must be
            // in place BEFORE the rebuild: datum planes are resolved against it.
            std::map<uint32_t, std::array<float, 4>> colours;
            try {
                uint32_t xlen;
                ar(xlen);
                std::string xbuf(xlen, '\0');
                if (xlen > 0)
                    ar(cereal::binary_data(&xbuf[0], xlen));
                std::istringstream xs(xbuf);
                cereal::BinaryInputArchive xa(xs);
                double ox = 0, oy = 0, oz = 0;
                bool   weld = true;
                xa(ox, oy, oz, colours, weld);
                modeling_origin    = Vec3d(ox, oy, oz);
                origin_from_recipe = true;
                auto_close_loops   = weld;
            } catch (...) {
                colours.clear();   // older project: the caller's origin stands
            }
            int32_t marker = -1;   // history marker, when the project carries one
            try {
                uint32_t mlen;
                ar(mlen);
                std::string mbuf(mlen, '\0');
                if (mlen > 0)
                    ar(cereal::binary_data(&mbuf[0], mlen));
                std::istringstream ms(mbuf);
                cereal::BinaryInputArchive ma(ms);
                ma(marker);
            } catch (...) {
                marker = -1;
            }
            rollback_index = (marker >= 0 && marker < int32_t(features.size())) ? int(marker) : -1;
            // AFTER the rebuild, never before: recompute() replaces the bodies vector wholesale.
            const bool ok = recompute();
            for (const auto& kv : named)
                if (kv.first < bodies.size()) {
                    bodies[kv.first].has_user_name = true;
                    bodies[kv.first].user_name     = kv.second;
                }
            for (const auto& kv : colours)
                if (kv.first < bodies.size()) {
                    bodies[kv.first].has_color = true;
                    bodies[kv.first].color     = ColorRGBA(kv.second[0], kv.second[1], kv.second[2], kv.second[3]);
                }
            return ok;
        }
        if (v == 4) {
            // Pre-framing flat path: v4 projects keep opening, read with the FROZEN v4 field list
            // (CadFeature::load_flat_v4) so fields appended to the framed layout since cannot
            // shift every byte after them.
            cereal::size_type n = 0;
            ar(cereal::make_size_tag(n));
            features.assign(size_t(n), CadFeature{});
            for (CadFeature& f : features) f.load_flat_v4(ar);
            ar(variables);
            return recompute();
        }
        // v < 4: the field lists for v2/v3 no longer exist in this code, so those files
        // cannot be recovered here. This fix is for the future, not the past.
        error = format(_u8L("saved with an older version of the Design tab (format v%1%); this project cannot be opened by this build"), v);
        return false;
    } catch (const std::exception& e) {
        error = *e.what() ? format(_u8L("CAD data could not be read: %1%"), e.what())
                          : _u8L("CAD data could not be read");
        return false;
    } catch (...) {
        error = _u8L("CAD data could not be read");
        return false;
    }
}

bool CadDocument::export_other(const std::string& format, const std::string& path, const std::vector<Transform3d>& body_xforms,
                               std::string& err) const
{
    err.clear();
    if (bodies.empty()) { err = _u8L("nothing to export"); return false; }
    try {
        std::vector<TopoDS_Shape> placed;
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (bodies[i].shape.IsNull()) continue;
            TopoDS_Shape s = bodies[i].shape;
            if (i < body_xforms.size() && !body_xforms[i].isApprox(Transform3d::Identity())) {
                const Transform3d& m = body_xforms[i];
                gp_Trsf t;
                t.SetValues(m(0,0), m(0,1), m(0,2), m(0,3), m(1,0), m(1,1), m(1,2), m(1,3), m(2,0), m(2,1), m(2,2), m(2,3));
                s = BRepBuilderAPI_Transform(s, t, true).Shape();
            }
            placed.push_back(s);
        }
        if (format == "iges") {
            IGESControl_Writer w("MM", 0);
            for (const TopoDS_Shape& s : placed) w.AddShape(s);
            w.ComputeModel();
            if (!w.Write(path.c_str())) { err = _u8L("cannot write IGES file"); return false; }
            return true;
        }
        if (format == "stl" || format == "obj") {
            indexed_triangle_set all;
            for (const TopoDS_Shape& s : placed) its_merge(all, SketchEngine::tessellate(s).its);
            TriangleMesh mesh(std::move(all));
            const bool ok = format == "stl" ? store_stl(path.c_str(), &mesh, true) : store_obj(path.c_str(), &mesh);
            if (!ok) err = _u8L("cannot write the file");
            return ok;
        }
        err = _u8L("unknown export format");
        return false;
    } catch (const std::exception& e) {
        err = *e.what() ? e.what() : _u8L("export failed");
        return false;
    }
}

bool CadDocument::export_step(const std::string& path,
                             const std::vector<Transform3d>& body_xforms,
                             std::string& err) const
{
    err.clear();
    if (bodies.empty()) { err = _u8L("nothing to export"); return false; }
    try {
        // Compound every body at its displayed (Move-gizmo) position so the STEP matches
        // what Commit ships. Move transforms are rigid, so gp_Trsf::SetValues is valid.
        BRep_Builder bld;
        TopoDS_Compound comp;
        bld.MakeCompound(comp);
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (bodies[i].shape.IsNull()) continue;
            TopoDS_Shape s = bodies[i].shape;
            if (i < body_xforms.size() && !body_xforms[i].isApprox(Transform3d::Identity())) {
                const Transform3d& m = body_xforms[i];
                gp_Trsf t;
                t.SetValues(m(0,0), m(0,1), m(0,2), m(0,3),
                            m(1,0), m(1,1), m(1,2), m(1,3),
                            m(2,0), m(2,1), m(2,2), m(2,3));
                s = BRepBuilderAPI_Transform(s, t, true).Shape();
            }
            bld.Add(comp, s);
        }
        STEPControl_Writer writer;
        if (writer.Transfer(comp, STEPControl_AsIs) != IFSelect_RetDone) {
            err = _u8L("STEP transfer failed");
            return false;
        }
        if (writer.Write(path.c_str()) != IFSelect_RetDone) {
            err = _u8L("cannot write STEP file");
            return false;
        }
    } catch (const std::exception& e) {
        err = *e.what() ? e.what() : _u8L("OCCT failed to write STEP");
        return false;
    }
    return true;
}

GeometryEngine::MassProps CadDocument::body_mass_properties(int body_index) const
{
    if (body_index < 0 || body_index >= int(bodies.size())) return {};
    return GeometryEngine::mass_properties(bodies[body_index].shape);
}

int CadDocument::add_surface_extrude(int sketch_ref, double distance, const std::string& name)
{
    CadFeature f;
    f.type       = CadFeatureType::SurfaceExtrude;
    f.name       = name;
    f.sketch_ref = sketch_ref;
    f.distance   = distance;
    f.mode       = BooleanMode::New;
    return append_feature(f);
}

int CadDocument::add_surface_revolve(int sketch_ref, double angle_deg, int axis, const std::string& name)
{
    CadFeature f;
    f.type          = CadFeatureType::SurfaceRevolve;
    f.name          = name;
    f.sketch_ref    = sketch_ref;
    f.revolve_angle = angle_deg;
    f.revolve_axis  = axis;
    f.mode          = BooleanMode::New;
    return append_feature(f);
}

int CadDocument::add_surface_loft(const std::vector<int>& profile_refs, bool ruled, const std::string& name)
{
    CadFeature f;
    f.type              = CadFeatureType::SurfaceLoft;
    f.name              = name;
    f.loft_profile_refs = profile_refs;
    f.loft_ruled        = ruled;
    f.mode              = BooleanMode::New;
    return append_feature(f);
}

int CadDocument::add_surface_fill(int sketch_ref, const std::string& name)
{
    CadFeature f;
    f.type       = CadFeatureType::SurfaceFill;
    f.name       = name;
    f.sketch_ref = sketch_ref;
    f.mode       = BooleanMode::New;
    return append_feature(f);
}

std::vector<CadDocument::Interference> CadDocument::check_interference(double min_volume) const
{
    std::vector<Interference> out;
    const int n = int(bodies.size());

    for (int i = 0; i < n; ++i) {
        if (bodies[i].shape.IsNull() || is_sheet_shape(bodies[i].shape)) continue;
        for (int j = i + 1; j < n; ++j) {
            if (bodies[j].shape.IsNull() || is_sheet_shape(bodies[j].shape)) continue;

            double v = 0;
            // A boolean that fails on one pair must not lose the report for the others.
            try {
                BRepAlgoAPI_Common common(bodies[i].shape, bodies[j].shape);
                common.Build();
                if (!common.IsDone()) continue;
                const TopoDS_Shape s = common.Shape();
                if (s.IsNull()) continue;
                GProp_GProps props;
                BRepGProp::VolumeProperties(s, props);
                v = std::abs(props.Mass());
            } catch (const Standard_Failure&) {
                continue;
            }

            // Bodies that merely touch share a face and enclose no volume, so the
            // threshold is what separates contact from interference.
            if (v > min_volume) out.push_back({i, j, v});
        }
    }
    return out;
}

// ponytail: derived from the OCCT shape type; no stored flag, bodies aren't serialized anyway.
bool CadDocument::is_sheet_shape(const TopoDS_Shape& s)
{
    return !TopExp_Explorer(s, TopAbs_SOLID).More();
}

} // namespace Slic3r
