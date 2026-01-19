#include "RotoptimizeJob.hpp"

#include "libslic3r/MTUtils.hpp"
#include "libslic3r/SLA/Rotfinder.hpp"
#include "libslic3r/MinAreaBoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SLAPrint.hpp"

#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"

namespace Slic3r { namespace GUI {

void RotoptimizeJob::prepare()
{
    std::string accuracy_str =
        wxGetApp().app_config->get("sla_auto_rotate", "accuracy");

    std::string method_str =
        wxGetApp().app_config->get("sla_auto_rotate", "method_id");

    if (!accuracy_str.empty())
        m_accuracy = std::stof(accuracy_str);

    if (!method_str.empty())
        m_method_id = std::stoi(method_str);

    m_accuracy = std::max(0.f, std::min(m_accuracy, 1.f));
    m_method_id = std::max(size_t(0), std::min(get_methods_count() - 1, m_method_id));

    m_default_print_cfg = wxGetApp().preset_bundle->full_config();

    const auto &sel = m_plater->get_selection().get_content();

    m_selected_object_ids.clear();
    m_selected_object_ids.reserve(sel.size());

    for (const auto &s : sel) {
        int obj_id;
        std::tie(obj_id, std::ignore) = s;
        m_selected_object_ids.emplace_back(obj_id);
    }
}

void RotoptimizeJob::process(Ctl &ctl)
{
    int prev_status = 0;
    auto statustxt = _u8L("Searching for optimal orientation");
    ctl.update_status(0, statustxt);

    auto params =
        sla::RotOptimizeParams{}
            .accuracy(m_accuracy)
            .print_config(&m_default_print_cfg)
            .statucb([this, &prev_status, &ctl/*, &statustxt*/](int s)
        {
            return !ctl.was_canceled();
        });


    for (ObjRot &objrot : m_selected_object_ids) {
        ModelObject *o = m_plater->model().objects[size_t(objrot.idx)];
        if (!o) continue;

        if (Methods[m_method_id].findfn)
            objrot.rot = Methods[m_method_id].findfn(*o, params);

        prev_status += 100 / m_selected_object_ids.size();

        if (ctl.was_canceled()) break;
    }

    ctl.update_status(100, ctl.was_canceled() ? _u8L("Orientation search canceled.") :
                                        _u8L("Orientation found."));
}

RotoptimizeJob::RotoptimizeJob() : m_plater{wxGetApp().plater()} { prepare(); }

void RotoptimizeJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (canceled || eptr)
        return;

    for (const ObjRot &objrot : m_selected_object_ids) {
        ModelObject *o = m_plater->model().objects[size_t(objrot.idx)];
        if (!o) continue;

        for(ModelInstance * oi : o->instances) {
            if (objrot.rot)
                oi->set_rotation({objrot.rot->x(), objrot.rot->y(), 0.});

            auto    trmatrix = oi->get_transformation().get_matrix();
            Polygon trchull  = o->convex_hull_2d(trmatrix);

            MinAreaBoundigBox rotbb(trchull, MinAreaBoundigBox::pcConvex);
            double            phi = rotbb.angle_to_X();

            // The box should be landscape
            if(rotbb.width() < rotbb.height()) phi += PI / 2;

            Vec3d rt = oi->get_rotation(); rt(Z) += phi;

            oi->set_rotation(rt);
        }

        // Correct the z offset of the object which was corrupted be
        // the rotation
        o->ensure_on_bed();

//        m_plater->find_new_position(o->instances);
    }

    if (!canceled)
        m_plater->update();
}

}}
