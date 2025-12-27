#include "libslic3r/libslic3r.h"
#include "SceneRaycaster.hpp"

#include "Camera.hpp"
#include "GUI_App.hpp"
#include "Selection.hpp"
#include "Plater.hpp"

namespace Slic3r {
namespace GUI {

// EIdBase::Volume must be greater than PartPlateList::MAX_PLATES_COUNT * PartPlate::GRABBER_COUNT
static_assert((int) SceneRaycaster::EIdBase::Volume > PartPlateList::MAX_PLATES_COUNT * PartPlate::GRABBER_COUNT, "EIdBase::Volume too small");

SceneRaycaster::SceneRaycaster() {
#if ENABLE_RAYCAST_PICKING_DEBUG
    // hit point
    m_sphere.init_from(its_make_sphere(1.0, double(PI) / 16.0));
    m_sphere.set_color(ColorRGBA::YELLOW());

    // hit normal
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.color = ColorRGBA::YELLOW();
    init_data.reserve_vertices(2);
    init_data.reserve_indices(2);

    // vertices
    init_data.add_vertex((Vec3f)Vec3f::Zero());
    init_data.add_vertex((Vec3f)Vec3f::UnitZ());

    // indices
    init_data.add_line(0, 1);

    m_line.init_from(std::move(init_data));
#endif // ENABLE_RAYCAST_PICKING_DEBUG
}

std::shared_ptr<SceneRaycasterItem> SceneRaycaster::add_raycaster(EType type, int id, const MeshRaycaster& raycaster,
    const Transform3d& trafo, bool use_back_faces)
{
    switch (type) {
    case EType::Bed:    { return m_bed.emplace_back(std::make_shared<SceneRaycasterItem>(encode_id(type, id), raycaster, trafo, use_back_faces)); }
    case EType::Volume: { return m_volumes.emplace_back(std::make_shared<SceneRaycasterItem>(encode_id(type, id), raycaster, trafo, use_back_faces)); }
    case EType::Gizmo:  { return m_gizmos.emplace_back(std::make_shared<SceneRaycasterItem>(encode_id(type, id), raycaster, trafo, use_back_faces)); }
    case EType::FallbackGizmo:  { return m_fallback_gizmos.emplace_back(std::make_shared<SceneRaycasterItem>(encode_id(type, id), raycaster, trafo, use_back_faces)); }
    default:            { assert(false);  return nullptr; }
    };
}

void SceneRaycaster::remove_raycasters(EType type, int id)
{
    std::vector<std::shared_ptr<SceneRaycasterItem>>* raycasters = get_raycasters(type);
    auto it = raycasters->begin();
    while (it != raycasters->end()) {
        if ((*it)->get_id() == encode_id(type, id))
            it = raycasters->erase(it);
        else
            ++it;
    }
}

void SceneRaycaster::remove_raycasters(EType type)
{
    switch (type) {
    case EType::Bed:    { m_bed.clear(); break; }
    case EType::Volume: { m_volumes.clear(); break; }
    case EType::Gizmo:  { m_gizmos.clear(); break; }
    case EType::FallbackGizmo:  { m_fallback_gizmos.clear(); break; }
    default:            { break; }
    };
}

void SceneRaycaster::remove_raycaster(std::shared_ptr<SceneRaycasterItem> item)
{
    for (auto it = m_bed.begin(); it != m_bed.end(); ++it) {
        if (*it == item) {
            m_bed.erase(it);
            return;
        }
    }
    for (auto it = m_volumes.begin(); it != m_volumes.end(); ++it) {
        if (*it == item) {
            m_volumes.erase(it);
            return;
        }
    }
    for (auto it = m_gizmos.begin(); it != m_gizmos.end(); ++it) {
        if (*it == item) {
            m_gizmos.erase(it);
            return;
        }
    }
    for (auto it = m_fallback_gizmos.begin(); it != m_fallback_gizmos.end(); ++it) {
        if (*it == item) {
            m_fallback_gizmos.erase(it);
            return;
        }
    }
}

SceneRaycaster::HitResult SceneRaycaster::hit(const Vec2d& mouse_pos, const Camera& camera, const ClippingPlane* clipping_plane) const
{
    // helper class used to return currently selected volume as hit when overlapping with other volumes
    // to allow the user to click and drag on a selected volume
    class VolumeKeeper
    {
        std::optional<unsigned int> m_selected_volume_id;
        Vec3f m_closest_hit_pos{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
        bool m_selected_volume_already_found{ false };

    public:
        VolumeKeeper() {
            const Selection& selection = wxGetApp().plater()->get_selection();
            if (selection.is_single_volume() || selection.is_single_modifier()) {
                const GLVolume* volume = selection.get_first_volume();
                if (!volume->is_wipe_tower && !volume->is_sla_pad() && !volume->is_sla_support())
                    m_selected_volume_id = *selection.get_volume_idxs().begin();
            }
        }

        bool is_active() const { return m_selected_volume_id.has_value(); }
        const Vec3f& get_closest_hit_pos() const { return m_closest_hit_pos; }
        bool check_hit_result(const HitResult& hit) {
            assert(is_active());

            if (m_selected_volume_already_found && hit.type == SceneRaycaster::EType::Volume && hit.position.isApprox(m_closest_hit_pos))
                return false;

            if (hit.type == SceneRaycaster::EType::Volume)
                m_selected_volume_already_found = *m_selected_volume_id == (unsigned int)decode_id(hit.type, hit.raycaster_id);

            m_closest_hit_pos = hit.position;
            return true;
        }
    };

    VolumeKeeper volume_keeper;

    double closest_hit_squared_distance = std::numeric_limits<double>::max();
    auto is_closest = [&closest_hit_squared_distance, &volume_keeper](const Camera& camera, const Vec3f& hit) {
        const double hit_squared_distance = (camera.get_position() - hit.cast<double>()).squaredNorm();
        bool ret = hit_squared_distance < closest_hit_squared_distance;
        if (volume_keeper.is_active())
            ret |= hit.isApprox(volume_keeper.get_closest_hit_pos());
        if (ret)
            closest_hit_squared_distance = hit_squared_distance;
        return ret;
    };

#if ENABLE_RAYCAST_PICKING_DEBUG
    const_cast<std::optional<HitResult>*>(&m_last_hit)->reset();
#endif // ENABLE_RAYCAST_PICKING_DEBUG

    HitResult ret;

    auto test_raycasters = [this, is_closest, clipping_plane, &volume_keeper](EType type, const Vec2d& mouse_pos, const Camera& camera, HitResult& ret) {
        const ClippingPlane* clip_plane = (clipping_plane != nullptr && type == EType::Volume) ? clipping_plane : nullptr;
        const std::vector<std::shared_ptr<SceneRaycasterItem>>* raycasters = get_raycasters(type);
        const Vec3f camera_forward = camera.get_dir_forward().cast<float>();
        HitResult current_hit = { type };
        for (std::shared_ptr<SceneRaycasterItem> item : *raycasters) {
            if (!item->is_active())
                continue;

            current_hit.raycaster_id = item->get_id();
            const Transform3d& trafo = item->get_transform();
            if (item->get_raycaster()->closest_hit(mouse_pos, trafo, camera, current_hit.position, current_hit.normal, clip_plane)) {
                current_hit.position = (trafo * current_hit.position.cast<double>()).cast<float>();
                current_hit.normal = (trafo.matrix().block(0, 0, 3, 3).inverse().transpose() * current_hit.normal.cast<double>()).normalized().cast<float>();
                if (item->use_back_faces() || current_hit.normal.dot(camera_forward) < 0.0f) {
                    if (is_closest(camera, current_hit.position)) {
                        if (volume_keeper.is_active()) {
                            if (volume_keeper.check_hit_result(current_hit))
                                ret = current_hit;
                        }
                        else
                            ret = current_hit;
                    }
                }
            }
        }
    };

    if (!m_gizmos.empty())
        test_raycasters(EType::Gizmo, mouse_pos, camera, ret);

    if (!m_fallback_gizmos.empty() && !ret.is_valid())
        test_raycasters(EType::FallbackGizmo, mouse_pos, camera, ret);

    if (!m_gizmos_on_top || !ret.is_valid()) {
        if (camera.is_looking_downward() && !m_bed.empty())
            test_raycasters(EType::Bed, mouse_pos, camera, ret);
        if (!m_volumes.empty())
            test_raycasters(EType::Volume, mouse_pos, camera, ret);
    }

    if (ret.is_valid())
        ret.raycaster_id = decode_id(ret.type, ret.raycaster_id);

#if ENABLE_RAYCAST_PICKING_DEBUG
    *const_cast<std::optional<HitResult>*>(&m_last_hit) = ret;
#endif // ENABLE_RAYCAST_PICKING_DEBUG
    return ret;
}

#if ENABLE_RAYCAST_PICKING_DEBUG
void SceneRaycaster::render_hit(const Camera& camera)
{
    if (!m_last_hit.has_value() || !(*m_last_hit).is_valid())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    shader->start_using();

    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    const Transform3d sphere_view_model_matrix = camera.get_view_matrix() * Geometry::translation_transform((*m_last_hit).position.cast<double>()) *
        Geometry::scale_transform(4.0 * camera.get_inv_zoom());
    shader->set_uniform("view_model_matrix", sphere_view_model_matrix);
    m_sphere.render();

    Eigen::Quaterniond q;
    Transform3d m = Transform3d::Identity();
    m.matrix().block(0, 0, 3, 3) = q.setFromTwoVectors(Vec3d::UnitZ(), (*m_last_hit).normal.cast<double>()).toRotationMatrix();

    const Transform3d line_view_model_matrix = sphere_view_model_matrix * m * Geometry::scale_transform(10.0);
    shader->set_uniform("view_model_matrix", line_view_model_matrix);
    m_line.render();

    shader->stop_using();
}

size_t SceneRaycaster::active_beds_count() const {
    size_t count = 0;
    for (const auto& b : m_bed) {
        if (b->is_active())
            ++count;
    }
    return count;
}
size_t SceneRaycaster::active_volumes_count() const {
    size_t count = 0;
    for (const auto& v : m_volumes) {
        if (v->is_active())
            ++count;
    }
    return count;
}
size_t SceneRaycaster::active_gizmos_count() const {
    size_t count = 0;
    for (const auto& g : m_gizmos) {
        if (g->is_active())
            ++count;
    }
    return count;
}
size_t SceneRaycaster::active_fallback_gizmos_count() const {
    size_t count = 0;
    for (const auto& g : m_fallback_gizmos) {
        if (g->is_active())
            ++count;
    }
    return count;
}
#endif // ENABLE_RAYCAST_PICKING_DEBUG

std::vector<std::shared_ptr<SceneRaycasterItem>>* SceneRaycaster::get_raycasters(EType type)
{
    std::vector<std::shared_ptr<SceneRaycasterItem>>* ret = nullptr;
    switch (type)
    {
    case EType::Bed:    { ret = &m_bed; break; }
    case EType::Volume: { ret = &m_volumes; break; }
    case EType::Gizmo:  { ret = &m_gizmos; break; }
    case EType::FallbackGizmo:  { ret = &m_fallback_gizmos; break; }
    default:            { break; }
    }
    assert(ret != nullptr);
    return ret;
}

const std::vector<std::shared_ptr<SceneRaycasterItem>>* SceneRaycaster::get_raycasters(EType type) const
{
    const std::vector<std::shared_ptr<SceneRaycasterItem>>* ret = nullptr;
    switch (type)
    {
    case EType::Bed:    { ret = &m_bed; break; }
    case EType::Volume: { ret = &m_volumes; break; }
    case EType::Gizmo:  { ret = &m_gizmos; break; }
    case EType::FallbackGizmo:  { ret = &m_fallback_gizmos; break; }
    default:            { break; }
    }
    assert(ret != nullptr);
    return ret;
}

int SceneRaycaster::base_id(EType type)
{
    switch (type)
    {
    case EType::Bed:    { return int(EIdBase::Bed); }
    case EType::Volume: { return int(EIdBase::Volume); }
    case EType::Gizmo:  { return int(EIdBase::Gizmo); }
    case EType::FallbackGizmo:  { return int(EIdBase::FallbackGizmo); }
    default:            { break; }
    };

    assert(false);
    return -1;
}

int SceneRaycaster::encode_id(EType type, int id) { return base_id(type) + id; }
int SceneRaycaster::decode_id(EType type, int id) { return id - base_id(type); }

} // namespace GUI
} // namespace Slic3r
