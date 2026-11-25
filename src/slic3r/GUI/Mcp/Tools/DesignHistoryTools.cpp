// The place Design, its History heading: feature_describe and feature_edit, a feature as its card shows and edits it;
// feature_set_expression; and the Feature tree's edits, feature_delete, feature_move, feature_suppress and
// feature_rename. history_silent_forms() binds them to their tool blocks, and run_design_tool() (DesignTools.cpp) runs
// them behind the Design commands' gate.

#include "slic3r/GUI/Mcp/Tools/DesignCommon.hpp"
#include "slic3r/GUI/Mcp/McpToolsDesign.hpp"
#include "libslic3r/Point.hpp"
#include <stdexcept>
#include <math.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <utility>
#include <initializer_list>
#include <map>
#include <optional>

#include <nlohmann/json.hpp>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/format.hpp"

#include <TopoDS_Shape.hxx>

using json = nlohmann::json;

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// ---- A feature as its card shows it: feature_describe and feature_edit --------------------------

// Names in the Design commands' vocabulary, in enum order, that only feature_describe and feature_edit use; those the
// other headings read too, BooleanMode's kOperations among them, are in DesignCommon.hpp.
const char* const kShapes[]         = {"rectangle", "circle"};                     // SketchShape
const char* const kAxisTypes[]      = {"two_points", "face_normal", "cylinder", "plane_intersection", "along_edge"};
const char* const kCoordSysTypes[]  = {"point_world", "face_and_direction"};

// A profile sketch's constraint, on the profile's point indices.
json profile_constraint_json(const SketchConstraintDef& c, int index)
{
    json points = json::array();
    for (int p : {c.a, c.b, c.c, c.d})
        if (p >= 0) points.push_back(p);
    json j{{"index", index}, {"type", enum_name(kConstraintTypes, int(c.type))}, {"points", std::move(points)}};
    if (json v = constraint_value(c.type, c.value); !v.is_null()) j["value"] = std::move(v);
    return j;
}

// A feature's card as data. `params` is what the card shows; `editable` the fields its Confirm
// writes, named as the CadFeature members (the names feature_set_expression binds), but for the
// members that hold a variant, read and written under their command's argument in its words: mode
// as `operation` (kOperations), and hole_style, mate_kind and pattern_circular as `type`; `ranges` the limits
// its spin controls set on the numeric ones; `refs` what it refers to (feature and body indices, face
// and edge ids, read in the model before it but for a datum's), which feature_edit's refs re-points
// (cad_apply_feature_refs()); `expr` the bindings.
// feature_describe shows it and feature_edit checks against it, so the two cannot disagree.
json feature_card(const CadFeature& f)
{
    json params = json::object(), ranges = json::object(), refs = json::object(), editable = json::array();
    auto show = [&](const char* k, json v) { params[k] = std::move(v); };
    auto edit = [&](const char* k, json v) { params[k] = std::move(v); editable.push_back(k); };
    auto num  = [&](const char* k, double v, double lo, double hi) { edit(k, v); ranges[k] = json::array({lo, hi}); };
    auto ref  = [&](const char* k, json v) { refs[k] = std::move(v); };
    using T = CadFeatureType;
    switch (f.type) {
    case T::Sketch:
        if (!f.imported_regions.empty()) {
            // Text or SVG art, vectorised once: its placement edits, and a Text feature's words, font and height,
            // which feature_edit outlines again.
            if (f.is_text()) {
                edit("text_string", f.text_string);
                edit("text_font", f.text_font);
                edit("text_height", f.text_height);
            }
            show("regions", int(f.imported_regions.size()));
            // Per region, its closed loops and the area it fills as placed, in mm². A region keeps its
            // outer loop first and its holes after it, so it has 1 + holes loops, and it fills the
            // outer loop's area less its holes', times the placement's scale. A glyph or path that
            // vectorised into a sliver or with a stray hole shows here, where the region count alone
            // does not.
            json loops = json::array(), areas = json::array();
            for (const auto& region : f.imported_regions) {
                loops.push_back(int(region.size()));
                double filled = 0.0;
                for (size_t c = 0; c < region.size(); ++c) {
                    const std::vector<Vec2d>& loop  = region[c];
                    double                    twice = 0.0;   // the shoelace sum: twice the signed area
                    for (size_t i = 0; i < loop.size(); ++i) {
                        const Vec2d& a = loop[i];
                        const Vec2d& b = loop[(i + 1) % loop.size()];
                        twice += a.x() * b.y() - b.x() * a.y();
                    }
                    filled += (c == 0 ? 0.5 : -0.5) * std::abs(twice);
                }
                areas.push_back(filled * std::abs(f.import_scale_x * f.import_scale_y));
            }
            show("region_loops", std::move(loops));
            show("region_areas", std::move(areas));
            edit("import_offset", json::array({f.import_offset.x(), f.import_offset.y()}));
            num("import_scale_x", f.import_scale_x, 0.001, 1000);
            num("import_scale_y", f.import_scale_y, 0.001, 1000);
            if (f.import_on_face) ref("import_face_body", f.import_face_body);
        } else if (f.entities.empty() && f.profile.points.empty()) {   // the Sketch card's shape
            edit("shape", enum_name(kShapes, int(f.shape)));
            num("width", f.width, 0.1, 1000);
            num("height", f.height, 0.1, 1000);
            num("radius", f.radius, 0.1, 1000);
        }
        break;
    case T::Extrude:
        num("distance", f.distance, -1000, 1000);
        num("distance2", f.distance2, 0, 100000);
        num("taper_deg", f.taper_deg, -89, 89);
        edit("extrude_end", enum_name(kExtrudeEnds, int(f.extrude_end)));
        edit("flip", f.flip);
        edit("operation", enum_name(kOperations, int(f.mode)));
        ref("sketch_ref", f.sketch_ref);
        ref("target_body", f.target_body);
        ref("up_to_face", f.up_to_face);
        ref("extrude_src_face", f.extrude_src_face);
        break;
    case T::Revolve:
        num("revolve_angle", f.revolve_angle, 1, 360);
        edit("revolve_axis", f.revolve_axis);
        edit("flip", f.flip);
        edit("operation", enum_name(kOperations, int(f.mode)));
        ref("sketch_ref", f.sketch_ref);
        ref("revolve_axis_entity", f.revolve_axis_entity);
        ref("target_body", f.target_body);
        break;
    case T::Sweep:
        edit("operation", enum_name(kOperations, int(f.mode)));
        ref("sketch_ref", f.sketch_ref);
        ref("sweep_path_ref", f.sweep_path_ref);
        ref("target_body", f.target_body);
        break;
    case T::Loft:
        edit("operation", enum_name(kOperations, int(f.mode)));
        edit("loft_ruled", f.loft_ruled);
        ref("loft_profile_refs", f.loft_profile_refs);
        ref("target_body", f.target_body);
        break;
    case T::Fillet:
    case T::Chamfer:
        num("dressup_size", f.dressup_size, 0.1, 1000);
        ref("edges", f.dressup_edge_ids());
        ref("face_group", enum_name(kFaceGroups, int(f.face_group)));
        ref("target_body", f.target_body);
        break;
    case T::Hole:
        num("hole_diameter", f.hole_diameter, 0.1, 1000);
        num("hole_depth", f.hole_depth, 0.1, 1000);
        edit("hole_through", f.hole_through);
        num("hole_x", f.hole_x, -1000, 1000);
        num("hole_y", f.hole_y, -1000, 1000);
        // The seat, which the card does not show; solid_hole's type and designation set it.
        edit("type", enum_name(kHoleTypes, f.hole_style));
        num("hole_cbore_diameter", f.hole_cbore_diameter, 0, 1000);
        num("hole_cbore_depth", f.hole_cbore_depth, 0, 1000);
        num("hole_csink_diameter", f.hole_csink_diameter, 0, 1000);
        num("hole_csink_angle", f.hole_csink_angle, 1, 179);
        show("hole_standard", f.hole_standard);
        ref("target_body", f.target_body);
        break;
    case T::Thread:
        // The card shows the diameter, the size a thread is named by; either one may be set.
        num("thread_diameter", 2.0 * f.thread_radius, 0.1, 1000);
        num("thread_radius", f.thread_radius, 0.05, 500);
        num("thread_pitch", f.thread_pitch, 0.1, 1000);
        num("thread_height", f.thread_height, 0.1, 1000);
        num("thread_depth", f.thread_depth, 0.1, 1000);
        edit("thread_internal", f.thread_internal);
        num("thread_x", f.thread_x, -1000, 1000);
        num("thread_y", f.thread_y, -1000, 1000);
        ref("target_body", f.target_body);
        break;
    case T::Shell:
        num("shell_thickness", f.shell_thickness, 0.01, 100000);
        ref("shell_face", f.shell_face);
        ref("target_body", f.target_body);
        break;
    case T::Draft:
        num("draft_angle", f.draft_angle, -89, 89);
        ref("draft_face", f.draft_face);
        ref("target_body", f.target_body);
        break;
    case T::Thicken:
        num("thicken_thickness", f.thicken_thickness, 0.01, 100000);
        edit("thicken_flip", f.thicken_flip);
        ref("thicken_face", f.thicken_face);
        ref("target_body", f.target_body);
        break;
    case T::ThickenSurface:
        num("thicken_thickness", f.thicken_thickness, 0.01, 100000);
        edit("thicken_flip", f.thicken_flip);
        ref("target_body", f.target_body);
        break;
    case T::Pattern:
        // A pattern along a curve follows its sketch's entity, which no field changes.
        if (f.pattern_curve_sketch >= 0)
            show("type", enum_name(kPatternTypes, 2));
        else
            edit("type", enum_name(kPatternTypes, f.pattern_circular ? 1 : 0));
        num("pattern_count", f.pattern_count, 1, 999);
        num("pattern_spacing", f.pattern_spacing, 0.01, 100000);
        edit("pattern_dir", f.pattern_dir);
        num("pattern_angle", f.pattern_angle, 1, 360);
        ref("pattern_curve_sketch", f.pattern_curve_sketch);
        ref("pattern_curve_entity", f.pattern_curve_entity);
        ref("target_body", f.target_body);
        break;
    case T::Boolean:
        edit("operation", enum_name(kOperations, int(f.mode)));
        edit("bool_keep_tool", f.bool_keep_tool);
        num("bool_tolerance", f.bool_tolerance, 0, 100);
        ref("target_body", f.target_body);
        ref("bool_tool_body", f.bool_tool_body);
        ref("bool_target_face", f.bool_target_face);
        ref("bool_tool_face", f.bool_tool_face);
        break;
    case T::Cut:
        num("cut_offset", f.cut_offset, -10000, 10000);
        edit("cut_flip", f.cut_flip);
        edit("cut_keep_upper", f.cut_keep_upper);
        edit("cut_keep_lower", f.cut_keep_lower);
        ref("cut_face_body", f.cut_face_body);
        ref("cut_face", f.cut_face);
        ref("target_body", f.target_body);
        break;
    case T::Mirror:
        edit("mirror_keep_original", f.mirror_keep_original);
        edit("operation", enum_name(kOperations, int(f.mode)));
        ref("target_body", f.target_body);
        break;
    case T::Transform:
        edit("xf_translate", vec3(f.xf_translate));
        edit("xf_axis", vec3(f.xf_axis));
        edit("xf_pivot", vec3(f.xf_pivot));
        num("xf_angle_deg", f.xf_angle_deg, -360, 360);
        edit("xf_copy", f.xf_copy);
        ref("target_body", f.target_body);
        break;
    case T::Plane:
        num("plane_offset", f.plane_offset, -100000, 100000);
        num("plane_angle_tilt", f.plane_angle_tilt, -180, 180);
        num("plane_u_size", f.plane_u_size, 1, 100000);
        num("plane_v_size", f.plane_v_size, 1, 100000);
        ref("plane_type", enum_name(kPlaneTypes, int(f.plane_type)));
        ref("plane_base", f.plane_base);
        ref("plane_axis", f.plane_axis);
        ref("plane_face_body", f.plane_face_body);
        ref("plane_face", f.plane_face);
        ref("plane_face2_body", f.plane_face2_body);
        ref("plane_face2", f.plane_face2);
        ref("plane_edge_body", f.plane_edge_body);
        ref("plane_edge", f.plane_edge);
        ref("plane_edge2_body", f.plane_edge2_body);
        ref("plane_edge2", f.plane_edge2);
        break;
    case T::Helix:
        num("helix_radius", f.helix_radius, 0.01, 10000);
        num("helix_pitch", f.helix_pitch, 0.01, 10000);
        num("helix_height", f.helix_height, 0.01, 10000);
        num("helix_taper_deg", f.helix_taper_deg, -89, 89);
        edit("helix_left_handed", f.helix_left_handed);
        break;
    case T::Rib:
        num("rib_thickness", f.rib_thickness, 0.01, 100000);
        num("rib_depth", f.rib_depth, 0.01, 100000);
        ref("rib_sketch_ref", f.rib_sketch_ref);
        ref("rib_entity", f.rib_entity);
        ref("target_body", f.target_body);
        break;
    case T::SurfaceExtrude:
        num("distance", f.distance, 0.1, 1000);
        ref("sketch_ref", f.sketch_ref);
        break;
    case T::SurfaceRevolve:
        num("revolve_angle", f.revolve_angle, 1, 360);
        edit("revolve_axis", f.revolve_axis);
        edit("flip", f.flip);
        ref("sketch_ref", f.sketch_ref);
        ref("revolve_axis_entity", f.revolve_axis_entity);
        break;
    case T::SurfaceOffset:
        num("plane_offset", f.plane_offset, -100000, 100000);
        ref("target_body", f.target_body);
        break;
    case T::Mate:
        edit("type", enum_name(kAlignTypes, f.mate_kind));
        num("mate_offset", f.mate_offset, -100000, 100000);
        num("mate_angle", f.mate_angle, -360, 360);
        edit("mate_flip", f.mate_flip);
        ref("mate_cs_a", f.mate_cs_a);
        ref("mate_cs_b", f.mate_cs_b);
        break;
    case T::Axis:
        ref("axis_type", enum_name(kAxisTypes, int(f.axis_type)));
        ref("axis_p1", vec3(f.axis_p1));
        ref("axis_p2", vec3(f.axis_p2));
        ref("axis_body", f.axis_body);
        ref("axis_face", f.axis_face);
        ref("axis_edge", f.axis_edge);
        ref("axis_plane_a", f.axis_plane_a);
        ref("axis_plane_b", f.axis_plane_b);
        break;
    case T::CoordSys:
        ref("coordsys_type", enum_name(kCoordSysTypes, int(f.coordsys_type)));
        ref("coordsys_point", vec3(f.coordsys_point));
        ref("coordsys_body", f.coordsys_body);
        ref("coordsys_face", f.coordsys_face);
        ref("coordsys_edge", f.coordsys_edge);
        ref("coordsys_x_hint", vec3(f.coordsys_x_hint));
        break;
    case T::Project:
        ref("project_source_body", f.project_source_body);
        ref("project_edges", f.project_edges);
        ref("project_face", f.project_face);
        break;
    case T::DeleteFace:
        ref("delete_faces", f.delete_faces);
        ref("target_body", f.target_body);
        break;
    case T::SurfaceLoft:
        show("loft_ruled", f.loft_ruled);
        ref("loft_profile_refs", f.loft_profile_refs);
        break;
    case T::SurfaceFill:
        ref("sketch_ref", f.sketch_ref);
        break;
    case T::Import:
        break;
    }
    json expr = json::object();
    for (const auto& [field, e] : f.expr) expr[field] = e;
    return json{{"params", std::move(params)}, {"editable", std::move(editable)}, {"ranges", std::move(ranges)},
                {"expr", std::move(expr)}, {"refs", std::move(refs)}};
}

// Writes one editable field that is not a bindable number: a card's check box, choice or vector.
// Throws for a value of the wrong type or one the card does not offer.
void set_card_choice(CadFeature& f, const std::string& k, const json& v)
{
    auto flag = [&](bool& field) {
        if (!v.is_boolean()) throw std::runtime_error(k + " must be true or false");
        field = v.get<bool>();
    };
    auto choice = [&](int& field, int hi) {
        if (!v.is_number_integer() || v.get<int>() < 0 || v.get<int>() > hi)
            throw std::runtime_error(k + " must be an integer from 0 to " + std::to_string(hi));
        field = v.get<int>();
    };
    auto point3 = [&](Vec3d& field, bool direction) {
        if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number())
            throw std::runtime_error(k + " must be [x, y, z]");
        const Vec3d p(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
        if (direction && !(p.norm() > 1e-9)) throw std::runtime_error(k + " must not be zero");
        if (!direction && !(p.cwiseAbs().maxCoeff() <= 100000))
            throw std::runtime_error(k + " must be between -100000 and 100000 on each axis");
        field = p;
    };
    if (k == "flip")                 return flag(f.flip);
    if (k == "hole_through")         return flag(f.hole_through);
    if (k == "thread_internal")      return flag(f.thread_internal);
    if (k == "loft_ruled")           return flag(f.loft_ruled);
    if (k == "bool_keep_tool")       return flag(f.bool_keep_tool);
    if (k == "cut_flip")             return flag(f.cut_flip);
    if (k == "cut_keep_upper")       return flag(f.cut_keep_upper);
    if (k == "cut_keep_lower")       return flag(f.cut_keep_lower);
    if (k == "mirror_keep_original") return flag(f.mirror_keep_original);
    if (k == "thicken_flip")         return flag(f.thicken_flip);
    if (k == "helix_left_handed")    return flag(f.helix_left_handed);
    if (k == "xf_copy")              return flag(f.xf_copy);
    if (k == "mate_flip")            return flag(f.mate_flip);
    if (k == "pattern_dir")          return choice(f.pattern_dir, 1);
    if (k == "revolve_axis") {
        // Choosing the plane's X or Y axis on the card drops a sketch line chosen as the axis.
        choice(f.revolve_axis, 1);
        f.revolve_axis_entity = -1;
        return;
    }
    if (k == "xf_translate") return point3(f.xf_translate, false);
    if (k == "xf_pivot")     return point3(f.xf_pivot, false);
    if (k == "xf_axis")      return point3(f.xf_axis, true);
    if (k == "import_offset") {
        if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number())
            throw std::runtime_error(k + " must be [x, y]");
        const Vec2d p(v[0].get<double>(), v[1].get<double>());
        if (!(p.cwiseAbs().maxCoeff() <= 100000)) throw std::runtime_error(k + " must be between -100000 and 100000 on each axis");
        f.import_offset = p;
        return;
    }
    // A Text feature's words, height and font, as sketch_text takes them. Written here only: the outline is the
    // panel's (DesignPanel::mcp_outline_text()), which feature_edit runs on them.
    if (k == "text_string") {
        if (!v.is_string() || v.get<std::string>().empty()) throw std::runtime_error("text_string must be the words to outline");
        f.text_string = v.get<std::string>();
        return;
    }
    if (k == "text_height") {
        if (!v.is_number() || !(v.get<double>() >= 0.5 && v.get<double>() <= 500.0))
            throw std::runtime_error("text_height must be 0.5 to 500 mm");
        f.text_height = v.get<double>();
        return;
    }
    if (k == "text_font") {
        if (!v.is_string() || v.get<std::string>().empty())
            throw std::runtime_error("text_font must be a font's face name, or the stored value feature_describe reports");
        f.text_font = v.get<std::string>();
        return;
    }
    if (k == "shape") {
        f.shape = SketchShape(enum_index(k, v, kShapes));
        return;
    }
    if (k == "type") {
        // A hole's seat, an align's kind and a pattern's layout, in their command's words.
        if (f.type == CadFeatureType::Hole) {
            f.hole_style = enum_index(k, v, kHoleTypes);
            return;
        }
        if (f.type == CadFeatureType::Mate) {
            f.mate_kind = enum_index(k, v, kAlignTypes);
            return;
        }
        if (f.type == CadFeatureType::Pattern) {
            const int layout = enum_index(k, v, kPatternTypes);
            if (layout == 2)
                throw std::runtime_error("a pattern's type is linear or circular; a curve pattern is made with "
                                         "solid_pattern {type: \"curve\"}");
            f.pattern_circular = layout == 1;
            return;
        }
    }
    if (k == "operation") {
        // The member mode, in the words the feature's card offers: a Boolean combines two bodies, so it makes no new
        // one; a Mirror adds its mirrored body or joins it back.
        const BooleanMode m = BooleanMode(enum_index(k, v, kOperations));
        if (f.type == CadFeatureType::Boolean && m == BooleanMode::New)
            throw std::runtime_error("a Boolean's operation is join, cut or intersect");
        if (f.type == CadFeatureType::Mirror && m != BooleanMode::New && m != BooleanMode::Add)
            throw std::runtime_error("a Mirror's operation is new_body or join");
        f.mode = m;
        return;
    }
    if (k == "extrude_end") {
        // The face or vertex an end goes up to is picked on the canvas; feature_edit keeps the
        // feature's own pick and cannot make one.
        const ExtrudeEnd e = ExtrudeEnd(enum_index(k, v, kExtrudeEnds));
        if (e == ExtrudeEnd::UpToFace && f.up_to_face < 0)
            throw std::runtime_error("extrude_end up_to_face needs a target face, which only the Extrude card picks");
        if (e == ExtrudeEnd::UpToVertex && f.extrude_end != ExtrudeEnd::UpToVertex)
            throw std::runtime_error("extrude_end up_to_vertex needs a target vertex, which only the Extrude card picks");
        f.extrude_end = e;
        return;
    }
    throw std::runtime_error(k + " cannot be set");
}

// The `feature` param (or `index`, as agents often call it): an index into design_describe's features.
int feature_arg(const json& params, const CadDocument& doc)
{
    const json v = !params.is_object() ? json() : params.contains("feature") ? params["feature"] : params.value("index", json());
    if (!v.is_number_integer()) throw std::runtime_error("feature must be a feature index (design_describe's features[].index)");
    check_feature_index(v.get<int>(), doc);
    return v.get<int>();
}

json action_feature_describe(DesignPanel* panel, const json& params)
{
    const CadDocument& doc = panel->mcp_doc();
    return cad_feature_json(doc, feature_arg(params, doc));
}

// A card's Confirm in edit mode (DesignPanel::confirm_tool): `fields`, then `refs`, written to a copy of the feature,
// a Text feature outlined again when its words, height or font change, then one undo step and replace_feature(),
// which rebuilds and puts the old features back if the rebuild fails. Every argument is checked before anything
// changes.
json action_feature_edit(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const int    fi  = feature_arg(params, doc);
    // The card's values go in fields; params and values are what agents often call them.
    const char* key    = params.contains("fields") ? "fields" : params.contains("params") ? "params" : params.contains("values") ? "values" : nullptr;
    json        fields = key != nullptr ? params[key] : json::object();
    const json  refs   = params.value("refs", json::object());
    if (!fields.is_object()) throw std::runtime_error("fields must be an object, as {\"distance\": 15}");
    if (!refs.is_object()) throw std::runtime_error("refs must be an object, as {\"sweep_path_ref\": 2}");
    if (fields.empty() && refs.empty())
        throw std::runtime_error("feature_edit needs fields or refs, as {\"fields\": {\"distance\": 15}} or {\"refs\": {\"sweep_path_ref\": 2}}");
    const CadFeature was    = doc.features[fi];
    CadFeature       edited = was;
    // A Text feature's Bold and Italic go with a face name in text_font, as the Text dialog's boxes go with its Font
    // list: they are no field of the feature, whose stored font holds them.
    bool bold = false, italic = false, styled = false;
    if (was.is_text())
        for (const auto& [k, flag] : {std::pair<const char*, bool*>{"text_bold", &bold}, {"text_italic", &italic}}) {
            if (!fields.contains(k)) continue;
            if (!fields[k].is_boolean()) throw std::runtime_error(std::string(k) + " must be true or false");
            *flag  = fields[k].get<bool>();
            styled = true;
            fields.erase(k);
        }
    if (!fields.empty()) cad_apply_feature_fields(edited, fields);
    if (was.is_text() && (styled || fields.contains("text_string") || fields.contains("text_height") || fields.contains("text_font"))) {
        // A text_font that is the stored value keeps the feature's font; any other names a face.
        const std::string face = fields.contains("text_font") && edited.text_font != was.text_font ? edited.text_font : std::string();
        if (styled && face.empty()) throw std::runtime_error("text_bold and text_italic go with text_font given as a face name");
        edited.text_font = was.text_font;
        std::string why;
        if (!panel->mcp_outline_text(edited, edited.text_string, face, bold, italic, edited.text_height, why))
            throw std::runtime_error(why);
    }
    if (!refs.empty()) cad_apply_feature_refs(edited, refs, cad_document_before(doc, fi), doc, fi);
    const json described = cad_feature_json(doc, fi);
    doc.checkpoint();
    if (!doc.replace_feature(fi, edited)) {
        // The features are back already; rollback() also takes back the undo step and restores
        // the redo branch, so the refused call leaves the history as it found it.
        const std::string why = doc.error.empty() ? "the model could not be rebuilt" : doc.error;
        doc.rollback();
        doc.error = why;
        panel->mcp_after_change(false);
        throw ToolFailure(why, {{"code", "rebuild_failed"}});
    }
    panel->mcp_after_change();
    return json{{"ok", true},
                {"feature", fi},
                {"changed", cad_feature_changes(described, cad_feature_json(doc, fi))},
                {"bodies", int(doc.bodies.size())},
                {"generation", doc.topo_generation},
                {"error", doc.error}};
}

json action_feature_set_expression(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const int    fi  = feature_arg(params, doc);
    if (!params.contains("field")) throw std::runtime_error("feature_set_expression needs 'field' name");
    if (!params.contains("expr"))  throw std::runtime_error("feature_set_expression needs 'expr' string");
    const std::string field = params["field"].get<std::string>();
    const std::string expr  = params["expr"].get<std::string>();
    // A name the evaluator does not know would fail every later rebuild; the binding row refuses it too.
    if (!CadDocument::is_bindable_field(field))
        throw std::runtime_error("\"" + field + "\" is not a value an expression can drive");
    // An empty expression removes the binding, as the binding row's Clear does; the field keeps
    // the value the binding last gave it.
    const bool clear = expr.empty();
    if (clear && doc.features[fi].expr.count(field) == 0)
        throw std::runtime_error(field + " has no binding on feature " + std::to_string(fi));
    doc.checkpoint();
    if (clear)
        doc.features[fi].expr.erase(field);
    else
        doc.features[fi].expr[field] = expr;
    const bool ok = doc.recompute();
    // rollback() rebuilds, which succeeds and clears doc.error; carry the reason across so the
    // reply says why the edit was rejected.
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature", fi}, {"field", field}, {"error", doc.error}};
}

} // namespace

// ---- Shared with the other Design files (DesignCommon.hpp) ----------------------------------

[[noreturn]] void tree_edit_refused(const CadDocument& doc)
{
    throw ToolFailure(doc.error.empty() ? "the Design tab refused the edit" : doc.error, {{"code", "rebuild_failed"}});
}

json tree_edit_reply(const CadDocument& doc, json reply)
{
    reply["ok"]         = true;
    reply["features"]   = doc.features.size();
    reply["bodies"]     = doc.bodies.size();
    reply["generation"] = doc.topo_generation;
    return reply;
}

std::string name_arg(const json& params)
{
    const json v = param(params, "name");
    if (!v.is_string()) throw std::runtime_error("name must be a string");
    std::string name = v.get<std::string>();
    name.erase(name.find_last_not_of(" \t\r\n") + 1);
    name.erase(0, name.find_first_not_of(" \t\r\n"));
    if (name.empty()) throw std::runtime_error("name must not be empty");
    return name;
}

namespace {

// ---- The Feature tree's edits -------------------------------------------------------------------

json action_feature_delete(DesignPanel* panel, const json& params)
{
    const CadDocument& doc    = panel->mcp_doc();
    const int          fi     = feature_arg(params, doc);
    const size_t       before = doc.features.size();
    if (!panel->delete_feature(fi)) tree_edit_refused(doc);
    return tree_edit_reply(doc, {{"removed", before - doc.features.size()}});
}

json action_feature_move(DesignPanel* panel, const json& params)
{
    const CadDocument& doc   = panel->mcp_doc();
    const int          fi    = feature_arg(params, doc);
    const json         delta = param(params, "delta");
    if (!delta.is_number_integer() || std::abs(delta.get<int>()) != 1)
        throw std::runtime_error("delta must be -1 (up) or 1 (down)");
    const int to = fi + delta.get<int>();
    if (to < 0 || to >= int(doc.features.size()))
        throw CadPrecondition("feature " + std::to_string(fi) + " is already " + (to < 0 ? "first" : "last"));
    if (!panel->move_feature(fi, delta.get<int>())) tree_edit_refused(doc);
    return tree_edit_reply(doc, {{"feature", to}, {"from", fi}});
}

// The row's eye: it shows a feature that is not suppressed and hides one that is.
json action_feature_suppress(DesignPanel* panel, const json& params)
{
    const CadDocument& doc        = panel->mcp_doc();
    const int          fi         = feature_arg(params, doc);
    const json         suppressed = param(params, "suppressed");
    if (!suppressed.is_boolean()) throw std::runtime_error("suppressed must be true or false");
    const bool shown   = !suppressed.get<bool>();
    const bool changed = doc.features[fi].enabled != shown;
    if (!panel->show_feature(fi, shown)) tree_edit_refused(doc);
    return tree_edit_reply(doc, {{"feature", fi}, {"suppressed", suppressed}, {"changed", changed}});
}

json action_feature_rename(DesignPanel* panel, const json& params)
{
    const CadDocument& doc     = panel->mcp_doc();
    const int          fi      = feature_arg(params, doc);
    const std::string  name    = name_arg(params);
    const bool         changed = doc.features[fi].name != name;
    panel->rename_feature(fi, name);
    return json{{"ok", true}, {"feature", fi}, {"name", name}, {"changed", changed}, {"undo_depth", doc.undo_depth()}};
}

} // namespace

json cad_feature_json(const CadDocument& doc, int index)
{
    check_feature_index(index, doc);
    const CadFeature& f = doc.features[index];
    json j = feature_card(f);
    j["index"]      = index;
    j["type"]       = feature_type_name(f.type);
    j["name"]       = f.name;
    j["suppressed"] = !f.enabled;

    // The plane the recipe builds on: a datum's resolved frame, or the plane the feature holds.
    using T = CadFeatureType;
    const bool own_profile = f.type != T::Sketch && f.sketch_ref < 0 && (!f.entities.empty() || !f.profile.points.empty());
    if (f.type == T::Plane) {
        const auto datums  = doc.resolve_datum_planes();
        const int  ordinal = datum_ordinal(doc, index);
        if (f.enabled && ordinal < int(datums.size())) j["plane"] = plane_json(datums[ordinal].second, doc.modeling_origin);
    } else if (f.type == T::Sketch || f.type == T::Hole || f.type == T::Thread || f.type == T::Helix || f.type == T::Pattern ||
               f.type == T::Mirror || f.type == T::Project || (f.type == T::Cut && f.cut_face < 0) || own_profile) {
        j["plane"] = plane_json(f.plane, doc.modeling_origin);
    }

    // A sketch's geometry, or the loop an extrude or revolve carries itself; points are in the
    // plane's frame.
    if (!f.entities.empty()) {
        json entities = json::array(), constraints = json::array();
        for (size_t i = 0; i < f.entities.size(); ++i) entities.push_back(sketch_entity_to(f.entities[i], int(i)));
        for (size_t i = 0; i < f.entity_constraints.size(); ++i)
            constraints.push_back(entity_constraint_json(f.entity_constraints[i], int(i)));
        j["entities"]    = std::move(entities);
        j["constraints"] = std::move(constraints);
    } else if (!f.profile.points.empty()) {
        json points = json::array(), constraints = json::array();
        for (const Vec2d& p : f.profile.points) points.push_back(json::array({p.x(), p.y()}));
        for (size_t i = 0; i < f.constraints.size(); ++i) constraints.push_back(profile_constraint_json(f.constraints[i], int(i)));
        j["profile"]     = json{{"points", std::move(points)}, {"closed", f.profile.closed}};
        j["constraints"] = std::move(constraints);
    }

    // What consumes this feature: the references remove_feature() remaps, and for a datum plane
    // the features built on it.
    const int datum = f.type == T::Plane && f.enabled ? 3 + datum_ordinal(doc, index) : -1;
    json used_by = json::array();
    for (int i = 0; i < int(doc.features.size()); ++i) {
        bool uses = false;
        CadDocument::for_each_feature_ref(doc.features[i], [&](int r) { uses = uses || r == index; });
        if (datum >= 0) CadDocument::for_each_datum_ref(doc.features[i], [&](int r) { uses = uses || r == datum; });
        if (uses) used_by.push_back(i);
    }
    json made = json::array();
    for (size_t b = 0; b < doc.bodies.size(); ++b)
        if (doc.bodies[b].source_feature == index) made.push_back(int(b));
    j["used_by"]     = std::move(used_by);
    j["bodies_made"] = std::move(made);
    for (const auto& [mate, reason] : doc.mate_conflicts)
        if (mate == index) j["conflict"] = reason;

    // The model its body indices and face and edge ids are read in: the one just before it, which feature_edit's
    // refs take, but for a datum, which the engine resolves against the finished model (for_each_body_ref()).
    j["ids_of"] = f.type == T::Plane || f.type == T::Axis || f.type == T::CoordSys ? "finished" : "before";
    // As the last rebuild recorded them, named as the bodies they became are; null where that rebuild did not reach.
    json before = nullptr;
    if (size_t(index) < doc.bodies_before.size() && doc.bodies_before[size_t(index)]) {
        before = json::array();
        const std::vector<CadDocument::BodyBefore>& was = *doc.bodies_before[size_t(index)];
        for (size_t b = 0; b < was.size(); ++b) {
            const int      now  = doc.body_index(was[b].id);
            const CadBody* body = now >= 0 ? &doc.bodies[size_t(now)] : nullptr;
            before.push_back(json{{"index", int(b)}, {"name", body != nullptr && body->has_user_name ? body->user_name : was[b].name}});
        }
    }
    j["bodies_before"] = std::move(before);
    return j;
}

json cad_feature_changes(const json& was, const json& now)
{
    json changed = json::object();
    for (const char* part : {"params", "refs"}) {
        const json before = was.value(part, json::object()), after = now.value(part, json::object());
        for (const auto& [k, v] : after.items()) {
            const json old = before.value(k, json());
            if (old != v) changed[k] = json::array({old, v});
        }
    }
    // The plane a Mirror, a Helix or a Cut builds on is reported beside them, and a re-point of it is a change too.
    const json old_plane = was.value("plane", json()), new_plane = now.value("plane", json());
    if (old_plane != new_plane) changed["plane"] = json::array({old_plane, new_plane});
    return changed;
}

void cad_apply_feature_fields(CadFeature& f, const json& fields)
{
    if (!fields.is_object() || fields.empty())
        throw std::runtime_error("fields must name the fields to change, as {\"distance\": 15}");
    const json  card     = feature_card(f);
    const json& editable = card["editable"];
    const json& ranges   = card["ranges"];
    const std::string type = feature_type_name(f.type);
    if (editable.empty())
        throw std::runtime_error(type + ": no field feature_edit can change; its inputs are geometry or picks");
    if (fields.contains("thread_diameter") && fields.contains("thread_radius"))
        throw std::runtime_error("give thread_diameter or thread_radius, not both");
    // The two are one value, so a binding on either drives both.
    auto same_value = [](const std::string& a, const std::string& b) {
        auto thread_size = [](const std::string& k) { return k == "thread_diameter" || k == "thread_radius"; };
        return a == b || (thread_size(a) && thread_size(b));
    };

    CadFeature edited = f;
    for (const auto& [key, v] : fields.items()) {
        if (std::find(editable.begin(), editable.end(), json(key)) == editable.end()) {
            std::string list;
            for (const json& e : editable) list += (list.empty() ? "" : ", ") + e.get<std::string>();
            throw std::runtime_error(type + ": '" + key + "' is not an editable field; editable: " + list);
        }
        // Every rebuild writes a bound field from its expression, so a literal would not last.
        for (const auto& [bound, e] : f.expr)
            if (same_value(bound, key))
                throw std::runtime_error(key + " is driven by the expression '" + e + "'; change it with design_set_variable, or "
                                         "remove the binding with feature_set_expression {field: '" + bound + "', expr: ''}");
        const auto range = ranges.find(key);
        if (range == ranges.end()) {
            set_card_choice(edited, key, v);
            continue;
        }
        if (!v.is_number()) throw std::runtime_error(key + " must be a number");
        const double x = v.get<double>(), lo = (*range)[0].get<double>(), hi = (*range)[1].get<double>();
        if (!(x >= lo && x <= hi)) throw std::runtime_error(Slic3r::format("%1% must be between %2% and %3%", key, lo, hi));
        if (key == "pattern_count" && x != std::floor(x)) throw std::runtime_error("pattern_count must be a whole number");
        CadDocument::set_numeric_field(edited, key, x);
    }
    f = std::move(edited);
}

CadDocument cad_document_before(const CadDocument& doc, int feature)
{
    check_feature_index(feature, doc);
    CadDocument before = doc.history_before(feature);
    if (!before.recompute())
        throw ToolFailure("the model before feature " + std::to_string(feature) + " (" + doc.features[size_t(feature)].name +
                              ") does not rebuild: " + (before.error.empty() ? std::string("no reason given") : before.error),
                          {{"code", "rebuild_failed"}});
    return before;
}

namespace {

// "feature 5 (Sweep 6)": the feature whose refs feature_edit writes, as its refusals name it.
std::string feature_label(const CadDocument& doc, int index)
{
    return "feature " + std::to_string(index) + " (" + doc.features[size_t(index)].name + ")";
}

// The names feature_edit's refs takes for a feature of type `t`: those of feature_describe's refs that its card can
// pick again when it edits. Empty for a type whose card picks nothing again, and for an Extrude, whose profile and
// target are fixed.
std::vector<std::string> ref_names(CadFeatureType t)
{
    using T = CadFeatureType;
    switch (t) {
    case T::Revolve:        return {"revolve_axis_entity", "target_body"};
    case T::SurfaceRevolve: return {"revolve_axis_entity"};
    case T::Sweep:          return {"sweep_path_ref", "target_body"};
    case T::Loft:           return {"loft_profile_refs", "target_body"};
    case T::SurfaceLoft:    return {"loft_profile_refs"};
    case T::Boolean:        return {"target_body", "bool_tool_body"};
    case T::Mirror:
    case T::Cut:
    case T::Hole:
    case T::Thread:         return {"plane", "target_body"};
    case T::Helix:          return {"plane"};
    case T::Rib:            return {"rib_sketch_ref", "rib_entity", "target_body"};
    case T::Mate:           return {"mate_cs_a", "mate_cs_b"};
    case T::Fillet:
    case T::Chamfer:        return {"edges", "face_group", "target_body"};
    case T::Shell:          return {"shell_face", "target_body"};
    case T::Draft:          return {"draft_face", "target_body"};
    case T::Thicken:        return {"thicken_face", "target_body"};
    case T::DeleteFace:     return {"delete_faces", "target_body"};
    case T::Plane:
        return {"plane_type", "plane_base", "plane_axis", "plane_face_body", "plane_face", "plane_face2_body", "plane_face2",
                "plane_edge_body", "plane_edge", "plane_edge2_body", "plane_edge2"};
    case T::Axis:
        return {"axis_type", "axis_p1", "axis_p2", "axis_body", "axis_face", "axis_edge", "axis_plane_a", "axis_plane_b"};
    case T::CoordSys:
        return {"coordsys_type", "coordsys_point", "coordsys_body", "coordsys_face", "coordsys_edge", "coordsys_x_hint"};
    case T::Pattern:
    case T::Transform:
    case T::ThickenSurface:
    case T::SurfaceOffset:  return {"target_body"};
    default:                return {};
    }
}

// Reads the values of feature_edit's refs for features[index] of `doc`, whose body indices and face and edge ids
// name those of `ids`: the model before the feature, or for a datum the finished model, `doc` itself.
struct RefReader
{
    const CadDocument& ids;
    const CadDocument& doc;
    int                index;
    bool               finished;   // `ids` is the finished model, a datum's
    std::string        at;         // feature_label() of the feature

    // Where `ids` stands, as a refusal says it.
    std::string where() const { return finished ? "in the finished model" : "before " + at; }

    int integer(const std::string& name, const json& v, const char* what) const
    {
        if (!v.is_number_integer()) throw std::runtime_error(name + " must be " + what);
        return v.get<int>();
    }

    // A feature before the one edited, of one of `types`, which `kinds` names.
    int earlier(const std::string& name, const json& v, std::initializer_list<CadFeatureType> types, const char* kinds) const
    {
        const int fi = integer(name, v, "a feature index (design_describe's features[].index)");
        if (fi < 0 || fi >= index)
            throw std::runtime_error(name + " " + std::to_string(fi) + " is not an earlier feature: it must come before " + at);
        const CadFeatureType t = doc.features[size_t(fi)].type;
        if (std::find(types.begin(), types.end(), t) == types.end())
            throw std::runtime_error(name + " " + std::to_string(fi) + " (" + feature_type_name(t) + ") is not " + kinds);
        return fi;
    }

    // A body of `ids`, or -1 where `unset` names what -1 stands for.
    int body(const std::string& name, const json& v, const char* unset) const
    {
        const int b = integer(name, v, unset != nullptr ? "a body index, or -1" : "a body index");
        if (unset != nullptr && b == -1) return b;
        const int n = int(ids.bodies.size());
        if (b < 0 || b >= n)
            throw std::runtime_error(name + " " + std::to_string(b) + " is not a body " + where() + ": there " +
                                     (finished ? (n == 1 ? "is 1 body" : "are " + std::to_string(n) + " bodies")
                                               : (n == 1 ? "was 1 body" : "were " + std::to_string(n) + " bodies")));
        return b;
    }

    // The body a feature's target_body names in `ids`: -1 is the last one.
    int target(int b) const
    {
        if (b >= 0) return b;
        if (ids.bodies.empty()) throw CadPrecondition("there is no body " + where());
        return int(ids.bodies.size()) - 1;
    }

    // Face (`edges` false) or edge ids, each of body `b` of `ids`.
    void on_body(const std::vector<int>& list, int b, bool edges) const
    {
        const TopoDS_Shape& shape = ids.bodies[size_t(b)].shape;
        const int           n     = edges ? GeometryEngine::edge_count(shape) : GeometryEngine::face_count(shape);
        const char*         kind  = edges ? "edge" : "face";
        for (const int id : list)
            if (id < 0 || id >= n)
                throw std::runtime_error(std::string(kind) + " " + std::to_string(id) + " of body " + std::to_string(b) + " does not exist " +
                                         where() + ": the body " + (finished ? "has " : "had ") + std::to_string(n) + " " + kind + "s" +
                                         (finished ? "" : " there"));
    }

    // One id, or for `many` a list of distinct ids.
    std::vector<int> id_list(const std::string& name, const json& v, bool many) const
    {
        std::vector<int> out;
        if (many && v.is_array()) {
            for (const json& e : v) {
                if (!e.is_number_integer()) throw std::runtime_error(name + " must be ids from body_describe");
                if (std::find(out.begin(), out.end(), e.get<int>()) != out.end())
                    throw std::runtime_error(name + ": " + std::to_string(e.get<int>()) + " is listed twice");
                out.push_back(e.get<int>());
            }
            if (out.empty()) throw std::runtime_error(name + " is an empty list");
            return out;
        }
        out.push_back(integer(name, v, many ? "an id or a list of ids from body_describe" : "an id from body_describe"));
        return out;
    }

    // A datum-plane reference: 0, 1 or 2 for XY, XZ or YZ, or 3 + N for the Nth shown Plane feature, which must come
    // before the feature edited; -1 where `unset` allows it.
    int datum(const std::string& name, const json& v, bool unset) const
    {
        const int d = integer(name, v, "0, 1 or 2 (XY, XZ or YZ), or 3 + N for the Nth shown datum plane");
        if ((unset && d == -1) || (d >= 0 && d < 3)) return d;
        int seen = 0;
        for (int i = 0; d >= 3 && i < index; ++i)
            if (doc.features[size_t(i)].type == CadFeatureType::Plane && doc.features[size_t(i)].enabled && seen++ == d - 3)
                return d;
        throw std::runtime_error(name + " " + std::to_string(d) + " names no datum plane before " + at);
    }
};

// A point or direction [x, y, z] of a datum's refs.
Vec3d vec3_ref(const std::string& name, const json& v, bool direction)
{
    if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number())
        throw std::runtime_error(name + " must be [x, y, z]");
    const Vec3d p(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
    if (direction && !(p.norm() > 1e-9)) throw std::runtime_error(name + " must not be zero");
    return p;
}

// The refs of a Plane, an Axis and a CoordSys: their types, points and datum references, and bodies, faces and edges
// of the finished model.
void apply_datum_refs(CadFeature& g, const json& refs, const RefReader& r)
{
    using T = CadFeatureType;
    const auto given = [&](const char* k) { return refs.contains(k); };
    const auto body  = [&](const char* k, int& field) { if (given(k)) field = r.body(k, refs[k], "unset"); };
    const auto id    = [&](const char* k, int& field) { if (given(k)) field = r.integer(k, refs[k], "an id from body_describe, or -1"); };
    // A face or an edge set on a body: one of that body's, in the finished model.
    const auto check = [&](int b, int ref, bool edges, const char* k) {
        if (ref < 0) return;
        if (b < 0) throw std::runtime_error(std::string(k) + " " + std::to_string(ref) + " is on no body: give its body too");
        r.on_body({ref}, b, edges);
    };
    if (g.type == T::Plane) {
        if (given("plane_type")) g.plane_type = PlaneType(enum_index("plane_type", refs["plane_type"], kPlaneTypes));
        if (given("plane_base")) g.plane_base = r.datum("plane_base", refs["plane_base"], false);
        if (given("plane_axis")) {
            g.plane_axis = r.integer("plane_axis", refs["plane_axis"], "0 (the base's X) or 1 (its Y)");
            if (g.plane_axis != 0 && g.plane_axis != 1) throw std::runtime_error("plane_axis must be 0 (the base's X) or 1 (its Y)");
        }
        body("plane_face_body", g.plane_face_body);
        id("plane_face", g.plane_face);
        body("plane_face2_body", g.plane_face2_body);
        id("plane_face2", g.plane_face2);
        body("plane_edge_body", g.plane_edge_body);
        id("plane_edge", g.plane_edge);
        body("plane_edge2_body", g.plane_edge2_body);
        id("plane_edge2", g.plane_edge2);
        check(g.plane_face_body, g.plane_face, false, "plane_face");
        check(g.plane_face2_body, g.plane_face2, false, "plane_face2");
        check(g.plane_edge_body, g.plane_edge, true, "plane_edge");
        check(g.plane_edge2_body, g.plane_edge2, true, "plane_edge2");
        // What the Plane card's Confirm refuses, which the rebuild would not: an unresolved plane falls back silently.
        if (const std::string why = r.doc.datum_plane_refusal(g); !why.empty()) throw std::runtime_error(why);
    } else if (g.type == T::Axis) {
        if (given("axis_type")) g.axis_type = AxisType(enum_index("axis_type", refs["axis_type"], kAxisTypes));
        if (given("axis_p1")) g.axis_p1 = vec3_ref("axis_p1", refs["axis_p1"], false);
        if (given("axis_p2")) g.axis_p2 = vec3_ref("axis_p2", refs["axis_p2"], false);
        body("axis_body", g.axis_body);
        id("axis_face", g.axis_face);
        id("axis_edge", g.axis_edge);
        if (given("axis_plane_a")) g.axis_plane_a = r.datum("axis_plane_a", refs["axis_plane_a"], true);
        if (given("axis_plane_b")) g.axis_plane_b = r.datum("axis_plane_b", refs["axis_plane_b"], true);
        check(g.axis_body, g.axis_face, false, "axis_face");
        check(g.axis_body, g.axis_edge, true, "axis_edge");
    } else {
        if (given("coordsys_type")) g.coordsys_type = CoordSysType(enum_index("coordsys_type", refs["coordsys_type"], kCoordSysTypes));
        if (given("coordsys_point")) g.coordsys_point = vec3_ref("coordsys_point", refs["coordsys_point"], false);
        if (given("coordsys_x_hint")) g.coordsys_x_hint = vec3_ref("coordsys_x_hint", refs["coordsys_x_hint"], true);
        body("coordsys_body", g.coordsys_body);
        id("coordsys_face", g.coordsys_face);
        id("coordsys_edge", g.coordsys_edge);
        check(g.coordsys_body, g.coordsys_face, false, "coordsys_face");
        check(g.coordsys_body, g.coordsys_edge, true, "coordsys_edge");
    }
}

// The refs of every other type, read in the model before the feature (`r.ids`).
void apply_model_refs(CadFeature& g, const json& refs, const RefReader& r)
{
    using T             = CadFeatureType;
    const auto given    = [&](const char* k) { return refs.contains(k); };
    const CadDocument& doc = r.doc;
    if (given("target_body")) g.target_body = r.body("target_body", refs["target_body"], g.type == T::Boolean ? nullptr : "the last body");

    if (given("revolve_axis_entity")) {
        // A line of the sketch the revolve turns, or of the profile it carries itself.
        const int  e     = r.integer("revolve_axis_entity", refs["revolve_axis_entity"], "a line of the feature's sketch (entity index), or -1 for the plane axis");
        const bool own   = g.sketch_ref < 0 || g.sketch_ref >= int(doc.features.size());
        if (e != -1 && !revolve_line(own ? g.entities : doc.features[size_t(g.sketch_ref)].entities, e))
            throw std::runtime_error("revolve_axis_entity " + std::to_string(e) + " is not a line of " +
                                     (own ? std::string("its profile") : "sketch " + std::to_string(g.sketch_ref)));
        g.revolve_axis_entity = e;
    }
    if (given("sweep_path_ref")) {
        g.sweep_path_ref = r.earlier("sweep_path_ref", refs["sweep_path_ref"], {T::Sketch, T::Helix}, "a Sketch or a Helix");
        if (g.sweep_path_ref == g.sketch_ref) throw std::runtime_error("sweep_path_ref cannot be the profile sketch itself");
    }
    if (given("loft_profile_refs")) {
        const json& v = refs["loft_profile_refs"];
        if (!v.is_array() || v.size() < 2) throw std::runtime_error("loft_profile_refs must be two or more sketch feature indices, in loft order");
        std::vector<int> profiles;
        for (const json& p : v) {
            const int s = r.earlier("loft_profile_refs", p, {T::Sketch, T::Project}, "a Sketch");
            if (std::find(profiles.begin(), profiles.end(), s) != profiles.end())
                throw std::runtime_error("loft_profile_refs: sketch " + std::to_string(s) + " is listed twice");
            profiles.push_back(s);
        }
        g.loft_profile_refs = std::move(profiles);
    }
    if (given("bool_tool_body")) g.bool_tool_body = r.body("bool_tool_body", refs["bool_tool_body"], nullptr);
    // A Boolean's two operands; for every other type bool_tool_body is unused, and target_body -1 is the last body.
    if (g.type == T::Boolean && (given("target_body") || given("bool_tool_body")) && g.target_body == g.bool_tool_body)
        throw std::runtime_error("target_body and bool_tool_body must be two different bodies");

    if (given("plane")) {
        // A face is one of the model before the feature; a datum's frame is the finished model's, as a card's plane
        // list resolves it. A hole or a thread drills the face's own body, as on its card.
        const json& v     = refs["plane"];
        const bool  drill = g.type == T::Hole || g.type == T::Thread;
        int         face_body = -1;
        if (v.is_object() && v.contains("datum")) {
            if (const json d = v["datum"]; d.is_number_integer() && (d.get<int>() < 0 || d.get<int>() >= r.index))
                throw std::runtime_error("plane's datum " + std::to_string(d.get<int>()) + " is not an earlier feature: it must come before " + r.at);
            g.plane = cad_resolve_plane(v, doc, drill);
        } else {
            g.plane = cad_resolve_plane(v, r.ids, drill, &face_body);
        }
        if (g.type == T::Cut) g.cut_face = g.cut_face_body = -1;   // a split along the plane, not along a face
        if (drill && face_body >= 0) {
            if (given("target_body") && g.target_body != face_body)
                throw std::runtime_error("the plane is a face of body " + std::to_string(face_body) + ", so that body is drilled, not body " +
                                         std::to_string(g.target_body));
            g.target_body = face_body;
        }
    }
    if (given("rib_sketch_ref")) g.rib_sketch_ref = r.earlier("rib_sketch_ref", refs["rib_sketch_ref"], {T::Sketch}, "a Sketch");
    if (given("rib_entity")) g.rib_entity = r.integer("rib_entity", refs["rib_entity"], "a line of the rib's sketch (entity index)");
    if ((given("rib_sketch_ref") || given("rib_entity")) &&
        !(g.rib_sketch_ref >= 0 && g.rib_sketch_ref < int(doc.features.size()) &&
          revolve_line(doc.features[size_t(g.rib_sketch_ref)].entities, g.rib_entity)))
        throw std::runtime_error("rib_entity " + std::to_string(g.rib_entity) + " is not a line of sketch " + std::to_string(g.rib_sketch_ref));
    if (given("mate_cs_a")) g.mate_cs_a = r.earlier("mate_cs_a", refs["mate_cs_a"], {T::CoordSys}, "a CoordSys");
    if (given("mate_cs_b")) g.mate_cs_b = r.earlier("mate_cs_b", refs["mate_cs_b"], {T::CoordSys}, "a CoordSys");
    if ((given("mate_cs_a") || given("mate_cs_b")) && g.mate_cs_a == g.mate_cs_b)
        throw std::runtime_error("mate_cs_a and mate_cs_b must be two different coordinate systems");

    if (given("edges") && given("face_group")) throw std::runtime_error("give edges or face_group, not both");
    if (given("edges")) {
        const std::vector<int> edges = r.id_list("edges", refs["edges"], true);
        g.dressup_edge  = edges.front();
        g.dressup_edges = edges.size() > 1 ? edges : std::vector<int>();
    }
    if (given("face_group")) {
        const std::optional<FaceGroup> group = face_group_named(refs["face_group"]);
        if (!group) throw std::runtime_error("face_group must be " + kFaceGroupWords);
        g.face_group    = *group;
        g.dressup_edge  = -1;
        g.dressup_edges.clear();
    }
    if (given("shell_face")) {
        g.shell_face = r.integer("shell_face", refs["shell_face"], "a face id from body_describe, or -1 for a closed hollow");
        if (g.shell_face < -1) throw std::runtime_error("shell_face must be a face id from body_describe, or -1 for a closed hollow");
    }
    if (given("draft_face")) g.draft_face = r.id_list("draft_face", refs["draft_face"], false).front();
    if (given("thicken_face")) g.thicken_face = r.id_list("thicken_face", refs["thicken_face"], false).front();
    if (given("delete_faces")) {
        if (!refs["delete_faces"].is_array()) throw std::runtime_error("delete_faces must be a list of face ids from body_describe");
        g.delete_faces = r.id_list("delete_faces", refs["delete_faces"], true);
    }

    // The faces and edges the feature works on, of its target body as it stood before the feature, whichever of the
    // two changed.
    const bool retarget = given("target_body");
    const int  shown    = g.target_body;
    if ((g.type == T::Fillet || g.type == T::Chamfer) && (retarget || given("edges")) && !g.dressup_edge_ids().empty())
        r.on_body(g.dressup_edge_ids(), r.target(shown), true);
    if (g.type == T::Shell && (retarget || given("shell_face")) && g.shell_face >= 0) r.on_body({g.shell_face}, r.target(shown), false);
    if (g.type == T::Draft && (retarget || given("draft_face"))) r.on_body({g.draft_face}, r.target(shown), false);
    if (g.type == T::Thicken && (retarget || given("thicken_face"))) r.on_body({g.thicken_face}, r.target(shown), false);
    if (g.type == T::DeleteFace && (retarget || given("delete_faces"))) r.on_body(g.delete_faces, r.target(shown), false);
}

} // namespace

void cad_apply_feature_refs(CadFeature& f, const json& refs, const CadDocument& before, const CadDocument& doc, int index)
{
    using T                = CadFeatureType;
    const std::string type = feature_type_name(f.type);
    if (f.type == T::Extrude) throw std::runtime_error("Extrude: its profile and its target are fixed");
    if (!refs.is_object() || refs.empty()) throw std::runtime_error("refs must name the references to change, as {\"sweep_path_ref\": 2}");
    const std::vector<std::string> names = ref_names(f.type);
    std::string                    list;
    for (const std::string& n : names) list += (list.empty() ? "" : ", ") + n;
    for (const auto& [name, v] : refs.items())
        if (std::find(names.begin(), names.end(), name) == names.end())
            throw std::runtime_error(names.empty() ? type + ": its card picks nothing again, so refs takes no name; '" + name + "' is not one"
                                                   : type + " takes " + list + " in refs, not '" + name + "'");
    const bool      datum = f.type == T::Plane || f.type == T::Axis || f.type == T::CoordSys;
    const RefReader r{datum ? doc : before, doc, index, datum, feature_label(doc, index)};
    CadFeature      g = f;
    if (datum) apply_datum_refs(g, refs, r);
    else       apply_model_refs(g, refs, r);
    f = std::move(g);
}

void history_silent_forms(std::vector<SilentForm>& forms)
{
    forms.insert(forms.end(), {
        {"feature_describe",             action_feature_describe},
        {"feature_set_expression",       action_feature_set_expression},
        {"feature_edit",                 action_feature_edit},
        {"feature_delete",               action_feature_delete},
        {"feature_move",                 action_feature_move},
        {"feature_suppress",             action_feature_suppress},
        {"feature_rename",               action_feature_rename},
    });
}

}}} // namespace Slic3r::GUI::Mcp
