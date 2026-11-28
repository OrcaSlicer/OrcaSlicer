// The place Design, its Surface heading: surface_extrude, surface_revolve, surface_loft, surface_patch and
// surface_offset, which make surface bodies (the Solid heading's thicken makes a solid of one). surface_silent_forms()
// binds them to their tool blocks, and run_design_tool() (DesignTools.cpp) runs them behind the Design commands' gate.

#include "slic3r/GUI/Mcp/Tools/DesignCommon.hpp"
#include "slic3r/GUI/Mcp/McpToolsDesign.hpp"
#include <stdexcept>
#include <math.h>

#include <string>
#include <vector>
#include <functional>
#include <initializer_list>
#include <map>

#include <nlohmann/json.hpp>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"

#include "libslic3r/CAD/CadDocument.hpp"

using json = nlohmann::json;

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

json action_surface_extrude(DesignPanel* panel, const json& params)
{
    if (!params.contains("sketch")) throw std::runtime_error("surface_extrude needs 'sketch' (feature index)");
    CadDocument& doc = panel->mcp_doc();
    int sketch = params["sketch"].get<int>();
    double distance = params.value("distance", 10.0);
    doc.checkpoint();
    int idx = doc.add_surface_extrude(sketch, distance, panel->mcp_feature_name(_L("Surface Extrude")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_revolve(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const auto   add = cad_surface_revolve(doc, params);
    doc.checkpoint();
    int idx = add(panel->mcp_feature_name(_L("Surface Revolve")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_offset(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw CadPrecondition("no sheet body to offset");
    int bi = target_body_arg(params, doc);
    double offset = params.value("offset", 1.0);
    doc.checkpoint();
    int idx = doc.add_surface_offset(bi, offset, panel->mcp_feature_name(_L("Surface Offset")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_loft(DesignPanel* panel, const json& params)
{
    if (!params.contains("profiles") || !params["profiles"].is_array())
        throw std::runtime_error("surface_loft needs 'profiles' (array of int feature indices)");
    CadDocument& doc = panel->mcp_doc();
    std::vector<int> profiles;
    for (const json& j : params["profiles"]) profiles.push_back(j.get<int>());
    bool ruled = params.value("ruled", false);
    doc.checkpoint();
    int idx = doc.add_surface_loft(profiles, ruled, panel->mcp_feature_name(_L("Surface Loft")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_patch(DesignPanel* panel, const json& params)
{
    if (!params.contains("sketch")) throw std::runtime_error("surface_patch needs 'sketch' (feature index)");
    CadDocument& doc = panel->mcp_doc();
    int sketch = params["sketch"].get<int>();
    doc.checkpoint();
    int idx = doc.add_surface_fill(sketch, panel->mcp_feature_name(_L("Surface Fill")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

} // namespace

std::function<int(const std::string&)> cad_surface_revolve(CadDocument& doc, const json& params)
{
    const int    si          = sketch_arg(params.value("sketch", json()), doc);
    const double angle       = params.value("angle", 360.0);
    const int    axis        = revolve_axis_arg(params);
    const bool   flip        = params.value("flip", false);
    const int    axis_entity = revolve_axis_entity_arg(params, doc, si);
    return [&doc, si, angle, axis, flip, axis_entity](const std::string& name) {
        const int r = doc.add_surface_revolve(si, angle, axis, name);
        doc.features[r].flip                = flip;
        doc.features[r].revolve_axis_entity = axis_entity;
        return r;
    };
}

void surface_silent_forms(std::vector<SilentForm>& forms)
{
    forms.insert(forms.end(), {
        {"surface_extrude",              action_surface_extrude},
        {"surface_revolve",              action_surface_revolve},
        {"surface_loft",                 action_surface_loft},
        {"surface_patch",                action_surface_patch},
        {"surface_offset",               action_surface_offset},
    });
}

}}} // namespace Slic3r::GUI::Mcp
