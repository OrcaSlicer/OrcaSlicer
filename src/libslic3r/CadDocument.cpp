#include "CadDocument.hpp"
#include "SketchConstraints.hpp"
#include "SketchSolver.hpp"
#include "SketchImport.hpp"   // transform_regions for imported art

#include <array>

#include <Standard_Failure.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakeFilling.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <gp_Pln.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
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
#include <GCE2d_MakeSegment.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopAbs.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Compound.hxx>      // multi-body: compound of bodies for display/compat
#include <BRep_Builder.hxx>
#include <TopAbs_Orientation.hxx>   // outward-normal orientation for face-extrude
#include <TopoDS.hxx>               // TopoDS::Edge for SurfaceFill
#include <TopExp_Explorer.hxx>      // is_sheet_shape
#include <gp_Circ.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>
#include <gp_Trsf.hxx>             // pattern: rigid copy transforms
#include <STEPControl_Writer.hxx>  // STEP export (native B-rep)
#include <STEPControl_StepModelType.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <gp_Ax1.hxx>             // pattern: rotation axis (circular)
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <cmath>
#include <cctype>
#include <stdexcept>
#include <algorithm>
#include <sstream>

#include <cereal/archives/binary.hpp>
#include <BRepTools.hxx>

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
    Handle(Geom2d_TrimmedCurve) seg = GCE2d_MakeSegment(p0, p1);
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

    if (R <= 0)   { err = "helix radius must be > 0"; return TopoDS_Wire(); }
    if (P <= 0)   { err = "helix pitch must be > 0";  return TopoDS_Wire(); }
    if (H < 0)    { err = "helix height must be >= 0"; return TopoDS_Wire(); }
    if (H == 0)   { err = "helix height of 0 (flat spiral) is not supported"; return TopoDS_Wire(); }
    const double turns = H / P;
    if (turns > 10000) { err = "helix turn count exceeds limit (10000)"; return TopoDS_Wire(); }
    if (std::abs(taper) > 1e-12) {
        const double R_top = R + H * std::tan(taper);
        if (R_top <= 0) {
            err = "helix taper drives radius negative before reaching height";
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
        Handle(Geom2d_TrimmedCurve) seg = GCE2d_MakeSegment(p0, p1);
        e = BRepBuilderAPI_MakeEdge(seg, cone).Edge();
    } else {
        Handle(Geom_CylindricalSurface) cyl = new Geom_CylindricalSurface(ax3, R);
        double u1 = f.helix_left_handed ? -2.0 * M_PI * turns : 2.0 * M_PI * turns;
        gp_Pnt2d p0(0.0, 0.0);
        gp_Pnt2d p1(u1, H);
        Handle(Geom2d_TrimmedCurve) seg = GCE2d_MakeSegment(p0, p1);
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
    BRepBuilderAPI_MakePolygon poly(top, bot, apex, Standard_True);
    return poly.Wire();   // closed triangle, swept by MakePipeShell with a fixed binormal
}

// ---- expression evaluator (file-local) -------------------------------------
// ponytail: self-contained shunting-yard arithmetic + function set for
// parametric dimension expressions. Extend the function table if needed.

// Evaluate an arithmetic expression to a double. Supports + - * /, parentheses,
// unary minus, decimal literals, and identifiers resolved via `vars`. Functions:
// sqrt, abs, sin, cos, tan (degrees), min, max, and the constant `pi`.
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
            t.num = std::stod(t.val);
            tokens.push_back(t);
            prev_was_val = true;
            continue;
        }
        if (isalpha(c) || c == '_') {
            size_t start = i;
            while (i < src.size() && (isalnum(src[i]) || src[i] == '_')) ++i;
            std::string id = src.substr(start, i - start);
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
        throw std::runtime_error("bad character in expression");
    }

    // ---- shunting-yard: infix -> RPN ----
    std::vector<Token> rpn;
    std::vector<Token> stack;
    auto flush_paren = [&]() {
        while (!stack.empty() && stack.back().type != Token::LParen) {
            rpn.push_back(stack.back()); stack.pop_back();
        }
        if (stack.empty()) throw std::runtime_error("mismatched parentheses");
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
                        throw std::runtime_error("unknown identifier: " + t.val);
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
            throw std::runtime_error("mismatched parentheses");
        rpn.push_back(stack.back()); stack.pop_back();
    }

    // ---- evaluate RPN ----
    std::vector<double> vs;
    for (const Token& t : rpn) {
        if (t.type == Token::Num) {
            vs.push_back(t.num);
        } else if (t.type == Token::Op) {
            if (t.val == "u") {
                if (vs.empty()) throw std::runtime_error("missing operand for unary minus");
                vs.back() = -vs.back();
            } else {
                if (vs.size() < 2) throw std::runtime_error("not enough operands for '" + t.val + "'");
                double b = vs.back(); vs.pop_back();
                double a = vs.back(); vs.pop_back();
                if (t.val == "+") vs.push_back(a + b);
                else if (t.val == "-") vs.push_back(a - b);
                else if (t.val == "*") vs.push_back(a * b);
                else if (t.val == "/") { if (b == 0) throw std::runtime_error("division by zero"); vs.push_back(a / b); }
            }
        } else if (t.type == Token::Id) {
            // function call (already on RPN via shunting-yard)
            auto call_fn = [&](const std::string& name, int arity) {
                if ((int)vs.size() < arity)
                    throw std::runtime_error("not enough arguments for " + name + "()");
                if (name == "sqrt") { double a = vs.back(); vs.pop_back(); vs.push_back(std::sqrt(a)); }
                else if (name == "abs") { double a = vs.back(); vs.pop_back(); vs.push_back(std::abs(a)); }
                else if (name == "sin") { double a = vs.back(); vs.pop_back(); vs.push_back(std::sin(a * M_PI / 180.0)); }
                else if (name == "cos") { double a = vs.back(); vs.pop_back(); vs.push_back(std::cos(a * M_PI / 180.0)); }
                else if (name == "tan") { double a = vs.back(); vs.pop_back(); vs.push_back(std::tan(a * M_PI / 180.0)); }
                else if (name == "min") { double b = vs.back(); vs.pop_back(); double a = vs.back(); vs.pop_back(); vs.push_back(std::min(a, b)); }
                else if (name == "max") { double b = vs.back(); vs.pop_back(); double a = vs.back(); vs.pop_back(); vs.push_back(std::max(a, b)); }
                else throw std::runtime_error("unknown function: " + name);
            };
            call_fn(t.val, (t.val == "min" || t.val == "max") ? 2 : 1);
        }
    }
    if (vs.size() != 1) throw std::runtime_error("invalid expression");
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
        if (visiting.count(name)) throw std::runtime_error("variable cycle: " + name);
        auto itv = variables.find(name);
        if (itv == variables.end()) throw std::runtime_error("unknown identifier: " + name);
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
                if (id == "pi" || id == "sqrt" || id == "abs" || id == "sin" || id == "cos" ||
                    id == "tan" || id == "min" || id == "max") continue;
                if (!variables.count(id)) throw std::runtime_error("unknown identifier: " + id);
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

// Write `value` into the CadFeature numeric field named `field`. Allow-list of
// the geometric dimension fields a variable realistically drives. Integer-valued
// fields are rounded. Throws std::runtime_error("unknown parameter: " + field)
// for anything not in the list.
static void assign_field(CadFeature& f, const std::string& field, double value)
{
    // double fields (alphabetical)
    if (field == "distance")          { f.distance = value; return; }
    if (field == "distance2")         { f.distance2 = value; return; }
    if (field == "draft_angle")       { f.draft_angle = value; return; }
    if (field == "dressup_size")      { f.dressup_size = value; return; }
    if (field == "height")            { f.height = value; return; }
    if (field == "hole_cbore_diameter") { f.hole_cbore_diameter = value; return; }
    if (field == "hole_cbore_depth")  { f.hole_cbore_depth = value; return; }
    if (field == "hole_csink_angle")  { f.hole_csink_angle = value; return; }
    if (field == "hole_csink_diameter") { f.hole_csink_diameter = value; return; }
    if (field == "hole_depth")        { f.hole_depth = value; return; }
    if (field == "hole_diameter")     { f.hole_diameter = value; return; }
    if (field == "hole_x")            { f.hole_x = value; return; }
    if (field == "hole_y")            { f.hole_y = value; return; }
    if (field == "import_scale_x")    { f.import_scale_x = value; return; }
    if (field == "import_scale_y")    { f.import_scale_y = value; return; }
    if (field == "pattern_angle")     { f.pattern_angle = value; return; }
    if (field == "pattern_spacing")   { f.pattern_spacing = value; return; }
    if (field == "plane_angle_tilt")  { f.plane_angle_tilt = value; return; }
    if (field == "plane_offset")      { f.plane_offset = value; return; }
    if (field == "radius")            { f.radius = value; return; }
    if (field == "revolve_angle")     { f.revolve_angle = value; return; }
    if (field == "rib_depth")         { f.rib_depth = value; return; }
    if (field == "rib_thickness")     { f.rib_thickness = value; return; }
    if (field == "shell_thickness")   { f.shell_thickness = value; return; }
    if (field == "taper_deg")         { f.taper_deg = value; return; }
    if (field == "thread_depth")      { f.thread_depth = value; return; }
    if (field == "thread_height")     { f.thread_height = value; return; }
    if (field == "thread_pitch")      { f.thread_pitch = value; return; }
    if (field == "thread_radius")     { f.thread_radius = value; return; }
    if (field == "thread_x")          { f.thread_x = value; return; }
    if (field == "thread_y")          { f.thread_y = value; return; }
    if (field == "width")             { f.width = value; return; }
    // int fields (rounded)
    if (field == "pattern_count")     { f.pattern_count = (int)std::lround(value); return; }
    throw std::runtime_error("unknown parameter: " + field);
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
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_sketch_profile(const SketchProfile& profile, const SketchPlane& plane,
                                    const std::string& name)
{
    CadFeature f;
    f.type    = CadFeatureType::Sketch;
    f.name    = name;
    f.plane   = plane;
    f.profile = profile;
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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

    SketchConstraints sc;
    for (const Vec2d& p : f.profile.points)
        sc.add_point(p.x(), p.y());

    for (const SketchConstraintDef& c : f.constraints) {
        switch (c.type) {
        case SketchConstraintType::Fix:          sc.fix_point(c.a); break;
        case SketchConstraintType::Coincident:   sc.coincident(c.a, c.b); break;
        case SketchConstraintType::Horizontal:   sc.horizontal(c.a, c.b); break;
        case SketchConstraintType::Vertical:     sc.vertical(c.a, c.b); break;
        case SketchConstraintType::Distance:     sc.distance(c.a, c.b, c.value); break;
        case SketchConstraintType::LockX:        sc.lock_x(c.a, c.value); break;
        case SketchConstraintType::LockY:        sc.lock_y(c.a, c.value); break;
        case SketchConstraintType::EqualLength:  sc.equal_length(c.a, c.b, c.c, c.d); break;
        case SketchConstraintType::Parallel:     sc.parallel(c.a, c.b, c.c, c.d); break;
        case SketchConstraintType::Perpendicular:sc.perpendicular(c.a, c.b, c.c, c.d); break;
        }
    }

    const bool ok = sc.solve();
    for (size_t i = 0; i < f.profile.points.size(); ++i)
        f.profile.points[i] = sc.get_point(int(i));
    return ok;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_fillet(double radius, FaceGroup faces, const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::Fillet;
    f.name         = name;
    f.dressup_size = radius;
    f.face_group   = faces;
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_fillet(double radius, int edge_id, const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::Fillet;
    f.name         = name;
    f.dressup_size = radius;
    f.dressup_edge = edge_id;
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_chamfer(double distance, FaceGroup faces, const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::Chamfer;
    f.name         = name;
    f.dressup_size = distance;
    f.face_group   = faces;
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_chamfer(double distance, int edge_id, const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::Chamfer;
    f.name         = name;
    f.dressup_size = distance;
    f.dressup_edge = edge_id;
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
}

// Hole standards lookup table (representative ISO 273 medium / ISO 4762 / ANSI unified).
struct HoleStdEntry { const char* desig; double clearance; double cbore_d; double cbore_depth; double csink_d; };
static const HoleStdEntry kHoleStdTable[] = {
    {"M3",   3.4,  6.0,  3.4,  6.3},
    {"M4",   4.5,  8.0,  4.4,  8.4},
    {"M5",   5.5, 10.0,  5.4, 10.4},
    {"M6",   6.6, 11.0,  6.8, 12.6},
    {"M8",   9.0, 15.0,  8.8, 17.3},
    {"M10", 11.0, 18.0, 11.0, 20.0},
    {"#6-32",    3.7,  8.8,  4.2,  8.7},
    {"#8-32",    4.4,  9.9,  5.1, 10.2},
    {"1/4-20",   6.9, 14.4,  7.2, 14.7},
    {"5/16-18",  8.8, 17.0,  8.2, 17.3},
    {"3/8-16",  10.5, 19.6,  9.5, 19.8},
};
static bool hole_std_lookup(const std::string& desig, double& clearance,
                            double& cbore_d, double& cbore_depth, double& csink_d)
{
    for (const auto& e : kHoleStdTable) {
        if (e.desig == desig) {
            clearance   = e.clearance;
            cbore_d     = e.cbore_d;
            cbore_depth = e.cbore_depth;
            csink_d     = e.csink_d;
            return true;
        }
    }
    return false;
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
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_hole_standard(const std::string& designation, int style, bool through,
                                   double depth, double x, double y,
                                   const SketchPlane& plane, const std::string& name)
{
    double clearance, cbore_d, cbore_depth, csink_d;
    if (!hole_std_lookup(designation, clearance, cbore_d, cbore_depth, csink_d))
        throw std::runtime_error("unknown hole standard \"" + designation + "\"");
    return add_hole_styled(clearance, depth, through, x, y, plane, style,
                           cbore_d, cbore_depth, csink_d, 90, designation, name);
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    f.target_body      = target_body;
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_shell(double thickness, int face, int target_body, const std::string& name)
{
    CadFeature f;
    f.type            = CadFeatureType::Shell;
    f.name            = name;
    f.shell_thickness = thickness;
    f.shell_face      = face;
    f.target_body     = target_body;
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_delete_face(int target_body, const std::vector<int>& faces,
                                  const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::DeleteFace;
    f.name         = name;
    f.target_body  = target_body;
    f.delete_faces = faces;
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_draft(double angle, int face, int target_body, const std::string& name)
{
    CadFeature f;
    f.type        = CadFeatureType::Draft;
    f.name        = name;
    f.draft_angle = angle;
    f.draft_face  = face;
    f.target_body = target_body;
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_surface_offset(int target_body, double offset, const std::string& name)
{
    CadFeature f;
    f.type         = CadFeatureType::SurfaceOffset;
    f.name         = name;
    f.target_body  = target_body;
    f.plane_offset = offset;
    f.mode         = BooleanMode::New;
    features.push_back(f);
    return int(features.size()) - 1;
}

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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_axis(AxisType axis_type_, const std::string& name)
{
    CadFeature f;
    f.type      = CadFeatureType::Axis;
    f.name      = name;
    f.axis_type = axis_type_;
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_coordsys(CoordSysType type, const Vec3d& point, const std::string& name)
{
    CadFeature f;
    f.type           = CadFeatureType::CoordSys;
    f.name           = name;
    f.coordsys_type  = type;
    f.coordsys_point = point;
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
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

std::vector<std::pair<std::string, SketchPlane>> CadDocument::resolve_datum_planes() const
{
    std::vector<std::pair<std::string, SketchPlane>> out;
    for (const CadFeature& f : features) {
        if (f.type != CadFeatureType::Plane || !f.enabled) continue;

        // Resolve base reference plane. The default XY/XZ/YZ planes pass through the modeling
        // origin (bed centre); datum bases (>=3) are already in world coords from earlier passes.
        SketchPlane base;
        if      (f.plane_base == 1) { base = SketchPlane::XZ(); base.origin += modeling_origin; }
        else if (f.plane_base == 2) { base = SketchPlane::YZ(); base.origin += modeling_origin; }
        else if (f.plane_base >= 3) {
            const int di = f.plane_base - 3;
            if (di < int(out.size())) base = out[di].second;
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

        out.emplace_back(f.name, result);
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
            if (dir.squaredNorm() < 1e-18) { da.error = "two points are coincident"; break; }
            da.origin    = f.axis_p1;
            da.direction = dir.normalized();
            break;
        }
        case AxisType::FaceNormal: {
            TopoDS_Face fc = resolve_face(f.axis_body, f.axis_face);
            if (fc.IsNull()) { da.error = "face not found"; break; }
            da.origin    = GeometryEngine::face_centroid_world(fc);
            da.direction = GeometryEngine::face_normal_world(fc);
            break;
        }
        case AxisType::CylinderCenterline: {
            TopoDS_Face fc = resolve_face(f.axis_body, f.axis_face);
            if (fc.IsNull()) { da.error = "face not found"; break; }
            GeometryEngine::CylinderFace cyl = GeometryEngine::cylinder_of_face(fc);
            if (!cyl.ok) { da.error = "face is not a cylinder"; break; }
            da.origin    = cyl.base;
            da.direction = cyl.axis;
            break;
        }
        case AxisType::AlongEdge: {
            Vec3d p0, dir;
            if (!resolve_edge(f.axis_body, f.axis_edge, p0, dir)) {
                da.error = "edge not found"; break;
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
            if (!ok0 || !ok1) { da.error = "plane ref not found"; break; }
            // Direction = cross product of the two plane normals.
            Vec3d dir = da.direction.cross(normal1); // da.direction was normal0
            if (dir.squaredNorm() < 1e-18) {
                da.error = "planes are parallel (no intersection)"; break;
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
            if (std::abs(det) < 1e-18) { da.error = "planes are parallel (no intersection)"; break; }
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
        if (fc.IsNull()) { ds.error = "face not found"; break; }
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
        if (X_tent.squaredNorm() < 1e-18) { ds.error = "zero-length direction"; break; }
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
    error.clear();
    mate_conflicts.clear();
    // A cleared document is a fresh start with no history.
    m_undo.clear();
    m_redo.clear();
}

void CadDocument::checkpoint()
{
    m_undo.push_back(Snapshot{features, variables});   // snapshot the pre-mutation recipe
    m_redo.clear();                                    // any new action invalidates the redo branch
    if (m_undo.size() > k_undo_cap)
        m_undo.erase(m_undo.begin());
}

bool CadDocument::undo()
{
    if (m_undo.empty())
        return false;
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
    m_undo.push_back(Snapshot{std::move(features), std::move(variables)});
    features  = std::move(m_redo.back().features);
    variables = std::move(m_redo.back().variables);
    m_redo.pop_back();
    recompute();
    return true;
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
        if (f.enabled && f.type != CadFeatureType::Sketch) { has_solid_feature = true; break; }
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
    doc.error = fail_err.empty() ? std::string("feature is used by a later feature")
                                 : fail_err;
    return false;
}

bool CadDocument::remove_feature(int index)
{
    if (index < 0 || index >= int(features.size()))
        return false;

    std::vector<CadFeature> snapshot = features;

    // Deleting a Sketch cascades to every Extrude that consumes it (a dangling
    // Extrude would have no wire). A lone Sketch, by contrast, is harmless.
    std::vector<int> remove{index};
    if (features[index].type == CadFeatureType::Sketch) {
        for (int j = 0; j < int(features.size()); ++j)
            if (features[j].type == CadFeatureType::Extrude && features[j].sketch_ref == index)
                remove.push_back(j);
    }
    std::sort(remove.begin(), remove.end());
    remove.erase(std::unique(remove.begin(), remove.end()), remove.end());

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
    for (auto& f : features) {
        if (f.type == CadFeatureType::Extrude) {
            remap(f.sketch_ref);
        } else if (f.type == CadFeatureType::Mate) {
            remap(f.mate_cs_a);
            remap(f.mate_cs_b);
        }
    }

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
    std::swap(features[index], features[target]);

    // The two slots traded places: fix every feature reference that pointed at either —
    // a mate's connectors as well as sketch_ref, for the reason given in remove_feature().
    auto swap_ref = [index, target](int& ref) {
        if (ref == index)       ref = target;
        else if (ref == target) ref = index;
    };
    for (auto& f : features) {
        if (f.type == CadFeatureType::Extrude) {
            swap_ref(f.sketch_ref);
        } else if (f.type == CadFeatureType::Mate) {
            swap_ref(f.mate_cs_a);
            swap_ref(f.mate_cs_b);
        }
    }

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

TopoDS_Wire CadDocument::build_sketch_wire(const CadFeature& sketch) const
{
    if (!sketch.entities.empty()) {
        TopoDS_Wire w = SketchEngine::entities_to_wire(sketch.entities, sketch.plane);
        if (!w.IsNull()) return w;
        // An entity sketch that yields no wire is an ERROR, not a cue to fall through. The
        // legacy tail of this function ends in a default rectangle built from width/height,
        // which for an entity sketch are whatever they happened to be initialised to — so a
        // sketch entities_to_wire cannot handle (a circle coexisting with a line, two circles:
        // snaporca-88v) used to extrude into a box the user never drew, silently. Failing here
        // costs the caller an error message; falling through cost them wrong geometry that
        // looked deliberate. The legacy profile/shape paths below are still reached by sketches
        // that legitimately carry no entities at all.
        throw std::runtime_error(
            "sketch has entities but they do not form a single closed wire — a closed entity "
            "(circle/ellipse) combined with other entities, or several closed entities, is not "
            "supported yet");
    }
    if (!sketch.profile.points.empty()) {
        SketchProfile prof = sketch.profile;
        prof.closed = true;               // extrude needs a closed wire
        TopoDS_Wire w = prof.to_occt_wire(sketch.plane);
        if (w.IsNull()) throw std::runtime_error("sketch profile wire failed");
        return w;
    }
    if (sketch.shape == SketchShape::Circle) {
        gp_Pnt o(sketch.plane.origin.x(), sketch.plane.origin.y(), sketch.plane.origin.z());
        gp_Dir n(sketch.plane.normal.x(), sketch.plane.normal.y(), sketch.plane.normal.z());
        gp_Circ circ(gp_Ax2(o, n), sketch.radius);
        TopoDS_Edge e = BRepBuilderAPI_MakeEdge(circ).Edge();
        BRepBuilderAPI_MakeWire wm(e);
        if (!wm.IsDone()) throw std::runtime_error("circle wire failed");
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

void CadDocument::apply_feature(TopoDS_Shape& result, bool& have_body,
                                const TopoDS_Shape& context, const CadFeature& f) const
{
    switch (f.type) {
    case CadFeatureType::Sketch:
        return; // sketches carry no solid; consumed by an extrude
    case CadFeatureType::Project:
        return; // edges-to-sketch: consumed downstream, no solid body
    case CadFeatureType::Helix:
        return; // helical curve; consumed by Sweep as a path (like Sketch)
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
            if (context.IsNull()) throw std::runtime_error("face-extrude needs a body");
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
            const CadFeature& sk = (f.sketch_ref >= 0 && f.sketch_ref < int(features.size())
                                    && (features[f.sketch_ref].type == CadFeatureType::Sketch
                                        || features[f.sketch_ref].type == CadFeatureType::Project))
                                   ? features[f.sketch_ref] : f;
            // Imported rigid art (Text/SVG) extrudes via the faces-with-holes path
            // (with its placement transform applied); otherwise build a single wire
            // from entities/profile/shape.
            tool = !sk.imported_regions.empty()
                ? SketchEngine::make_extrude_regions(
                      transform_regions(sk.imported_regions, sk.import_offset,
                                        sk.import_scale_x, sk.import_scale_y),
                      sk.plane,
                      f.extrude_end == ExtrudeEnd::ThroughAll ? 1e5 : signed_d,
                      f.extrude_end == ExtrudeEnd::ThroughAll ? true : (sym || f.extrude_end == ExtrudeEnd::TwoSided))
                : [&]() {
                      TopoDS_Wire wire = build_sketch_wire(sk);
                      TopoDS_Shape t;
                      switch (f.extrude_end) {
                          case ExtrudeEnd::Blind:
                              t = (std::abs(f.taper_deg) > 1e-6)
                                  ? SketchEngine::make_extrude_taper(wire, sk.plane, signed_d, f.taper_deg)
                                  : SketchEngine::make_extrude(wire, sk.plane, signed_d, false);
                              break;
                          case ExtrudeEnd::Symmetric:  t = SketchEngine::make_extrude(wire, sk.plane, f.distance, true); break;
                          case ExtrudeEnd::TwoSided:   t = SketchEngine::make_extrude_two_sided(wire, sk.plane, f.distance, f.distance2); break;
                          case ExtrudeEnd::ThroughAll: t = SketchEngine::make_extrude(wire, sk.plane, 1.0e5, true); break;
                          case ExtrudeEnd::UpToFace: {
                              const TopoDS_Face tgt = GeometryEngine::face_by_index(context, f.up_to_face);
                              double L = signed_d;
                              if (!tgt.IsNull()) {
                                  const Vec3d c = GeometryEngine::face_centroid_world(tgt);
                                  L = (c - sk.plane.origin).dot(sk.plane.normal);
                              }
                              t = (std::abs(f.taper_deg) > 1e-6)
                                  ? SketchEngine::make_extrude_taper(wire, sk.plane, L, f.taper_deg)
                                  : SketchEngine::make_extrude(wire, sk.plane, L, false);
                              break;
                          }
                          case ExtrudeEnd::UpToVertex: {
                              const double L = (f.up_to_point - sk.plane.origin).dot(sk.plane.normal);
                              t = SketchEngine::make_extrude(wire, sk.plane, L, false);
                              break;
                          }
                          default: t = SketchEngine::make_extrude(wire, sk.plane, signed_d, false); break;
                      }
                      return t;
                  }();
        }
        // New / first-of-a-body => result becomes the tool (route_feature sends New extrudes
        // here with an empty result, so a face-extrude New builds a fresh body from the source
        // face in `context` without touching it). Add/Cut/Intersect boolean into `result`.
        if (!have_body || f.mode == BooleanMode::New) {
            result = tool;
            have_body = true;
        } else if (f.mode == BooleanMode::Add) {
            BRepAlgoAPI_Fuse fuse(result, tool);
            if (!fuse.IsDone()) throw std::runtime_error("fuse failed");
            result = fuse.Shape();
        } else if (f.mode == BooleanMode::Cut) {
            BRepAlgoAPI_Cut cut(result, tool);
            if (!cut.IsDone()) throw std::runtime_error("cut failed");
            result = cut.Shape();
        } else if (f.mode == BooleanMode::Intersect) {
            BRepAlgoAPI_Common common(result, tool);
            if (!common.IsDone()) throw std::runtime_error("intersect failed");
            result = common.Shape();
        }
        break;
    }
    case CadFeatureType::Revolve: {
        // Resolve the profile sketch like Extrude: referenced Sketch when valid,
        // else this feature's own inline entities/profile (self-contained candidate).
        const CadFeature& sk = (f.sketch_ref >= 0 && f.sketch_ref < int(features.size())
                                && (features[f.sketch_ref].type == CadFeatureType::Sketch
                                    || features[f.sketch_ref].type == CadFeatureType::Project))
                               ? features[f.sketch_ref] : f;
        TopoDS_Wire wire = build_sketch_wire(sk);
        const double ang = f.flip ? -f.revolve_angle : f.revolve_angle;
        TopoDS_Shape tool = SketchEngine::make_revolve(wire, sk.plane, ang, f.revolve_axis);
        if (!have_body || f.mode == BooleanMode::New) {
            result = tool;
            have_body = true;
        } else if (f.mode == BooleanMode::Add) {
            BRepAlgoAPI_Fuse fuse(result, tool);
            if (!fuse.IsDone()) throw std::runtime_error("fuse failed");
            result = fuse.Shape();
        } else if (f.mode == BooleanMode::Cut) {
            BRepAlgoAPI_Cut cut(result, tool);
            if (!cut.IsDone()) throw std::runtime_error("cut failed");
            result = cut.Shape();
        } else if (f.mode == BooleanMode::Intersect) {
            BRepAlgoAPI_Common common(result, tool);
            if (!common.IsDone()) throw std::runtime_error("intersect failed");
            result = common.Shape();
        }
        break;
    }
    case CadFeatureType::SurfaceExtrude: {
        if (f.sketch_ref < 0 || f.sketch_ref >= int(features.size()))
            throw std::runtime_error("surface-extrude: bad sketch ref");
        const CadFeature& sk = features[f.sketch_ref];
        if (sk.type != CadFeatureType::Sketch && sk.type != CadFeatureType::Project)
            throw std::runtime_error("surface-extrude: ref is not a sketch");
        TopoDS_Wire wire = build_sketch_wire(sk);
        if (wire.IsNull()) throw std::runtime_error("surface-extrude: empty profile");
        gp_Dir nrm(sk.plane.normal.x(), sk.plane.normal.y(), sk.plane.normal.z());
        gp_Vec v(nrm.XYZ() * f.distance);
        TopoDS_Shape shell = BRepPrimAPI_MakePrism(wire, v, false, true).Shape();
        if (shell.IsNull()) throw std::runtime_error("surface-extrude: prism failed");
        result = shell; have_body = true;
        break;
    }
    case CadFeatureType::SurfaceRevolve: {
        if (f.sketch_ref < 0 || f.sketch_ref >= int(features.size()))
            throw std::runtime_error("surface-revolve: bad sketch ref");
        const CadFeature& sk = features[f.sketch_ref];
        if (sk.type != CadFeatureType::Sketch && sk.type != CadFeatureType::Project)
            throw std::runtime_error("surface-revolve: ref is not a sketch");
        TopoDS_Wire wire = build_sketch_wire(sk);
        if (wire.IsNull()) throw std::runtime_error("surface-revolve: empty profile");
        const Vec3d& adir = (f.revolve_axis == 1) ? sk.plane.y_axis : sk.plane.x_axis;
        gp_Pnt o(sk.plane.origin.x(), sk.plane.origin.y(), sk.plane.origin.z());
        gp_Dir xd(adir.x(), adir.y(), adir.z());
        gp_Ax1 axis(o, xd);
        const double ang = f.revolve_angle * M_PI / 180.0;
        BRepPrimAPI_MakeRevol rev(wire, axis, ang, false);
        if (!rev.IsDone()) throw std::runtime_error("surface-revolve: revolve failed");
        result = rev.Shape(); have_body = true;
        break;
    }
    case CadFeatureType::SurfaceLoft: {
        std::vector<TopoDS_Wire> profiles;
        for (int ref : f.loft_profile_refs) {
            if (ref < 0 || ref >= int(features.size())
                || (features[ref].type != CadFeatureType::Sketch
                    && features[ref].type != CadFeatureType::Project))
                continue;
            profiles.push_back(build_sketch_wire(features[ref]));
        }
        if (profiles.size() < 2)
            throw std::runtime_error("surface-loft needs 2+ valid profile sketches");
        TopoDS_Shape skin = SketchEngine::make_loft_surface(profiles, f.loft_ruled);
        if (skin.IsNull()) throw std::runtime_error("surface-loft: loft failed");
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
        if (wire.IsNull()) throw std::runtime_error("surface-fill: empty boundary");
        BRepOffsetAPI_MakeFilling fill;
        int nedges = 0;
        for (TopExp_Explorer ex(wire, TopAbs_EDGE); ex.More(); ex.Next()) {
            fill.Add(TopoDS::Edge(ex.Current()), GeomAbs_C0);
            ++nedges;
        }
        if (nedges == 0) throw std::runtime_error("surface-fill: boundary has no edges");
        fill.Build();
        if (!fill.IsDone()) throw std::runtime_error("surface-fill: fill failed");
        TopoDS_Shape face = fill.Shape();
        if (face.IsNull()) throw std::runtime_error("surface-fill: produced no geometry");
        result = face; have_body = true;
        break;
    }
    case CadFeatureType::Sweep: {
        const CadFeature& sk = (f.sketch_ref >= 0 && f.sketch_ref < int(features.size())
                                && (features[f.sketch_ref].type == CadFeatureType::Sketch
                                    || features[f.sketch_ref].type == CadFeatureType::Project))
                               ? features[f.sketch_ref] : f;
        if (f.sweep_path_ref < 0 || f.sweep_path_ref >= int(features.size()))
            throw std::runtime_error("sweep needs a valid path reference");
        const CadFeature& path_feat = features[f.sweep_path_ref];
        TopoDS_Wire path;
        if (path_feat.type == CadFeatureType::Helix) {
            std::string helix_err;
            path = make_helix_spine(path_feat, helix_err);
            if (path.IsNull()) throw std::runtime_error("helix path: " + helix_err);
        } else if (path_feat.type == CadFeatureType::Sketch) {
            path = build_sketch_wire(path_feat);
        } else {
            throw std::runtime_error("sweep path must be a sketch or helix");
        }
        TopoDS_Wire profile = build_sketch_wire(sk);
        TopoDS_Shape tool   = SketchEngine::make_sweep(profile, path);
        if (!have_body || f.mode == BooleanMode::New) {
            result = tool;
            have_body = true;
        } else if (f.mode == BooleanMode::Add) {
            BRepAlgoAPI_Fuse fuse(result, tool);
            if (!fuse.IsDone()) throw std::runtime_error("fuse failed");
            result = fuse.Shape();
        } else if (f.mode == BooleanMode::Cut) {
            BRepAlgoAPI_Cut cut(result, tool);
            if (!cut.IsDone()) throw std::runtime_error("cut failed");
            result = cut.Shape();
        } else if (f.mode == BooleanMode::Intersect) {
            BRepAlgoAPI_Common common(result, tool);
            if (!common.IsDone()) throw std::runtime_error("intersect failed");
            result = common.Shape();
        }
        break;
    }
    case CadFeatureType::Loft: {
        // Loft through 2+ closed profile Sketches, in recipe order. Each profile builds
        // a wire via build_sketch_wire (so it keeps its own plane); make_loft skins them.
        std::vector<TopoDS_Wire> profiles;
        for (int ref : f.loft_profile_refs) {
            if (ref < 0 || ref >= int(features.size())
                || (features[ref].type != CadFeatureType::Sketch
                    && features[ref].type != CadFeatureType::Project))
                continue;
            profiles.push_back(build_sketch_wire(features[ref]));
        }
        if (profiles.size() < 2)
            throw std::runtime_error("loft needs 2+ valid profile sketches");
        TopoDS_Shape tool = SketchEngine::make_loft(profiles, f.loft_ruled);
        if (!have_body || f.mode == BooleanMode::New) {
            result = tool;
            have_body = true;
        } else if (f.mode == BooleanMode::Add) {
            BRepAlgoAPI_Fuse fuse(result, tool);
            if (!fuse.IsDone()) throw std::runtime_error("fuse failed");
            result = fuse.Shape();
        } else if (f.mode == BooleanMode::Cut) {
            BRepAlgoAPI_Cut cut(result, tool);
            if (!cut.IsDone()) throw std::runtime_error("cut failed");
            result = cut.Shape();
        } else if (f.mode == BooleanMode::Intersect) {
            BRepAlgoAPI_Common common(result, tool);
            if (!common.IsDone()) throw std::runtime_error("intersect failed");
            result = common.Shape();
        }
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
            if (!have_body) throw std::runtime_error("pattern needs a body");
            const SketchEntity& gc = gs.entities[f.pattern_curve_entity];
            const int n = std::max(1, f.pattern_count);
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
                if (!fuse.IsDone()) throw std::runtime_error("pattern fuse failed");
                result = fuse.Shape();
            }
            break;
        }
        // Replicate the target body. Each copy is a rigid gp_Trsf of the seed, all
        // fused into one body. Linear: i*spacing along plane axis pattern_dir
        // (0=X,1=Y). Circular: i*(angle/count) about the plane normal through the
        // plane origin (so a seed offset from the origin orbits the axis).
        if (!have_body) throw std::runtime_error("pattern needs a body");
        const int n = std::max(1, f.pattern_count);
        const TopoDS_Shape seed = result;
        for (int i = 1; i < n; ++i) {
            gp_Trsf trsf;
            if (f.pattern_circular) {
                Vec3d  o = f.plane.to_world(Vec2d(0, 0));
                gp_Ax1 ax(gp_Pnt(o.x(), o.y(), o.z()),
                          gp_Dir(f.plane.normal.x(), f.plane.normal.y(), f.plane.normal.z()));
                const double step = (f.pattern_angle * M_PI / 180.0) / double(n);
                trsf.SetRotation(ax, step * i);
            } else {
                const Vec3d& d = (f.pattern_dir == 1) ? f.plane.y_axis : f.plane.x_axis;
                trsf.SetTranslation(gp_Vec(d.x() * f.pattern_spacing * i,
                                           d.y() * f.pattern_spacing * i,
                                           d.z() * f.pattern_spacing * i));
            }
            TopoDS_Shape copy = BRepBuilderAPI_Transform(seed, trsf, true).Shape();
            BRepAlgoAPI_Fuse fuse(result, copy);
            if (!fuse.IsDone()) throw std::runtime_error("pattern fuse failed");
            result = fuse.Shape();
        }
        break;
    }
    case CadFeatureType::Rib: {
        if (!have_body) throw std::runtime_error("rib needs a body");
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
            throw std::runtime_error("rib: entity must be a line"); // ponytail: line-only for now
        // Thin rectangle centred on the line, in the sketch plane: offset both endpoints by
        // +/-thickness/2 along the in-plane perpendicular of the line direction.
        const SketchPlane& pl = sk.plane;
        Vec2d a = ln.p0, b = ln.p1;
        Vec2d dir = (b - a); double L = dir.norm();
        if (L < 1e-9) throw std::runtime_error("rib: degenerate line");
        dir /= L;
        Vec2d perp(-dir.y(), dir.x());
        double h = f.rib_thickness * 0.5;
        Vec2d q0 = a + perp*h, q1 = b + perp*h, q2 = b - perp*h, q3 = a - perp*h;
        auto w3 = [&](const Vec2d& p){ Vec3d w = pl.to_world(p); return gp_Pnt(w.x(),w.y(),w.z()); };
        BRepBuilderAPI_MakePolygon poly(w3(q0), w3(q1), w3(q2), w3(q3), Standard_True);
        if (!poly.IsDone()) throw std::runtime_error("rib: profile failed");
        TopoDS_Shape wall = SketchEngine::make_extrude(poly.Wire(), pl, f.rib_depth, false, 0.0);
        if (wall.IsNull()) throw std::runtime_error("rib: extrude failed");
        BRepAlgoAPI_Fuse fuse(result, wall);
        if (!fuse.IsDone()) throw std::runtime_error("rib: fuse failed");
        result = fuse.Shape();
        break;
    }
    case CadFeatureType::Fillet:
        if (!have_body) throw std::runtime_error("fillet needs a body");
        if (f.dressup_edge >= 0)
            result = GeometryEngine::apply_fillet(result, f.dressup_size, f.dressup_edge);
        else
            result = GeometryEngine::apply_fillet(result, f.dressup_size, f.face_group);
        break;
    case CadFeatureType::Chamfer:
        if (!have_body) throw std::runtime_error("chamfer needs a body");
        if (f.dressup_edge >= 0)
            result = GeometryEngine::apply_chamfer(result, f.dressup_size, f.dressup_edge);
        else
            result = GeometryEngine::apply_chamfer(result, f.dressup_size, f.face_group);
        break;
    case CadFeatureType::Hole: {
        if (!have_body) throw std::runtime_error("hole needs a body");
        // Circle wire centered at the positioned point on the plane
        Vec3d c = f.plane.to_world(Vec2d(f.hole_x, f.hole_y));
        gp_Pnt o(c.x(), c.y(), c.z());
        gp_Dir n(f.plane.normal.x(), f.plane.normal.y(), f.plane.normal.z());
        gp_Circ circ(gp_Ax2(o, n), f.hole_diameter * 0.5);
        TopoDS_Edge e = BRepBuilderAPI_MakeEdge(circ).Edge();
        BRepBuilderAPI_MakeWire wm(e);
        if (!wm.IsDone()) throw std::runtime_error("hole wire failed");
        // Through = symmetric huge cut (passes fully through any body);
        // Blind = +normal extrude of hole_depth into the body.
        TopoDS_Shape tool = f.hole_through
            ? SketchEngine::make_extrude(wm.Wire(), f.plane, 1.0e5, true, 0.0)
            : SketchEngine::make_extrude(wm.Wire(), f.plane, f.hole_depth, false, 0.0);
        BRepAlgoAPI_Cut cut(result, tool);
        if (!cut.IsDone()) throw std::runtime_error("hole cut failed");
        result = cut.Shape();

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
        // pitch, depth >= half-pitch, an enormous turn count, depth eating the whole wall). Better
        // a no-op than a crash. Leave the body unchanged when the spec can't be built safely.
        {
            const double R = f.thread_radius, P = f.thread_pitch, H = f.thread_height, D = f.thread_depth;
            // ISO external thread depth is ~0.61*P, so allow up to 0.7*P (0.49 wrongly rejected
            // every real thread -> nothing rendered). Still bound it well under a full pitch.
            const bool ok = R > 0.5 && P > 0.1 && D > 1e-3 && D < 0.7 * P && D < 0.45 * R
                            && H > 0.5 * P && (H / P) < 400.0;
            if (!ok) break;   // result/have_body untouched
        }
        // Axis at the positioned point on the plane; +normal = thread rise.
        Vec3d c3 = f.plane.to_world(Vec2d(f.thread_x, f.thread_y));
        gp_Pnt  c(c3.x(), c3.y(), c3.z());
        gp_Dir  zdir(f.plane.normal.x(), f.plane.normal.y(), f.plane.normal.z());
        gp_Dir  xdir(f.plane.x_axis.x(), f.plane.x_axis.y(), f.plane.x_axis.z());
        gp_Ax3  ax3(c, zdir, xdir);
        gp_Ax2  ax2(c, zdir, xdir);

        // Build the swept helical ridge (guarded — never fatal).
        TopoDS_Shape ridge;
        bool have_ridge = false;
        try {
            TopoDS_Wire spine = make_helix_wire(ax3, f.thread_radius,
                                                f.thread_pitch, f.thread_height);
            TopoDS_Wire prof  = make_thread_profile(c, xdir, zdir, f.thread_radius,
                                                    f.thread_pitch, f.thread_depth,
                                                    f.thread_internal);
            // MakePipeShell with a FIXED BINORMAL = cylinder axis keeps the V-profile's orientation
            // constant along the helix (axial edge always parallel to the axis, V always pointing
            // radially out). The plain MakePipe used a Frenet frame that TWISTED the profile around
            // the helix -> the wedge inclination varied and looked mirrored.
            BRepOffsetAPI_MakePipeShell pipe(spine);
            pipe.SetMode(zdir);
            pipe.Add(prof);
            pipe.Build();
            if (pipe.IsDone() && pipe.MakeSolid()) {
                ridge = pipe.Shape();
                have_ridge = !ridge.IsNull();
            }
        } catch (const std::exception&) {
            have_ridge = false; // fall back to the bare cylinder/bore below
        } catch (const Standard_Failure&) {
            have_ridge = false; // OCCT failure (not a std::exception) — must be caught here too
        }

        if (f.thread_internal) {
            if (!have_body) throw std::runtime_error("internal thread needs a body");
            // Tapped bore: ensure a clean cylindrical pocket, then carve the
            // OUTWARD helical groove into its wall. When the thread is invoked on
            // an existing hole the bore cut is coincident (a no-op that may report
            // !IsDone) — tolerate it so the visible groove cut below still runs.
            // Cut the pocket at the MINOR diameter (radius - depth), not the nominal radius.
            // A nominal-radius bore that coincides with an existing hole's wall creates
            // coincident faces that foul the following groove boolean (the groove then removes
            // ~nothing -> invisible thread). The minor bore stays strictly inside any existing
            // hole wall, leaving it clean for the groove; on solid stock it forms the tap-drill.
            const double bore_r = std::max(0.5, f.thread_radius - f.thread_depth);
            TopoDS_Shape bore = BRepPrimAPI_MakeCylinder(ax2, bore_r,
                                                         f.thread_height).Shape();
            try {
                BRepAlgoAPI_Cut cut_bore(result, bore);
                if (cut_bore.IsDone() && !cut_bore.Shape().IsNull())
                    result = cut_bore.Shape();
            } catch (const std::exception&) { /* keep existing bore */ }
            if (have_ridge) {
                BRepAlgoAPI_Cut cut_ridge(result, ridge);
                if (cut_ridge.IsDone() && !cut_ridge.Shape().IsNull())
                    result = cut_ridge.Shape();
            }
        } else {
            // External thread: FUSE the helical ridge ONTO the existing body (the picked cylinder),
            // leaving the rest of the part intact. Replacing the body with a bare rod — the old
            // behaviour — wiped whatever the user picked; that was the "mess". With no body yet
            // (a thread from scratch on a dropdown plane), fall back to a standalone threaded rod.
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
        if (!have_body) throw std::runtime_error("shell needs a body");
        // Hollow the body to a wall thickness; the picked face (if any) is removed so the
        // shell is open there. MakeThickSolidByJoin with a NEGATIVE offset shells inward.
        TopTools_ListOfShape remove;
        if (f.shell_face >= 0) {
            TopoDS_Face fc = GeometryEngine::face_by_index(result, f.shell_face);
            if (!fc.IsNull()) remove.Append(fc);
        }
        BRepOffsetAPI_MakeThickSolid mts;
        mts.MakeThickSolidByJoin(result, remove, -std::abs(f.shell_thickness), 1.0e-3);
        mts.Build();
        if (!mts.IsDone()) throw std::runtime_error("shell failed");
        result = mts.Shape();
        if (result.IsNull()) throw std::runtime_error("shell produced no geometry");
        break;
    }
    case CadFeatureType::Draft: {
        if (!have_body) throw std::runtime_error("draft needs a body");
        if (f.draft_face < 0) throw std::runtime_error("draft needs a picked face");
        TopoDS_Face fc = GeometryEngine::face_by_index(result, f.draft_face);
        if (fc.IsNull()) throw std::runtime_error("draft: face not found");
        // Neutral plane = horizontal plane through the body's bbox bottom, pull direction +Z.
        // The face pivots about the line where it meets the neutral plane and tilts by the angle.
        // ponytail: neutral plane / pull direction fixed to world up; pick-based neutral plane
        // deferred (same as the datum-plane pick types, snaporca-dgv).
        Bnd_Box bb; BRepBndLib::Add(result, bb);
        Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
        bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        gp_Dir pull(0, 0, 1);
        gp_Pln neutral(gp_Pnt(0, 0, zmin), pull);
        BRepOffsetAPI_DraftAngle draft(result);
        draft.Add(fc, pull, f.draft_angle * M_PI / 180.0, neutral);
        if (!draft.AddDone())
            throw std::runtime_error("draft: face cannot be drafted (is it parallel to the base?)");
        draft.Build();
        if (!draft.IsDone()) throw std::runtime_error("draft failed");
        result = draft.Shape();
        if (result.IsNull()) throw std::runtime_error("draft produced no geometry");
        break;
    }
    case CadFeatureType::DeleteFace: {
        if (!have_body) throw std::runtime_error("delete_face needs a body");
        if (f.delete_faces.empty()) throw std::runtime_error("delete_face needs at least one face");
        BRepAlgoAPI_Defeaturing df;
        df.SetShape(result);
        for (int fi : f.delete_faces) {
            TopoDS_Face fc = GeometryEngine::face_by_index(result, fi);
            if (fc.IsNull()) throw std::runtime_error("delete_face: face not found");
            df.AddFaceToRemove(fc);
        }
        df.Build();
        if (!df.IsDone()) throw std::runtime_error("delete_face failed");
        result = df.Shape();
        if (result.IsNull()) throw std::runtime_error("delete_face produced no geometry");
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
    const int tgt  = (f.target_body    >= 0 && f.target_body    < nb) ? f.target_body    : nb - 1;
    const int tool = (f.bool_tool_body >= 0 && f.bool_tool_body < nb) ? f.bool_tool_body : -1;
    if (tgt < 0 || tool < 0 || tgt == tool) return;   // need two distinct bodies; otherwise no-op
    const TopoDS_Shape A = bodies[tgt].shape;          // target survives
    const TopoDS_Shape B = bodies[tool].shape;         // tool, consumed unless kept
    if (A.IsNull() || B.IsNull()) return;

    TopTools_ListOfShape args, tools;
    args.Append(A);
    tools.Append(B);
    auto run = [&](BRepAlgoAPI_BooleanOperation& bop) -> TopoDS_Shape {
        bop.SetArguments(args);
        bop.SetTools(tools);
        if (f.bool_tolerance > 0.0) bop.SetFuzzyValue(f.bool_tolerance);   // OCCT fuzzy: merge near-coincident faces
        bop.Build();
        if (!bop.IsDone()) throw std::runtime_error("boolean operation failed");
        return bop.Shape();
    };
    TopoDS_Shape result;
    switch (f.mode) {
    case BooleanMode::Add:       { BRepAlgoAPI_Fuse   op; result = run(op); break; }   // union
    case BooleanMode::Cut:       { BRepAlgoAPI_Cut    op; result = run(op); break; }   // target - tool
    case BooleanMode::Intersect: { BRepAlgoAPI_Common op; result = run(op); break; }   // overlap
    default: return;   // BooleanMode::New is meaningless between two existing bodies
    }
    if (result.IsNull()) throw std::runtime_error("boolean produced an empty shape");

    bodies[tgt].shape = result;
    if (!f.bool_keep_tool) bodies.erase(bodies.begin() + tool);   // consume the tool body
}

void CadDocument::apply_cut(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb  = int(bodies.size());
    if (nb == 0) throw std::runtime_error("cut: no target body");
    const int tgt = (f.target_body >= 0 && f.target_body < nb) ? f.target_body : nb - 1;
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error("cut: no target body");

    if (!f.cut_keep_upper && !f.cut_keep_lower)
        throw std::runtime_error("cut keeps nothing");

    SketchPlane cp;
    if (f.cut_face >= 0) {
        const int fb = (f.cut_face_body >= 0 && f.cut_face_body < nb) ? f.cut_face_body : tgt;
        if (bodies[fb].shape.IsNull()) throw std::runtime_error("cut: face body is empty");
        TopoDS_Face fc = GeometryEngine::face_by_index(bodies[fb].shape, f.cut_face);
        if (fc.IsNull()) throw std::runtime_error("cut: face not found");
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
    if (!poly.IsDone()) throw std::runtime_error("cut: failed to build cut wire");
    TopoDS_Wire wire = poly.Wire();

    TopoDS_Shape upper_piece, lower_piece;
    const TopoDS_Shape& target = bodies[tgt].shape;

    if (f.cut_keep_upper) {
        TopoDS_Shape upper_tool = SketchEngine::make_extrude(wire, cp, L, false, 0.0);
        BRepAlgoAPI_Common common(target, upper_tool);
        if (!common.IsDone()) throw std::runtime_error("cut operation failed");
        upper_piece = common.Shape();
    }

    if (f.cut_keep_lower) {
        SketchPlane lp = cp;
        lp.normal = -lp.normal;
        TopoDS_Shape lower_tool = SketchEngine::make_extrude(wire, lp, L, false, 0.0);
        BRepAlgoAPI_Common common(target, lower_tool);
        if (!common.IsDone()) throw std::runtime_error("cut operation failed");
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
    if (nb == 0) throw std::runtime_error("mirror: no target body");
    const int tgt = (f.target_body >= 0 && f.target_body < nb) ? f.target_body : nb - 1;
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error("mirror: no target body");

    const TopoDS_Shape& src = bodies[tgt].shape;

    gp_Trsf trsf;
    trsf.SetMirror(gp_Ax2(gp_Pnt(f.plane.origin.x(), f.plane.origin.y(), f.plane.origin.z()),
                          gp_Dir(f.plane.normal.x(), f.plane.normal.y(), f.plane.normal.z())));
    BRepBuilderAPI_Transform xform(src, trsf, true /*copy*/);
    if (!xform.IsDone()) throw std::runtime_error("mirror: transform failed");
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
                throw std::runtime_error("mirror: result has zero or negative volume");
        }
    }

    switch (f.mode) {
    case BooleanMode::Add: {
        BRepAlgoAPI_Fuse fuse(src, mirrored);
        if (!fuse.IsDone()) throw std::runtime_error("mirror fuse failed");
        bodies[tgt].shape = fuse.Shape();
        break;
    }
    case BooleanMode::New: {
        if (!f.mirror_keep_original)
            bodies.erase(bodies.begin() + tgt);   // replace: the mirrored copy takes the source slot
        bodies.push_back({mirrored, f.name.empty() ? std::string("Mirror") : f.name});
        break;
    }
    default:
        throw std::runtime_error("mirror: mode must be New or Add");
    }
}

void CadDocument::apply_transform(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb  = int(bodies.size());
    if (nb == 0) throw std::runtime_error("transform: no target body");
    const int tgt = (f.target_body >= 0 && f.target_body < nb) ? f.target_body : nb - 1;
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error("transform: no target body");

    gp_Trsf rot;
    if (std::abs(f.xf_angle_deg) > 1e-12) {
        if (f.xf_axis.norm() < 1e-9)
            throw std::runtime_error("transform: rotation axis is degenerate");
        rot.SetRotation(gp_Ax1(gp_Pnt(f.xf_pivot.x(), f.xf_pivot.y(), f.xf_pivot.z()),
                               gp_Dir(f.xf_axis.x(), f.xf_axis.y(), f.xf_axis.z())),
                        f.xf_angle_deg * M_PI / 180.0);
    }
    gp_Trsf tr;
    tr.SetTranslation(gp_Vec(f.xf_translate.x(), f.xf_translate.y(), f.xf_translate.z()));
    const gp_Trsf trsf = tr * rot;          // rotate first, then translate

    BRepBuilderAPI_Transform xform(bodies[tgt].shape, trsf, true /*copy*/);
    if (!xform.IsDone()) throw std::runtime_error("transform: failed");
    TopoDS_Shape moved = xform.Shape();

    if (f.xf_copy)
        bodies.push_back({moved, f.name.empty() ? std::string("Transform") : f.name});
    else
        bodies[tgt].shape = moved;
}

void CadDocument::apply_thicken(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb = int(bodies.size());
    if (nb == 0) throw std::runtime_error("thicken: no target body");
    const int tgt = (f.target_body >= 0 && f.target_body < nb) ? f.target_body : nb - 1;
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error("thicken: no target body");

    TopoDS_Face fc = GeometryEngine::face_by_index(bodies[tgt].shape, f.thicken_face);
    if (fc.IsNull()) throw std::runtime_error("thicken: face not found");
    if (std::abs(f.thicken_thickness) < 1e-9) throw std::runtime_error("thicken: thickness is zero");

    TopoDS_Shell shell;
    BRep_Builder bb;
    bb.MakeShell(shell);
    bb.Add(shell, fc);

    const double off = f.thicken_flip ? -std::abs(f.thicken_thickness)
                                      :  std::abs(f.thicken_thickness);
    BRepOffsetAPI_MakeThickSolid mts;
    mts.MakeThickSolidBySimple(shell, off);
    mts.Build();
    if (!mts.IsDone()) throw std::runtime_error("thicken: failed");
    TopoDS_Shape solid = mts.Shape();
    if (solid.IsNull()) throw std::runtime_error("thicken: produced no geometry");

    // MakeThickSolidBySimple may produce a reversed solid. Ensure positive volume.
    {
        GProp_GProps props;
        BRepGProp::VolumeProperties(solid, props);
        if (props.Mass() < 0.0) solid.Reverse();
    }

    bodies.push_back({solid, f.name.empty() ? std::string("Thicken") : f.name});
}

void CadDocument::apply_thicken_surface(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb = int(bodies.size());
    if (nb == 0) throw std::runtime_error("thicken-surface: no target body");
    const int tgt = (f.target_body >= 0 && f.target_body < nb) ? f.target_body : nb - 1;
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error("thicken-surface: no target body");
    if (!is_sheet_shape(bodies[tgt].shape)) throw std::runtime_error("thicken-surface: target is not a sheet body");
    if (std::abs(f.thicken_thickness) < 1e-9) throw std::runtime_error("thicken-surface: thickness is zero");

    const double off = f.thicken_flip ? -std::abs(f.thicken_thickness)
                                      :  std::abs(f.thicken_thickness);
    BRepOffsetAPI_MakeThickSolid mts;
    mts.MakeThickSolidBySimple(bodies[tgt].shape, off);
    mts.Build();
    if (!mts.IsDone()) throw std::runtime_error("thicken-surface: failed");
    TopoDS_Shape solid = mts.Shape();
    if (solid.IsNull()) throw std::runtime_error("thicken-surface: produced no geometry");

    {
        GProp_GProps props;
        BRepGProp::VolumeProperties(solid, props);
        if (props.Mass() < 0.0) solid.Reverse();
    }

    bodies.push_back({solid, f.name.empty() ? std::string("ThickenSurface") : f.name});
}

void CadDocument::apply_surface_offset(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    const int nb = int(bodies.size());
    if (nb == 0) throw std::runtime_error("surface-offset: no target body");
    const int tgt = (f.target_body >= 0 && f.target_body < nb) ? f.target_body : nb - 1;
    if (tgt < 0 || bodies[tgt].shape.IsNull()) throw std::runtime_error("surface-offset: no target body");
    if (!is_sheet_shape(bodies[tgt].shape)) throw std::runtime_error("surface-offset: target is not a sheet body");
    const double d = f.plane_offset;
    if (std::abs(d) < 1e-9) throw std::runtime_error("surface-offset: zero offset");

    BRepOffsetAPI_MakeOffsetShape mos;
    mos.PerformBySimple(bodies[tgt].shape, d);
    if (!mos.IsDone()) throw std::runtime_error("surface-offset: failed");
    TopoDS_Shape off_shape = mos.Shape();
    if (off_shape.IsNull()) throw std::runtime_error("surface-offset: produced no geometry");

    bodies.push_back({off_shape, f.name.empty() ? std::string("SurfaceOffset") : f.name});
}

void CadDocument::apply_project(const std::vector<CadBody>& bodies, CadFeature& f) const
{
    f.entities.clear();
    const int nb = int(bodies.size());
    if (nb == 0) throw std::runtime_error("project: no source body");
    const int src = (f.project_source_body >= 0 && f.project_source_body < nb)
                    ? f.project_source_body : nb - 1;
    if (src < 0 || bodies[src].shape.IsNull()) throw std::runtime_error("project: source body is empty");
    const TopoDS_Shape& shape = bodies[src].shape;

    std::vector<TopoDS_Edge> edges;
    if (!f.project_edges.empty()) {
        for (int id : f.project_edges) {
            TopoDS_Edge e = GeometryEngine::edge_by_index(shape, id);
            if (e.IsNull()) throw std::runtime_error("project: edge not found");
            edges.push_back(e);
        }
    } else if (f.project_face >= 0) {
        TopoDS_Face fc = GeometryEngine::face_by_index(shape, f.project_face);
        if (fc.IsNull()) throw std::runtime_error("project: face not found");
        edges = GeometryEngine::edges_of_face(fc);
    } else {
        // No face and no explicit selection means "all edges" — the state the Project card
        // starts in, and its label says so. Edges perpendicular to the target plane collapse
        // to a point when projected; they are dropped below rather than emitted as
        // zero-length lines.
        edges = GeometryEngine::edges_of(shape);
    }
    if (edges.empty()) throw std::runtime_error("project: no edges to project");

    auto to2d = [&](const gp_Pnt& p) -> Vec2d {
        Vec3d d(p.X() - f.plane.origin.x(), p.Y() - f.plane.origin.y(), p.Z() - f.plane.origin.z());
        return Vec2d(d.dot(f.plane.x_axis), d.dot(f.plane.y_axis));
    };

    // A segment whose endpoints coincide after projection carries no geometry: that is what
    // an edge perpendicular to the target plane becomes. Emitting it as a zero-length line
    // would poison the sketch downstream, so drop it here.
    auto push_line = [&](const Vec2d& a, const Vec2d& b) {
        if ((b - a).norm() < 1e-7) return;
        SketchEntity se; se.type = SketchEntity::Type::Line;
        se.p0 = a; se.p1 = b;
        f.entities.push_back(se);
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
            const double par = std::abs(cnv.dot(f.plane.normal));
            const bool full = BRep_Tool::IsClosed(e) ||
                std::abs((ac.LastParameter() - ac.FirstParameter()) - 2.0 * M_PI) < 1e-6;
            if (par > 0.999) {
                Vec2d ctr = to2d(c.Location());
                if (full) {
                    SketchEntity se; se.type = SketchEntity::Type::Circle;
                    se.center = ctr; se.radius = c.Radius();
                    f.entities.push_back(se);
                } else {
                    gp_Pnt a = ac.Value(ac.FirstParameter());
                    gp_Pnt b = ac.Value(ac.LastParameter());
                    Vec2d a2 = to2d(a), b2 = to2d(b);
                    SketchEntity se; se.type = SketchEntity::Type::Arc;
                    se.center = ctr; se.radius = c.Radius();
                    se.p0 = a2; se.p1 = b2;
                    se.start_angle = std::atan2(a2.y() - ctr.y(), a2.x() - ctr.x());
                    se.end_angle   = std::atan2(b2.y() - ctr.y(), b2.x() - ctr.x());
                    f.entities.push_back(se);
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
    if (f.entities.empty()) throw std::runtime_error("project: produced no entities");
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
            std::string first_name = features[first_fi].name.empty() ? "Mate" : features[first_fi].name;
            std::string this_name = f.name.empty() ? "Mate" : f.name;
            mate_conflicts.push_back({fi,
                "Body " + std::to_string(dst + 1) + " is already positioned by '" +
                first_name + "' (feature " + std::to_string(first_fi) +
                ") — '" + this_name + "' overrides it; suppress one"});
        } else {
            first_driver[dst] = fi;
        }

        // (b) self-mate or cycle detection
        if (src >= 0 && dst >= 0) {
            if (src == dst) {
                mate_conflicts.push_back({fi,
                    "this mate positions Body " + std::to_string(dst + 1) + " against itself"});
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
                            "circular mate chain: Body " + std::to_string(top.node + 1) +
                            " depends on Body " + std::to_string(child + 1) +
                            ", which leads back to Body " + std::to_string(top.node + 1) +
                            " — the result depends on feature order"});
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
    if (!A.error.empty()) throw std::runtime_error("mate: " + A.error);
    if (!B.error.empty()) throw std::runtime_error("mate: " + B.error);

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
    if (!xform.IsDone()) throw std::runtime_error("mate: transform failed");
    bodies[tgt_body].shape = xform.Shape();
}

// Volume of a shape, 0 for anything that isn't a solid we can measure. Used to catch a
// subtraction that removed nothing (snaporca-daf).
static double solid_volume(const TopoDS_Shape& s)
{
    if (s.IsNull()) return 0.0;
    GProp_GProps props;
    BRepGProp::VolumeProperties(s, props);
    return std::abs(props.Mass());
}

void CadDocument::route_feature(std::vector<CadBody>& bodies, const CadFeature& f) const
{
    if (f.type == CadFeatureType::Plane)   return;   // datum plane: not part of the body pipeline
    if (f.type == CadFeatureType::Axis)    return;   // datum axis
    if (f.type == CadFeatureType::CoordSys) return; // datum coordinate system
    if (f.type == CadFeatureType::Helix)   return;   // helical curve; consumed by Sweep
    if (f.type == CadFeatureType::Boolean) { apply_boolean(bodies, f); return; }   // body-body op
    if (f.type == CadFeatureType::Cut)     { apply_cut(bodies, f);     return; }   // plane-split body
    if (f.type == CadFeatureType::Mirror)  { apply_mirror(bodies, f);  return; }   // mirror body about plane
    if (f.type == CadFeatureType::Transform) { apply_transform(bodies, f); return; }   // move/rotate body
    if (f.type == CadFeatureType::Mate)     { apply_mate(bodies, f);     return; }   // assembly mate
    if (f.type == CadFeatureType::Thicken) { apply_thicken(bodies, f); return; }   // face -> plate
    if (f.type == CadFeatureType::ThickenSurface) { apply_thicken_surface(bodies, f); return; }
    if (f.type == CadFeatureType::SurfaceOffset) { apply_surface_offset(bodies, f); return; }
    if (f.type == CadFeatureType::Project) return;   // sketch-like: consumed downstream, no body
    // Resolve the target body: explicit target_body when valid, else the last body.
    const int t = (f.target_body >= 0 && f.target_body < int(bodies.size()))
                  ? f.target_body : int(bodies.size()) - 1;
    const TopoDS_Shape context = (t >= 0) ? bodies[t].shape : TopoDS_Shape();
    // A New extrude (or the very first solid feature) starts a fresh body; everything else
    // mutates the target body in place.
    const bool starts_new = bodies.empty()
        || f.type == CadFeatureType::Import   // an imported solid is always its own base body
        || f.type == CadFeatureType::SurfaceExtrude || f.type == CadFeatureType::SurfaceRevolve
        || f.type == CadFeatureType::ThickenSurface || f.type == CadFeatureType::SurfaceOffset
        || f.type == CadFeatureType::SurfaceLoft || f.type == CadFeatureType::SurfaceFill
        || ((f.type == CadFeatureType::Extrude || f.type == CadFeatureType::Revolve
             || f.type == CadFeatureType::Sweep || f.type == CadFeatureType::Loft)
            && f.mode == BooleanMode::New);

    if (starts_new) {
        TopoDS_Shape result;            // empty -> apply_feature fills it (New path)
        bool have_body = false;
        apply_feature(result, have_body, context, f);
        if (have_body && !result.IsNull())
            bodies.push_back({ result, f.name.empty() ? std::string("Body") : f.name });
    } else {
        if (t < 0) throw std::runtime_error("feature needs a body");
        TopoDS_Shape result = bodies[t].shape;   // shallow handle; apply_feature mutates it
        bool have_body = true;
        // A subtraction whose tool misses the target is a legal boolean that removes nothing, so
        // OCCT reports IsDone() and the feature lands in the recipe reporting success. A caller —
        // an agent especially — then has no signal at all that the hole it asked for was never
        // drilled: same body, same volume, ok:true. Measure the volume across the op and refuse
        // the no-op. Only for removals: every other feature type may legitimately leave the volume
        // alone (a Transform certainly does). snaporca-daf.
        const bool removes = f.type == CadFeatureType::Hole
                          || f.type == CadFeatureType::Thread
                          || ((f.type == CadFeatureType::Extrude || f.type == CadFeatureType::Revolve
                               || f.type == CadFeatureType::Sweep  || f.type == CadFeatureType::Loft)
                              && f.mode == BooleanMode::Cut);
        const double before = removes ? solid_volume(result) : 0.0;
        apply_feature(result, have_body, context, f);
        if (removes && before > 0.0) {
            const double after = solid_volume(result);
            // Relative tolerance: a cut that shaves a numerically invisible sliver is a miss too,
            // and an absolute epsilon would be wrong across the mm-to-metre range of real parts.
            if (after >= before - 1e-9 * std::max(1.0, before))
                throw std::runtime_error(std::string(
                    f.type == CadFeatureType::Hole   ? "hole" :
                    f.type == CadFeatureType::Thread ? "thread" : "cut") +
                    " removed no material — the tool does not intersect the target body"
                    " (coordinates are in the sketch plane's frame, not world)");
        }
        bodies[t].shape = result;
    }
}

bool CadDocument::recompute()
{
    error.clear();
    detect_mate_conflicts();
    std::vector<CadBody> built;
    try {
        // Parametric pass: evaluate document variables, then each feature's expression bindings,
        // writing the results into the feature's numeric fields before geometry runs.
        std::map<std::string, double> varvals = evaluate_variables(variables);
        for (CadFeature& f : features)
            for (const auto& [field, e] : f.expr)
                assign_field(f, field, eval_expr(e, varvals));
        for (size_t fi = 0; fi < features.size(); ++fi) {
            CadFeature& f = features[fi];
            if (!f.enabled) continue;
            if (f.type == CadFeatureType::Sketch) continue; // consumed by an extrude
            if (f.type == CadFeatureType::Helix)  continue; // consumed by Sweep as a path
            if (f.type == CadFeatureType::Plane)   continue; // datum: no solid, derived on demand
            if (f.type == CadFeatureType::Axis)    continue; // datum axis
            if (f.type == CadFeatureType::CoordSys) continue; // datum coordinate system
            if (f.type == CadFeatureType::Project) { apply_project(built, f); }
            else                                   { route_feature(built, f); }
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
    } catch (const Standard_Failure& e) {
        // OCCT raises Standard_Failure (NOT a std::exception) — must be caught
        // here or it escapes the event handler and terminates the app.
        error = e.GetMessageString() ? e.GetMessageString() : "OCCT operation failed";
        return false;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    } catch (...) {
        error = "unknown geometry error";
        return false;
    }
    if (built.empty()) { error = "no solid-producing features"; return false; }

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
                ? std::string("an unidentified feature")
                : (features[src].name.empty() ? ("feature " + std::to_string(src + 1))
                                              : features[src].name);
        error = "body " + std::to_string(i + 1) + " was destroyed by " + fname
              + " (the operation produced an empty shape)";
        return false;
    }

    // recompute() replaces the bodies vector wholesale, which would drop any per-body
    // colour override (Color tool). Body indices are stable across a rebuild (bodies are
    // appended in feature order), so carry the override forward by index — same indexing
    // contract the GUI relies on for per-body visibility/Move.
    for (size_t i = 0; i < built.size() && i < bodies.size(); ++i) {
        if (bodies[i].has_color) {
            built[i].has_color = true;
            built[i].color     = bodies[i].color;
        }
    }
    bodies = std::move(built);

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
                "connector \"" + f.name + "\" may have moved to a different face "
                "(an upstream edit renumbered this body's faces)");
        }
    }

    body = compound_of(bodies);
    display_mesh = tessellate_bodies(bodies, display_tri_face, display_tri_body,
                                     display_body_meshes,
                                     linear_deflection, angular_deflection);
    if (display_mesh.its.indices.empty()) {
        error = "tessellation produced an empty mesh";
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
    } catch (const Standard_Failure& e) {
        err = e.GetMessageString() ? e.GetMessageString() : "OCCT operation failed";
        return false;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    } catch (...) {
        err = "unknown geometry error";
        return false;
    }
    if (tmp.empty()) {
        err = "preview produced no geometry";
        return false;
    }
    // Tessellate per body (same path as recompute) so the GUI can re-apply its display-only
    // per-body Move transforms to the ghost; out_mesh is the merged whole.
    std::vector<int> tf, tb;
    out_mesh = tessellate_bodies(tmp, tf, tb, out_body_meshes, linear_deflection, angular_deflection);
    if (out_mesh.its.indices.empty()) {
        err = "preview produced an empty mesh";
        return false;
    }
    return true;
}

bool CadDocument::preview(const CadFeature& candidate, TriangleMesh& out_mesh, std::string& err) const
{
    std::vector<TriangleMesh> ignore;
    return preview(candidate, out_mesh, ignore, err);
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
        uint32_t v = SNAPORCA_CAD_RECIPE_VERSION;
        ar(v);
        ar(features);
        ar(variables);
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
        if (v > SNAPORCA_CAD_RECIPE_VERSION) {
            error = "saved with a newer version of SnapOrca CAD (format v"
                  + std::to_string(v) + ", this build reads up to v"
                  + std::to_string(SNAPORCA_CAD_RECIPE_VERSION) + ")";
            return false;
        }
        if (v < SNAPORCA_CAD_RECIPE_VERSION) {
            error = "saved with an older version of SnapOrca CAD (format v"
                  + std::to_string(v) + "); this project cannot be opened by this build";
            return false;
        }
        ar(features);
        ar(variables);
        return recompute();
    } catch (const Standard_Failure& e) {
        const char* what = e.GetMessageString();
        error = std::string("CAD data could not be read")
              + (what && *what ? ": " + std::string(what) : "");
        return false;
    } catch (const std::exception& e) {
        error = std::string("CAD data could not be read: ") + e.what();
        return false;
    } catch (...) {
        error = "CAD data could not be read";
        return false;
    }
}

bool CadDocument::export_step(const std::string& path,
                             const std::vector<Transform3d>& body_xforms,
                             std::string& err) const
{
    err.clear();
    if (bodies.empty()) { err = "nothing to export"; return false; }
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
            err = "STEP transfer failed";
            return false;
        }
        if (writer.Write(path.c_str()) != IFSelect_RetDone) {
            err = "cannot write STEP file";
            return false;
        }
    } catch (const Standard_Failure& e) {
        err = e.GetMessageString() ? e.GetMessageString() : "OCCT failed to write STEP";
        return false;
    } catch (const std::exception& e) {
        err = e.what();
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
    features.push_back(f);
    return int(features.size()) - 1;
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
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_surface_loft(const std::vector<int>& profile_refs, bool ruled, const std::string& name)
{
    CadFeature f;
    f.type              = CadFeatureType::SurfaceLoft;
    f.name              = name;
    f.loft_profile_refs = profile_refs;
    f.loft_ruled        = ruled;
    f.mode              = BooleanMode::New;
    features.push_back(f);
    return int(features.size()) - 1;
}

int CadDocument::add_surface_fill(int sketch_ref, const std::string& name)
{
    CadFeature f;
    f.type       = CadFeatureType::SurfaceFill;
    f.name       = name;
    f.sketch_ref = sketch_ref;
    f.mode       = BooleanMode::New;
    features.push_back(f);
    return int(features.size()) - 1;
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
            // A boolean that blows up on one pair must not lose the report for the others,
            // and OCCT signals those as Standard_Failure, which is NOT a std::exception.
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
