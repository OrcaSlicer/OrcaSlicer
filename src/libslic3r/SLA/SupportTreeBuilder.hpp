#ifndef SLA_SUPPORTTREEBUILDER_HPP
#define SLA_SUPPORTTREEBUILDER_HPP

#include <libslic3r/SLA/Concurrency.hpp>
#include <libslic3r/SLA/SupportTree.hpp>
//#include <libslic3r/SLA/Contour3D.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/SLA/Pad.hpp>
#include <libslic3r/MTUtils.hpp>

namespace Slic3r {
namespace sla {

/**
 * Terminology:
 *
 * Support point:
 * The point on the model surface that needs support.
 *
 * Pillar:
 * A thick column that spans from a support point to the ground and has
 * a thick cone shaped base where it touches the ground.
 *
 * Ground facing support point:
 * A support point that can be directly connected with the ground with a pillar
 * that does not collide or cut through the model.
 *
 * Non ground facing support point:
 * A support point that cannot be directly connected with the ground (only with
 * the model surface).
 *
 * Head:
 * The pinhead that connects to the model surface with the sharp end end
 * to a pillar or bridge stick with the dull end.
 *
 * Headless support point:
 * A support point on the model surface for which there is not enough place for
 * the head. It is either in a hole or there is some barrier that would collide
 * with the head geometry. The headless support point can be ground facing and
 * non ground facing as well.
 *
 * Bridge:
 * A stick that connects two pillars or a head with a pillar.
 *
 * Junction:
 * A small ball in the intersection of two or more sticks (pillar, bridge, ...)
 *
 * CompactBridge:
 * A bridge that connects a headless support point with the model surface or a
 * nearby pillar.
 */

template<class Vec> double distance(const Vec& p) {
    return std::sqrt(p.transpose() * p);
}

template<class Vec> double distance(const Vec& pp1, const Vec& pp2) {
    auto p = pp2 - pp1;
    return distance(p);
}

const Vec3d DOWN = {0.0, 0.0, -1.0};

struct SupportTreeNode
{
    static const constexpr long ID_UNSET = -1;

    long id = ID_UNSET; // For identification withing a tree.
};

// A pinhead originating from a support point
struct Head: public SupportTreeNode {
    Vec3d dir = DOWN;
    Vec3d pos = {0, 0, 0};
    
    double r_back_mm = 1;
    double r_pin_mm = 0.5;
    double width_mm = 2;
    double penetration_mm = 0.5;

    
    // If there is a pillar connecting to this head, then the id will be set.
    long pillar_id = ID_UNSET;
    
    long bridge_id = ID_UNSET;
    
    inline void invalidate() { id = ID_UNSET; }
    inline bool is_valid() const { return id >= 0; }
    
    Head(double r_big_mm,
         double r_small_mm,
         double length_mm,
         double penetration,
         const Vec3d &direction = DOWN,  // direction (normal to the dull end)
         const Vec3d &offset = {0, 0, 0}      // displacement
         );

    inline double real_width() const
    {
        return 2 * r_pin_mm + width_mm + 2 * r_back_mm ;
    }

    inline double fullwidth() const
    {
        return real_width() - penetration_mm;
    }
    
    inline Vec3d junction_point() const
    {
        return pos + (fullwidth() - r_back_mm) * dir;
    }
    
    inline double request_pillar_radius(double radius) const
    {
        const double rmax = r_back_mm;
        return radius > 0 && radius < rmax ? radius : rmax;
    }
};

// A junction connecting bridges and pillars
struct Junction: public SupportTreeNode {
    double r = 1;
    Vec3d pos;

    Junction(const Vec3d &tr, double r_mm) : r(r_mm), pos(tr) {}
};

struct Pillar: public SupportTreeNode {
    double height, r;
    Vec3d endpt;
    
    // If the pillar connects to a head, this is the id of that head
    bool starts_from_head = true; // Could start from a junction as well
    long start_junction_id = ID_UNSET;
    
    // How many bridges are connected to this pillar
    unsigned bridges = 0;
    
    // How many pillars are cascaded with this one
    unsigned links = 0;

    Pillar(const Vec3d &endp, double h, double radius = 1.):
        height{h}, r(radius), endpt(endp), starts_from_head(false) {}

    Vec3d startpoint() const
    {
        return {endpt.x(), endpt.y(), endpt.z() + height};
    }
    
    const Vec3d& endpoint() const { return endpt; }
};

// A base for pillars or bridges that end on the ground
struct Pedestal: public SupportTreeNode {
    Vec3d pos;
    double height, r_bottom, r_top;

    Pedestal(const Vec3d &p, double h, double rbottom, double rtop)
        : pos{p}, height{h}, r_bottom{rbottom}, r_top{rtop}
    {}
};

// This is the thing that anchors a pillar or bridge to the model body.
// It is actually a reverse pinhead.
struct Anchor: public Head { using Head::Head; };

// A Bridge between two pillars (with junction endpoints)
struct Bridge: public SupportTreeNode {
    double r = 0.8;
    Vec3d startp = Vec3d::Zero(), endp = Vec3d::Zero();
    
    Bridge(const Vec3d &j1,
           const Vec3d &j2,
           double       r_mm  = 0.8): r{r_mm}, startp{j1}, endp{j2}
    {}

    double get_length() const { return (endp - startp).norm(); }
    Vec3d  get_dir() const { return (endp - startp).normalized(); }
};

struct DiffBridge: public Bridge {
    double end_r;

    DiffBridge(const Vec3d &p_s, const Vec3d &p_e, double r_s, double r_e)
        : Bridge{p_s, p_e, r_s}, end_r{r_e}
    {}
};

// A wrapper struct around the pad
struct Pad {
    indexed_triangle_set tmesh;
    PadConfig cfg;
    double zlevel = 0;
    
    Pad() = default;

    Pad(const indexed_triangle_set &support_mesh,
        const ExPolygons &          model_contours,
        double                      ground_level,
        const PadConfig &           pcfg,
        ThrowOnCancel               thr);

    bool empty() const { return tmesh.indices.size() == 0; }
};

// This class will hold the support tree meshes with some additional
// bookkeeping as well. Various parts of the support geometry are stored
// separately and are merged when the caller queries the merged mesh. The
// merged result is cached for fast subsequent delivery of the merged mesh
// which can be quite complex. The support tree creation algorithm can use an
// instance of this class as a somewhat higher level tool for crafting the 3D
// support mesh. Parts can be added with the appropriate methods such as
// add_head or add_pillar which forwards the constructor arguments and fills
// the IDs of these substructures. The IDs are basically indices into the
// arrays of the appropriate type (heads, pillars, etc...). One can later query
// e.g. a pillar for a specific head...
//
// The support pad is considered an auxiliary geometry and is not part of the
// merged mesh. It can be retrieved using a dedicated method (pad())
class SupportTreeBuilder: public SupportTree {
    // For heads it is beneficial to use the same IDs as for the support points.
    std::vector<Head>       m_heads;
    std::vector<size_t>     m_head_indices;
    std::vector<Pillar>     m_pillars;
    std::vector<Junction>   m_junctions;
    std::vector<Bridge>     m_bridges;
    std::vector<Bridge>     m_crossbridges;
    std::vector<DiffBridge> m_diffbridges;
    std::vector<Pedestal>   m_pedestals;
    std::vector<Anchor>     m_anchors;

    Pad m_pad;
    
    using Mutex = ccr::SpinningMutex;
    
    mutable indexed_triangle_set m_meshcache;
    mutable Mutex m_mutex;
    mutable bool m_meshcache_valid = false;
    mutable double m_model_height = 0; // the full height of the model
    
    template<class BridgeT, class...Args>
    const BridgeT& _add_bridge(std::vector<BridgeT> &br, Args&&... args)
    {
        std::lock_guard<Mutex> lk(m_mutex);
        br.emplace_back(std::forward<Args>(args)...);
        br.back().id = long(br.size() - 1);
        m_meshcache_valid = false;
        return br.back();
    }
    
public:
    double ground_level = 0;
    
    SupportTreeBuilder() = default;
    SupportTreeBuilder(SupportTreeBuilder &&o);
    SupportTreeBuilder(const SupportTreeBuilder &o);
    SupportTreeBuilder& operator=(SupportTreeBuilder &&o);
    SupportTreeBuilder& operator=(const SupportTreeBuilder &o);

    template<class...Args> Head& add_head(unsigned id, Args&&... args)
    {
        std::lock_guard<Mutex> lk(m_mutex);
        m_heads.emplace_back(std::forward<Args>(args)...);
        m_heads.back().id = id;
        
        if (id >= m_head_indices.size()) m_head_indices.resize(id + 1);
        m_head_indices[id] = m_heads.size() - 1;
        
        m_meshcache_valid = false;
        return m_heads.back();
    }
    
    template<class...Args> long add_pillar(long headid, double length)
    {
        std::lock_guard<Mutex> lk(m_mutex);
        if (m_pillars.capacity() < m_heads.size())
            m_pillars.reserve(m_heads.size() * 10);
        
        assert(headid >= 0 && size_t(headid) < m_head_indices.size());
        Head &head = m_heads[m_head_indices[size_t(headid)]];
        
        Vec3d hjp = head.junction_point() - Vec3d{0, 0, length};
        m_pillars.emplace_back(hjp, length, head.r_back_mm);

        Pillar& pillar = m_pillars.back();
        pillar.id = long(m_pillars.size() - 1);
        head.pillar_id = pillar.id;
        pillar.start_junction_id = head.id;
        pillar.starts_from_head = true;
        
        m_meshcache_valid = false;
        return pillar.id;
    }
    
    void add_pillar_base(long pid, double baseheight = 3, double radius = 2);

    template<class...Args> const Anchor& add_anchor(Args&&...args)
    {
        std::lock_guard<Mutex> lk(m_mutex);
        m_anchors.emplace_back(std::forward<Args>(args)...);
        m_anchors.back().id = long(m_junctions.size() - 1);
        m_meshcache_valid = false;
        return m_anchors.back();
    }
    
    void increment_bridges(const Pillar& pillar)
    {
        std::lock_guard<Mutex> lk(m_mutex);
        assert(pillar.id >= 0 && size_t(pillar.id) < m_pillars.size());
        
        if(pillar.id >= 0 && size_t(pillar.id) < m_pillars.size())
            m_pillars[size_t(pillar.id)].bridges++;
    }
    
    void increment_links(const Pillar& pillar)
    {
        std::lock_guard<Mutex> lk(m_mutex);
        assert(pillar.id >= 0 && size_t(pillar.id) < m_pillars.size());
        
        if(pillar.id >= 0 && size_t(pillar.id) < m_pillars.size()) 
            m_pillars[size_t(pillar.id)].links++;
    }
    
    unsigned bridgecount(const Pillar &pillar) const {
        std::lock_guard<Mutex> lk(m_mutex);
        assert(pillar.id >= 0 && size_t(pillar.id) < m_pillars.size());
        return pillar.bridges;
    }
    
    template<class...Args> long add_pillar(Args&&...args)
    {
        std::lock_guard<Mutex> lk(m_mutex);
        if (m_pillars.capacity() < m_heads.size())
            m_pillars.reserve(m_heads.size() * 10);
        
        m_pillars.emplace_back(std::forward<Args>(args)...);
        Pillar& pillar = m_pillars.back();
        pillar.id = long(m_pillars.size() - 1);
        pillar.starts_from_head = false;
        m_meshcache_valid = false;
        return pillar.id;
    }
    
    template<class...Args> const Junction& add_junction(Args&&... args)
    {
        std::lock_guard<Mutex> lk(m_mutex);
        m_junctions.emplace_back(std::forward<Args>(args)...);
        m_junctions.back().id = long(m_junctions.size() - 1);
        m_meshcache_valid = false;
        return m_junctions.back();
    }
    
    const Bridge& add_bridge(const Vec3d &s, const Vec3d &e, double r)
    {
        return _add_bridge(m_bridges, s, e, r);
    }
    
    const Bridge& add_bridge(long headid, const Vec3d &endp)
    {
        std::lock_guard<Mutex> lk(m_mutex);
        assert(headid >= 0 && size_t(headid) < m_head_indices.size());
        
        Head &h = m_heads[m_head_indices[size_t(headid)]];
        m_bridges.emplace_back(h.junction_point(), endp, h.r_back_mm);
        m_bridges.back().id = long(m_bridges.size() - 1);
        
        h.bridge_id = m_bridges.back().id;
        m_meshcache_valid = false;
        return m_bridges.back();
    }
    
    template<class...Args> const Bridge& add_crossbridge(Args&&... args)
    {
        return _add_bridge(m_crossbridges, std::forward<Args>(args)...);
    }

    template<class...Args> const DiffBridge& add_diffbridge(Args&&... args)
    {
        return _add_bridge(m_diffbridges, std::forward<Args>(args)...);
    }
    
    Head &head(unsigned id)
    {
        std::lock_guard<Mutex> lk(m_mutex);
        assert(id < m_head_indices.size());
        
        m_meshcache_valid = false;
        return m_heads[m_head_indices[id]];
    }
    
    inline size_t pillarcount() const {
        std::lock_guard<Mutex> lk(m_mutex);
        return m_pillars.size();
    }
    
    inline const std::vector<Pillar> &pillars() const { return m_pillars; }
    inline const std::vector<Head>   &heads() const { return m_heads; }
    inline const std::vector<Bridge> &bridges() const { return m_bridges; }
    inline const std::vector<Bridge> &crossbridges() const { return m_crossbridges; }
    
    template<class T> inline IntegerOnly<T, const Pillar&> pillar(T id) const
    {
        std::lock_guard<Mutex> lk(m_mutex);
        assert(id >= 0 && size_t(id) < m_pillars.size() &&
               size_t(id) < std::numeric_limits<size_t>::max());
        
        return m_pillars[size_t(id)];
    }
    
    template<class T> inline IntegerOnly<T, Pillar&> pillar(T id) 
    {
        std::lock_guard<Mutex> lk(m_mutex);
        assert(id >= 0 && size_t(id) < m_pillars.size() &&
               size_t(id) < std::numeric_limits<size_t>::max());
        
        return m_pillars[size_t(id)];
    }
    
    const Pad& pad() const { return m_pad; }
    
    // WITHOUT THE PAD!!!
    const indexed_triangle_set &merged_mesh(size_t steps = 45) const;
    
    // WITH THE PAD
    double full_height() const;
    
    // WITHOUT THE PAD!!!
    inline double mesh_height() const
    {
        if (!m_meshcache_valid) merged_mesh();
        return m_model_height;
    }
    
    // Intended to be called after the generation is fully complete
    const indexed_triangle_set & merge_and_cleanup();
    
    // Implement SupportTree interface:

    const indexed_triangle_set &add_pad(const ExPolygons &modelbase,
                                        const PadConfig & pcfg) override;

    void remove_pad() override { m_pad = Pad(); }

    virtual const indexed_triangle_set &retrieve_mesh(
        MeshType meshtype = MeshType::Support) const override;
};

}} // namespace Slic3r::sla

#endif // SUPPORTTREEBUILDER_HPP
