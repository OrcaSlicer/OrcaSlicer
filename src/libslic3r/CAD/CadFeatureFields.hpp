#ifndef slic3r_CadFeatureFields_hpp_
#define slic3r_CadFeatureFields_hpp_

// The recipe's view of a CadFeature (format v7 and later): its fields in GROUPS, each group one function.
//
// One list per group serves every consumer through a visitor: saving, loading, the index remap
// (CadDocument::for_each_feature_ref), and the coverage test. A new field is added HERE, once, at the
// end of the group it belongs to; it cannot be saved but not remapped, or loaded but not saved.
//
// v7 writes a group only when it differs from a default CadFeature's, each as (id, length, bytes). So a
// feature stores only what it uses, a group an older build wrote short keeps its leading fields and
// defaults the rest, and a short or unknown group never costs the groups after it (the v5/v6 layout
// was one positional list, where one early end dropped everything behind it).
//
// RULES: group ids are never reused or renumbered; fields are only ever APPENDED to a group, and only
// to the end of its function; a field that is an index into features[] is declared with v.ref().
// The test "the recipe groups cover every serialised CadFeature field" checks that every field the legacy list writes is in a group;
// it cannot see a new field that is in no list at all: adding it to a group is on you.

#include "CadDocument.hpp"

#include <TopoDS_Shape.hxx>
#include <cereal/archives/binary.hpp>
#include <cereal/cereal.hpp>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// Plain fields go through v(x). v.ref(x) marks an index into features[] (an int or a vector<int>), the
// ones CadDocument remaps when the history changes. v.brep(x) is the imported solid, saved as BRep text.

template<class V, class F> void cad_fields_sketch(V& v, F& f)
{
    v(f.shape); v(f.plane); v(f.width); v(f.height); v(f.radius);
    v(f.profile); v(f.entities); v(f.constraints); v(f.entity_constraints); v(f.imported_regions);
    v(f.import_offset); v(f.import_scale_x); v(f.import_scale_y); v(f.import_on_face); v(f.import_face_body);
    v(f.text_string); v(f.text_font); v(f.text_height);
    v(f.sk3_points); v(f.sk3_closed); v(f.sk3_smooth);
    v(f.attach_body); v(f.attach_face);
    v.ref(f.attach_datum);
    v(f.dim_names);
}
template<class V, class F> void cad_fields_import(V& v, F& f)
{
    v.brep(f.imported_solid);
}
template<class V, class F> void cad_fields_extrude(V& v, F& f)
{
    v.ref(f.sketch_ref);
    v(f.distance); v(f.symmetric); v(f.mode); v(f.extrude_end); v(f.distance2); v(f.taper_deg); v(f.flip);
    v(f.up_to_face); v(f.extrude_src_face); v(f.up_to_point); v(f.target_body);
    v(f.thin_in); v(f.thin_out); v(f.extrude_entities);
}
template<class V, class F> void cad_fields_dressup(V& v, F& f)
{
    v(f.dressup_size); v(f.face_group); v(f.dressup_edge); v(f.dressup_edges);
    v(f.dressup_size2); v(f.dressup_chord); v(f.dressup_has_start); v(f.dressup_start);
    v(f.dressup_radii); v(f.dressup_knots); v(f.dressup_fit_taper);
}
template<class V, class F> void cad_fields_hole_thread(V& v, F& f)
{
    v(f.hole_diameter); v(f.hole_depth); v(f.hole_through); v(f.hole_x); v(f.hole_y);
    v(f.hole_style); v(f.hole_cbore_diameter); v(f.hole_cbore_depth);
    v(f.hole_csink_diameter); v(f.hole_csink_angle); v(f.hole_standard); v(f.hole_tip_angle);
    v(f.thread_radius); v(f.thread_pitch); v(f.thread_height); v(f.thread_depth);
    v(f.thread_internal); v(f.thread_x); v(f.thread_y); v(f.thread_major_nominal);
}
template<class V, class F> void cad_fields_shell_draft(V& v, F& f)
{
    v(f.shell_thickness); v(f.shell_face); v(f.shell_faces);
    v(f.draft_face); v(f.draft_angle); v(f.draft_faces); v(f.draft_has_neutral); v(f.draft_pull); v(f.draft_neutral);
    v(f.delete_faces);
}
template<class V, class F> void cad_fields_revolve_sweep_loft(V& v, F& f)
{
    v(f.revolve_angle); v(f.revolve_axis); v(f.revolve_axis_entity);
    v.ref(f.sweep_path_ref);
    v.ref(f.loft_profile_refs); v(f.loft_ruled); v(f.loft_start_cond); v(f.loft_end_cond);
    v(f.loft_profile_loops); v(f.loft_profile_faces);
}
template<class V, class F> void cad_fields_pattern(V& v, F& f)
{
    v(f.pattern_circular); v(f.pattern_count); v(f.pattern_spacing); v(f.pattern_dir); v(f.pattern_angle);
    v(f.pattern_inclusive);
    v.ref(f.pattern_features);
    v(f.pattern_axis); v(f.pattern_origin); v(f.pattern_axis2); v(f.pattern_count2); v(f.pattern_spacing2);
    v(f.pattern_mirror);
    v.ref(f.pattern_curve_sketch); v(f.pattern_curve_entity);
}
template<class V, class F> void cad_fields_datum(V& v, F& f)
{
    v(f.plane_base); v(f.plane_offset); v(f.plane_angle_tilt); v(f.plane_axis); v(f.plane_type);
    v(f.plane_face_body); v(f.plane_face); v(f.plane_face2_body); v(f.plane_face2);
    v(f.plane_edge_body); v(f.plane_edge); v(f.plane_edge2_body); v(f.plane_edge2);
    v(f.plane_u_size); v(f.plane_v_size);
    v(f.axis_type); v(f.axis_p1); v(f.axis_p2); v(f.axis_body); v(f.axis_face); v(f.axis_edge);
    v(f.axis_plane_a); v(f.axis_plane_b);
    v(f.coordsys_type); v(f.coordsys_point); v(f.coordsys_body); v(f.coordsys_face); v(f.coordsys_edge);
    v(f.coordsys_x_hint); v(f.coordsys_face_kind); v(f.coordsys_face_edges); v(f.coordsys_snap);
}
template<class V, class F> void cad_fields_boolean_cut_mirror(V& v, F& f)
{
    v(f.bool_tool_body); v(f.bool_keep_tool); v(f.bool_tolerance); v(f.bool_target_face); v(f.bool_tool_face);
    v(f.cut_offset); v(f.cut_flip); v(f.cut_keep_upper); v(f.cut_keep_lower); v(f.cut_face_body); v(f.cut_face);
    v(f.mirror_keep_original);
}
template<class V, class F> void cad_fields_helix_xf_thicken(V& v, F& f)
{
    v(f.helix_radius); v(f.helix_pitch); v(f.helix_height); v(f.helix_left_handed); v(f.helix_taper_deg);
    v(f.xf_translate); v(f.xf_axis); v(f.xf_pivot); v(f.xf_angle_deg); v(f.xf_copy);
    v(f.thicken_face); v(f.thicken_thickness); v(f.thicken_flip);
}
template<class V, class F> void cad_fields_project_rib_mate(V& v, F& f)
{
    v(f.project_source_body); v(f.project_edges); v(f.project_face);
    v(f.project_from_sketch); v.ref(f.project_source_sketch); v(f.project_sketch_entities);
    v.ref(f.rib_sketch_ref); v(f.rib_entity); v(f.rib_thickness); v(f.rib_depth);
    v(f.mate_kind); v.ref(f.mate_cs_a); v.ref(f.mate_cs_b); v(f.mate_offset); v(f.mate_angle); v(f.mate_flip);
}
template<class V, class F> void cad_fields_sheet_metal(V& v, F& f)
{
    v(f.sm_thickness); v(f.sm_radius); v(f.sm_k); v(f.sm_gap); v(f.sm_angle);
    v.ref(f.sm_ref);
}
template<class V, class F> void cad_fields_expr(V& v, F& f)
{
    v(f.expr);
}

// Group ids, as written to the recipe. NEVER renumber or reuse one.
enum CadGroupId : uint16_t {
    GroupSketch = 1, GroupImport, GroupExtrude, GroupDressup, GroupHoleThread, GroupShellDraft,
    GroupRevolveSweepLoft, GroupPattern, GroupDatum, GroupBooleanCutMirror, GroupHelixXfThicken,
    GroupProjectRibMate, GroupSheetMetal, GroupExpr,
    GroupLast = GroupExpr
};

// Run `v` over the fields of group `id`; false for an id this build does not know.
template<class V, class F> bool cad_fields_of_group(uint16_t id, V& v, F& f)
{
    switch (id) {
    case GroupSketch:           cad_fields_sketch(v, f);           return true;
    case GroupImport:           cad_fields_import(v, f);           return true;
    case GroupExtrude:          cad_fields_extrude(v, f);          return true;
    case GroupDressup:          cad_fields_dressup(v, f);          return true;
    case GroupHoleThread:       cad_fields_hole_thread(v, f);      return true;
    case GroupShellDraft:       cad_fields_shell_draft(v, f);      return true;
    case GroupRevolveSweepLoft: cad_fields_revolve_sweep_loft(v, f); return true;
    case GroupPattern:          cad_fields_pattern(v, f);          return true;
    case GroupDatum:            cad_fields_datum(v, f);            return true;
    case GroupBooleanCutMirror: cad_fields_boolean_cut_mirror(v, f); return true;
    case GroupHelixXfThicken:   cad_fields_helix_xf_thicken(v, f); return true;
    case GroupProjectRibMate:   cad_fields_project_rib_mate(v, f); return true;
    case GroupSheetMetal:       cad_fields_sheet_metal(v, f);      return true;
    case GroupExpr:             cad_fields_expr(v, f);             return true;
    }
    return false;
}

template<class V, class F> void cad_fields_of_all_groups(V& v, F& f)
{
    for (uint16_t id = 1; id <= GroupLast; ++id)
        cad_fields_of_group(id, v, f);
}

// Visitors. Save and Load wrap a cereal archive; Refs hands every features[] index to a callback.
template<class Archive> struct CadSaveVisitor {
    Archive& ar;
    template<class T> void operator()(const T& x) { ar(x); }
    template<class T> void ref(const T& x)        { ar(x); }
    void brep(const TopoDS_Shape& s)              { ar(brep_to_string(s)); }
};
template<class Archive> struct CadLoadVisitor {
    Archive& ar;
    template<class T> void operator()(T& x) { ar(x); }
    template<class T> void ref(T& x)        { ar(x); }
    void brep(TopoDS_Shape& s)              { std::string d; ar(d); s = brep_from_string(d); }
};
template<class Fn> struct CadRefVisitor {
    Fn& fn;
    template<class T> void operator()(T&) {}
    void ref(int& i)                { fn(i); }
    void ref(std::vector<int>& v)   { for (int& i : v) fn(i); }
    void brep(TopoDS_Shape&) {}
};

// The bytes of one group of `f`, as a BinaryOutputArchive writes them.
inline std::string cad_group_bytes(uint16_t id, const CadFeature& f)
{
    std::ostringstream o;
    {
        cereal::BinaryOutputArchive a(o);
        CadSaveVisitor<cereal::BinaryOutputArchive> v{a};
        cad_fields_of_group(id, v, f);
    }
    return o.str();
}

// v7 feature stream: type, name, enabled, then the number of blocks and each as (id, length, bytes).
template<class Archive> void cad_save_feature(Archive& ar, const CadFeature& f)
{
    static const std::array<std::string, GroupLast + 1> defaults = [] {
        std::array<std::string, GroupLast + 1> d;
        const CadFeature def;
        for (uint16_t id = 1; id <= GroupLast; ++id)
            d[id] = cad_group_bytes(id, def);
        return d;
    }();
    ar(f.type, f.name, f.enabled);
    std::vector<std::pair<uint16_t, std::string>> blocks;
    for (uint16_t id = 1; id <= GroupLast; ++id) {
        std::string b = cad_group_bytes(id, f);
        if (b != defaults[id])
            blocks.emplace_back(id, std::move(b));
    }
    ar(uint16_t(blocks.size()));
    for (const auto& blk : blocks) {
        ar(blk.first, uint32_t(blk.second.size()));
        ar(cereal::binary_data(blk.second.data(), blk.second.size()));
    }
}

// A group this build does not know is skipped; one that ends early keeps the fields it did read and
// defaults the rest. Neither costs the groups after it. A stream that ends between blocks throws (the
// caller keeps what was read).
template<class Archive> void cad_load_feature(Archive& ar, CadFeature& f)
{
    ar(f.type, f.name, f.enabled);
    uint16_t n = 0;
    ar(n);
    for (uint16_t i = 0; i < n; ++i) {
        uint16_t id = 0;
        uint32_t len = 0;
        ar(id, len);
        std::string buf(len, '\0');
        if (len > 0)
            ar(cereal::binary_data(&buf[0], len));
        try {
            std::istringstream is(buf);
            cereal::BinaryInputArchive ia(is);
            CadLoadVisitor<cereal::BinaryInputArchive> v{ia};
            cad_fields_of_group(id, v, f);
        } catch (...) {
            // a group an older build wrote short: the fields read so far stay
        }
    }
}

} // namespace Slic3r

#endif
