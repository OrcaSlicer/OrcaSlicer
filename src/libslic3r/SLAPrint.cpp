#include "SLAPrint.hpp"
#include "SLAPrintSteps.hpp"

#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "MTUtils.hpp"
#include "Thread.hpp"

#include <unordered_set>
#include <numeric>

#include <tbb/parallel_for.h>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

// #define SLAPRINT_DO_BENCHMARK

#ifdef SLAPRINT_DO_BENCHMARK
#include <libnest2d/tools/benchmark.h>
#endif

#include "I18N.hpp"

//! macro used to mark string used at localization,
//! return same string
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {


bool is_zero_elevation(const SLAPrintObjectConfig &c)
{
    return c.pad_enable.getBool() && c.pad_around_object.getBool();
}

// Compile the argument for support creation from the static print config.
sla::SupportTreeConfig make_support_cfg(const SLAPrintObjectConfig& c)
{
    sla::SupportTreeConfig scfg;

    scfg.enabled = c.supports_enable.getBool();
    scfg.head_front_radius_mm = 0.5*c.support_head_front_diameter.getFloat();
    double pillar_r = 0.5 * c.support_pillar_diameter.getFloat();
    scfg.head_back_radius_mm = pillar_r;
    scfg.head_fallback_radius_mm =
        0.01 * c.support_small_pillar_diameter_percent.getFloat() * pillar_r;
    scfg.head_penetration_mm = c.support_head_penetration.getFloat();
    scfg.head_width_mm = c.support_head_width.getFloat();
    scfg.object_elevation_mm = is_zero_elevation(c) ?
                                   0. : c.support_object_elevation.getFloat();
    scfg.bridge_slope = c.support_critical_angle.getFloat() * PI / 180.0 ;
    scfg.max_bridge_length_mm = c.support_max_bridge_length.getFloat();
    scfg.max_pillar_link_distance_mm = c.support_max_pillar_link_distance.getFloat();
    switch(c.support_pillar_connection_mode.getInt()) {
    case slapcmZigZag:
        scfg.pillar_connection_mode = sla::PillarConnectionMode::zigzag; break;
    case slapcmCross:
        scfg.pillar_connection_mode = sla::PillarConnectionMode::cross; break;
    case slapcmDynamic:
        scfg.pillar_connection_mode = sla::PillarConnectionMode::dynamic; break;
    }
    scfg.ground_facing_only = c.support_buildplate_only.getBool();
    scfg.pillar_widening_factor = c.support_pillar_widening_factor.getFloat();
    scfg.base_radius_mm = 0.5*c.support_base_diameter.getFloat();
    scfg.base_height_mm = c.support_base_height.getFloat();
    scfg.pillar_base_safety_distance_mm =
        c.support_base_safety_distance.getFloat() < EPSILON ?
            scfg.safety_distance_mm : c.support_base_safety_distance.getFloat();

    scfg.max_bridges_on_pillar = unsigned(c.support_max_bridges_on_pillar.getInt());

    return scfg;
}

sla::PadConfig::EmbedObject builtin_pad_cfg(const SLAPrintObjectConfig& c)
{
    sla::PadConfig::EmbedObject ret;

    ret.enabled = is_zero_elevation(c);

    if(ret.enabled) {
        ret.everywhere           = c.pad_around_object_everywhere.getBool();
        ret.object_gap_mm        = c.pad_object_gap.getFloat();
        ret.stick_width_mm       = c.pad_object_connector_width.getFloat();
        ret.stick_stride_mm      = c.pad_object_connector_stride.getFloat();
        ret.stick_penetration_mm = c.pad_object_connector_penetration
                                       .getFloat();
    }

    return ret;
}

sla::PadConfig make_pad_cfg(const SLAPrintObjectConfig& c)
{
    sla::PadConfig pcfg;

    pcfg.wall_thickness_mm = c.pad_wall_thickness.getFloat();
    pcfg.wall_slope = c.pad_wall_slope.getFloat() * PI / 180.0;

    pcfg.max_merge_dist_mm = c.pad_max_merge_distance.getFloat();
    pcfg.wall_height_mm = c.pad_wall_height.getFloat();
    pcfg.brim_size_mm = c.pad_brim_size.getFloat();

    // set builtin pad implicitly ON
    pcfg.embed_object = builtin_pad_cfg(c);

    return pcfg;
}

bool validate_pad(const indexed_triangle_set &pad, const sla::PadConfig &pcfg)
{
    // An empty pad can only be created if embed_object mode is enabled
    // and the pad is not forced everywhere
    return !pad.empty() || (pcfg.embed_object.enabled && !pcfg.embed_object.everywhere);
}

void SLAPrint::clear()
{
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    // The following call should stop background processing if it is running.
    this->invalidate_all_steps();
    for (SLAPrintObject *object : m_objects)
        delete object;
    m_objects.clear();
    m_model.clear_objects();
}

// Transformation without rotation around Z and without a shift by X and Y.
Transform3d SLAPrint::sla_trafo(const ModelObject &model_object) const
{

    Vec3d corr = this->relative_correction();

    ModelInstance &model_instance = *model_object.instances.front();
    Vec3d          offset         = model_instance.get_offset();
    Vec3d          rotation       = model_instance.get_rotation();
    offset(0) = 0.;
    offset(1) = 0.;
    rotation(2) = 0.;

    offset.z() *= corr.z();

    auto trafo = Transform3d::Identity();
    trafo.translate(offset);
    trafo.scale(corr);
    trafo.rotate(Eigen::AngleAxisd(rotation.z(), Vec3d::UnitZ()));
    trafo.rotate(Eigen::AngleAxisd(rotation.y(), Vec3d::UnitY()));
    trafo.rotate(Eigen::AngleAxisd(rotation.x(), Vec3d::UnitX()));
    trafo.scale(model_instance.get_scaling_factor());
    trafo.scale(model_instance.get_mirror());

    if (model_instance.is_left_handed())
        trafo = Eigen::Scaling(Vec3d(-1., 1., 1.)) * trafo;

    return trafo;
}

// List of instances, where the ModelInstance transformation is a composite of sla_trafo and the transformation defined by SLAPrintObject::Instance.
static std::vector<SLAPrintObject::Instance> sla_instances(const ModelObject &model_object)
{
    std::vector<SLAPrintObject::Instance> instances;
    assert(! model_object.instances.empty());
    if (! model_object.instances.empty()) {
        Vec3d rotation0 = model_object.instances.front()->get_rotation();
        rotation0(2) = 0.;
        for (ModelInstance *model_instance : model_object.instances)
            if (model_instance->is_printable()) {
                instances.emplace_back(
                    model_instance->id(),
                    Point::new_scale(model_instance->get_offset(X), model_instance->get_offset(Y)),
                    float(Geometry::rotation_diff_z(rotation0, model_instance->get_rotation())));
            }
    }
    return instances;
}

std::vector<ObjectID> SLAPrint::print_object_ids() const
{
    std::vector<ObjectID> out;
    // Reserve one more for the caller to append the ID of the Print itself.
    out.reserve(m_objects.size() + 1);
    for (const SLAPrintObject *print_object : m_objects)
        out.emplace_back(print_object->id());
    return out;
}

SLAPrint::ApplyStatus SLAPrint::apply(const Model &model, DynamicPrintConfig config)
{
#ifdef _DEBUG
    check_model_ids_validity(model);
#endif /* _DEBUG */

    // Normalize the config.
    config.option("sla_print_settings_id",        true);
    config.option("sla_material_settings_id",     true);
    config.option("printer_settings_id",          true);
    // Collect changes to print config.
    t_config_option_keys print_diff    = m_print_config.diff(config);
    t_config_option_keys printer_diff  = m_printer_config.diff(config);
    t_config_option_keys material_diff = m_material_config.diff(config);
    t_config_option_keys object_diff   = m_default_object_config.diff(config);
    t_config_option_keys placeholder_parser_diff = m_placeholder_parser.config_diff(config);

    // Do not use the ApplyStatus as we will use the max function when updating apply_status.
    unsigned int apply_status = APPLY_STATUS_UNCHANGED;
    auto update_apply_status = [&apply_status](bool invalidated)
        { apply_status = std::max<unsigned int>(apply_status, invalidated ? APPLY_STATUS_INVALIDATED : APPLY_STATUS_CHANGED); };
    if (! (print_diff.empty() && printer_diff.empty() && material_diff.empty() && object_diff.empty()))
        update_apply_status(false);

    // Grab the lock for the Print / PrintObject milestones.
    std::scoped_lock<std::mutex> lock(this->state_mutex());

    // The following call may stop the background processing.
    bool invalidate_all_model_objects = false;
    if (! print_diff.empty())
        update_apply_status(this->invalidate_state_by_config_options(print_diff, invalidate_all_model_objects));
    if (! printer_diff.empty())
        update_apply_status(this->invalidate_state_by_config_options(printer_diff, invalidate_all_model_objects));
    if (! material_diff.empty())
        update_apply_status(this->invalidate_state_by_config_options(material_diff, invalidate_all_model_objects));

    // Apply variables to placeholder parser. The placeholder parser is currently used
    // only to generate the output file name.
    if (! placeholder_parser_diff.empty()) {
        // update_apply_status(this->invalidate_step(slapsRasterize));
        m_placeholder_parser.apply_config(config);
        // Set the profile aliases for the PrintBase::output_filename()
        m_placeholder_parser.set("print_preset",            config.option("sla_print_settings_id")->clone());
        m_placeholder_parser.set("material_preset",         config.option("sla_material_settings_id")->clone());
        m_placeholder_parser.set("printer_preset",          config.option("printer_settings_id")->clone());
    }

    // It is also safe to change m_config now after this->invalidate_state_by_config_options() call.
    m_print_config.apply_only(config, print_diff, true);
    m_printer_config.apply_only(config, printer_diff, true);
    // Handle changes to material config.
    m_material_config.apply_only(config, material_diff, true);
    // Handle changes to object config defaults
    m_default_object_config.apply_only(config, object_diff, true);

    if (m_printer) m_printer->apply(m_printer_config);

    struct ModelObjectStatus {
        enum Status {
            Unknown,
            Old,
            New,
            Moved,
            Deleted,
        };
        ModelObjectStatus(ObjectID id, Status status = Unknown) : id(id), status(status) {}
        ObjectID                id;
        Status                  status;
        // Search by id.
        bool operator<(const ModelObjectStatus &rhs) const { return id < rhs.id; }
    };
    std::set<ModelObjectStatus> model_object_status;

    // 1) Synchronize model objects.
    if (model.id() != m_model.id() || invalidate_all_model_objects) {
        // Kill everything, initialize from scratch.
        // Stop background processing.
        this->call_cancel_callback();
        update_apply_status(this->invalidate_all_steps());
        for (SLAPrintObject *object : m_objects) {
            model_object_status.emplace(object->model_object()->id(), ModelObjectStatus::Deleted);
            update_apply_status(object->invalidate_all_steps());
            delete object;
        }
        m_objects.clear();
        m_model.assign_copy(model);
        for (const ModelObject *model_object : m_model.objects)
            model_object_status.emplace(model_object->id(), ModelObjectStatus::New);
    } else {
        if (model_object_list_equal(m_model, model)) {
            // The object list did not change.
            for (const ModelObject *model_object : m_model.objects)
                model_object_status.emplace(model_object->id(), ModelObjectStatus::Old);
        } else if (model_object_list_extended(m_model, model)) {
            // Add new objects. Their volumes and configs will be synchronized later.
            update_apply_status(this->invalidate_step(slapsMergeSlicesAndEval));
            for (const ModelObject *model_object : m_model.objects)
                model_object_status.emplace(model_object->id(), ModelObjectStatus::Old);
            for (size_t i = m_model.objects.size(); i < model.objects.size(); ++ i) {
                model_object_status.emplace(model.objects[i]->id(), ModelObjectStatus::New);
                m_model.objects.emplace_back(ModelObject::new_copy(*model.objects[i]));
                m_model.objects.back()->set_model(&m_model);
            }
        } else {
            // Reorder the objects, add new objects.
            // First stop background processing before shuffling or deleting the PrintObjects in the object list.
            this->call_cancel_callback();
            update_apply_status(this->invalidate_step(slapsMergeSlicesAndEval));
            // Second create a new list of objects.
            std::vector<ModelObject*> model_objects_old(std::move(m_model.objects));
            m_model.objects.clear();
            m_model.objects.reserve(model.objects.size());
            auto by_id_lower = [](const ModelObject *lhs, const ModelObject *rhs){ return lhs->id() < rhs->id(); };
            std::sort(model_objects_old.begin(), model_objects_old.end(), by_id_lower);
            for (const ModelObject *mobj : model.objects) {
                auto it = std::lower_bound(model_objects_old.begin(), model_objects_old.end(), mobj, by_id_lower);
                if (it == model_objects_old.end() || (*it)->id() != mobj->id()) {
                    // New ModelObject added.
                    m_model.objects.emplace_back(ModelObject::new_copy(*mobj));
                    m_model.objects.back()->set_model(&m_model);
                    model_object_status.emplace(mobj->id(), ModelObjectStatus::New);
                } else {
                    // Existing ModelObject re-added (possibly moved in the list).
                    m_model.objects.emplace_back(*it);
                    model_object_status.emplace(mobj->id(), ModelObjectStatus::Moved);
                }
            }
            bool deleted_any = false;
            for (ModelObject *&model_object : model_objects_old) {
                if (model_object_status.find(ModelObjectStatus(model_object->id())) == model_object_status.end()) {
                    model_object_status.emplace(model_object->id(), ModelObjectStatus::Deleted);
                    deleted_any = true;
                } else
                    // Do not delete this ModelObject instance.
                    model_object = nullptr;
            }
            if (deleted_any) {
                // Delete PrintObjects of the deleted ModelObjects.
                std::vector<SLAPrintObject*> print_objects_old = std::move(m_objects);
                m_objects.clear();
                m_objects.reserve(print_objects_old.size());
                for (SLAPrintObject *print_object : print_objects_old) {
                    auto it_status = model_object_status.find(ModelObjectStatus(print_object->model_object()->id()));
                    assert(it_status != model_object_status.end());
                    if (it_status->status == ModelObjectStatus::Deleted) {
                        update_apply_status(print_object->invalidate_all_steps());
                        delete print_object;
                    } else
                        m_objects.emplace_back(print_object);
                }
                for (ModelObject *model_object : model_objects_old)
                    delete model_object;
            }
        }
    }

    // 2) Map print objects including their transformation matrices.
    struct PrintObjectStatus {
        enum Status {
            Unknown,
            Deleted,
            Reused,
            New
        };
        PrintObjectStatus(SLAPrintObject *print_object, Status status = Unknown) :
            id(print_object->model_object()->id()),
            print_object(print_object),
            trafo(print_object->trafo()),
            status(status) {}
        PrintObjectStatus(ObjectID id) : id(id), print_object(nullptr), trafo(Transform3d::Identity()), status(Unknown) {}
        // ID of the ModelObject & PrintObject
        ObjectID         id;
        // Pointer to the old PrintObject
        SLAPrintObject  *print_object;
        // Trafo generated with model_object->world_matrix(true)
        Transform3d      trafo;
        Status           status;
        // Search by id.
        bool operator<(const PrintObjectStatus &rhs) const { return id < rhs.id; }
    };
    std::multiset<PrintObjectStatus> print_object_status;
    for (SLAPrintObject *print_object : m_objects)
        print_object_status.emplace(PrintObjectStatus(print_object));

    // 3) Synchronize ModelObjects & PrintObjects.
    std::vector<SLAPrintObject*> print_objects_new;
    print_objects_new.reserve(std::max(m_objects.size(), m_model.objects.size()));
    bool new_objects = false;
    for (size_t idx_model_object = 0; idx_model_object < model.objects.size(); ++ idx_model_object) {
        ModelObject &model_object = *m_model.objects[idx_model_object];
        auto it_status = model_object_status.find(ModelObjectStatus(model_object.id()));
        assert(it_status != model_object_status.end());
        assert(it_status->status != ModelObjectStatus::Deleted);
        // PrintObject for this ModelObject, if it exists.
        auto it_print_object_status = print_object_status.end();
        if (it_status->status != ModelObjectStatus::New) {
            // Update the ModelObject instance, possibly invalidate the linked PrintObjects.
            assert(it_status->status == ModelObjectStatus::Old || it_status->status == ModelObjectStatus::Moved);
            const ModelObject &model_object_new       = *model.objects[idx_model_object];
            it_print_object_status = print_object_status.lower_bound(PrintObjectStatus(model_object.id()));
            if (it_print_object_status != print_object_status.end() && it_print_object_status->id != model_object.id())
                it_print_object_status = print_object_status.end();
            // Check whether a model part volume was added or removed, their transformations or order changed.
            bool model_parts_differ = model_volume_list_changed(model_object, model_object_new, ModelVolumeType::MODEL_PART);
            bool sla_trafo_differs  =
                model_object.instances.empty() != model_object_new.instances.empty() ||
                (! model_object.instances.empty() &&
                  (! sla_trafo(model_object).isApprox(sla_trafo(model_object_new)) ||
                    model_object.instances.front()->is_left_handed() != model_object_new.instances.front()->is_left_handed()));
            if (model_parts_differ || sla_trafo_differs) {
                // The very first step (the slicing step) is invalidated. One may freely remove all associated PrintObjects.
                if (it_print_object_status != print_object_status.end()) {
                    update_apply_status(it_print_object_status->print_object->invalidate_all_steps());
                    const_cast<PrintObjectStatus&>(*it_print_object_status).status = PrintObjectStatus::Deleted;
                }
                // Copy content of the ModelObject including its ID, do not change the parent.
                model_object.assign_copy(model_object_new);
            } else {
                // Synchronize Object's config.
                bool object_config_changed = ! model_object.config.timestamp_matches(model_object_new.config);
                if (object_config_changed)
                    model_object.config.assign_config(model_object_new.config);
                if (! object_diff.empty() || object_config_changed) {
                    SLAPrintObjectConfig new_config = m_default_object_config;
                    new_config.apply(model_object.config.get(), true);
                    if (it_print_object_status != print_object_status.end()) {
                        t_config_option_keys diff = it_print_object_status->print_object->config().diff(new_config);
                        if (! diff.empty()) {
                            update_apply_status(it_print_object_status->print_object->invalidate_state_by_config_options(diff));
                            it_print_object_status->print_object->config_apply_only(new_config, diff, true);
                        }
                    }
                }

                bool old_user_modified = model_object.sla_points_status == sla::PointsStatus::UserModified;
                bool new_user_modified = model_object_new.sla_points_status == sla::PointsStatus::UserModified;
                if ((old_user_modified && ! new_user_modified) || // switching to automatic supports from manual supports
                    (! old_user_modified && new_user_modified) || // switching to manual supports from automatic supports
                    (new_user_modified && model_object.sla_support_points != model_object_new.sla_support_points)) {
                    if (it_print_object_status != print_object_status.end())
                        update_apply_status(it_print_object_status->print_object->invalidate_step(slaposSupportPoints));

                    model_object.sla_support_points = model_object_new.sla_support_points;
                }
                model_object.sla_points_status = model_object_new.sla_points_status;

                // Invalidate hollowing if drain holes have changed
                if (model_object.sla_drain_holes != model_object_new.sla_drain_holes)
                {
                    model_object.sla_drain_holes = model_object_new.sla_drain_holes;
                    update_apply_status(it_print_object_status->print_object->invalidate_step(slaposDrillHoles));
                }

                // Copy the ModelObject name, input_file and instances. The instances will compared against PrintObject instances in the next step.
                model_object.name       = model_object_new.name;
                model_object.input_file = model_object_new.input_file;
                model_object.clear_instances();
                model_object.instances.reserve(model_object_new.instances.size());
                for (const ModelInstance *model_instance : model_object_new.instances) {
                    model_object.instances.emplace_back(new ModelInstance(*model_instance));
                    model_object.instances.back()->set_model_object(&model_object);
                }
            }
        }

        std::vector<SLAPrintObject::Instance> new_instances = sla_instances(model_object);
        if (it_print_object_status != print_object_status.end() && it_print_object_status->status != PrintObjectStatus::Deleted) {
            // The SLAPrintObject is already there.
            if (new_instances.empty()) {
                const_cast<PrintObjectStatus&>(*it_print_object_status).status = PrintObjectStatus::Deleted;
            } else {
                if (new_instances != it_print_object_status->print_object->instances()) {
                    // Instances changed.
                    it_print_object_status->print_object->set_instances(new_instances);
                    update_apply_status(this->invalidate_step(slapsMergeSlicesAndEval));
                }
                print_objects_new.emplace_back(it_print_object_status->print_object);
                const_cast<PrintObjectStatus&>(*it_print_object_status).status = PrintObjectStatus::Reused;
            }
        } else if (! new_instances.empty()) {
            auto print_object = new SLAPrintObject(this, &model_object);

            // FIXME: this invalidates the transformed mesh in SLAPrintObject
            // which is expensive to calculate (especially the raw_mesh() call)
            print_object->set_trafo(sla_trafo(model_object), model_object.instances.front()->is_left_handed());

            print_object->set_instances(std::move(new_instances));

            print_object->config_apply(m_default_object_config, true);
            print_object->config_apply(model_object.config.get(), true);
            print_objects_new.emplace_back(print_object);
            new_objects = true;
        }
    }

    if (m_objects != print_objects_new) {
        this->call_cancel_callback();
        update_apply_status(this->invalidate_all_steps());
        m_objects = print_objects_new;
        // Delete the PrintObjects marked as Unknown or Deleted.
        for (auto &pos : print_object_status)
            if (pos.status == PrintObjectStatus::Unknown || pos.status == PrintObjectStatus::Deleted) {
                update_apply_status(pos.print_object->invalidate_all_steps());
                delete pos.print_object;
            }
        if (new_objects)
            update_apply_status(false);
    }

    if(m_objects.empty()) {
        m_printer_input = {};
        m_print_statistics = {};
    }

#ifdef _DEBUG
    check_model_ids_equal(m_model, model);
#endif /* _DEBUG */

    m_full_print_config = std::move(config);
    return static_cast<ApplyStatus>(apply_status);
}

// After calling the apply() function, set_task() may be called to limit the task to be processed by process().
void SLAPrint::set_task(const TaskParams &params)
{
    // Grab the lock for the Print / PrintObject milestones.
    std::scoped_lock<std::mutex> lock(this->state_mutex());

    int n_object_steps = int(params.to_object_step) + 1;
    if (n_object_steps == 0)
        n_object_steps = int(slaposCount);

    if (params.single_model_object.valid()) {
        // Find the print object to be processed with priority.
        SLAPrintObject *print_object = nullptr;
        size_t          idx_print_object = 0;
        for (; idx_print_object < m_objects.size(); ++ idx_print_object)
            if (m_objects[idx_print_object]->model_object()->id() == params.single_model_object) {
                print_object = m_objects[idx_print_object];
                break;
            }
        assert(print_object != nullptr);
        // Find out whether the priority print object is being currently processed.
        bool running = false;
        for (int istep = 0; istep < n_object_steps; ++ istep) {
            if (! print_object->m_stepmask[size_t(istep)])
                // Step was skipped, cancel.
                break;
            if (print_object->is_step_started_unguarded(SLAPrintObjectStep(istep))) {
                // No step was skipped, and a wanted step is being processed. Don't cancel.
                running = true;
                break;
            }
        }
        if (! running)
            this->call_cancel_callback();

        // Now the background process is either stopped, or it is inside one of the print object steps to be calculated anyway.
        if (params.single_model_instance_only) {
            // Suppress all the steps of other instances.
            for (SLAPrintObject *po : m_objects)
                for (size_t istep = 0; istep < slaposCount; ++ istep)
                    po->m_stepmask[istep] = false;
        } else if (! running) {
            // Swap the print objects, so that the selected print_object is first in the row.
            // At this point the background processing must be stopped, so it is safe to shuffle print objects.
            if (idx_print_object != 0)
                std::swap(m_objects.front(), m_objects[idx_print_object]);
        }
        // and set the steps for the current object.
        for (int istep = 0; istep < n_object_steps; ++ istep)
            print_object->m_stepmask[size_t(istep)] = true;
        for (int istep = n_object_steps; istep < int(slaposCount); ++ istep)
            print_object->m_stepmask[size_t(istep)] = false;
    } else {
        // Slicing all objects.
        bool running = false;
        for (SLAPrintObject *print_object : m_objects)
            for (int istep = 0; istep < n_object_steps; ++ istep) {
                if (! print_object->m_stepmask[size_t(istep)]) {
                    // Step may have been skipped. Restart.
                    goto loop_end;
                }
                if (print_object->is_step_started_unguarded(SLAPrintObjectStep(istep))) {
                    // This step is running, and the state cannot be changed due to the this->state_mutex() being locked.
                    // It is safe to manipulate m_stepmask of other SLAPrintObjects and SLAPrint now.
                    running = true;
                    goto loop_end;
                }
            }
    loop_end:
        if (! running)
            this->call_cancel_callback();
        for (SLAPrintObject *po : m_objects) {
            for (int istep = 0; istep < n_object_steps; ++ istep)
                po->m_stepmask[size_t(istep)] = true;
            for (auto istep = size_t(n_object_steps); istep < slaposCount; ++ istep)
                po->m_stepmask[istep] = false;
        }
    }

    if (params.to_object_step != -1 || params.to_print_step != -1) {
        // Limit the print steps.
        size_t istep = (params.to_object_step != -1) ? 0 : size_t(params.to_print_step) + 1;
        for (; istep < m_stepmask.size(); ++ istep)
            m_stepmask[istep] = false;
    }
}

// Clean up after process() finished, either with success, error or if canceled.
// The adjustments on the SLAPrint / SLAPrintObject data due to set_task() are to be reverted here.
void SLAPrint::finalize()
{
    for (SLAPrintObject *po : m_objects)
        for (size_t istep = 0; istep < slaposCount; ++ istep)
            po->m_stepmask[istep] = true;
    for (size_t istep = 0; istep < slapsCount; ++ istep)
        m_stepmask[istep] = true;
}

// Generate a recommended output file name based on the format template, default extension, and template parameters
// (timestamps, object placeholders derived from the model, current placeholder prameters and print statistics.
// Use the final print statistics if available, or just keep the print statistics placeholders if not available yet (before the output is finalized).
std::string SLAPrint::output_filename(const std::string &filename_base) const
{
    DynamicConfig config = this->finished() ? this->print_statistics().config() : this->print_statistics().placeholders();
    return this->PrintBase::output_filename(m_print_config.filename_format.value, ".sl1", filename_base, &config);
}

StringObjectException SLAPrint::validate(StringObjectException *exception, Polygons *collison_polygons, std::vector<std::pair<Polygon, float>> *height_polygons) const
{
    for(SLAPrintObject * po : m_objects) {

        const ModelObject *mo = po->model_object();
        bool supports_en = po->config().supports_enable.getBool();

        if(supports_en &&
           mo->sla_points_status == sla::PointsStatus::UserModified &&
           mo->sla_support_points.empty())
            return {L("Cannot proceed without support points! "
                     "Add support points or disable support generation."), po};

        sla::SupportTreeConfig cfg = make_support_cfg(po->config());

        double elv = cfg.object_elevation_mm;

        sla::PadConfig padcfg = make_pad_cfg(po->config());
        sla::PadConfig::EmbedObject &builtinpad = padcfg.embed_object;

        if(supports_en && !builtinpad.enabled && elv < cfg.head_fullwidth())
            return {L(
                "Elevation is too low for object. Use the \"Pad around "
                "object\" feature to print the object without elevation."), po};

        if(supports_en && builtinpad.enabled &&
           cfg.pillar_base_safety_distance_mm < builtinpad.object_gap_mm) {
            return {L(
                "The endings of the support pillars will be deployed on the "
                "gap between the object and the pad. 'Support base safety "
                "distance' has to be greater than the 'Pad object gap' "
                "parameter to avoid this."), po};
        }

        std::string pval = padcfg.validate();
        if (!pval.empty()) return {pval, po};
    }

    double expt_max = m_printer_config.max_exposure_time.getFloat();
    double expt_min = m_printer_config.min_exposure_time.getFloat();
    double expt_cur = m_material_config.exposure_time.getFloat();

    if (expt_cur < expt_min || expt_cur > expt_max)
        return {L("Exposition time is out of printer profile bounds.")};

    double iexpt_max = m_printer_config.max_initial_exposure_time.getFloat();
    double iexpt_min = m_printer_config.min_initial_exposure_time.getFloat();
    double iexpt_cur = m_material_config.initial_exposure_time.getFloat();

    if (iexpt_cur < iexpt_min || iexpt_cur > iexpt_max)
        return {L("Initial exposition time is out of printer profile bounds.")};

    return {};
}

void SLAPrint::set_printer(SLAArchive *arch)
{
    invalidate_step(slapsRasterize);
    m_printer = arch;
}

bool SLAPrint::invalidate_step(SLAPrintStep step)
{
    bool invalidated = Inherited::invalidate_step(step);

    // propagate to dependent steps
    if (step == slapsMergeSlicesAndEval) {
        invalidated |= this->invalidate_all_steps();
    }

    return invalidated;
}

void SLAPrint::process(long long *time_cost_with_cache, bool use_cache)
{
    if (m_objects.empty())
        return;

    name_tbb_thread_pool_threads_set_locale();

    // Assumption: at this point the print objects should be populated only with
    // the model objects we have to process and the instances are also filtered

    Steps printsteps(this);

    // We want to first process all objects...
    std::vector<SLAPrintObjectStep> level1_obj_steps = {
        slaposHollowing, slaposDrillHoles, slaposObjectSlice, slaposSupportPoints, slaposSupportTree, slaposPad
    };

    // and then slice all supports to allow preview to be displayed ASAP
    std::vector<SLAPrintObjectStep> level2_obj_steps = {
        slaposSliceSupports
    };

    SLAPrintStep print_steps[] = { slapsMergeSlicesAndEval, slapsRasterize };

    double st = Steps::min_objstatus;

    BOOST_LOG_TRIVIAL(info) << "Start slicing process.";

#ifdef SLAPRINT_DO_BENCHMARK
    Benchmark bench;
#else
    struct {
        void start() {} void stop() {} double getElapsedSec() { return .0; }
    } bench;
#endif

    std::array<double, slaposCount + slapsCount> step_times {};

    auto apply_steps_on_objects =
        [this, &st, &printsteps, &step_times, &bench]
        (const std::vector<SLAPrintObjectStep> &steps)
    {
        double incr = 0;
        for (SLAPrintObject *po : m_objects) {
            for (SLAPrintObjectStep step : steps) {

                // Cancellation checking. Each step will check for
                // cancellation on its own and return earlier gracefully.
                // Just after it returns execution gets to this point and
                // throws the canceled signal.
                throw_if_canceled();

                st += incr;

                if (po->m_stepmask[step] && po->set_started(step)) {
                    m_report_status(*this, st, printsteps.label(step));
                    bench.start();
                    printsteps.execute(step, *po);
                    bench.stop();
                    step_times[step] += bench.getElapsedSec();
                    throw_if_canceled();
                    po->set_done(step);
                }

                incr = printsteps.progressrange(step);
            }
        }
    };

    apply_steps_on_objects(level1_obj_steps);
    apply_steps_on_objects(level2_obj_steps);

    // this would disable the rasterization step
    // std::fill(m_stepmask.begin(), m_stepmask.end(), false);

    st = Steps::max_objstatus;
    for(SLAPrintStep currentstep : print_steps) {
        throw_if_canceled();

        if (m_stepmask[currentstep] && set_started(currentstep)) {
            m_report_status(*this, st, printsteps.label(currentstep));
            bench.start();
            printsteps.execute(currentstep);
            bench.stop();
            step_times[slaposCount + currentstep] += bench.getElapsedSec();
            throw_if_canceled();
            set_done(currentstep);
        }

        st += printsteps.progressrange(currentstep);
    }

    // If everything vent well
    m_report_status(*this, 100, L("Slicing done"));

#ifdef SLAPRINT_DO_BENCHMARK
    std::string csvbenchstr;
    for (size_t i = 0; i < size_t(slaposCount); ++i)
        csvbenchstr += printsteps.label(SLAPrintObjectStep(i)) + ";";

    for (size_t i = 0; i < size_t(slapsCount); ++i)
        csvbenchstr += printsteps.label(SLAPrintStep(i)) + ";";

    csvbenchstr += "\n";
    for (double t : step_times) csvbenchstr += std::to_string(t) + ";";

    std::cout << "Performance stats: \n" << csvbenchstr << std::endl;
#endif

}

bool SLAPrint::invalidate_state_by_config_options(const std::vector<t_config_option_key> &opt_keys, bool &invalidate_all_model_objects)
{
    if (opt_keys.empty())
        return false;

    static std::unordered_set<std::string> steps_full = {
        "initial_layer_height",
        "material_correction",
        "material_correction_x",
        "material_correction_y",
        "material_correction_z",
        "relative_correction",
        "relative_correction_x",
        "relative_correction_y",
        "relative_correction_z",
        "absolute_correction",
        "elefant_foot_compensation",
        "elefant_foot_min_width",
        "gamma_correction"
    };

    // Cache the plenty of parameters, which influence the final rasterization only,
    // or they are only notes not influencing the rasterization step.
    static std::unordered_set<std::string> steps_rasterize = {
        "min_exposure_time",
        "max_exposure_time",
        "exposure_time",
        "min_initial_exposure_time",
        "max_initial_exposure_time",
        "initial_exposure_time",
        "display_width",
        "display_height",
        "display_pixels_x",
        "display_pixels_y",
        "display_mirror_x",
        "display_mirror_y",
        "display_orientation"
    };

    static std::unordered_set<std::string> steps_ignore = {
        "printable_area",
        "printable_height",
        "printer_technology",
        "filename_format",
        "fast_tilt_time",
        "slow_tilt_time",
        "area_fill",
        "bottle_cost",
        "bottle_volume",
        "bottle_weight",
        "material_density"
    };

    std::vector<SLAPrintStep> steps;
    std::vector<SLAPrintObjectStep> osteps;
    bool invalidated = false;

    for (const t_config_option_key &opt_key : opt_keys) {
        if (steps_rasterize.find(opt_key) != steps_rasterize.end()) {
            // These options only affect the final rasterization, or they are just notes without influence on the output,
            // so there is nothing to invalidate.
            steps.emplace_back(slapsMergeSlicesAndEval);
        } else if (steps_ignore.find(opt_key) != steps_ignore.end()) {
            // These steps have no influence on the output. Just ignore them.
        } else if (steps_full.find(opt_key) != steps_full.end()) {
            steps.emplace_back(slapsMergeSlicesAndEval);
            osteps.emplace_back(slaposObjectSlice);
            invalidate_all_model_objects = true;
        } else {
            // All values should be covered.
            assert(false);
        }
    }

    sort_remove_duplicates(steps);
    for (SLAPrintStep step : steps)
        invalidated |= this->invalidate_step(step);
    sort_remove_duplicates(osteps);
    for (SLAPrintObjectStep ostep : osteps)
        for (SLAPrintObject *object : m_objects)
            invalidated |= object->invalidate_step(ostep);
    return invalidated;
}

// Returns true if an object step is done on all objects and there's at least one object.
bool SLAPrint::is_step_done(SLAPrintObjectStep step) const
{
    if (m_objects.empty())
        return false;
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    for (const SLAPrintObject *object : m_objects)
        if (! object->is_step_done_unguarded(step))
            return false;
    return true;
}

SLAPrintObject::SLAPrintObject(SLAPrint *print, ModelObject *model_object)
    : Inherited(print, model_object)
    , m_stepmask(slaposCount, true)
    , m_transformed_rmesh([this](TriangleMesh &obj) {
        obj = m_model_object->raw_mesh();
        if (!obj.empty()) {
            obj.transform(m_trafo);
        }
    })
{}

SLAPrintObject::~SLAPrintObject() {}

// Called by SLAPrint::apply().
// This method only accepts SLAPrintObjectConfig option keys.
bool SLAPrintObject::invalidate_state_by_config_options(const std::vector<t_config_option_key> &opt_keys)
{
    if (opt_keys.empty())
        return false;

    std::vector<SLAPrintObjectStep> steps;
    bool invalidated = false;
    for (const t_config_option_key &opt_key : opt_keys) {
        if (   opt_key == "hollowing_enable"
            || opt_key == "hollowing_min_thickness"
            || opt_key == "hollowing_quality"
            || opt_key == "hollowing_closing_distance"
            ) {
            steps.emplace_back(slaposHollowing);
        } else if (
               opt_key == "layer_height"
            || opt_key == "faded_layers"
            || opt_key == "pad_enable"
            || opt_key == "pad_wall_thickness"
            || opt_key == "supports_enable"
            || opt_key == "support_object_elevation"
            || opt_key == "pad_around_object"
            || opt_key == "pad_around_object_everywhere"
            || opt_key == "slice_closing_radius") {
            steps.emplace_back(slaposObjectSlice);
        } else if (
               opt_key == "support_points_density_relative"
            || opt_key == "support_points_minimal_distance") {
            steps.emplace_back(slaposSupportPoints);
        } else if (
               opt_key == "support_head_front_diameter"
            || opt_key == "support_head_penetration"
            || opt_key == "support_head_width"
            || opt_key == "support_pillar_diameter"
            || opt_key == "support_small_pillar_diameter_percent"
            || opt_key == "support_max_bridges_on_pillar"
            || opt_key == "support_pillar_connection_mode"
            || opt_key == "support_buildplate_only"
            || opt_key == "support_base_diameter"
            || opt_key == "support_base_height"
            || opt_key == "support_critical_angle"
            || opt_key == "support_max_bridge_length"
            || opt_key == "support_max_pillar_link_distance"
            || opt_key == "support_base_safety_distance"
            ) {
            steps.emplace_back(slaposSupportTree);
        } else if (
               opt_key == "pad_wall_height"
            || opt_key == "pad_brim_size"
            || opt_key == "pad_max_merge_distance"
            || opt_key == "pad_wall_slope"
            || opt_key == "pad_edge_radius"
            || opt_key == "pad_object_gap"
            || opt_key == "pad_object_connector_stride"
            || opt_key == "pad_object_connector_width"
            || opt_key == "pad_object_connector_penetration"
            ) {
            steps.emplace_back(slaposPad);
        } else {
            // All keys should be covered.
            assert(false);
        }
    }

    sort_remove_duplicates(steps);
    for (SLAPrintObjectStep step : steps)
        invalidated |= this->invalidate_step(step);
    return invalidated;
}

bool SLAPrintObject::invalidate_step(SLAPrintObjectStep step)
{
    bool invalidated = Inherited::invalidate_step(step);
    // propagate to dependent steps
    if (step == slaposHollowing) {
        invalidated |= this->invalidate_all_steps();
    } else if (step == slaposDrillHoles) {
        invalidated |= this->invalidate_steps({ slaposObjectSlice, slaposSupportPoints, slaposSupportTree, slaposPad, slaposSliceSupports });
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    } else if (step == slaposObjectSlice) {
        invalidated |= this->invalidate_steps({ slaposSupportPoints, slaposSupportTree, slaposPad, slaposSliceSupports });
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    } else if (step == slaposSupportPoints) {
        invalidated |= this->invalidate_steps({ slaposSupportTree, slaposPad, slaposSliceSupports });
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    } else if (step == slaposSupportTree) {
        invalidated |= this->invalidate_steps({ slaposPad, slaposSliceSupports });
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    } else if (step == slaposPad) {
        invalidated |= this->invalidate_steps({slaposSliceSupports});
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    } else if (step == slaposSliceSupports) {
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    }
    return invalidated;
}

bool SLAPrintObject::invalidate_all_steps()
{
    return Inherited::invalidate_all_steps() | m_print->invalidate_all_steps();
}

double SLAPrintObject::get_elevation() const {
    if (is_zero_elevation(m_config)) return 0.;

    bool en = m_config.supports_enable.getBool();

    double ret = en ? m_config.support_object_elevation.getFloat() : 0.;

    if(m_config.pad_enable.getBool()) {
        // Normally the elevation for the pad itself would be the thickness of
        // its walls but currently it is half of its thickness. Whatever it
        // will be in the future, we provide the config to the get_pad_elevation
        // method and we will have the correct value
        sla::PadConfig pcfg = make_pad_cfg(m_config);
        if(!pcfg.embed_object) ret += pcfg.required_elevation();
    }

    return ret;
}

double SLAPrintObject::get_current_elevation() const
{
    if (is_zero_elevation(m_config)) return 0.;

    bool has_supports = is_step_done(slaposSupportTree);
    bool has_pad      = is_step_done(slaposPad);

    if(!has_supports && !has_pad)
        return 0;
    else if(has_supports && !has_pad) {
        return m_config.support_object_elevation.getFloat();
    }

    return get_elevation();
}

Vec3d SLAPrint::relative_correction() const
{
    Vec3d corr(1., 1., 1.);

    if(printer_config().relative_correction.values.size() >= 2) {
        corr.x() = printer_config().relative_correction_x.value;
        corr.y() = printer_config().relative_correction_y.value;
        corr.z() = printer_config().relative_correction_z.value;
    }

    if(material_config().material_correction.values.size() >= 2) {
        corr.x() *= material_config().material_correction_x.value;
        corr.y() *= material_config().material_correction_y.value;
        corr.z() *= material_config().material_correction_z.value;
    }

    return corr;
}

namespace { // dummy empty static containers for return values in some methods
const std::vector<ExPolygons> EMPTY_SLICES;
const TriangleMesh EMPTY_MESH;
const indexed_triangle_set EMPTY_TRIANGLE_SET;
const ExPolygons EMPTY_SLICE;
const std::vector<sla::SupportPoint> EMPTY_SUPPORT_POINTS;
}

const SliceRecord SliceRecord::EMPTY(0, std::nanf(""), 0.f);

const std::vector<sla::SupportPoint>& SLAPrintObject::get_support_points() const
{
    return m_supportdata? m_supportdata->pts : EMPTY_SUPPORT_POINTS;
}

const std::vector<ExPolygons> &SLAPrintObject::get_support_slices() const
{
    // assert(is_step_done(slaposSliceSupports));
    if (!m_supportdata) return EMPTY_SLICES;
    return m_supportdata->support_slices;
}

const ExPolygons &SliceRecord::get_slice(SliceOrigin o) const
{
    size_t idx = o == soModel ? m_model_slices_idx : m_support_slices_idx;

    if(m_po == nullptr) return EMPTY_SLICE;

    const std::vector<ExPolygons>& v = o == soModel? m_po->get_model_slices() :
                                                     m_po->get_support_slices();

    return idx >= v.size() ? EMPTY_SLICE : v[idx];
}

bool SLAPrintObject::has_mesh(SLAPrintObjectStep step) const
{
    switch (step) {
    case slaposDrillHoles:
        return m_hollowing_data && !m_hollowing_data->hollow_mesh_with_holes.empty();
    case slaposSupportTree:
        return ! this->support_mesh().empty();
    case slaposPad:
        return ! this->pad_mesh().empty();
    default:
        return false;
    }
}

TriangleMesh SLAPrintObject::get_mesh(SLAPrintObjectStep step) const
{
    switch (step) {
    case slaposSupportTree:
        return this->support_mesh();
    case slaposPad:
        return this->pad_mesh();
    case slaposDrillHoles:
        if (m_hollowing_data)
            return get_mesh_to_print();
        [[fallthrough]];
    default:
        return TriangleMesh();
    }
}

const TriangleMesh& SLAPrintObject::support_mesh() const
{
    if(m_config.supports_enable.getBool() && m_supportdata)
        return m_supportdata->tree_mesh;

    return EMPTY_MESH;
}

const TriangleMesh& SLAPrintObject::pad_mesh() const
{
    if(m_config.pad_enable.getBool() && m_supportdata)
        return m_supportdata->pad_mesh;

    return EMPTY_MESH;
}

const indexed_triangle_set &SLAPrintObject::hollowed_interior_mesh() const
{
    if (m_hollowing_data && m_hollowing_data->interior &&
        m_config.hollowing_enable.getBool())
        return sla::get_mesh(*m_hollowing_data->interior);

    return EMPTY_TRIANGLE_SET;
}

const TriangleMesh &SLAPrintObject::transformed_mesh() const {
    // we need to transform the raw mesh...
    // currently all the instances share the same x and y rotation and scaling
    // so we have to extract those from e.g. the first instance and apply to the
    // raw mesh. This is also true for the support points.
    // BUT: when the support structure is spawned for each instance than it has
    // to omit the X, Y rotation and scaling as those have been already applied
    // or apply an inverse transformation on the support structure after it
    // has been created.

    return m_transformed_rmesh.get();
}

sla::SupportPoints SLAPrintObject::transformed_support_points() const
{
    assert(m_model_object != nullptr);
    auto spts = m_model_object->sla_support_points;
    auto tr = trafo().cast<float>();
    for (sla::SupportPoint& suppt : spts) {
        suppt.pos = tr * suppt.pos;
    }

    return spts;
}

sla::DrainHoles SLAPrintObject::transformed_drainhole_points() const
{
    assert(m_model_object != nullptr);
    auto pts = m_model_object->sla_drain_holes;
    auto tr = trafo().cast<float>();
    auto sc = m_model_object->instances.front()->get_scaling_factor().cast<float>();
    for (sla::DrainHole &hl : pts) {
        hl.pos = tr * hl.pos;
        hl.normal = tr * hl.normal - tr.translation();

        // The normal scales as a covector (and we must also
        // undo the damage already done).
        hl.normal = Vec3f(hl.normal(0)/(sc(0)*sc(0)),
                          hl.normal(1)/(sc(1)*sc(1)),
                          hl.normal(2)/(sc(2)*sc(2)));

        // Now shift the hole a bit above the object and make it deeper to
        // compensate for it. This is to avoid problems when the hole is placed
        // on (nearly) flat surface.
        hl.pos -= hl.normal.normalized() * sla::HoleStickOutLength;
        hl.height += sla::HoleStickOutLength;
    }

    return pts;
}

DynamicConfig SLAPrintStatistics::config() const
{
    DynamicConfig config;
    const std::string print_time = Slic3r::short_time(get_time_dhms(float(this->estimated_print_time)));
    config.set_key_value("print_time", new ConfigOptionString(print_time));
    config.set_key_value("objects_used_material", new ConfigOptionFloat(this->objects_used_material));
    config.set_key_value("support_used_material", new ConfigOptionFloat(this->support_used_material));
    config.set_key_value("total_cost", new ConfigOptionFloat(this->total_cost));
    config.set_key_value("total_weight", new ConfigOptionFloat(this->total_weight));
    return config;
}

DynamicConfig SLAPrintStatistics::placeholders()
{
    DynamicConfig config;
    for (const char *key : {
        "print_time", "total_cost", "total_weight",
        "objects_used_material", "support_used_material" })
        config.set_key_value(key, new ConfigOptionString(std::string("{") + key + "}"));

    return config;
}

std::string SLAPrintStatistics::finalize_output_path(const std::string &path_in) const
{
    std::string final_path;
    try {
        boost::filesystem::path path(path_in);
        DynamicConfig cfg = this->config();
        PlaceholderParser pp;
        std::string new_stem = pp.process(path.stem().string(), 0, &cfg);
        final_path = (path.parent_path() / (new_stem + path.extension().string())).string();
    }
    catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Failed to apply the print statistics to the export file name: " << ex.what();
        final_path = path_in;
    }
    return final_path;
}

void SLAPrint::StatusReporter::operator()(SLAPrint &         p,
                                          double             st,
                                          const std::string &msg,
                                          unsigned           flags,
                                          const std::string &logmsg)
{
    m_st = st;
    BOOST_LOG_TRIVIAL(info)
        << st << "% " << msg << (logmsg.empty() ? "" : ": ") << logmsg
        << log_memory_info();

    p.set_status(int(std::round(st)), msg, flags);
}

} // namespace Slic3r
