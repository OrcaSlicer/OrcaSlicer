#include "ArrangeJob.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/SVG.hpp"
#include "libslic3r/MTUtils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ModelArrange.hpp"

#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"

#include "libnest2d/common.hpp"

#define SAVE_ARRANGE_POLY 0

namespace Slic3r { namespace GUI {
    using ArrangePolygon = arrangement::ArrangePolygon;

// Cache the wti info
class WipeTower: public GLCanvas3D::WipeTowerInfo {
public:
    explicit WipeTower(const GLCanvas3D::WipeTowerInfo &wti)
        : GLCanvas3D::WipeTowerInfo(wti)
    {}

    explicit WipeTower(GLCanvas3D::WipeTowerInfo &&wti)
        : GLCanvas3D::WipeTowerInfo(std::move(wti))
    {}

    void apply_arrange_result(const Vec2d& tr, double rotation, int item_id)
    {
        m_pos = unscaled(tr); m_rotation = rotation;
        apply_wipe_tower();
    }

    ArrangePolygon get_arrange_polygon() const
    {
        Polygon ap({
            {scaled(m_bb.min)},
            {scaled(m_bb.max.x()), scaled(m_bb.min.y())},
            {scaled(m_bb.max)},
            {scaled(m_bb.min.x()), scaled(m_bb.max.y())}
            });

        ArrangePolygon ret;
        ret.poly.contour = std::move(ap);
        ret.translation  = scaled(m_pos);
        ret.rotation     = m_rotation;
        //BBS
        ret.name = "WipeTower";
        ret.is_virt_object = true;
        ret.is_wipe_tower = true;
        ++ret.priority;

        BOOST_LOG_TRIVIAL(debug) << " arrange: wipe tower info:" << m_bb << ", m_pos: " << m_pos.transpose();

        return ret;
    }
};

// BBS: add partplate logic
static WipeTower get_wipe_tower(const Plater &plater, int plate_idx)
{
    return WipeTower{plater.canvas3D()->get_wipe_tower_info(plate_idx)};
}

arrangement::ArrangePolygon get_wipetower_arrange_poly(WipeTower* tower)
{
    ArrangePolygon ap = tower->get_arrange_polygon();
    ap.bed_idx = 0;
    ap.setter = NULL; // do not move wipe tower
    return ap;
}

void ArrangeJob::clear_input()
{
    const Model &model = m_plater->model();

    size_t count = 0, cunprint = 0; // To know how much space to reserve
    for (auto obj : model.objects)
        for (auto mi : obj->instances)
            mi->printable ? count++ : cunprint++;

    params.nonprefered_regions.clear();
    m_selected.clear();
    m_unselected.clear();
    m_unprintable.clear();
    m_locked.clear();
    m_unarranged.clear();
    m_uncompatible_plates.clear();
    m_selected.reserve(count + 1 /* for optional wti */);
    m_unselected.reserve(count + 1 /* for optional wti */);
    m_unprintable.reserve(cunprint /* for optional wti */);
    m_locked.reserve(count + 1 /* for optional wti */);
    current_plate_index = 0;
}

ArrangePolygon ArrangeJob::prepare_arrange_polygon(void* model_instance)
{
    ModelInstance* instance = (ModelInstance*)model_instance;
    const Slic3r::DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
    return get_instance_arrange_poly(instance, config);
}

void ArrangeJob::prepare_selected() {
    PartPlateList& plate_list = m_plater->get_partplate_list();

    clear_input();

    Model& model = m_plater->model();
    bool selected_is_locked = false;
    //BBS: remove logic for unselected object
    //double stride = bed_stride_x(m_plater);

    std::vector<const Selection::InstanceIdxsList*>
        obj_sel(model.objects.size(), nullptr);

    for (auto& s : m_plater->get_selection().get_content())
        if (s.first < int(obj_sel.size()))
            obj_sel[size_t(s.first)] = &s.second;

    // Go through the objects and check if inside the selection
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx) {
        const Selection::InstanceIdxsList* instlist = obj_sel[oidx];
        ModelObject* mo = model.objects[oidx];

        std::vector<bool> inst_sel(mo->instances.size(), false);

        if (instlist)
            for (auto inst_id : *instlist)
                inst_sel[size_t(inst_id)] = true;

        for (size_t i = 0; i < inst_sel.size(); ++i) {
            ModelInstance* mi = mo->instances[i];
            ArrangePolygon&& ap = prepare_arrange_polygon(mo->instances[i]);
            //BBS: partplate_list preprocess
            //remove the locked plate's instances, neither in selected, nor in un-selected
            bool locked = plate_list.preprocess_arrange_polygon(oidx, i, ap, inst_sel[i]);
            if (!locked)
                {
                ArrangePolygons& cont = mo->instances[i]->printable ?
                    (inst_sel[i] ? m_selected :
                        m_unselected) :
                    m_unprintable;

                ap.itemid = cont.size();
                cont.emplace_back(std::move(ap));
                }
            else
                {
                //skip this object due to be locked in plate
                ap.itemid = m_locked.size();
                m_locked.emplace_back(std::move(ap));
                if (inst_sel[i])
                    selected_is_locked = true;
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": skip locked instance, obj_id %1%, instance_id %2%, name %3%") % oidx % i % mo->name;
                }
            }
        }


    // If the selection was empty arrange everything
    //if (m_selected.empty()) m_selected.swap(m_unselected);
    if (m_selected.empty()) {
        if (!selected_is_locked)
            m_selected.swap(m_unselected);
        else {
            m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
                NotificationManager::NotificationLevel::WarningNotificationLevel, into_u8(_L("All the selected objects are on a locked plate.\nCannot auto-arrange these objects.")));
            }
        }

    prepare_wipe_tower();


    // The strides have to be removed from the fixed items. For the
    // arrangeable (selected) items bed_idx is ignored and the
    // translation is irrelevant.
    //BBS: remove logic for unselected object
    //for (auto &p : m_unselected) p.translation(X) -= p.bed_idx * stride;
}

void ArrangeJob::prepare_all() {
    clear_input();

    PartPlateList& plate_list = m_plater->get_partplate_list();    
    for (size_t i = 0; i < plate_list.get_plate_count(); i++) {
        PartPlate* plate = plate_list.get_plate(i);
        bool same_as_global_print_seq = true;
        plate->get_real_print_seq(&same_as_global_print_seq);
        if (plate->is_locked() == false && !same_as_global_print_seq) {
            plate->lock(true);
            m_uncompatible_plates.push_back(i);
        }
    }


    Model &model = m_plater->model();
    bool selected_is_locked = false;

    // Go through the objects and check if inside the selection
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx) {
        ModelObject *mo = model.objects[oidx];

        for (size_t i = 0; i < mo->instances.size(); ++i) {
            ModelInstance * mi = mo->instances[i];
            ArrangePolygon&& ap = prepare_arrange_polygon(mo->instances[i]);
            //BBS: partplate_list preprocess
            //remove the locked plate's instances, neither in selected, nor in un-selected
            bool locked = plate_list.preprocess_arrange_polygon(oidx, i, ap, true);
            if (!locked)
            {
                ArrangePolygons& cont = mo->instances[i]->printable ? m_selected :m_unprintable;

                ap.itemid = cont.size();
                cont.emplace_back(std::move(ap));
            }
            else
            {
                //skip this object due to be locked in plate
                ap.itemid = m_locked.size();
                m_locked.emplace_back(std::move(ap));
                selected_is_locked = true;
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": skip locked instance, obj_id %1%, instance_id %2%") % oidx % i;
            }
        }
    }


    // If the selection was empty arrange everything
    //if (m_selected.empty()) m_selected.swap(m_unselected);
    if (m_selected.empty()) {
        if (!selected_is_locked) {
            m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
                NotificationManager::NotificationLevel::WarningNotificationLevel, into_u8(_L("No arrangeable objects are selected.")));
        }
        else {
            m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
                NotificationManager::NotificationLevel::WarningNotificationLevel, into_u8(_L("All the selected objects are on a locked plate.\nCannot auto-arrange these objects.")));
        }
    }

    prepare_wipe_tower();

    const DynamicPrintConfig& current_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    bool   enable_wrapping = current_config.option<ConfigOptionBool>("enable_wrapping_detection")->value;

    // add the virtual object into unselect list if has
    plate_list.preprocess_exclude_areas(m_unselected, enable_wrapping, MAX_NUM_PLATES);
}

arrangement::ArrangePolygon estimate_wipe_tower_info(int plate_index, std::set<int>& extruder_ids)
{
    PartPlateList& ppl = wxGetApp().plater()->get_partplate_list();
    const auto& full_config = wxGetApp().preset_bundle->full_config();
    int plate_count = ppl.get_plate_count();
    int plate_index_valid = std::min(plate_index, plate_count - 1);

    // we have to estimate the depth using the extruder number of all plates
    int extruder_size = extruder_ids.size();

    Vec3d wipe_tower_size, wipe_tower_pos;
    int nozzle_nums = wxGetApp().preset_bundle->get_printer_extruder_count();
    auto arrange_poly = ppl.get_plate(plate_index_valid)->estimate_wipe_tower_polygon(full_config, plate_index, wipe_tower_pos, wipe_tower_size, nozzle_nums, extruder_size);
    arrange_poly.bed_idx = plate_index;
    return arrange_poly;
}

// 准备料塔。逻辑如下：
// 1. 以下几种情况不需要料塔：
//    1）料塔被禁用，
//    2）逐件打印，
//    3）不允许不同材料落在相同盘，且没有多色对象
// 2. 以下情况需要料塔：
//    1）某对象是多色对象；
//    2）打开了支撑，且支撑体与接触面使用的是不同材料
//    3）允许不同材料落在相同盘，且所有选定对象中使用了多种热床温度相同的材料
//     （所有对象都是单色的，但不同对象的材料不同，例如：对象A使用红色PLA，对象B使用白色PLA）
void ArrangeJob::prepare_wipe_tower()
{
    bool need_wipe_tower = false;

    // if wipe tower is explicitly disabled, no need to estimate
    DynamicPrintConfig& current_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto                op = current_config.option("enable_prime_tower");
    bool enable_prime_tower = op && op->getBool();
    if (!enable_prime_tower || params.is_seq_print) return;

    bool smooth_timelapse = false;
    auto sop = current_config.option("timelapse_type");
    if (sop) { smooth_timelapse = sop->getInt() == TimelapseType::tlSmooth; }
    if (smooth_timelapse) { need_wipe_tower = true; }

    // estimate if we need wipe tower for all plates:
    // need wipe tower if some object has multiple extruders (has paint-on colors or support material)
    for (const auto& item : m_selected) {
        std::set<int> obj_extruders;
        obj_extruders.insert(item.extrude_ids.begin(), item.extrude_ids.end());
        if (obj_extruders.size() > 1) {
            need_wipe_tower = true;
            BOOST_LOG_TRIVIAL(info) << "arrange: need wipe tower because object " << item.name << " has multiple extruders (has paint-on colors)";
            break;
        }
    }

    // if multile extruders have same bed temp, we need wipe tower
    // 允许不同材料落在相同盘，且所有选定对象中使用了多种热床温度相同的材料
    if (params.allow_multi_materials_on_same_plate) {
        std::map<int, std::set<int>> bedTemp2extruderIds;
        for (const auto& item : m_selected)
            for (auto id : item.extrude_ids) { bedTemp2extruderIds[item.bed_temp].insert(id); }
        for (const auto& be : bedTemp2extruderIds) {
            if (be.second.size() > 1) {
                need_wipe_tower = true;
                BOOST_LOG_TRIVIAL(info) << "arrange: need wipe tower because allow_multi_materials_on_same_plate=true and we have multiple extruders of same type";
                break;
            }
        }
    }
    BOOST_LOG_TRIVIAL(info) << "arrange: need_wipe_tower=" << need_wipe_tower;


    ArrangePolygon    wipe_tower_ap;
    wipe_tower_ap.name = "WipeTower";
    wipe_tower_ap.is_virt_object = true;
    wipe_tower_ap.is_wipe_tower = true;
    const GLCanvas3D* canvas3D = static_cast<const GLCanvas3D*>(m_plater->canvas3D());

    std::set<int> extruder_ids;
    PartPlateList& ppl = wxGetApp().plater()->get_partplate_list();
    int plate_count = ppl.get_plate_count();
    if (!only_on_partplate) {
        extruder_ids = ppl.get_extruders(true);
    }

    int bedid_unlocked = 0;
    for (int bedid = 0; bedid < MAX_NUM_PLATES; bedid++) {
        int plate_index_valid = std::min(bedid, plate_count - 1);
        PartPlate* pl = ppl.get_plate(plate_index_valid);
        if(bedid<plate_count && pl->is_locked())
            continue;
        if (auto wti = get_wipe_tower(*m_plater, bedid)) {
            // wipe tower is already there
            wipe_tower_ap = get_wipetower_arrange_poly(&wti);
            wipe_tower_ap.bed_idx = bedid_unlocked;
            m_unselected.emplace_back(wipe_tower_ap);
        }
        else if (need_wipe_tower) {
            if (only_on_partplate) {
                auto plate_extruders = pl->get_extruders(true);
                extruder_ids.clear();
                extruder_ids.insert(plate_extruders.begin(), plate_extruders.end());
            }
            wipe_tower_ap = estimate_wipe_tower_info(bedid, extruder_ids);
            wipe_tower_ap.bed_idx = bedid_unlocked;
            m_unselected.emplace_back(wipe_tower_ap);
        }
        bedid_unlocked++;
    }
}


//BBS: prepare current part plate for arranging
void ArrangeJob::prepare_partplate() {
    clear_input();

    PartPlateList& plate_list = m_plater->get_partplate_list();
    PartPlate* plate = plate_list.get_curr_plate();
    current_plate_index = plate_list.get_curr_plate_index();
    assert(plate != nullptr);

    if (plate->empty())
    {
        //no instances on this plate
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": no instances in current plate!");

        return;
    }

    if (plate->is_locked()) {
        m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
            NotificationManager::NotificationLevel::WarningNotificationLevel, into_u8(_L("This plate is locked.\nCannot auto-arrange on this plate.")));
        return;
    }

    Model& model = m_plater->model();

    // Go through the objects and check if inside the selection
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx)
    {
        ModelObject* mo = model.objects[oidx];
        for (size_t inst_idx = 0; inst_idx < mo->instances.size(); ++inst_idx)
        {
            bool             in_plate = plate->contain_instance(oidx, inst_idx) || plate->intersect_instance(oidx, inst_idx);
            ArrangePolygon&& ap = prepare_arrange_polygon(mo->instances[inst_idx]);

            ArrangePolygons& cont = mo->instances[inst_idx]->printable ?
                (in_plate ? m_selected : m_unselected) :
                m_unprintable;
            bool locked = plate_list.preprocess_arrange_polygon_other_locked(oidx, inst_idx, ap, in_plate);
            if (!locked)
            {
                ap.itemid = cont.size();
                cont.emplace_back(std::move(ap));
            }
            else
            {
                //skip this object due to be not in current plate, treated as locked
                ap.itemid = m_locked.size();
                m_locked.emplace_back(std::move(ap));
                //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": skip locked instance, obj_id %1%, name %2%") % oidx % mo->name;
            }
        }
    }

    // BBS
    if (auto wti = get_wipe_tower(*m_plater, current_plate_index)) {
        ArrangePolygon&& ap = get_wipetower_arrange_poly(&wti);
        m_unselected.emplace_back(std::move(ap));
    }

    const DynamicPrintConfig &current_config  = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    bool   enable_wrapping = current_config.option<ConfigOptionBool>("enable_wrapping_detection")->value;

    // add the virtual object into unselect list if has
    plate_list.preprocess_exclude_areas(m_unselected, enable_wrapping, current_plate_index + 1);
}

//BBS: add partplate logic
void ArrangeJob::prepare()
{
    m_plater->get_notification_manager()->push_notification(NotificationType::ArrangeOngoing,
        NotificationManager::NotificationLevel::RegularNotificationLevel, _u8L("Arranging..."));
    m_plater->get_notification_manager()->bbl_close_plateinfo_notification();

    params = init_arrange_params(m_plater);

    //BBS update extruder params and speed table before arranging
    const Slic3r::DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
    auto& print = wxGetApp().plater()->get_partplate_list().get_current_fff_print();
    auto print_config = print.config();
    int numExtruders = wxGetApp().preset_bundle->filament_presets.size();

    Model::setExtruderParams(config, numExtruders);
    Model::setPrintSpeedTable(config, print_config);

    int state = m_plater->get_prepare_state();
    if (state == Job::JobPrepareState::PREPARE_STATE_DEFAULT) {
        only_on_partplate = false;
        prepare_all();
    }
    else if (state == Job::JobPrepareState::PREPARE_STATE_MENU) {
        only_on_partplate = true;   // only arrange items on current plate
        prepare_partplate();
    }


#if SAVE_ARRANGE_POLY
    if (1)
    { // subtract excluded region and get a polygon bed
        auto& print = wxGetApp().plater()->get_partplate_list().get_current_fff_print();
        auto print_config = print.config();
        bed_poly.points = get_bed_shape(*m_plater->config());
        Polygons exclude_polys = get_bed_excluded_area(print_config);
        bed_poly = diff({ bed_poly }, exclude_polys)[0];
    }

    BoundingBox bbox = bed_poly.bounding_box();
    Point center = bbox.center();
    auto polys_to_draw = m_selected;
    for (auto it = polys_to_draw.begin(); it != polys_to_draw.end(); it++) {
        it->poly.translate(center);
        bbox.merge(it->poly);
    }
    SVG svg("SVG/arrange_poly.svg", bbox);
    if (svg.is_opened()) {
        svg.draw_outline(bed_poly);
        //svg.draw_grid(bbox, "gray", scale_(0.05));
        std::vector<std::string> color_array = { "red","black","yellow","gree","blue" };
        for (auto it = polys_to_draw.begin(); it != polys_to_draw.end(); it++) {
            std::string color = color_array[(it - polys_to_draw.begin()) % color_array.size()];
            svg.add_comment(it->name);
            svg.draw_text(get_extents(it->poly).min, it->name.c_str(), color.c_str());
            svg.draw_outline(it->poly, color);
        }
    }
#endif

    check_unprintable();
}

void ArrangeJob::check_unprintable()
{
    for (auto it = m_selected.begin(); it != m_selected.end();) {
        if (it->poly.area() < 0.001 || it->height>params.printable_height)
        {
#if SAVE_ARRANGE_POLY
            SVG svg(data_dir() + "/SVG/arrange_unprintable_"+it->name+".svg", get_extents(it->poly));
            if (svg.is_opened())
                svg.draw_outline(it->poly);
#endif
            if (it->poly.area() < 0.001) {
                auto msg = (boost::format(
                    _utf8("Object %s has zero size and can't be arranged."))
                    % _utf8(it->name)).str();
                m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
                    NotificationManager::NotificationLevel::WarningNotificationLevel, msg);
            }
            m_unprintable.push_back(*it);
            it = m_selected.erase(it);
        }
        else
            it++;
    }
}

void ArrangeJob::process(Ctl &ctl)
{
    static const auto arrangestr = _u8L("Arranging");
    ctl.update_status(0, arrangestr);
    ctl.call_on_main_thread([this]{ prepare(); }).wait();;

    auto & partplate_list = m_plater->get_partplate_list();

    const Slic3r::DynamicPrintConfig& global_config = wxGetApp().preset_bundle->full_config();
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    const bool is_bbl = wxGetApp().preset_bundle->is_bbl_vendor();
    if (is_bbl && params.avoid_extrusion_cali_region && global_config.opt_bool("scan_first_layer"))
        partplate_list.preprocess_nonprefered_areas(m_unselected, MAX_NUM_PLATES);

    update_arrange_params(params, m_plater->config(), m_selected);
    update_selected_items_inflation(m_selected, m_plater->config(), params);
    update_unselected_items_inflation(m_unselected, m_plater->config(), params);
    update_selected_items_axis_align(m_selected, m_plater->config(), params);

    Points      bedpts = get_shrink_bedpts(m_plater->config(),params);

    bool   enable_wrapping = global_config.option<ConfigOptionBool>("enable_wrapping_detection")->value;
    partplate_list.preprocess_exclude_areas(params.excluded_regions, enable_wrapping, 1, scale_(1));

    BOOST_LOG_TRIVIAL(debug) << "arrange bedpts:" << bedpts[0].transpose() << ", " << bedpts[1].transpose() << ", " << bedpts[2].transpose() << ", " << bedpts[3].transpose();

    params.stopcondition = [&ctl]() { return ctl.was_canceled(); };

    params.progressind = [this, &ctl](unsigned num_finished, std::string str = "") {
        ctl.update_status(num_finished * 100 / status_range(), _u8L("Arranging") + str);
    };

    {
        BOOST_LOG_TRIVIAL(warning)<< "Arrange full params: "<< params.to_json();
        BOOST_LOG_TRIVIAL(info) << boost::format("arrange: items selected before arranging: %1%") % m_selected.size();
        for (auto selected : m_selected) {
            BOOST_LOG_TRIVIAL(debug) << selected.name << ", extruder: " << selected.extrude_ids.back() << ", bed: " << selected.bed_idx << ", filemant_type:" << selected.filament_temp_type
                << ", trans: " << selected.translation.transpose();
        }
        BOOST_LOG_TRIVIAL(debug) << "arrange: items unselected before arrange: " << m_unselected.size();
        for (auto item : m_unselected)
            BOOST_LOG_TRIVIAL(debug) << item.name << ", bed: " << item.bed_idx << ", trans: " << item.translation.transpose()
            <<", bbox:"<<get_extents(item.poly).min.transpose()<<","<<get_extents(item.poly).max.transpose();
    }

    arrangement::arrange(m_selected, m_unselected, bedpts, params);

    // sort by item id
    std::sort(m_selected.begin(), m_selected.end(), [](auto a, auto b) {return a.itemid < b.itemid; });
    {
        BOOST_LOG_TRIVIAL(info) << boost::format("arrange: items selected after arranging: %1%") % m_selected.size();
        for (auto selected : m_selected)
            BOOST_LOG_TRIVIAL(debug) << selected.name << ", extruder: " << selected.extrude_ids.back() << ", bed: " << selected.bed_idx
                                     << ", bed_temp: " << selected.first_bed_temp << ", print_temp: " << selected.print_temp
                                     << ", trans: " << unscale<double>(selected.translation(X)) << ","<< unscale<double>(selected.translation(Y));
        BOOST_LOG_TRIVIAL(debug) << "arrange: items unselected after arrange: "<< m_unselected.size();
        for (auto item : m_unselected)
            BOOST_LOG_TRIVIAL(debug) << item.name << ", bed: " << item.bed_idx << ", trans: " << item.translation.transpose();
    }

    // put unpackable items to m_unprintable so they goes outside
    bool we_have_unpackable_items = false;
    for (auto item : m_selected) {
        if (item.bed_idx < 0) {
            //BBS: already processed in m_selected
            //m_unprintable.push_back(std::move(item));
            we_have_unpackable_items = true;
        }
    }

    // finalize just here.
    ctl.update_status(100,
        ctl.was_canceled() ? _u8L("Arranging canceled.") :
        we_have_unpackable_items ? _u8L("Arranging is done but there are unpacked items. Reduce spacing and try again.") : _u8L("Arranging done."));
}

ArrangeJob::ArrangeJob() : m_plater{wxGetApp().plater()} { }

static std::string concat_strings(const std::set<std::string> &strings,
                                  const std::string &delim = "\n")
{
    return std::accumulate(
        strings.begin(), strings.end(), std::string(""),
        [delim](const std::string &s, const std::string &name) {
            return s + name + delim;
        });
}

void ArrangeJob::finalize(bool canceled, std::exception_ptr &eptr) {
    try {
        if (eptr)
            std::rethrow_exception(eptr);
    } catch (libnest2d::GeometryException &) {
        show_error(m_plater, _(L("Arrange failed. "
                                 "Found some exceptions when processing object geometries.")));
        eptr = nullptr;
    } catch (...) {
        eptr = std::current_exception();
    }

    if (canceled || eptr)
        return;

    // Unprintable items go to the last virtual bed
    int beds = 0;

    //BBS: partplate
    PartPlateList& plate_list = m_plater->get_partplate_list();
    //clear all the relations before apply the arrangement results
    if (only_on_partplate) {
        plate_list.clear(false, false, true, current_plate_index);
    }
    else
        plate_list.clear(false, false, true, -1);
    //BBS: adjust the bed_index, create new plates, get the max bed_index
    for (ArrangePolygon& ap : m_selected) {
        //if (ap.bed_idx < 0) continue;  // bed_idx<0 means unarrangable
        //BBS: partplate postprocess
        if (only_on_partplate)
            plate_list.postprocess_bed_index_for_current_plate(ap);
        else
            plate_list.postprocess_bed_index_for_selected(ap);

        beds = std::max(ap.bed_idx, beds);

        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": arrange selected %4%: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y)) % ap.name;
    }

    //BBS: adjust the bed_index, create new plates, get the max bed_index
    for (ArrangePolygon& ap : m_unselected)
    {
        if (ap.is_virt_object)
            continue;

        //BBS: partplate postprocess
        if (!only_on_partplate)
            plate_list.postprocess_bed_index_for_unselected(ap);

        beds = std::max(ap.bed_idx, beds);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":arrange unselected %4%: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y)) % ap.name;
    }

    for (ArrangePolygon& ap : m_locked) {
        beds = std::max(ap.bed_idx, beds);

        plate_list.postprocess_arrange_polygon(ap, false);

        ap.apply();
    }

    // Apply the arrange result to all selected objects
    for (ArrangePolygon& ap : m_selected) {
        //BBS: partplate postprocess
        plate_list.postprocess_arrange_polygon(ap, true);

        ap.apply();
    }

    // Apply the arrange result to unselected objects(due to the sukodu-style column changes, the position of unselected may also be modified)
    for (ArrangePolygon& ap : m_unselected)
    {
        if (ap.is_virt_object)
            continue;

        //BBS: partplate postprocess
        plate_list.postprocess_arrange_polygon(ap, false);

        ap.apply();
    }

    // Move the unprintable items to the last virtual bed.
    // Note ap.apply() moves relatively according to bed_idx, so we need to subtract the orignal bed_idx
    for (ArrangePolygon& ap : m_unprintable) {
        ap.bed_idx = beds + 1;
        plate_list.postprocess_arrange_polygon(ap, true);

        ap.apply();
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":arrange m_unprintable: name: %4%, bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y)) % ap.name;
    }

    m_plater->update();
    // BBS
    //wxGetApp().obj_manipul()->set_dirty();

    if (!m_unarranged.empty()) {
        std::set<std::string> names;
        for (ModelInstance *mi : m_unarranged)
            names.insert(mi->get_object()->name);

        m_plater->get_notification_manager()->push_notification(GUI::format(
            _L("Arrangement ignored the following objects which can't fit into a single bed:\n%s"),
            concat_strings(names, "\n")));
    }
    m_plater->get_notification_manager()->close_notification_of_type(NotificationType::ArrangeOngoing);

    //BBS: reload all objects due to arrange
    if (only_on_partplate) {
        plate_list.rebuild_plates_after_arrangement(!only_on_partplate, true, current_plate_index);
    }
    else {
        plate_list.rebuild_plates_after_arrangement(!only_on_partplate, true);
    }

    // unlock the plates we just locked
    for (int i : m_uncompatible_plates)
        plate_list.get_plate(i)->lock(false);

    // BBS: update slice context and gcode result.
    m_plater->update_slicing_context_to_current_partplate();

    wxGetApp().obj_list()->reload_all_plates();

    m_plater->update();

    m_plater->m_arrange_running.store(false);
}

std::optional<arrangement::ArrangePolygon>
get_wipe_tower_arrangepoly(const Plater &plater)
{
    int id = plater.canvas3D()->fff_print()->get_plate_index();
    if (auto wti = get_wipe_tower(plater, id))
        return get_wipetower_arrange_poly(&wti);

    return {};
}

//BBS: add sudoku-style stride
double bed_stride_x(const Plater* plater) {
    double bedwidth = plater->build_volume().bounding_box().size().x();
    return (1. + LOGICAL_BED_GAP) * bedwidth;
}

double bed_stride_y(const Plater* plater) {
    double beddepth = plater->build_volume().bounding_box().size().y();
    return (1. + LOGICAL_BED_GAP) * beddepth;
}

// call before get selected and unselected
arrangement::ArrangeParams init_arrange_params(Plater *p)
{
    arrangement::ArrangeParams         params;
    GLCanvas3D::ArrangeSettings       &settings     = p->canvas3D()->get_arrange_settings();
    auto                              &print        = wxGetApp().plater()->get_partplate_list().get_current_fff_print();
    const PrintConfig                 &print_config = print.config();

    float object_skirt_offset = print.object_skirt_offset();

    params.clearance_height_to_rod             = print_config.extruder_clearance_height_to_rod.value;
    params.clearance_height_to_lid             = print_config.extruder_clearance_height_to_lid.value;
    params.clearance_radius                    = print_config.extruder_clearance_radius.value + object_skirt_offset * 2;
    params.object_skirt_offset                 = object_skirt_offset;
    params.printable_height                    = print_config.printable_height.value;
    params.allow_rotations                     = settings.enable_rotation;
    params.nozzle_height                       = print_config.nozzle_height.value;
    params.align_center                        = print_config.best_object_pos.value;
    params.allow_multi_materials_on_same_plate = settings.allow_multi_materials_on_same_plate;
    params.avoid_extrusion_cali_region         = settings.avoid_extrusion_cali_region;
    params.is_seq_print                        = settings.is_seq_print;
    params.min_obj_distance                    = scaled(settings.distance);
    params.align_to_y_axis                     = settings.align_to_y_axis;

    int state = p->get_prepare_state();
    if (state == Job::JobPrepareState::PREPARE_STATE_MENU) {
        PartPlateList &plate_list = p->get_partplate_list();
        PartPlate *    plate      = plate_list.get_curr_plate();
        bool plate_same_as_global = true;
        params.is_seq_print       = plate->get_real_print_seq(&plate_same_as_global) == PrintSequence::ByObject;
        // if plate's print sequence is not the same as global, the settings.distance is no longer valid, we set it to auto
        if (!plate_same_as_global)
            params.min_obj_distance = 0;
    }

    if (params.is_seq_print) {
        params.bed_shrink_x = BED_SHRINK_SEQ_PRINT;
        params.bed_shrink_y = BED_SHRINK_SEQ_PRINT;
    }
    return params;
}

}} // namespace Slic3r::GUI
