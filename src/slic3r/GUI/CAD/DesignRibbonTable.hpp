#ifndef slic3r_DesignRibbonTable_hpp_
#define slic3r_DesignRibbonTable_hpp_

// The ribbon's tab -> group -> tool table, DERIVED from the offer atlas (DesignOffer.hpp), not a
// second registry: every verb the atlas knows lands in exactly one group, in atlas order. Model
// verbs are placed by the small id -> (tab, group) map below; a verb the map has never heard of
// falls into Tools / More, so adding a verb to the atlas can never make it unreachable from the
// ribbon. Sketch-mode verbs go to the Sketch tab, grouped by their family (Rectangle, Circle, ...)
// or, lacking one, by the atlas row. A group of two or more verbs is a split button: the main
// button runs the last-used variant, the arrow opens the others.
//
// Header-only and free of wx / ImGui so the unit tests can walk it.

#include "slic3r/GUI/CAD/DesignOffer.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

struct RibbonGroup {
    std::string                   name;    // the English key; translate at draw time
    std::vector<const OfferVerb*> verbs;
};
struct RibbonTab {
    std::string              name;
    std::vector<RibbonGroup> groups;
};

namespace ribbon_detail {
struct Place { const char* verb; int tab; const char* group; };
// tab: 0 Solid, 1 Surface, 3 Tools (2 = Sketch is derived from the sketch-mode verbs).
static const Place kPlace[] = {
    {"sketch", 0, L("Sketch")},  {"sketch3d", 0, L("Sketch")},
    {"extrude", 0, L("Add material")}, {"revolve", 0, L("Add material")}, {"sweep", 0, L("Add material")}, {"loft", 0, L("Add material")},
    {"rib", 0, L("Rib / Thicken")}, {"thicken", 0, L("Rib / Thicken")},
    {"boolean", 0, L("Combine")}, {"bool_subtract", 0, L("Combine")}, {"bool_intersect", 0, L("Combine")},
    {"hole", 0, L("Hole / Thread")}, {"thread", 0, L("Hole / Thread")},
    {"cut", 0, L("Remove")}, {"split", 0, L("Remove")}, {"shell", 0, L("Remove")},
    {"fillet", 0, L("Dress-up")}, {"chamfer", 0, L("Dress-up")}, {"draft", 0, L("Dress-up")},
    {"pattern", 0, L("Repeat")}, {"pattern_circular", 0, L("Repeat")}, {"pat_curve", 0, L("Repeat")}, {"mirror", 0, L("Repeat")},
    {"transform", 0, L("Place")}, {"mate", 0, L("Place")}, {"align", 0, L("Place")},
    {"sheet_base", 0, L("Sheet metal")}, {"sheet_flange", 0, L("Sheet metal")}, {"sheet_contour", 0, L("Sheet metal")}, {"sheet_hem", 0, L("Sheet metal")},
    {"sheet_unfold", 0, L("Sheet metal")}, {"sheet_refold", 0, L("Sheet metal")}, {"sheet_rule", 0, L("Sheet metal")},
    {"surf_extrude", 1, L("Surface")}, {"surf_revolve", 1, L("Surface")}, {"surf_loft", 1, L("Surface")}, {"surf_fill", 1, L("Surface")},
    {"surf_offset", 1, L("Offset / Thicken")}, {"thicken_surf", 1, L("Offset / Thicken")},
    {"plane", 3, L("Reference")}, {"axis", 3, L("Reference")}, {"coordsys_v", 3, L("Reference")}, {"helix", 3, L("Reference")}, {"project", 3, L("Reference")},
    {"measure", 3, L("Inspect")}, {"mass_props", 3, L("Inspect")}, {"quotes", 3, L("Inspect")}, {"interference", 3, L("Inspect")},
    {"edit_feature", 3, L("Edit")}, {"rename", 3, L("Edit")}, {"colour", 3, L("Edit")}, {"zoom_to", 3, L("Edit")},
    {"delete_face", 3, L("Edit")}, {"delete", 3, L("Edit")}, {"delete_body", 3, L("Edit")},
};
} // namespace ribbon_detail

inline std::vector<RibbonTab> build_ribbon_table()
{
    using namespace ribbon_detail;
    std::vector<RibbonTab> tabs = { {L("Solid"), {}}, {L("Surface"), {}}, {L("Sketch"), {}}, {L("Tools"), {}} };
    auto group_of = [&](int tab, const std::string& name) -> RibbonGroup& {
        for (RibbonGroup& g : tabs[tab].groups) if (g.name == name) return g;
        tabs[tab].groups.push_back({name, {}});
        return tabs[tab].groups.back();
    };
    // Pre-create the model groups in the table's order, so the ribbon reads left to right the way
    // a design is built, whatever order the atlas lists the verbs in.
    for (const Place& p : kPlace) group_of(p.tab, p.group);
    for (int i = 0; i < kOfferVerbCount; ++i) {
        const OfferVerb& v = kOfferVerbs[i];
        if (v.sketch_mode) {
            const std::string g = v.family ? std::string(v.family) : std::string(kOfferRowNames[v.row]);
            group_of(2, g).verbs.push_back(&v);
            continue;
        }
        const Place* hit = nullptr;
        for (const Place& p : kPlace) if (std::strcmp(p.verb, v.id) == 0) { hit = &p; break; }
        if (hit) group_of(hit->tab, hit->group).verbs.push_back(&v);
        else     group_of(3, L("More")).verbs.push_back(&v);
    }
    // Drop groups nothing landed in (a pre-created group whose verbs the atlas no longer has).
    for (RibbonTab& t : tabs)
        t.groups.erase(std::remove_if(t.groups.begin(), t.groups.end(),
                                      [](const RibbonGroup& g) { return g.verbs.empty(); }), t.groups.end());
    return tabs;
}

}} // namespace Slic3r::GUI

#endif // slic3r_DesignRibbonTable_hpp_
