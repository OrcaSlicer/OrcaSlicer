#ifndef slic3r_SceneRaycaster_hpp_
#define slic3r_SceneRaycaster_hpp_

#include "MeshUtils.hpp"
#include "GLModel.hpp"
#include <vector>
#include <string>
#include <optional>

namespace Slic3r {
namespace GUI {

struct Camera;

class SceneRaycasterItem
{
    int m_id{ -1 };
    bool m_active{ true };
    bool m_use_back_faces{ false };
    const MeshRaycaster* m_raycaster;
    Transform3d m_trafo;

public:
    SceneRaycasterItem(int id, const MeshRaycaster& raycaster)
        : m_id(id), m_raycaster(&raycaster), m_trafo(Transform3d::Identity()), m_use_back_faces(false)
    {}
    SceneRaycasterItem(int id, const MeshRaycaster& raycaster, const Transform3d& trafo, bool use_back_faces = false)
        : m_id(id), m_raycaster(&raycaster), m_trafo(trafo), m_use_back_faces(use_back_faces)
    {}

    int get_id() const { return m_id; }
    bool is_active() const { return m_active; }
    void set_active(bool active) { m_active = active; }
    bool use_back_faces() const { return m_use_back_faces; }
    const MeshRaycaster* get_raycaster() const { return m_raycaster; }
    const Transform3d& get_transform() const { return m_trafo; }
    void set_transform(const Transform3d& trafo) { m_trafo = trafo; }
};

class SceneRaycaster
{
public:
    enum class EType
    {
        None,
        Bed,
        Volume,
        Gizmo,
        FallbackGizmo // Is used for gizmo grabbers which will be hit after all grabbers of Gizmo type
    };

    enum class EIdBase
    {
        Bed    = 0,
        Volume = 1000, // Must be greater than PartPlateList::MAX_PLATES_COUNT * PartPlate::GRABBER_COUNT
        Gizmo  = 1000000,
        FallbackGizmo = 2000000
    };

    struct HitResult
    {
        EType type{ EType::None };
        int raycaster_id{ -1 };
        Vec3f position{ Vec3f::Zero() };
        Vec3f normal{ Vec3f::Zero() };

        bool is_valid() const { return raycaster_id != -1; }
    };

private:
    std::vector<std::shared_ptr<SceneRaycasterItem>> m_bed;
    std::vector<std::shared_ptr<SceneRaycasterItem>> m_volumes;
    std::vector<std::shared_ptr<SceneRaycasterItem>> m_gizmos;
    std::vector<std::shared_ptr<SceneRaycasterItem>> m_fallback_gizmos;

    // When set to true, if checking gizmos returns a valid hit,
    // the search is not performed on other types
    bool m_gizmos_on_top{ false };

#if ENABLE_RAYCAST_PICKING_DEBUG
    GLModel m_sphere;
    GLModel m_line;
    std::optional<HitResult> m_last_hit;
#endif // ENABLE_RAYCAST_PICKING_DEBUG

public:
    SceneRaycaster();

    std::shared_ptr<SceneRaycasterItem> add_raycaster(EType type, int picking_id, const MeshRaycaster& raycaster,
        const Transform3d& trafo, bool use_back_faces = false);
    void remove_raycasters(EType type, int id);
    void remove_raycasters(EType type);
    void remove_raycaster(std::shared_ptr<SceneRaycasterItem> item);

    std::vector<std::shared_ptr<SceneRaycasterItem>>* get_raycasters(EType type);
    const std::vector<std::shared_ptr<SceneRaycasterItem>>* get_raycasters(EType type) const;

    void set_gizmos_on_top(bool value) { m_gizmos_on_top = value; }

    HitResult hit(const Vec2d& mouse_pos, const Camera& camera, const ClippingPlane* clipping_plane = nullptr) const;

#if ENABLE_RAYCAST_PICKING_DEBUG
    void render_hit(const Camera& camera);

    size_t beds_count() const    { return m_bed.size(); }
    size_t volumes_count() const { return m_volumes.size(); }
    size_t gizmos_count() const  { return m_gizmos.size(); }
    size_t fallback_gizmos_count() const  { return m_fallback_gizmos.size(); }
    size_t active_beds_count() const;
    size_t active_volumes_count() const;
    size_t active_gizmos_count() const;
    size_t active_fallback_gizmos_count() const;
#endif // ENABLE_RAYCAST_PICKING_DEBUG

    static int decode_id(EType type, int id);

private:
    static int encode_id(EType type, int id);
    static int base_id(EType type);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_SceneRaycaster_hpp_
