#define NOMINMAX

#include <libslic3r/SLA/SupportTreeBuilder.hpp>
#include <libslic3r/SLA/SupportTreeBuildsteps.hpp>
#include <libslic3r/SLA/SupportTreeMesher.hpp>
//#include <libslic3r/SLA/Contour3D.hpp>

namespace Slic3r {
namespace sla {

Head::Head(double       r_big_mm,
           double       r_small_mm,
           double       length_mm,
           double       penetration,
           const Vec3d &direction,
           const Vec3d &offset)
    : dir(direction)
    , pos(offset)
    , r_back_mm(r_big_mm)
    , r_pin_mm(r_small_mm)
    , width_mm(length_mm)
    , penetration_mm(penetration)
{
}

Pad::Pad(const indexed_triangle_set &support_mesh,
         const ExPolygons &          model_contours,
         double                      ground_level,
         const PadConfig &           pcfg,
         ThrowOnCancel               thr)
    : cfg(pcfg)
    , zlevel(ground_level + pcfg.full_height() - pcfg.required_elevation())
{
    thr();
    
    ExPolygons sup_contours;
    
    float zstart = float(zlevel);
    float zend   = zstart + float(pcfg.full_height() + EPSILON);
    
    pad_blueprint(support_mesh, sup_contours, grid(zstart, zend, 0.1f), thr);
    create_pad(sup_contours, model_contours, tmesh, pcfg);
    
    Vec3f offs{.0f, .0f, float(zlevel)};
    for (auto &p : tmesh.vertices) p += offs;

    its_merge_vertices(tmesh);
}

const indexed_triangle_set &SupportTreeBuilder::add_pad(
    const ExPolygons &modelbase, const PadConfig &cfg)
{
    m_pad = Pad{merged_mesh(), modelbase, ground_level, cfg, ctl().cancelfn};
    return m_pad.tmesh;
}

SupportTreeBuilder::SupportTreeBuilder(SupportTreeBuilder &&o)
    : m_heads(std::move(o.m_heads))
    , m_head_indices{std::move(o.m_head_indices)}
    , m_pillars{std::move(o.m_pillars)}
    , m_bridges{std::move(o.m_bridges)}
    , m_crossbridges{std::move(o.m_crossbridges)}
    , m_pad{std::move(o.m_pad)}
    , m_meshcache{std::move(o.m_meshcache)}
    , m_meshcache_valid{o.m_meshcache_valid}
    , m_model_height{o.m_model_height}
    , ground_level{o.ground_level}
{}

SupportTreeBuilder::SupportTreeBuilder(const SupportTreeBuilder &o)
    : m_heads(o.m_heads)
    , m_head_indices{o.m_head_indices}
    , m_pillars{o.m_pillars}
    , m_bridges{o.m_bridges}
    , m_crossbridges{o.m_crossbridges}
    , m_pad{o.m_pad}
    , m_meshcache{o.m_meshcache}
    , m_meshcache_valid{o.m_meshcache_valid}
    , m_model_height{o.m_model_height}
    , ground_level{o.ground_level}
{}

SupportTreeBuilder &SupportTreeBuilder::operator=(SupportTreeBuilder &&o)
{
    m_heads = std::move(o.m_heads);
    m_head_indices = std::move(o.m_head_indices);
    m_pillars = std::move(o.m_pillars);
    m_bridges = std::move(o.m_bridges);
    m_crossbridges = std::move(o.m_crossbridges);
    m_pad = std::move(o.m_pad);
    m_meshcache = std::move(o.m_meshcache);
    m_meshcache_valid = o.m_meshcache_valid;
    m_model_height = o.m_model_height;
    ground_level = o.ground_level;
    return *this;
}

SupportTreeBuilder &SupportTreeBuilder::operator=(const SupportTreeBuilder &o)
{
    m_heads = o.m_heads;
    m_head_indices = o.m_head_indices;
    m_pillars = o.m_pillars;
    m_bridges = o.m_bridges;
    m_crossbridges = o.m_crossbridges;
    m_pad = o.m_pad;
    m_meshcache = o.m_meshcache;
    m_meshcache_valid = o.m_meshcache_valid;
    m_model_height = o.m_model_height;
    ground_level = o.ground_level;
    return *this;
}

void SupportTreeBuilder::add_pillar_base(long pid, double baseheight, double radius)
{
    std::lock_guard<Mutex> lk(m_mutex);
    assert(pid >= 0 && size_t(pid) < m_pillars.size());
    Pillar& pll = m_pillars[size_t(pid)];
    m_pedestals.emplace_back(pll.endpt, std::min(baseheight, pll.height),
                             std::max(radius, pll.r), pll.r);

    m_pedestals.back().id = m_pedestals.size() - 1;
    m_meshcache_valid = false;
}

const indexed_triangle_set &SupportTreeBuilder::merged_mesh(size_t steps) const
{
    if (m_meshcache_valid) return m_meshcache;
    
    indexed_triangle_set merged;
    
    for (auto &head : m_heads) {
        if (ctl().stopcondition()) break;
        if (head.is_valid()) its_merge(merged, get_mesh(head, steps));
    }
    
    for (auto &pill : m_pillars) {
        if (ctl().stopcondition()) break;
        its_merge(merged, get_mesh(pill, steps));
    }

    for (auto &pedest : m_pedestals) {
        if (ctl().stopcondition()) break;
        its_merge(merged, get_mesh(pedest, steps));
    }
    
    for (auto &j : m_junctions) {
        if (ctl().stopcondition()) break;
        its_merge(merged, get_mesh(j, steps));
    }

    for (auto &bs : m_bridges) {
        if (ctl().stopcondition()) break;
        its_merge(merged, get_mesh(bs, steps));
    }
    
    for (auto &bs : m_crossbridges) {
        if (ctl().stopcondition()) break;
        its_merge(merged, get_mesh(bs, steps));
    }

    for (auto &bs : m_diffbridges) {
        if (ctl().stopcondition()) break;
        its_merge(merged, get_mesh(bs, steps));
    }

    for (auto &anch : m_anchors) {
        if (ctl().stopcondition()) break;
        its_merge(merged, get_mesh(anch, steps));
    }

    if (ctl().stopcondition()) {
        // In case of failure we have to return an empty mesh
        m_meshcache = {};
        return m_meshcache;
    }
    
    m_meshcache = merged;
    
    // The mesh will be passed by const-pointer to TriangleMeshSlicer,
    // which will need this.
    its_merge_vertices(m_meshcache);
    
    BoundingBoxf3 bb = bounding_box(m_meshcache);
    m_model_height   = bb.max(Z) - bb.min(Z);

    m_meshcache_valid = true;
    return m_meshcache;
}

double SupportTreeBuilder::full_height() const
{
    if (merged_mesh().indices.empty() && !pad().empty())
        return pad().cfg.full_height();
    
    double h = mesh_height();
    if (!pad().empty()) h += pad().cfg.required_elevation();
    return h;
}

const indexed_triangle_set &SupportTreeBuilder::merge_and_cleanup()
{
    // in case the mesh is not generated, it should be...
    auto &ret = merged_mesh(); 
    
    // Doing clear() does not garantee to release the memory.
    m_heads = {};
    m_head_indices = {};
    m_pillars = {};
    m_junctions = {};
    m_bridges = {};
    
    return ret;
}

const indexed_triangle_set &SupportTreeBuilder::retrieve_mesh(MeshType meshtype) const
{
    switch(meshtype) {
    case MeshType::Support: return merged_mesh();
    case MeshType::Pad:     return pad().tmesh;
    }
    
    return m_meshcache;
}

}} // namespace Slic3r::sla
