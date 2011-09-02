#ifndef SLA_SUPPORTTREE_HPP
#define SLA_SUPPORTTREE_HPP

#include <vector>
#include <memory>
#include <Eigen/Geometry>

#include <libslic3r/Polygon.hpp>
#include <libslic3r/ExPolygon.hpp>

#include <libslic3r/SLA/Pad.hpp>
#include <libslic3r/SLA/IndexedMesh.hpp>
#include <libslic3r/SLA/SupportPoint.hpp>
#include <libslic3r/SLA/JobController.hpp>

namespace Slic3r {

class TriangleMesh;
class Model;
class ModelInstance;
class ModelObject;

namespace sla {

enum class PillarConnectionMode
{
    zigzag,
    cross,
    dynamic
};

struct SupportTreeConfig
{
    bool   enabled = true;
    
    // Radius in mm of the pointing side of the head.
    double head_front_radius_mm = 0.2;

    // How much the pinhead has to penetrate the model surface
    double head_penetration_mm = 0.5;

    // Radius of the back side of the 3d arrow.
    double head_back_radius_mm = 0.5;

    double head_fallback_radius_mm = 0.25;

    // Width in mm from the back sphere center to the front sphere center.
    double head_width_mm = 1.0;

    // How to connect pillars
    PillarConnectionMode pillar_connection_mode = PillarConnectionMode::dynamic;

    // Only generate pillars that can be routed to ground
    bool ground_facing_only = false;

    // TODO: unimplemented at the moment. This coefficient will have an impact
    // when bridges and pillars are merged. The resulting pillar should be a bit
    // thicker than the ones merging into it. How much thicker? I don't know
    // but it will be derived from this value.
    double pillar_widening_factor = 0.5;

    // Radius in mm of the pillar base.
    double base_radius_mm = 2.0;

    // The height of the pillar base cone in mm.
    double base_height_mm = 1.0;

    // The default angle for connecting support sticks and junctions.
    double bridge_slope = M_PI/4;

    // The max length of a bridge in mm
    double max_bridge_length_mm = 10.0;

    // The max distance of a pillar to pillar link.
    double max_pillar_link_distance_mm = 10.0;

    // The elevation in Z direction upwards. This is the space between the pad
    // and the model object's bounding box bottom.
    double object_elevation_mm = 10;
    
    // The shortest distance between a pillar base perimeter from the model
    // body. This is only useful when elevation is set to zero.
    double pillar_base_safety_distance_mm = 0.5;
    
    unsigned max_bridges_on_pillar = 3;
    
    double head_fullwidth() const {
        return 2 * head_front_radius_mm + head_width_mm +
               2 * head_back_radius_mm - head_penetration_mm;
    }

    // /////////////////////////////////////////////////////////////////////////
    // Compile time configuration values (candidates for runtime)
    // /////////////////////////////////////////////////////////////////////////

    // The max Z angle for a normal at which it will get completely ignored.
    static const double constexpr normal_cutoff_angle = 150.0 * M_PI / 180.0;

    // The shortest distance of any support structure from the model surface
    static const double constexpr safety_distance_mm = 0.5;

    static const double constexpr max_solo_pillar_height_mm = 15.0;
    static const double constexpr max_dual_pillar_height_mm = 35.0;
    static const double constexpr optimizer_rel_score_diff = 1e-6;
    static const unsigned constexpr optimizer_max_iterations = 1000;
    static const unsigned constexpr pillar_cascade_neighbors = 3;
    
};

// TODO: Part of future refactor
//class SupportConfig {
//    std::optional<SupportTreeConfig> tree_cfg {std::in_place_t{}}; // fill up
//    std::optional<PadConfig>         pad_cfg;
//};

enum class MeshType { Support, Pad };

struct SupportableMesh
{
    IndexedMesh  emesh;
    SupportPoints pts;
    SupportTreeConfig cfg;
//    PadConfig     pad_cfg;

    explicit SupportableMesh(const indexed_triangle_set & trmsh,
                             const SupportPoints &sp,
                             const SupportTreeConfig &c)
        : emesh{trmsh}, pts{sp}, cfg{c}
    {}
    
    explicit SupportableMesh(const IndexedMesh   &em,
                             const SupportPoints &sp,
                             const SupportTreeConfig &c)
        : emesh{em}, pts{sp}, cfg{c}
    {}
};

/// The class containing mesh data for the generated supports.
class SupportTree
{
    JobController m_ctl;
public:
    using UPtr = std::unique_ptr<SupportTree>;
    
    static UPtr create(const SupportableMesh &input,
                       const JobController &ctl = {});

    virtual ~SupportTree() = default;

    virtual const indexed_triangle_set &retrieve_mesh(MeshType meshtype) const = 0;

    /// Adding the "pad" under the supports.
    /// modelbase will be used according to the embed_object flag in PoolConfig.
    /// If set, the plate will be interpreted as the model's intrinsic pad. 
    /// Otherwise, the modelbase will be unified with the base plate calculated
    /// from the supports.
    virtual const indexed_triangle_set &add_pad(const ExPolygons &modelbase,
                                                const PadConfig & pcfg) = 0;

    virtual void remove_pad() = 0;
    
    std::vector<ExPolygons> slice(const std::vector<float> &,
                                  float closing_radius) const;
    
    void retrieve_full_mesh(indexed_triangle_set &outmesh) const;
    
    const JobController &ctl() const { return m_ctl; }
};

}

}

#endif // SLASUPPORTTREE_HPP
