#pragma once

// Step-by-step capture of a bake.
//
// A bake is a chain of stages that each rewrite the whole mesh, so when the result looks wrong the
// only useful question is which stage made it wrong. This records the geometry, the wall time and the
// topology after every stage, which is what the gizmo's debug view steps through and what the
// benchmark's --dump-stages writes out.
//
// Deliberately independent of TriangleMesh: a stage is held as a plain vertex/index pair, which is
// layout-compatible with indexed_triangle_set's own members (stl_vertex is Vec3f,
// stl_triangle_vertex_indices is Vec3i32), so the GUI assigns rather than converts and the standalone
// benchmark does not have to link admesh to use this.
//
// Recording is off unless enable(true) was called, and every capture site is a null-pointer check, so
// a normal bake pays nothing for this being here.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "TextureBakeIndex.hpp"

namespace Slic3r {

struct BakeStageMesh
{
    std::vector<Vec3f>   vertices;
    std::vector<Vec3i32> indices;

    bool   empty() const { return indices.empty(); }
    size_t triangle_count() const { return indices.size(); }
};

struct BakeStageSnapshot
{
    std::string   name;   // "remesh", "subdivide", ...
    std::string   detail; // whatever the stage has to say: collapse counts, rejected moves, ...
    BakeStageMesh mesh;   // empty when the stage was over the memory cap - see mesh_dropped

    double ms        = 0.0;
    size_t triangles = 0;
    size_t vertices  = 0;

    // Filled only when the recorder was asked to check topology: it is a sort over every half-edge,
    // which on a multi-million triangle stage costs more than the stage being measured.
    size_t open_edges         = 0;
    size_t non_manifold_edges = 0;
    size_t degenerate         = 0;
    bool   topology_checked   = false;

    // The geometry was dropped to stay inside the memory cap; every count above is still real.
    bool mesh_dropped = false;
};

// Edge and area defects of a captured stage. Split out so a caller can run it on its own.
void bake_stage_topology(const BakeStageMesh &mesh, size_t &open_edges, size_t &non_manifold_edges,
                         size_t &degenerate);

// Writes `<dir>/NN_name.obj` for every stage that still holds geometry, plus a `stages.txt` summary.
// Returns how many meshes were written. Existing files with the same names are overwritten.
//
// A free function rather than a recorder method because by the time anyone wants the files the
// recorder is usually gone and only the stages survive - that is how the gizmo holds them.
size_t dump_bake_stages(const std::vector<BakeStageSnapshot> &stages, const std::string &dir);

class BakeStageRecorder
{
public:
    // Nothing is recorded until this is on.
    void enable(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }

    // The edge scan is optional because it is O(n log n) over every half-edge, and a debug run that
    // only wants to see the geometry should not pay for it on every stage.
    void set_check_topology(bool on) { m_check_topology = on; }
    bool check_topology() const { return m_check_topology; }

    // Stages above this keep their counts but not their geometry. A debug run holds every stage at
    // once, and a 4 M triangle stage is about 150 MB on its own, so without a cap stepping through a
    // fine bake would need more memory than the bake did.
    void   set_mesh_cap(size_t triangles) { m_mesh_cap = triangles; }
    size_t mesh_cap() const { return m_mesh_cap; }

    // `ms` is passed in rather than measured here: the caller is already timing the stage, and the
    // capture itself (a weld, a copy, possibly an edge scan) must not land inside that measurement.
    void capture(const char *name, const TextureBake::TriSoup &soup, double ms,
                 const std::string &detail = {});
    void capture(const char *name, const std::vector<Vec3f> &vertices,
                 const std::vector<Vec3i32> &indices, double ms, const std::string &detail = {});
    // For a stage that changed nothing a caller can still show, e.g. a skipped remesh.
    void capture_note(const char *name, double ms, const std::string &detail);

    // Index of the next stage to be recorded. Paired with rebase() to fix up a range afterwards.
    size_t mark() const { return m_stages.size(); }

    // Brings stages [from, end) into the caller's own space and winding. The bake runs in world
    // millimetres and, for a mirrored placement, against a reversed winding; the debug view draws in
    // the volume's local frame, so a captured range has to be brought back the same way the bake's
    // own result is. `to_local` may be null for no transform.
    void rebase(size_t from, const Transform3d *to_local, bool flip_winding);

    const std::vector<BakeStageSnapshot> &stages() const { return m_stages; }
    std::vector<BakeStageSnapshot>        take() { return std::move(m_stages); }
    void                                  clear() { m_stages.clear(); }
    bool                                  empty() const { return m_stages.empty(); }

    // Total recorded wall time, which is the bake's own time minus whatever it does outside a stage.
    double total_ms() const;

    size_t dump_obj(const std::string &dir) const { return dump_bake_stages(m_stages, dir); }

private:
    void finish(BakeStageSnapshot &s);

    std::vector<BakeStageSnapshot> m_stages;
    bool                           m_enabled        = false;
    bool                           m_check_topology = true;
    size_t                         m_mesh_cap       = 4'000'000;
};

} // namespace Slic3r
