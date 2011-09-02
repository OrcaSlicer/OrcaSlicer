#include "RaycastManager.hpp"
#include <utility>

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"

using namespace Slic3r::GUI;

namespace{
using namespace Slic3r;
void actualize(RaycastManager::Meshes &meshes, const ModelVolumePtrs &volumes, const RaycastManager::ISkip *skip, RaycastManager::Meshes *input = nullptr);
const AABBMesh * get_mesh(const RaycastManager::Meshes &meshes, size_t volume_id);
RaycastManager::TrKey create_key(const ModelVolume& volume, const ModelInstance& instance){ 
    return std::make_pair(instance.id().id, volume.id().id); }
RaycastManager::TrItems::iterator find(RaycastManager::TrItems &items, const RaycastManager::TrKey &key);
RaycastManager::TrItems::const_iterator find(const RaycastManager::TrItems &items, const RaycastManager::TrKey &key);
bool is_lower_key(const RaycastManager::TrKey &k1, const RaycastManager::TrKey &k2) {
    return k1.first < k2.first || (k1.first == k2.first && k1.second < k2.second); }
bool is_lower(const RaycastManager::TrItem &i1, const RaycastManager::TrItem &i2) {
    return is_lower_key(i1.first, i2.first); };
template<typename VecType> inline void erase(std::vector<VecType> &vec, const std::vector<bool> &flags);
}

void RaycastManager::actualize(const ModelObject &object, const ISkip *skip, Meshes *meshes)
{
    // actualize MeshRaycaster
    ::actualize(m_meshes, object.volumes, skip, meshes);

    // check if inscance was removed
    std::vector<bool> removed_transf(m_transformations.size(), {true});

    bool need_sort = false;
    // actualize transformation matrices
    for (const ModelVolume *volume : object.volumes) {
        if (skip != nullptr && skip->skip(volume->id().id)) continue;
        const Transform3d &volume_tr = volume->get_matrix();
        for (const ModelInstance *instance : object.instances) {
            const Transform3d &instrance_tr = instance->get_matrix();
            Transform3d transformation = instrance_tr * volume_tr;
            TrKey key = ::create_key(*volume, *instance);
            auto item = ::find(m_transformations, key);
            if (item != m_transformations.end()) {
                // actualize transformation all the time
                item->second = transformation;
                size_t index = item - m_transformations.begin();
                removed_transf[index] = false;
            } else {
                // add new transformation
                m_transformations.emplace_back(key, transformation);
                need_sort = true;
            }
        }
    }

    // clean other transformation
    ::erase(m_transformations, removed_transf);

    if (need_sort)
        std::sort(m_transformations.begin(), m_transformations.end(), ::is_lower);
}

void RaycastManager::actualize(const ModelInstance &instance, const ISkip *skip, Meshes *meshes)
{
    const ModelVolumePtrs &volumes = instance.get_object()->volumes;

    // actualize MeshRaycaster
    ::actualize(m_meshes, volumes, skip, meshes);

    // check if inscance was removed
    std::vector<bool> removed_transf(m_transformations.size(), {true});

    bool need_sort = false;
    // actualize transformation matrices
    for (const ModelVolume *volume : volumes) {
        if (skip != nullptr && skip->skip(volume->id().id))
            continue;
        const Transform3d &volume_tr = volume->get_matrix();
        const Transform3d &instrance_tr   = instance.get_matrix();
        Transform3d        transformation = instrance_tr * volume_tr;
        TrKey key = ::create_key(*volume, instance);
        auto item = ::find(m_transformations, key);
        if (item != m_transformations.end()) {
            // actualize transformation all the time
            item->second          = transformation;
            size_t index          = item - m_transformations.begin();
            removed_transf[index] = false;
        } else {
            // add new transformation
            m_transformations.emplace_back(key, transformation);
            need_sort = true;
        }        
    }

    // clean other transformation
    ::erase(m_transformations, removed_transf);

    if (need_sort)
        std::sort(m_transformations.begin(), m_transformations.end(), ::is_lower);
}
 
std::optional<RaycastManager::Hit> RaycastManager::first_hit(const Vec3d& point, const Vec3d& direction, const ISkip *skip) const
{
    // Improve: it is not neccessaru to use AABBMesh and calc normal for every hit
    
    // Results
    const AABBMesh *hit_mesh = nullptr;
    double hit_squared_distance = 0.;
    int hit_face = -1;
    Vec3d hit_world;
    const Transform3d *hit_tramsformation = nullptr;
    const TrKey *hit_key = nullptr;

    for (const auto &[key, transformation]: m_transformations) {
        size_t volume_id = key.second;
        if (skip != nullptr && skip->skip(volume_id)) continue;
        const AABBMesh *mesh = ::get_mesh(m_meshes, volume_id);
        if (mesh == nullptr) continue;
        Transform3d inv = transformation.inverse();

        // transform input into mesh world
        Vec3d point_    = inv * point;
        Vec3d direction_= inv.linear() * direction;

        std::vector<AABBMesh::hit_result> hits = mesh->query_ray_hits(point_, direction_);
        if (hits.empty()) continue; // no intersection found

        const AABBMesh::hit_result &hit = hits.front();

        // convert to world
        Vec3d world = transformation * hit.position();
        double squared_distance = (point - world).squaredNorm();
        if (hit_mesh != nullptr &&
            hit_squared_distance < squared_distance)
            continue; // exist closer one

        hit_mesh = mesh;
        hit_squared_distance = squared_distance;
        hit_face = hit.face();
        hit_world = world;
        hit_tramsformation = &transformation;
        hit_key = &key;
    }

    if (hit_mesh == nullptr)
        return {};

    // Calculate normal from transformed triangle
    // NOTE: Anisotropic transformation of normal is not perpendiculat to triangle
    const Vec3i32 tri = hit_mesh->indices(hit_face);
    std::array<Vec3d,3> pts;
    auto tr = hit_tramsformation->linear().eval();
    for (int i = 0; i < 3; ++i)
        pts[i] = tr * hit_mesh->vertices(tri[i]).cast<double>();
    Vec3d normal_world = (pts[1] - pts[0]).cross(pts[2] - pts[1]);
    if (has_reflection(*hit_tramsformation))
        normal_world *= -1;
    normal_world.normalize();

    SurfacePoint<double> point_world{hit_world, normal_world};
    return RaycastManager::Hit{point_world, *hit_key, hit_squared_distance};
}

std::optional<RaycastManager::Hit> RaycastManager::closest_hit(const Vec3d &point, const Vec3d &direction, const ISkip *skip) const
{
    std::optional<Hit> closest;
    for (const auto &[key, transformation] : m_transformations) {
        size_t volume_id = key.second;
        if (skip != nullptr && skip->skip(volume_id)) continue;
        const AABBMesh *mesh = ::get_mesh(m_meshes, volume_id);
        if (mesh == nullptr) continue;
        Transform3d tr_inv = transformation.inverse();
        Vec3d mesh_point = tr_inv * point;
        Vec3d mesh_direction = tr_inv.linear() * direction;

        // Need for detect that actual point position is on correct place
        Vec3d point_positive = mesh_point - mesh_direction;
        Vec3d point_negative = mesh_point + mesh_direction;

        // Throw ray to both directions of ray
        std::vector<AABBMesh::hit_result> hits = mesh->query_ray_hits(point_positive, mesh_direction);
        std::vector<AABBMesh::hit_result> hits_neg = mesh->query_ray_hits(point_negative, -mesh_direction);
        hits.insert(hits.end(), std::make_move_iterator(hits_neg.begin()), std::make_move_iterator(hits_neg.end()));
        for (const AABBMesh::hit_result &hit : hits) { 
            Vec3d diff = mesh_point - hit.position();
            double squared_distance = diff.squaredNorm();
            if (closest.has_value() &&
                closest->squared_distance < squared_distance)
                continue;
            closest = Hit{{hit.position(), hit.normal()}, key, squared_distance};
        }
    }
    return closest;
}

std::optional<RaycastManager::ClosePoint> RaycastManager::closest(const Vec3d &point, const ISkip *skip) const
{
    std::optional<ClosePoint> closest;
    for (const auto &[key, transformation] : m_transformations) {
        size_t       volume_id = key.second;
        if (skip != nullptr && skip->skip(volume_id))
            continue;
        const AABBMesh *mesh = ::get_mesh(m_meshes, volume_id);
        if (mesh == nullptr) continue;
        Transform3d tr_inv = transformation.inverse();
        Vec3d mesh_point = tr_inv * point;
                
        int   face_idx = 0;
        Vec3d closest_point;
        Vec3d pointd = point.cast<double>();
        mesh->squared_distance(pointd, face_idx, closest_point);

        double squared_distance = (mesh_point - closest_point).squaredNorm();
        if (closest.has_value() && closest->squared_distance < squared_distance)
            continue;

        closest = ClosePoint{key, closest_point, squared_distance};
    }
    return closest;
}

Slic3r::Transform3d RaycastManager::get_transformation(const TrKey &tr_key) const {
    auto tr = ::find(m_transformations, tr_key);
    if (tr == m_transformations.end())
        return Transform3d::Identity();
    return tr->second;
}


namespace {
void actualize(RaycastManager::Meshes &meshes, const ModelVolumePtrs &volumes, const RaycastManager::ISkip *skip, RaycastManager::Meshes* inputs)
{
    // check if volume was removed
    std::vector<bool> removed_meshes(meshes.size(), {true});
    bool need_sort = false;
    // actualize MeshRaycaster
    for (const ModelVolume *volume : volumes) {
        size_t oid = volume->id().id;
        if (skip != nullptr && skip->skip(oid))
            continue;
        auto is_oid = [oid](const RaycastManager::Mesh &it) { return oid == it.first; };        
        if (auto item = std::find_if(meshes.begin(), meshes.end(), is_oid);
            item != meshes.end()) {
            size_t index = item - meshes.begin();
            removed_meshes[index] = false;
            continue;
        }

        // exist AABB in inputs ?
        if (inputs != nullptr) {
            auto input = std::find_if(inputs->begin(), inputs->end(), is_oid);
            if (input != inputs->end()) {
                meshes.emplace_back(std::move(*input));
                need_sort = true;    
                continue;
            }
        }

        // add new raycaster
        bool calculate_epsilon = true;
        auto mesh = std::make_unique<AABBMesh>(volume->mesh(), calculate_epsilon);
        meshes.emplace_back(std::make_pair(oid, std::move(mesh)));
        need_sort = true;        
    }

    // clean other raycasters
    erase(meshes, removed_meshes);

    // All the time meshes must be sorted by volume id - for faster search
    if (need_sort) {
        auto is_lower = [](const RaycastManager::Mesh &m1, const RaycastManager::Mesh &m2) { return m1.first < m2.first; };
        std::sort(meshes.begin(), meshes.end(), is_lower);
    }
}

const Slic3r::AABBMesh *get_mesh(const RaycastManager::Meshes &meshes, size_t volume_id)
{
    auto is_lower_index = [](const RaycastManager::Mesh &m, size_t i) { return m.first < i; };
    auto it = std::lower_bound(meshes.begin(), meshes.end(), volume_id, is_lower_index);
    if (it == meshes.end() || it->first != volume_id)
        return nullptr;
    return &(*(it->second));
}

RaycastManager::TrItems::iterator find(RaycastManager::TrItems &items, const RaycastManager::TrKey &key) {
    auto fnc = [](const RaycastManager::TrItem &it, const RaycastManager::TrKey &l_key) { return is_lower_key(it.first, l_key); };
    auto it = std::lower_bound(items.begin(), items.end(), key, fnc);
    if (it != items.end() && it->first != key)
        return items.end();
    return it;
}

RaycastManager::TrItems::const_iterator find(const RaycastManager::TrItems &items, const RaycastManager::TrKey &key)
{
    auto fnc = [](const RaycastManager::TrItem &it, const RaycastManager::TrKey &l_key) { return is_lower_key(it.first, l_key); };
    auto it  = std::lower_bound(items.begin(), items.end(), key, fnc);
    if (it != items.end() && it->first != key)
        return items.end();
    return it;
}

template<typename VecType> inline void erase(std::vector<VecType> &vec, const std::vector<bool> &flags)
{
    if (vec.size() < flags.size() || flags.empty())
        return;

    // reverse iteration over flags to erase indices from back to front.
    for (int i = static_cast<int>(flags.size()) - 1; i >= 0; --i)
        if (flags[i])
            vec.erase(vec.begin() + i);
}

} // namespace

namespace Slic3r::GUI{

RaycastManager::Meshes create_meshes(GLCanvas3D &canvas, const RaycastManager::AllowVolumes &condition)
{
    SceneRaycaster::EType type = SceneRaycaster::EType::Volume;
    auto scene_casters = canvas.get_raycasters_for_picking(type);
    if (scene_casters == nullptr)
        return {};
    const std::vector<std::shared_ptr<SceneRaycasterItem>> &casters = *scene_casters;

    const GLVolumePtrs    &gl_volumes = canvas.get_volumes().volumes;
    const ModelObjectPtrs &objects    = canvas.get_model()->objects;

    RaycastManager::Meshes meshes;
    for (const std::shared_ptr<SceneRaycasterItem> &caster : casters) {
        int index = SceneRaycaster::decode_id(type, caster->get_id());
        if (index < 0)
            continue;
        auto index_ = static_cast<size_t>(index);
        if(index_ >= gl_volumes.size())
            continue;
        const GLVolume *gl_volume = gl_volumes[index_];
        if (gl_volume == nullptr)
            continue;
        const ModelVolume *volume = get_model_volume(*gl_volume, objects);
        if (volume == nullptr)
            continue;
        size_t id = volume->id().id;
        if (condition.skip(id))
            continue;
        auto mesh = std::make_unique<AABBMesh>(caster->get_raycaster()->get_aabb_mesh());
        meshes.emplace_back(std::make_pair(id, std::move(mesh)));
    }
    return meshes;
}


std::optional<RaycastManager::Hit> ray_from_camera(const RaycastManager        &raycaster,
                                                   const Vec2d                 &mouse_pos,
                                                   const Camera                &camera,
                                                   const RaycastManager::ISkip *skip)
{
    Vec3d point;
    Vec3d direction;
    CameraUtils::ray_from_screen_pos(camera, mouse_pos, point, direction);
    return raycaster.first_hit(point, direction, skip);
}

RaycastManager::AllowVolumes create_condition(const ModelVolumePtrs &volumes, const ObjectID &disallowed_volume_id) {
    std::vector<size_t> allowed_volumes_id;
    if (volumes.size() > 1) {
        allowed_volumes_id.reserve(volumes.size() - 1);
        for (const ModelVolume *v : volumes) {
            // drag only above part not modifiers or negative surface
            if (!v->is_model_part())
                continue;

            // skip actual selected object
            if (v->id() == disallowed_volume_id)
                continue;

            allowed_volumes_id.emplace_back(v->id().id);
        }
    }
    return RaycastManager::AllowVolumes(allowed_volumes_id);
}

} // namespace Slic3r::GUI
