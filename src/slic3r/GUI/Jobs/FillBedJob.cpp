#include "FillBedJob.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "libnest2d/common.hpp"

#include <numeric>

namespace Slic3r {
namespace GUI {

//BBS: add partplate related logic
void FillBedJob::prepare()
{
    PartPlateList& plate_list = m_plater->get_partplate_list();

    m_locked.clear();
    m_selected.clear();
    m_unselected.clear();
    m_bedpts.clear();

    params = init_arrange_params(m_plater);

    m_object_idx = m_plater->get_selected_object_idx();
    if (m_object_idx == -1)
        return;

    //select current plate at first
    int sel_id = m_plater->get_selection().get_instance_idx();
    sel_id = std::max(sel_id, 0);

    int sel_ret = plate_list.select_plate_by_obj(m_object_idx, sel_id);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":select plate obj_id %1%, ins_id %2%, ret %3%}") % m_object_idx % sel_id % sel_ret;

    PartPlate* plate = plate_list.get_curr_plate();
    Model& model = m_plater->model();
    BoundingBox plate_bb = plate->get_bounding_box_crd();
    int plate_cols = plate_list.get_plate_cols();
    int cur_plate_index = plate->get_index();

    ModelObject *model_object = m_plater->model().objects[m_object_idx];
    if (model_object->instances.empty()) return;

    const Slic3r::DynamicPrintConfig& global_config = wxGetApp().preset_bundle->full_config();
    m_selected.reserve(model_object->instances.size());
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx)
    {
        ModelObject* mo = model.objects[oidx];
        for (size_t inst_idx = 0; inst_idx < mo->instances.size(); ++inst_idx)
        {
            bool selected = (oidx == m_object_idx);

            ArrangePolygon ap = get_instance_arrange_poly(mo->instances[inst_idx], global_config);
            set_arrange_polygon_bed_exclusion_extruders(ap, *mo, *plate, global_config);
            BoundingBox ap_bb = ap.transformed_poly().contour.bounding_box();
            ap.name = mo->name;

            if (selected)
            {
                if (mo->instances[inst_idx]->printable)
                {
                    ++ap.priority;
                    ap.itemid = m_selected.size();
                    m_selected.emplace_back(ap);
                }
                else
                {
                    if (plate_bb.contains(ap_bb))
                    {
                        ap.bed_idx = 0;
                        ap.itemid = m_unselected.size();
                        ap.row = cur_plate_index / plate_cols;
                        ap.col = cur_plate_index % plate_cols;
                        ap.translation(X) -= bed_stride_x(m_plater) * ap.col;
                        ap.translation(Y) += bed_stride_y(m_plater) * ap.row;
                        m_unselected.emplace_back(ap);
                    }
                    else
                    {
                        ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
                        ap.itemid = m_locked.size();
                        m_locked.emplace_back(ap);
                    }
                }
            }
            else
            {
                if (plate_bb.contains(ap_bb))
                {
                    ap.bed_idx = 0;
                    ap.itemid = m_unselected.size();
                    ap.row = cur_plate_index / plate_cols;
                    ap.col = cur_plate_index % plate_cols;
                    ap.translation(X) -= bed_stride_x(m_plater) * ap.col;
                    ap.translation(Y) += bed_stride_y(m_plater) * ap.row;
                    m_unselected.emplace_back(ap);
                }
                else
                {
                    ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
                    ap.itemid = m_locked.size();
                    m_locked.emplace_back(ap);
                }
            }
        }
    }
    /*
    for (ModelInstance *inst : model_object->instances)
        if (inst->printable) {
            ArrangePolygon ap = get_arrange_poly(inst);
            // Existing objects need to be included in the result. Only
            // the needed amount of object will be added, no more.
            ++ap.priority;
            m_selected.emplace_back(ap);
        }*/

    if (m_selected.empty()) return;

    bool enable_wrapping = global_config.option<ConfigOptionBool>("enable_wrapping_detection")->value;
    //add the virtual object into unselect list if has
    double scaled_exclusion_gap = scale_(1);
    plate_list.preprocess_exclude_areas(params.excluded_regions, global_config, enable_wrapping, 1, scaled_exclusion_gap);
    plate_list.preprocess_exclude_areas(m_unselected, global_config, enable_wrapping);

    m_bedpts = get_bed_shape(*m_plater->config());

    auto &objects = m_plater->model().objects;
    /*BoundingBox bedbb = get_extents(m_bedpts);

    for (size_t idx = 0; idx < objects.size(); ++idx)
        if (int(idx) != m_object_idx)
            for (ModelInstance *mi : objects[idx]->instances) {
                ArrangePolygon ap = get_arrange_poly(mi);
                auto ap_bb = ap.transformed_poly().contour.bounding_box();

                if (ap.bed_idx == 0 && !bedbb.contains(ap_bb))
                    ap.bed_idx = arrangement::UNARRANGED;

                m_unselected.emplace_back(ap);
            }*/
    if (auto wt = get_wipe_tower_arrangepoly(*m_plater))
        m_unselected.emplace_back(std::move(*wt));

    double sc = scaled<double>(1.) * scaled(1.);

    auto polys = offset_ex(m_selected.front().poly, params.min_obj_distance / 2);
    ExPolygon poly = polys.empty() ? m_selected.front().poly : polys.front();
    double poly_area = poly.area() / sc;
    const ArrangePolygon &fill_template = m_selected.front();
    double unsel_area = std::accumulate(m_unselected.begin(),
                                        m_unselected.end(), 0.,
                                        [cur_plate_index, &fill_template](double s, const auto &ap) {
                                            //BBS: m_unselected instance is in the same partplate
                                            const bool exclusion_applies = !ap.is_bed_exclusion ||
                                                bed_exclusion_applies(fill_template, ap);
                                            return s + (ap.bed_idx == cur_plate_index && exclusion_applies) * ap.poly.area();
                                            //return s + (ap.bed_idx == 0) * ap.poly.area();
                                        }) / sc;

    double fixed_area = unsel_area + m_selected.size() * poly_area;
    double bed_area   = Polygon{m_bedpts}.area() / sc;

    // This is the maximum number of items, the real number will always be close but less.
    int needed_items = (bed_area - fixed_area) / poly_area;

    //int sel_id = m_plater->get_selection().get_instance_idx();
    // if the selection is not a single instance, choose the first as template
    //sel_id = std::max(sel_id, 0);
    ModelInstance *mi = model_object->instances[sel_id];
    ArrangePolygon template_ap = get_instance_arrange_poly(mi, global_config);
    set_arrange_polygon_bed_exclusion_extruders(template_ap, *model_object, *plate, global_config);

    for (int i = 0; i < needed_items; ++i) {
        ArrangePolygon ap = template_ap;
        ap.poly = m_selected.front().poly;
        ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
        ap.itemid = -1;
        ap.setter = [this, mi](const ArrangePolygon &p) {
            ModelObject *mo = m_plater->model().objects[m_object_idx];
            if (m_instances) {
                ModelInstance *new_instance = mo->add_instance(*mi);
                new_instance->apply_arrange_result(p.translation.cast<double>(), p.rotation);
            } else {
                ModelObject* newObj = m_plater->model().add_object(*mo);
                newObj->name = mo->name +" "+ std::to_string(p.itemid);
                for (ModelInstance *new_instance : newObj->instances)
                    new_instance->apply_arrange_result(p.translation.cast<double>(), p.rotation);
            }
            //m_plater->sidebar().obj_list()->paste_objects_into_list({m_plater->model().objects.size()-1});
        };
        m_selected.emplace_back(ap);
    }

    m_status_range = m_selected.size();

    // The strides have to be removed from the fixed items. For the
    // arrangeable (selected) items bed_idx is ignored and the
    // translation is irrelevant.
    //BBS: remove logic for unselected object
    /*double stride = bed_stride(m_plater);
    for (auto &p : m_unselected)
        if (p.bed_idx > 0)
            p.translation(X) -= p.bed_idx * stride;*/
}

void FillBedJob::process(Ctl &ctl)
{
    auto statustxt = _u8L("Filling");
    ctl.call_on_main_thread([this] { prepare(); }).wait();
    ctl.update_status(0, statustxt);

    if (m_object_idx == -1 || m_selected.empty()) return;

    update_arrange_params(params, m_plater->config(), m_selected);
    m_bedpts = get_shrink_bedpts(m_plater->config(), params);

    auto &partplate_list               = m_plater->get_partplate_list();
    auto &print                        = wxGetApp().plater()->get_partplate_list().get_current_fff_print();
    const Slic3r::DynamicPrintConfig& global_config = wxGetApp().preset_bundle->full_config();
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    const bool is_bbl = wxGetApp().preset_bundle->is_bbl_vendor();
    if (is_bbl && params.avoid_extrusion_cali_region && global_config.opt_bool("scan_first_layer"))
        partplate_list.preprocess_nonprefered_areas(m_unselected, MAX_NUM_PLATES);

    update_selected_items_inflation(m_selected, m_plater->config(), params);
    update_unselected_items_inflation(m_unselected, m_plater->config(), params);

    bool do_stop = false;
    params.stopcondition = [&ctl, &do_stop]() {
        return ctl.was_canceled() || do_stop;
    };

    params.progressind = [this, &ctl, &statustxt](unsigned st,std::string str="") {
         if (st > 0)
             ctl.update_status(st * 100 / status_range(), statustxt + " " + str);
    };

    params.on_packed = [&do_stop] (const ArrangePolygon &ap) {
        do_stop = ap.bed_idx > 0 && ap.priority == 0;
    };
    // final align用的是凸包，在有fixed item的情况下可能找到的参考点位置是错的，这里就不做了。见STUDIO-3265
    params.do_final_align = !is_bbl;
    if (has_bed_exclusion_regions(params.excluded_regions))
        params.do_final_align = false;

    if (m_selected.size() > 100) {
        // too many items, just find grid empty cells to put them
        std::vector<BoundingBoxf> occupied_boxes;
        for (const ArrangePolygon &fixed : m_unselected) {
            if (fixed.bed_idx != 0 || fixed.is_extrusion_cali_object)
                continue;

            if (fixed.is_bed_exclusion) {
                const bool relevant = std::any_of(
                    m_selected.begin(), m_selected.end(), [&fixed](const ArrangePolygon &item) {
                        return bed_exclusion_applies(item, fixed);
                    });
                if (!relevant)
                    continue;
            }

            BoundingBox bbox = get_extents(fixed.transformed_poly());
            bbox.offset(std::max<coord_t>(0, fixed.inflation));
            occupied_boxes.emplace_back(unscale(bbox.min), unscale(bbox.max));
        }

        // Fill the remaining space without disturbing instances which are
        // already placed. Besides matching the meaning of "Fill bed", this
        // also prevents candidates rejected by the grid from remaining under
        // newly placed copies at their old positions.
        const Vec3d plate_origin = partplate_list.get_curr_plate()->get_origin();
        for (const ArrangePolygon &existing : m_selected) {
            if (existing.priority == 0 || existing.bed_idx != 0)
                continue;

            BoundingBox bbox = get_extents(existing.transformed_poly());
            bbox.offset(std::max<coord_t>(0, existing.inflation));
            BoundingBoxf local_box(unscale(bbox.min), unscale(bbox.max));
            local_box.translate(Vec2d(-plate_origin.x(), -plate_origin.y()));
            occupied_boxes.emplace_back(std::move(local_box));
        }

        const Vec2f step = unscaled<float>(get_extents(m_selected.front().poly).size()) +
                           Vec2f(m_selected.front().brim_width, m_selected.front().brim_width);
        const bool has_conditional_exclusions = has_bed_exclusion_regions(m_unselected);
        const std::vector<Vec2f> empty_cells =
            Plater::get_empty_cells(step, occupied_boxes, !has_conditional_exclusions);

        size_t cell_index = 0;
        for (ArrangePolygon &item : m_selected) {
            if (item.priority != 0)
                continue;

            if (cell_index < empty_cells.size()) {
                item.translation = scaled<coord_t>(empty_cells[cell_index++]);
                item.bed_idx = 0;
            } else {
                item.bed_idx = arrangement::UNARRANGED;
            }
        }

        invalidate_bed_exclusion_conflicts(
            m_selected, m_unselected, Vec2crd(scale_(plate_origin.x()), scale_(plate_origin.y())));
    } else {
        arrangement::arrange(m_selected, m_unselected, m_bedpts, params);
        invalidate_bed_exclusion_conflicts(m_selected, m_unselected);
    }

    // finalize just here.
    ctl.update_status(100, ctl.was_canceled() ?
                                       _u8L("Bed filling canceled.") :
                                       _u8L("Bed filling done."));
}

FillBedJob::FillBedJob(bool instances) : m_plater{wxGetApp().plater()}, m_instances{instances} {}

void FillBedJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    // Ignore the arrange result if aborted.
    if (canceled || eptr)
        return;

    if (m_object_idx == -1) return;

    ModelObject *model_object = m_plater->model().objects[m_object_idx];
    if (model_object->instances.empty()) return;

    //BBS: partplate
    PartPlateList& plate_list = m_plater->get_partplate_list();
    int plate_cols = plate_list.get_plate_cols();
    int cur_plate = plate_list.get_curr_plate_index();

    size_t inst_cnt = model_object->instances.size();

    int added_cnt = std::accumulate(m_selected.begin(), m_selected.end(), 0, [](int s, auto &ap) {
        return s + int(ap.priority == 0 && ap.bed_idx == 0);
    });

    int oldSize = m_plater->model().objects.size();

    if (added_cnt > 0) {
        //BBS: adjust the selected instances
        for (ArrangePolygon& ap : m_selected) {
            if (ap.bed_idx != 0) {
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":skipped: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y));
                /*if (ap.itemid == -1)*/
                    continue;
                ap.bed_idx = plate_list.get_plate_count();
            }
            else
                ap.bed_idx = cur_plate;

            if (m_selected.size() <= 100) {
                ap.row = ap.bed_idx / plate_cols;
                ap.col = ap.bed_idx % plate_cols;
                ap.translation(X) += bed_stride_x(m_plater) * ap.col;
                ap.translation(Y) -= bed_stride_y(m_plater) * ap.row;
            }

            ap.apply();

            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":selected: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y));
        }

        int   newSize = m_plater->model().objects.size();
        auto obj_list = m_plater->sidebar().obj_list();
        for (size_t i = oldSize; i < newSize; i++) {
            obj_list->add_object_to_list(i, true, true, false);
            obj_list->update_printable_state(i, 0);
        }

        if (m_instances && model_object->instances.size() > inst_cnt) {
            const size_t new_instance_count = model_object->instances.size() - inst_cnt;
            // Match the normal add-instance flow: update the scene before the
            // object tree starts referring to the newly created instances.
            m_plater->update();
            // A single-instance object has no instance children in the object
            // tree yet, so the first refresh must add the original as well.
            obj_list->increase_object_instances(
                m_object_idx, inst_cnt == 1 ? new_instance_count + 1 : new_instance_count);
        }

        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ": paste_objects_into_list";

        /*for (ArrangePolygon& ap : m_selected) {
            if (ap.bed_idx != arrangement::UNARRANGED && (ap.priority != 0 || ap.bed_idx == 0))
                ap.apply();
        }*/

        //model_object->ensure_on_bed();
        //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ": model_object->ensure_on_bed()";

        // FillBedJob has already computed and validated the final positions.
        // The instance tree is synchronized above, so rerunning the general
        // arranger here would only risk discarding a valid fill result.
        m_plater->update();
    }

    m_plater->mark_plate_toolbar_image_dirty();
}

}} // namespace Slic3r::GUI
