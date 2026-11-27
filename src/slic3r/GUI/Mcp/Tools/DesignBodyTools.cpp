// The place Design, its Bodies heading, the Bodies list's edits of a body: body_set, body_place, body_transform and
// body_delete. body_silent_forms() binds them to their tool blocks, and run_design_tool() (DesignTools.cpp) runs them
// behind the Design commands' gate.

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
#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <map>
#include <optional>

#include <nlohmann/json.hpp>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#include "slic3r/GUI/CAD/DesignCanvas.hpp"

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/Color.hpp"

using json = nlohmann::json;

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

json action_transform(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw CadPrecondition("no body to transform");
    int bi = target_body_arg(params, doc);
    Vec3d translate(params.value("dx", 0.0), params.value("dy", 0.0), params.value("dz", 0.0));
    Vec3d axis(params.value("axis_x", 0.0), params.value("axis_y", 0.0), params.value("axis_z", 1.0));
    Vec3d pivot(params.value("pivot_x", 0.0), params.value("pivot_y", 0.0), params.value("pivot_z", 0.0));
    double angle = params.value("angle", 0.0);
    bool copy = params.value("copy", false);
    doc.checkpoint();
    int idx = doc.add_transform(bi, translate, axis, pivot, angle, copy, panel->mcp_feature_name(_L("Transform")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"transform_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

// The `body` param: an index into design_describe's bodies.
int body_arg(const json& params, const CadDocument& doc)
{
    const json v = param(params, "body");
    if (!v.is_number_integer()) throw std::runtime_error("body must be a body index (design_describe's bodies[].index)");
    const int b = v.get<int>();
    if (b < 0 || b >= int(doc.bodies.size()))
        throw std::runtime_error("body " + std::to_string(b) + " is out of range; the design has " +
                                 std::to_string(doc.bodies.size()) + " bodies");
    return b;
}

// The Bodies row's rename and eye and the Color button, in one call. Every argument is checked
// before anything changes.
json action_body_set(DesignPanel* panel, const json& params)
{
    const CadDocument& doc = panel->mcp_doc();
    const int          b   = body_arg(params, doc);
    std::optional<std::string> name;
    std::optional<ColorRGBA>   color;
    std::optional<bool>        visible;
    if (!param(params, "name").is_null()) name = name_arg(params);
    if (const json c = param(params, "color"); !c.is_null()) {
        const std::string s = c.is_string() ? c.get<std::string>() : std::string();
        ColorRGBA         rgba;
        if (s.size() != 7 || s[0] != '#' ||
            !std::all_of(s.begin() + 1, s.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }) ||
            !decode_color(s, rgba))
            throw std::runtime_error("color must be \"#RRGGBB\"");
        color = rgba;
    }
    if (const json v = param(params, "visible"); !v.is_null()) {
        if (!v.is_boolean()) throw std::runtime_error("visible must be true or false");
        visible = v.get<bool>();
    }
    if (!name && !color && !visible) throw std::runtime_error("body_set needs name, color or visible");
    if (name) panel->rename_body(b, *name);
    if (color) panel->set_body_color(b, *color);
    if (visible) panel->show_body(b, *visible);

    const CadBody& body = doc.bodies[b];
    json r{{"ok", true}, {"body", b}, {"name", body.has_user_name ? body.user_name : body.name},
           {"visible", panel->mcp_body_shown(b)}};
    if (const DesignCanvas* vp = panel->mcp_viewport(); vp != nullptr) r["color"] = encode_color(vp->body_color(b));
    return r;
}

json action_body_delete(DesignPanel* panel, const json& params)
{
    const CadDocument& doc = panel->mcp_doc();
    const int          b   = body_arg(params, doc);
    const int          src = doc.bodies[b].source_feature;
    if (src < 0 || src >= int(doc.features.size()))
        throw CadPrecondition("body " + std::to_string(b) + " has no feature to delete; use design_new to start over");
    const json   deleted{{"index", src}, {"type", feature_type_name(doc.features[src].type)}, {"name", doc.features[src].name}};
    const size_t before = doc.features.size();
    if (!panel->delete_body(b)) tree_edit_refused(doc);
    return tree_edit_reply(doc, {{"deleted_feature", deleted}, {"removed", before - doc.features.size()}});
}

// The Bodies row's Move and Place on Face from arguments: where a body stands, outside the history. Every argument is
// read before the body moves.
json action_body_place(DesignPanel* panel, const json& params)
{
    const int  b      = body_arg(params, panel->mcp_doc());
    const json move   = param(params, "move");
    const json rotate = param(params, "rotate");
    const json face   = param(params, "face");
    const json reset  = param(params, "reset");
    if (!reset.is_null() && !reset.is_boolean()) throw std::runtime_error("reset must be true or false");
    const bool resets = reset.is_boolean() && reset.get<bool>();
    const int  forms  = int(!move.is_null() || !rotate.is_null()) + int(!face.is_null()) + int(resets);
    if (forms == 0) throw std::runtime_error("body_place takes move and rotate, face, or reset: true");
    if (forms > 1) throw std::runtime_error("move and rotate, face and reset are three forms: give one of them");

    std::string why;
    if (!face.is_null()) {
        if (!face.is_number_integer() || face.get<int>() < 0) throw std::runtime_error("face must be a face id (body_describe)");
        if (!panel->mcp_place_body_on_face(b, face.get<int>(), why)) throw std::runtime_error(why);
    } else if (resets) {
        panel->mcp_reset_body_placement(b);
    } else {
        Vec3d d = Vec3d::Zero();
        if (!move.is_null()) {
            if (!move.is_array() || move.size() != 3 || !std::all_of(move.begin(), move.end(), [](const json& v) { return v.is_number(); }))
                throw std::runtime_error("move must be [dx, dy, dz] in mm");
            d = Vec3d(move[0].get<double>(), move[1].get<double>(), move[2].get<double>());
        }
        int    axis  = -1;
        double angle = 0.0;
        if (!rotate.is_null()) {
            static const std::map<std::string, int> axes{{"x", 0}, {"y", 1}, {"z", 2}};
            const json a = param(rotate, "axis");
            const json g = param(rotate, "angle");
            const auto named = a.is_string() ? axes.find(a.get<std::string>()) : axes.end();
            if (!rotate.is_object() || named == axes.end() || !g.is_number() || rotate.size() != 2)
                throw std::runtime_error("rotate must be {\"axis\": \"x\", \"y\" or \"z\", \"angle\": degrees}");
            axis  = named->second;
            angle = g.get<double>();
        }
        if (!panel->mcp_place_body(b, d, axis, angle, why)) throw std::runtime_error(why);
    }
    return json{{"ok", true}, {"body", b}, {"moved", panel->mcp_body_moved(b)}, {"placement", placement_json(*panel, b)}};
}

} // namespace

void body_silent_forms(std::vector<SilentForm>& forms)
{
    forms.insert(forms.end(), {
        {"body_transform",               action_transform},
        {"body_set",                     action_body_set},
        {"body_delete",                  action_body_delete},
        {"body_place",                   action_body_place},
    });
}

}}} // namespace Slic3r::GUI::Mcp
