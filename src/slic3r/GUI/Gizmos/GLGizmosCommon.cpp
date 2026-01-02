#include "GLGizmosCommon.hpp"

#include <cassert>

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/PresetBundle.hpp"

#include <GL/glew.h>

namespace Slic3r {
namespace GUI {

using namespace CommonGizmosDataObjects;

CommonGizmosDataPool::CommonGizmosDataPool(GLCanvas3D* canvas)
    : m_canvas(canvas)
{
    using c = CommonGizmosDataID;
    m_data[c::SelectionInfo].reset(   new SelectionInfo(this));
    m_data[c::InstancesHider].reset(  new InstancesHider(this));
//    m_data[c::HollowedMesh].reset(    new HollowedMesh(this));
    m_data[c::Raycaster].reset(       new Raycaster(this));
    m_data[c::ObjectClipper].reset(   new ObjectClipper(this));
    // m_data[c::SupportsClipper].reset( new SupportsClipper(this));

}

void CommonGizmosDataPool::update(CommonGizmosDataID required)
{
    assert(check_dependencies(required));
    for (auto& [id, data] : m_data) {
        if (int(required) & int(CommonGizmosDataID(id)))
            data->update();
        else
            if (data->is_valid())
                data->release();

    }
}


SelectionInfo* CommonGizmosDataPool::selection_info() const
{
    SelectionInfo* sel_info = dynamic_cast<SelectionInfo*>(m_data.at(CommonGizmosDataID::SelectionInfo).get());
    assert(sel_info);
    return sel_info->is_valid() ? sel_info : nullptr;
}


InstancesHider* CommonGizmosDataPool::instances_hider() const
{
    InstancesHider* inst_hider = dynamic_cast<InstancesHider*>(m_data.at(CommonGizmosDataID::InstancesHider).get());
    assert(inst_hider);
    return inst_hider->is_valid() ? inst_hider : nullptr;
}

CommonGizmosDataObjects::Raycaster *CommonGizmosDataPool::raycaster_ptr() {
    return dynamic_cast<Raycaster *>(m_data.at(CommonGizmosDataID::Raycaster).get());
}

Raycaster* CommonGizmosDataPool::raycaster() const
{
    Raycaster* rc = dynamic_cast<Raycaster*>(m_data.at(CommonGizmosDataID::Raycaster).get());
    assert(rc);
    return rc->is_valid() ? rc : nullptr;
}

ObjectClipper* CommonGizmosDataPool::object_clipper() const
{
    ObjectClipper* oc = dynamic_cast<ObjectClipper*>(m_data.at(CommonGizmosDataID::ObjectClipper).get());
    // ObjectClipper is used from outside the gizmos to report current clipping plane.
    // This function can be called when oc is nullptr.
    return (oc && oc->is_valid()) ? oc : nullptr;
}

#ifndef NDEBUG
// Check the required resources one by one and return true if all
// dependencies are met.
bool CommonGizmosDataPool::check_dependencies(CommonGizmosDataID required) const
{
    // This should iterate over currently required data. Each of them should
    // be asked about its dependencies and it must check that all dependencies
    // are also in required and before the current one.
    for (auto& [id, data] : m_data) {
        // in case we don't use this, the deps are irrelevant
        if (! (int(required) & int(CommonGizmosDataID(id))))
            continue;


        CommonGizmosDataID deps = data->get_dependencies();
        assert(int(deps) == (int(deps) & int(required)));
    }


    return true;
}
#endif // NDEBUG




void SelectionInfo::on_update()
{
    const Selection& selection = get_pool()->get_canvas()->get_selection();

    m_model_object = nullptr;

    // BBS still keep object pointer when selection is volume
    // if (selection.is_single_full_instance()) {
    if (!selection.is_empty()) {
        m_model_object = selection.get_model()->objects[selection.get_object_idx()];
        m_z_shift = selection.get_first_volume()->get_sla_shift_z();
    }
}

void SelectionInfo::on_release()
{
    m_model_object = nullptr;
}

int SelectionInfo::get_active_instance() const
{
    return get_pool()->get_canvas()->get_selection().get_instance_idx();
}





void InstancesHider::on_update()
{
    const ModelObject* mo = get_pool()->selection_info()->model_object();
    int active_inst = get_pool()->selection_info()->get_active_instance();
    GLCanvas3D* canvas = get_pool()->get_canvas();
    double z_min;
    if (canvas->get_canvas_type() == GLCanvas3D::CanvasAssembleView)
        z_min = std::numeric_limits<double>::max();
    else
        z_min = -SINKING_Z_THRESHOLD;

    if (mo && active_inst != -1) {
        canvas->toggle_model_objects_visibility(false);
        canvas->toggle_model_objects_visibility(true, mo, active_inst);
        canvas->toggle_sla_auxiliaries_visibility(false, mo, active_inst);
        canvas->set_use_clipping_planes(true);
        // Some objects may be sinking, do not show whatever is below the bed.
        canvas->set_clipping_plane(0, ClippingPlane(Vec3d::UnitZ(), z_min));
        canvas->set_clipping_plane(1, ClippingPlane(-Vec3d::UnitZ(), std::numeric_limits<double>::max()));


        std::vector<const TriangleMesh*> meshes;
        for (const ModelVolume* mv : mo->volumes)
            meshes.push_back(&mv->mesh());

        if (meshes != m_old_meshes) {
            m_clippers.clear();
            for (const TriangleMesh* mesh : meshes) {
                m_clippers.emplace_back(new MeshClipper);
                m_clippers.back()->set_plane(ClippingPlane(-Vec3d::UnitZ(), z_min));
                m_clippers.back()->set_mesh(mesh->its);
            }
            m_old_meshes = meshes;
        }
    }
    else
        canvas->toggle_model_objects_visibility(true);
}

void InstancesHider::on_release()
{
    get_pool()->get_canvas()->toggle_model_objects_visibility(true);
    get_pool()->get_canvas()->set_use_clipping_planes(false);
    m_old_meshes.clear();
    m_clippers.clear();
}

void InstancesHider::render_cut() const
{
    const SelectionInfo* sel_info = get_pool()->selection_info();
    const ModelObject* mo = sel_info->model_object();
    Geometry::Transformation inst_trafo = mo->instances[sel_info->get_active_instance()]->get_transformation();

    size_t clipper_id = 0;
    for (const ModelVolume* mv : mo->volumes) {
        Geometry::Transformation vol_trafo  = mv->get_transformation();
        Geometry::Transformation trafo = inst_trafo * vol_trafo;
        trafo.set_offset(trafo.get_offset() + Vec3d(0., 0., sel_info->get_sla_shift()));

        auto& clipper = m_clippers[clipper_id];
        clipper->set_transformation(trafo);
        const ObjectClipper* obj_clipper = get_pool()->object_clipper();
        if (obj_clipper->is_valid() && obj_clipper->get_clipping_plane()
         && obj_clipper->get_position() != 0.) {
            ClippingPlane clp = *get_pool()->object_clipper()->get_clipping_plane();
            clp.set_normal(-clp.get_normal());
            clipper->set_limiting_plane(clp);
        }
        else
            clipper->set_limiting_plane(ClippingPlane::ClipsNothing());

        glsafe(::glPushAttrib(GL_DEPTH_TEST));
        glsafe(::glDisable(GL_DEPTH_TEST));
        clipper->render_cut(mv->is_model_part() ? ColorRGBA(0.8f, 0.3f, 0.0f, 1.0f) : color_from_model_volume(*mv));
        glsafe(::glPopAttrib());

        ++clipper_id;
    }
}


void Raycaster::on_update()
{
    wxBusyCursor wait;
    const ModelObject* mo = get_pool()->selection_info()->model_object();

    if (mo == nullptr)
        return;

    std::vector<const TriangleMesh*> meshes;
    const std::vector<ModelVolume*>& mvs = mo->volumes;
    for (const ModelVolume* mv : mvs) {
        if (m_only_support_model_part) {
            if (mv->is_model_part()) {
                meshes.push_back(&mv->mesh());
            }
        } else {
            meshes.push_back(&mv->mesh());
        }
    }

    if (meshes != m_old_meshes) {
        m_raycasters.clear();
        for (const TriangleMesh* mesh : meshes)
            m_raycasters.emplace_back(new MeshRaycaster(std::make_shared<const TriangleMesh>(*mesh)));
        m_old_meshes = meshes;
    }
}

void Raycaster::on_release()
{
    m_raycasters.clear();
    m_old_meshes.clear();
}

std::vector<const MeshRaycaster*> Raycaster::raycasters() const
{
    std::vector<const MeshRaycaster*> mrcs;
    for (const auto& raycaster_unique_ptr : m_raycasters)
        mrcs.push_back(raycaster_unique_ptr.get());
    return mrcs;
}

void CommonGizmosDataObjects::Raycaster::set_only_support_model_part_flag(bool flag) {
    m_only_support_model_part = flag;
}

void ObjectClipper::on_update()
{
    const ModelObject* mo = get_pool()->selection_info()->model_object();
    if (! mo)
        return;

    // which mesh should be cut?
    std::vector<const TriangleMesh*> meshes;
    std::vector<Geometry::Transformation> trafos;
    for (const ModelVolume* mv : mo->volumes) {
        meshes.emplace_back(&mv->mesh());
        trafos.emplace_back(mv->get_transformation());
    }

    if (meshes != m_old_meshes) {
        m_clippers.clear();
        for (size_t i = 0; i < meshes.size(); ++i) {
            m_clippers.emplace_back(new MeshClipper, trafos[i]);
            m_clippers.back().first->set_mesh(meshes[i]->its);
        }
        m_old_meshes = std::move(meshes);

        m_active_inst_bb_radius =
            mo->instance_bounding_box(get_pool()->selection_info()->get_active_instance()).radius();
    }
}


void ObjectClipper::on_release()
{
    m_clippers.clear();
    m_old_meshes.clear();
    m_clp.reset();
    m_clp_ratio = 0.;

}

void ObjectClipper::render_cut(const std::vector<size_t>* ignore_idxs) const
{
    if (m_clp_ratio == 0.)
        return;
    const SelectionInfo* sel_info = get_pool()->selection_info();
    const Geometry::Transformation inst_trafo = sel_info->model_object()->instances[sel_info->get_active_instance()]->get_transformation();
    
    std::vector<size_t> ignore_idxs_local = ignore_idxs ? *ignore_idxs : std::vector<size_t>();

    for (auto& clipper : m_clippers) {
        Geometry::Transformation trafo = inst_trafo * clipper.second;
        trafo.set_offset(trafo.get_offset() + Vec3d(0., 0., sel_info->get_sla_shift()));
        clipper.first->set_plane(*m_clp);
        clipper.first->set_transformation(trafo);
        clipper.first->set_limiting_plane(ClippingPlane(Vec3d::UnitZ(), -SINKING_Z_THRESHOLD));
		// BBS      
        clipper.first->render_cut({ 0.25f, 0.25f, 0.25f, 1.0f }, &ignore_idxs_local); // SECTION FILL COLOR
        clipper.first->render_contour({ 1.f, 1.f, 1.f, 1.f },  &ignore_idxs_local);
  
        // Now update the ignore idxs. Find the first element belonging to the next clipper,
        // and remove everything before it and decrement everything by current number of contours.
        const int num_of_contours = clipper.first->get_number_of_contours();
        ignore_idxs_local.erase(ignore_idxs_local.begin(), std::find_if(ignore_idxs_local.begin(), ignore_idxs_local.end(), [num_of_contours](size_t idx) { return idx >= size_t(num_of_contours); } ));
        for (size_t& idx : ignore_idxs_local)
            idx -= num_of_contours;
    }
}

void ObjectClipper::set_position_to_init_layer()
{
    m_clp.reset(new ClippingPlane({0, 0, 1}, 0.1));
    get_pool()->get_canvas()->set_as_dirty();
}

int ObjectClipper::get_number_of_contours() const
{
    int sum = 0;
    for (const auto& [clipper, trafo] : m_clippers)
        sum += clipper->get_number_of_contours();
    return sum;
}

int ObjectClipper::is_projection_inside_cut(const Vec3d& point) const
{
    if (m_clp_ratio == 0.)
        return -1;
    int idx_offset = 0;
    for (const auto& [clipper, trafo] : m_clippers) {
        if (int idx = clipper->is_projection_inside_cut(point); idx != -1)
            return idx_offset + idx;
        idx_offset += clipper->get_number_of_contours();
    }
    return -1;
}

bool ObjectClipper::has_valid_contour() const
{
    return m_clp_ratio != 0. && std::any_of(m_clippers.begin(), m_clippers.end(), [](const auto& cl) { return cl.first->has_valid_contour(); });
}

std::vector<Vec3d> ObjectClipper::point_per_contour() const
{
    std::vector<Vec3d> pts;

    for (const auto& clipper : m_clippers) {
        const std::vector<Vec3d> pts_clipper = clipper.first->point_per_contour();
        pts.insert(pts.end(), pts_clipper.begin(), pts_clipper.end());;
    }
    return pts;
}


void ObjectClipper::set_position_by_ratio(double pos, bool keep_normal, bool vertical_normal)
{
    const ModelObject* mo = get_pool()->selection_info()->model_object();
    int active_inst = get_pool()->selection_info()->get_active_instance();
    double z_shift = get_pool()->selection_info()->get_sla_shift();

    Vec3d normal;
    if(vertical_normal) {
        normal = {0, 0, 1};
    }else {
        //Vec3d camera_dir = wxGetApp().plater()->get_camera().get_dir_forward();
        //if (abs(camera_dir(0)) > EPSILON || abs(camera_dir(1)) > EPSILON)
        //    camera_dir(2) = 0;

        normal = (keep_normal && m_clp) ? m_clp->get_normal() : /*-camera_dir;*/ -wxGetApp().plater()->get_camera().get_dir_forward();
    }
    Vec3d center;

    if (get_pool()->get_canvas()->get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
        const SelectionInfo* sel_info = get_pool()->selection_info();
        auto trafo = mo->instances[sel_info->get_active_instance()]->get_assemble_transformation();
        auto offset_to_assembly = mo->instances[0]->get_offset_to_assembly();
        center = trafo.get_offset() + offset_to_assembly * (GLVolume::explosion_ratio - 1.0);
    }
    else {
        center = mo->instances[active_inst]->get_offset() + Vec3d(0., 0., z_shift);
    }

    float dist = normal.dot(center);

    if (pos < 0.)
        pos = m_clp_ratio;

    m_clp_ratio = pos;

    m_clp.reset(new ClippingPlane(normal, (dist - (-m_active_inst_bb_radius) - m_clp_ratio * 2 * m_active_inst_bb_radius)));

    get_pool()->get_canvas()->set_as_dirty();
}

void ObjectClipper::set_range_and_pos(const Vec3d& cpl_normal, double cpl_offset, double pos)
{
    m_clp.reset(new ClippingPlane(cpl_normal, cpl_offset));
    m_clp_ratio = pos;
    get_pool()->get_canvas()->set_as_dirty();
}

const ClippingPlane* ObjectClipper::get_clipping_plane(bool ignore_hide_clipped) const
{
    static const ClippingPlane no_clip = ClippingPlane::ClipsNothing();
    return (ignore_hide_clipped || m_hide_clipped) ? m_clp.get() : &no_clip;
}

void ObjectClipper::set_behavior(bool hide_clipped, bool fill_cut, double contour_width)
{
    m_hide_clipped = hide_clipped;
    for (auto& clipper : m_clippers)
        clipper.first->set_behaviour(fill_cut, contour_width);
}


using namespace AssembleViewDataObjects;
AssembleViewDataPool::AssembleViewDataPool(GLCanvas3D* canvas)
    : m_canvas(canvas)
{
    using c = AssembleViewDataID;
    m_data[c::ModelObjectsInfo].reset(new ModelObjectsInfo(this));
    m_data[c::ModelObjectsClipper].reset(new ModelObjectsClipper(this));
}

void AssembleViewDataPool::update(AssembleViewDataID required)
{
    assert(check_dependencies(required));
    for (auto& [id, data] : m_data) {
        if (int(required) & int(AssembleViewDataID(id)))
            data->update();
        else
            if (data->is_valid())
                data->release();
    }
}


ModelObjectsInfo* AssembleViewDataPool::model_objects_info() const
{
    ModelObjectsInfo* sel_info = dynamic_cast<ModelObjectsInfo*>(m_data.at(AssembleViewDataID::ModelObjectsInfo).get());
    assert(sel_info);
    return sel_info->is_valid() ? sel_info : nullptr;
}


ModelObjectsClipper* AssembleViewDataPool::model_objects_clipper() const
{
    ModelObjectsClipper* oc = dynamic_cast<ModelObjectsClipper*>(m_data.at(AssembleViewDataID::ModelObjectsClipper).get());
    // ObjectClipper is used from outside the gizmos to report current clipping plane.
    // This function can be called when oc is nullptr.
    return (oc && oc->is_valid()) ? oc : nullptr;
}

#ifndef NDEBUG
// Check the required resources one by one and return true if all
// dependencies are met.
bool AssembleViewDataPool::check_dependencies(AssembleViewDataID required) const
{
    // This should iterate over currently required data. Each of them should
    // be asked about its dependencies and it must check that all dependencies
    // are also in required and before the current one.
    for (auto& [id, data] : m_data) {
        // in case we don't use this, the deps are irrelevant
        if (!(int(required) & int(AssembleViewDataID(id))))
            continue;


        AssembleViewDataID deps = data->get_dependencies();
        assert(int(deps) == (int(deps) & int(required)));
    }


return true;
}
#endif // NDEBUG




void ModelObjectsInfo::on_update()
{
    if (!get_pool()->get_canvas()->get_model()->objects.empty()) {
        m_model_objects = get_pool()->get_canvas()->get_model()->objects;
    }
    else {
        m_model_objects.clear();
    }
}

void ModelObjectsInfo::on_release()
{
    m_model_objects.clear();
}

//int ModelObjectsInfo::get_active_instance() const
//{
//    const Selection& selection = get_pool()->get_canvas()->get_selection();
//    return selection.get_instance_idx();
//}


void ModelObjectsClipper::on_update()
{
    const ModelObjectPtrs model_objects = get_pool()->model_objects_info()->model_objects();
    if (model_objects.empty())
        return;

    // which mesh should be cut?
    std::vector<const TriangleMesh*> meshes;

    if (meshes.empty())
        for (auto mo : model_objects) {
            for (const ModelVolume* mv : mo->volumes)
                meshes.push_back(&mv->mesh());
        }

    if (meshes != m_old_meshes) {
        m_clippers.clear();
        for (const TriangleMesh* mesh : meshes) {
            m_clippers.emplace_back(new MeshClipper);
            m_clippers.back()->set_mesh(mesh->its);
        }
        m_old_meshes = meshes;

        m_active_inst_bb_radius = get_pool()->get_canvas()->volumes_bounding_box().radius();
    }
}


void ModelObjectsClipper::on_release()
{
    m_clippers.clear();
    m_old_meshes.clear();
    m_clp.reset();
    m_clp_ratio = 0.;

}

void ModelObjectsClipper::render_cut() const
{
    if (m_clp_ratio == 0.)
        return;
    const ModelObjectPtrs model_objects = get_pool()->model_objects_info()->model_objects();

    size_t clipper_id = 0;
    for (const ModelObject* mo : model_objects) {
        Geometry::Transformation assemble_objects_trafo = mo->instances[0]->get_assemble_transformation();
        auto offset_to_assembly = mo->instances[0]->get_offset_to_assembly();
        for (const ModelVolume* mv : mo->volumes) {
            Geometry::Transformation vol_trafo = mv->get_transformation();
            Geometry::Transformation trafo = assemble_objects_trafo * vol_trafo;
            trafo.set_offset(trafo.get_offset() + vol_trafo.get_offset() * (GLVolume::explosion_ratio - 1.0) + offset_to_assembly * (GLVolume::explosion_ratio - 1.0));

            auto& clipper = m_clippers[clipper_id];
            clipper->set_plane(*m_clp);
            clipper->set_transformation(trafo);
            // BBS
            clipper->render_cut({0.25f, 0.25f, 0.25f, 1.0f}); // SECTION FILL COLOR

            ++clipper_id;
        }
    }
}


void ModelObjectsClipper::set_position(double pos, bool keep_normal)
{
    Vec3d normal = (keep_normal && m_clp) ? m_clp->get_normal() : -wxGetApp().plater()->get_camera().get_dir_forward();
    const Vec3d& center = get_pool()->get_canvas()->volumes_bounding_box().center();
    float dist = normal.dot(center);

    if (pos < 0.)
        pos = m_clp_ratio;

    m_clp_ratio = pos;
    m_clp.reset(new ClippingPlane(normal, (dist - (-m_active_inst_bb_radius * GLVolume::explosion_ratio) - m_clp_ratio * 2 * m_active_inst_bb_radius * GLVolume::explosion_ratio)));
    get_pool()->get_canvas()->set_as_dirty();

}


} // namespace GUI
} // namespace Slic3r
