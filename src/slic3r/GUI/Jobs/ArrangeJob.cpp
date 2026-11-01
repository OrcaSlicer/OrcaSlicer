#include "ArrangeJob.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/SVG.hpp"
#include "libslic3r/MTUtils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/IMEXArrange.hpp"

#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include "libnest2d/common.hpp"
#include "libslic3r/Arrange.hpp"
#include <libslic3r/BoundingBox.hpp>
#include <utility>
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include <boost/log/trivial.hpp>
#include <cstddef>
#include <vector>
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include <set>
#include <algorithm>
#include "libslic3r/PrintConfig.hpp"
#include <map>
#include <atomic>
#include <wx/string.h>
#include <cassert>
#include "slic3r/GUI/Jobs/Job.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/libslic3r.h"
#include <numeric>
#include <boost/format.hpp>
#include <exception>
#include "libslic3r/LifecycleEvents.hpp"
#include <optional>
#include "libslic3r/Geometry.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

#define SAVE_ARRANGE_POLY 0

namespace Slic3r { namespace GUI {
    using ArrangePolygon = arrangement::ArrangePolygon;

void set_arrange_polygon_bed_exclusion_extruders(ArrangePolygon &polygon, const ModelObject &object,
                                                  const PartPlate &plate, const DynamicPrintConfig &config)
{
    polygon.bed_exclusion_extruder_ids.clear();
    if (active_bed_exclude_volume_mode(config) == BedExcludeVolumeMode::Shared)
        return;

    const ConfigOptionFloats *nozzle_diameters = config.opt<ConfigOptionFloats>("nozzle_diameter");
    const size_t extruder_count = nozzle_diameters != nullptr ? nozzle_diameters->size() : 0;
    if (extruder_count == 0)
        return;

    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return;

    const DynamicPrintConfig &project_config = preset_bundle->project_config;
    // Automatic Bambu mapping may be recalculated after arrangement or differ
    // between destination plates. Leave it unresolved so every nozzle volume
    // is checked rather than embedding a temporary assignment in the item.
    if (preset_bundle->is_bbl_vendor() &&
        is_auto_filament_map_mode(plate.get_real_filament_map_mode(project_config)))
        return;

    std::set<int> extruder_ids;
    for (const auto &mapping : plate.get_object_filament_extruders(object, project_config, extruder_count))
        extruder_ids.emplace(int(mapping.second));
    polygon.bed_exclusion_extruder_ids.assign(extruder_ids.begin(), extruder_ids.end());
}

static void set_arrange_polygon_bed_exclusion_extruders_for_all_plates(ArrangePolygon &polygon,
                                                                       const ModelObject &object,
                                                                       PartPlateList &plate_list,
                                                                       const DynamicPrintConfig &config)
{
    polygon.bed_exclusion_extruder_ids.clear();
    if (active_bed_exclude_volume_mode(config) == BedExcludeVolumeMode::Shared)
        return;

    std::set<int> extruder_ids;
    for (int plate_index = 0; plate_index < plate_list.get_plate_count(); ++plate_index) {
        ArrangePolygon mapped = polygon;
        set_arrange_polygon_bed_exclusion_extruders(mapped, object, *plate_list.get_plate(plate_index), config);
        if (mapped.bed_exclusion_extruder_ids.empty()) {
            polygon.bed_exclusion_extruder_ids.clear();
            return;
        }
        extruder_ids.insert(mapped.bed_exclusion_extruder_ids.begin(), mapped.bed_exclusion_extruder_ids.end());
    }
    polygon.bed_exclusion_extruder_ids.assign(extruder_ids.begin(), extruder_ids.end());
}

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

// The belt purge prism is generated from the arranged parts (ensure_belt_purge_tower),
// so arrange neither moves it nor packs around it; it reserves the prism's strip instead.
static bool is_belt_purge_prism(const ModelObject *mo)
{
    const ConfigOption *opt = mo->config.option("belt_purge_tower_object");
    return opt != nullptr && opt->getBool();
}

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

// Belt printers pack their parts against the edges of the bed, so what has to stay
// free there is reserved with fixed virtual items on every plate, the way the bed's
// own exclusion areas are:
// - the strip the purge prism comes back to, flush with the far lateral edge (see
//   ensure_belt_purge_tower), when the parts use more than one filament;
// - the brim along every edge: a belt brim is printed brim_width wide for every brim
//   type but none. Between parts the brims may overlap, as on any printer.
void ArrangeJob::prepare_belt_regions(int num_plates)
{
    if (!params.is_belt || params.is_seq_print)
        return;
    const DynamicPrintConfig &config    = wxGetApp().preset_bundle->full_config();
    const BoundingBoxf        bed       = get_extents(config.opt<ConfigOptionPoints>("printable_area")->values);
    const bool                belt_is_y = params.belt_axis == 1;
    std::vector<BoundingBoxf> regions;

    std::set<int> filaments;
    for (const ArrangePolygons *items : { &m_selected, &m_unselected })
        for (const ArrangePolygon &ap : *items)
            if (!ap.is_virt_object)
                filaments.insert(ap.extrude_ids.begin(), ap.extrude_ids.end());
    if (config.opt_bool("enable_belt_purge_tower") && filaments.size() > 1) {
        // The prism is at least one millimetre per island plus the gaps between them.
        const int    islands = int(filaments.size()) - 1;
        const double width   = std::max(config.opt_float("belt_purge_tower_width"), 2. * islands - 1.) + 1.; // + the prism's edge inset
        BoundingBoxf strip   = bed;
        if (belt_is_y)
            strip.min.x() = std::max(bed.min.x(), bed.max.x() - width);
        else
            strip.min.y() = std::max(bed.min.y(), bed.max.y() - width);
        regions.push_back(strip);
    }

    // Virtual items are inflated by the one millimetre exclusion gap already.
    const double brim = config.opt_enum<BrimType>("brim_type") == btNoBrim ? 0. :
        config.opt_float("brim_width") + config.opt_float("brim_object_gap") + config.opt_float("extra_brim_width") - 1.;
    if (brim > 0.) {
        regions.emplace_back(bed.min, Vec2d(bed.min.x() + brim, bed.max.y()));
        regions.emplace_back(Vec2d(bed.max.x() - brim, bed.min.y()), bed.max);
        regions.emplace_back(bed.min, Vec2d(bed.max.x(), bed.min.y() + brim));
        regions.emplace_back(Vec2d(bed.min.x(), bed.max.y() - brim), bed.max);
    }

    for (int j = 0; j < num_plates; ++j)
        for (size_t i = 0; i < regions.size(); ++i) {
            ArrangePolygon ap;
            ap.poly.contour   = scaled(regions[i]).polygon();
            ap.translation    = Vec2crd(0, 0);
            ap.rotation       = 0.f;
            ap.is_virt_object = true;
            ap.bed_idx        = j;
            ap.height         = 1;
            ap.name           = "BeltRegion" + std::to_string(i);
            m_unselected.emplace_back(std::move(ap));
        }
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

ArrangePolygon ArrangeJob::prepare_arrange_polygon(void *model_instance, const PartPlate *mapping_plate)
{
    ModelInstance* instance = (ModelInstance*)model_instance;
    const Slic3r::DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
    ArrangePolygon polygon = get_instance_arrange_poly(instance, config);
    if (mapping_plate != nullptr)
        set_arrange_polygon_bed_exclusion_extruders(polygon, *instance->get_object(), *mapping_plate, config);
    return polygon;
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
        if (is_belt_purge_prism(mo))
            continue;

        std::vector<bool> inst_sel(mo->instances.size(), false);

        if (instlist)
            for (auto inst_id : *instlist)
                inst_sel[size_t(inst_id)] = true;

        for (size_t i = 0; i < inst_sel.size(); ++i) {
            ModelInstance* mi = mo->instances[i];
            ArrangePolygon&& ap = prepare_arrange_polygon(mo->instances[i]);
            set_arrange_polygon_bed_exclusion_extruders_for_all_plates(
                ap, *mo, plate_list, wxGetApp().preset_bundle->full_config());
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
    prepare_belt_regions(MAX_NUM_PLATES);


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
        if (is_belt_purge_prism(mo))
            continue;

        for (size_t i = 0; i < mo->instances.size(); ++i) {
            ModelInstance * mi = mo->instances[i];
            ArrangePolygon&& ap = prepare_arrange_polygon(mo->instances[i]);
            set_arrange_polygon_bed_exclusion_extruders_for_all_plates(
                ap, *mo, plate_list, wxGetApp().preset_bundle->full_config());
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
    plate_list.preprocess_exclude_areas(m_unselected, wxGetApp().preset_bundle->full_config(), enable_wrapping, MAX_NUM_PLATES);
    prepare_belt_regions(MAX_NUM_PLATES);
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
    auto arrange_poly = ppl.get_plate(plate_index_valid)->estimate_wipe_tower_polygon(full_config, plate_index, wipe_tower_pos, wipe_tower_size, extruder_size);
    arrange_poly.bed_idx = plate_index;
    return arrange_poly;
}

// Smooth timelapse needs a prime tower on every plate, even with a single filament.
static bool wipe_tower_is_mandatory()
{
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    const auto *timelapse = cfg.option("timelapse_type");
    return timelapse && timelapse->getInt() == TimelapseType::tlSmooth;
}

// Single-filament parts that the main pass could not place next to a tower placeholder need
// no tower of their own: retry them on fresh plates, one plate group per filament.
static void retry_unfit_single_color_items(ArrangePolygons &selected, const ArrangePolygons &unselected,
                                           const Points &bedpts, const arrangement::ArrangeParams &params)
{
    // Without a tower in the way, the rejected parts really are too big.
    if (std::none_of(unselected.begin(), unselected.end(), [](const ArrangePolygon &ap) { return ap.is_wipe_tower; }))
        return;

    // Virtual items stay out: finalize() applies no result for them, so a bed here would only open empty plates.
    std::map<int, std::vector<size_t>> groups;
    for (size_t i = 0; i < selected.size(); ++i) {
        const ArrangePolygon &ap = selected[i];
        if (ap.is_virt_object || ap.bed_idx >= 0 || ap.extrude_ids.empty())
            continue;
        const std::set<int> filaments(ap.extrude_ids.begin(), ap.extrude_ids.end());
        if (filaments.size() == 1)
            groups[*filaments.begin()].push_back(i);
    }
    if (groups.empty())
        return;

    ArrangePolygons fallback_unselected;
    for (const ArrangePolygon &ap : unselected)
        if (!ap.is_wipe_tower)
            fallback_unselected.push_back(ap);

    int base_bed = 0; // first plate no part has taken
    for (const ArrangePolygon &ap : selected)
        if (ap.bed_idx >= 0)
            base_bed = std::max(base_bed, ap.bed_idx + 1);

    // The progress counter of the main pass must not jump back.
    arrangement::ArrangeParams fallback_params = params;
    fallback_params.progressind                = nullptr;

    for (const auto &[filament, indices] : groups) {
        ArrangePolygons group;
        for (size_t idx : indices) {
            group.push_back(selected[idx]);
            group.back().bed_idx = 0;
        }
        arrangement::arrange(group, fallback_unselected, bedpts, fallback_params);

        int next_base = base_bed;
        for (ArrangePolygon &ap : group) {
            if (ap.bed_idx < 0)
                continue; // still too big: stays outside
            ap.bed_idx += base_bed;
            if (ap.bed_idx >= MAX_NUM_PLATES)
                ap.bed_idx = arrangement::UNARRANGED;
            else
                next_base = std::max(next_base, ap.bed_idx + 1);
        }
        base_bed = next_base;

        // By index: arrange() overwrites itemid with the packing order.
        for (size_t k = 0; k < indices.size() && k < group.size(); ++k) {
            const int itemid = selected[indices[k]].itemid;
            selected[indices[k]] = group[k];
            selected[indices[k]].itemid = itemid;
        }
    }
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

    // Belt printers have no classic wipe tower; purging goes into the belt purge
    // prism, whose strip prepare_belt_regions() reserves.
    if (params.is_belt)
        return;

    if (wipe_tower_is_mandatory()) { need_wipe_tower = true; }

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
        if (is_belt_purge_prism(mo))
            continue;
        for (size_t inst_idx = 0; inst_idx < mo->instances.size(); ++inst_idx)
        {
            bool             in_plate = plate->contain_instance(oidx, inst_idx) || plate->intersect_instance(oidx, inst_idx);
            ArrangePolygon&& ap = prepare_arrange_polygon(mo->instances[inst_idx], plate);

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
    plate_list.preprocess_exclude_areas(
        m_unselected, wxGetApp().preset_bundle->full_config(), enable_wrapping, current_plate_index + 1);
    prepare_belt_regions(current_plate_index + 1);
}

// Snapshot each plate's IMEX zones on the main thread, so process() never has to touch
// PartPlate's IMEX cache; see the members' declaration for why that matters. The beds are the
// arranger's: arranging one plate packs into bed 0, and arranging all of them numbers the beds
// over the unlocked plates, as postprocess_bed_index_for_selected() maps them back.
void ArrangeJob::prepare_imex_zones()
{
    m_imex = {};
    PartPlateList& plate_list = m_plater->get_partplate_list();
    std::vector<PartPlate*> plates;
    if (only_on_partplate)
        plates.push_back(plate_list.get_curr_plate());
    else
        for (int i = 0; i < plate_list.get_plate_count(); ++i)
            if (!plate_list.get_plate(i)->is_locked())
                plates.push_back(plate_list.get_plate(i));

    for (PartPlate* plate : plates) {
        std::optional<ImexArrangeZones> zones;
        if (const std::optional<BoundingBoxf> pz = plate ? plate->imex_primary_zone() : std::nullopt) {
            const Vec2d o = plate->get_origin().head<2>();
            zones = ImexArrangeZones{BoundingBoxf(Vec2d(pz->min - o), Vec2d(pz->max - o)), {}};
            for (const BoundingBoxf3& strip : plate->imex_collision_zones())
                zones->collision_zones.emplace_back(Vec2d(strip.min.head<2>() - o), Vec2d(strip.max.head<2>() - o));
        }
        m_imex.beds.push_back(std::move(zones));
    }
    m_imex.adds_plates = !only_on_partplate;
    m_imex.read_config(wxGetApp().preset_bundle->full_config());
}

//BBS: add partplate logic
void ArrangeJob::prepare()
{
    m_plater->get_notification_manager()->push_notification(NotificationType::ArrangeOngoing,
        NotificationManager::NotificationLevel::RegularNotificationLevel, _u8L("Arranging..."));
    m_plater->get_notification_manager()->bbl_close_plateinfo_notification();

    params = init_arrange_params(m_plater);

    // The packer runs as a second pass after the stock arrange (pack_tighter()); the stock
    // arrange takes the convex hulls of the outlines collected for it.
    m_pack_eligible = params.use_sparrow;
    params.use_sparrow = false;
    m_packing          = false;
    m_stock_score      = -1;
    m_packed_score     = -1;
    m_stock_unplaced   = m_packed_unplaced = 0;

    // After the close above, or the toast is gone before it is seen.
    if (arrangement::sparrow_available() && m_plater->canvas3D()->get_arrange_settings().arrange_use_sparrow && !m_pack_eligible) {
        wxString why = params.is_seq_print ? _L("print sequence is By Object")
                                           : _L("the printer is a belt printer");
        m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
            NotificationManager::NotificationLevel::WarningNotificationLevel,
            into_u8(wxString::Format(_L("Experimental packer skipped: %s.\nUsing the standard arranger."), why)));
    }

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

    // After prepare_all(), which locks the plates it must leave alone.
    prepare_imex_zones();


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

    // The live view of the packing pass starts from what is on the plates now.
    m_stock_selected.clear();
    m_live_stock.clear();
    m_live_restore.clear();
    m_live_open = false;
    m_live_used = false;
    m_live_alive = std::make_shared<std::atomic<bool>>(true);
    if (m_pack_eligible) {
        m_live_aps = m_selected;
        for (ModelObject *mo : m_plater->model().objects)
            for (ModelInstance *mi : mo->instances)
                m_live_restore.emplace_back(mi, mi->get_transformation());
        DynamicConfig &proj_cfg = wxGetApp().preset_bundle->project_config;
        const auto    *wtx      = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x");
        const auto    *wty      = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y");
        m_live_tower_x   = wtx ? wtx->values : std::vector<double>();
        m_live_tower_y   = wty ? wty->values : std::vector<double>();
        m_live_tower_pos = m_plater->model().wipe_tower.positions;
    }
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
                    _u8L("Object %s has zero size and can't be arranged."))
                    % it->name).str();
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

int ArrangeJob::score(const ArrangePolygons &selected, const ArrangePolygons &unselected) const
{
    return arrangement::count_plates_used(selected, unselected, only_on_partplate);
}

int ArrangeJob::unplaced(const ArrangePolygons &aps) const
{
    return arrangement::count_unplaced(aps, only_on_partplate);
}

// Stock arrange packs convex hulls, sparrow true outlines, so the area the hulls waste is
// what sparrow can win back. Below 15% waste a second pass rarely saves a plate; above it,
// a wrong yes costs a visible, stoppable search, a wrong no a missed plate.
bool ArrangeJob::predict_better(int stock_score, double bed_area) const
{
    double outline = 0.;
    const double ratio = arrangement::outline_hull_ratio(m_selected, &outline);
    BOOST_LOG_TRIVIAL(info) << "arrange: " << "pack tighter check: stock score " << stock_score << ", unplaced " << m_stock_unplaced << ", outline/hull " << ratio;
    // A plate arrange that left parts off tries whenever their outlines could fit the plate:
    // a miss costs one plate's search.
    if (only_on_partplate)
        return stock_score >= 1 && outline <= bed_area;
    return (stock_score >= 2 || m_stock_unplaced > 0) && ratio < 0.85;
}

std::string ArrangeJob::done_message(bool canceled) const
{
    if (m_packed_score < 0)
        return canceled ? _u8L("Packing canceled. Kept the standard arrangement.")
                        : _u8L("No tighter layout found. Kept the standard arrangement.");
    if (m_packed_unplaced < m_stock_unplaced)
        return into_u8(GUI::format_wxstr(_L_PLURAL("Placed %1% more part.", "Placed %1% more parts.", m_stock_unplaced - m_packed_unplaced),
                                         m_stock_unplaced - m_packed_unplaced));
    // Relative to the standard arrange, which the user saw first.
    if (only_on_partplate)
        return m_packed_score == 0
            ? _u8L("Packed tighter: all parts fit on the plate.")
            : into_u8(GUI::format_wxstr(_L_PLURAL("Packed tighter: %1% part left off the plate.", "Packed tighter: %1% parts left off the plate.", m_packed_score),
                                        m_packed_score));
    return into_u8(GUI::format_wxstr(_L_PLURAL("Packed onto %1% plate instead of %2%.", "Packed onto %1% plates instead of %2%.", m_packed_score),
                                     m_packed_score, m_stock_score));
}

// Re-arranges `selected` (the input of the stock arrange) with sparrow and keeps its result
// in m_selected only if it places more parts or uses fewer plates than the stock one.
void ArrangeJob::pack_tighter(Ctl &ctl, ArrangePolygons &selected, const Points &bedpts)
{
    m_packing        = true;
    m_progress_value = 0;
    ctl.update_status(0, _u8L("Packing tighter..."));

    // Show the stock result first, held for a moment before sparrow's layouts replace it.
    // The stock arranger may reorder its items, so poses follow m_live_aps by item id.
    std::map<int, const ArrangePolygon *> stock_by_id;
    for (const ArrangePolygon &ap : m_selected)
        stock_by_id[ap.itemid] = &ap;
    std::vector<arrangement::ArrangeParams::Pose> stock;
    for (const ArrangePolygon &ap : m_live_aps) {
        const auto it = stock_by_id.find(ap.itemid);
        stock.push_back(it == stock_by_id.end() ? arrangement::ArrangeParams::Pose{ap.bed_idx, ap.translation, ap.rotation}
                                                : arrangement::ArrangeParams::Pose{it->second->bed_idx, it->second->translation, it->second->rotation});
    }
    const std::shared_ptr<std::atomic<bool>> alive = m_live_alive;
    wxGetApp().CallAfter([this, alive, stock] {
        if (!alive->load())
            return;
        m_live_open = true;
        m_live_stock = stock;
        apply_live(stock);
    });
    auto last = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now() + std::chrono::milliseconds(875));

    // Sparrow ignores the input bed of movable items, so one left unplaced means it failed.
    for (ArrangePolygon &ap : selected)
        if (!ap.is_virt_object) // prime towers stay pinned to their plates
            ap.bed_idx = arrangement::UNARRANGED;
    arrangement::ArrangeParams pack = params;
    pack.use_sparrow  = true;
    pack.sparrow_only = true;
    // Mixing never puts more filaments on a plate than stock's busiest one: if that
    // plate loads on this printer, so does any of sparrow's.
    std::map<int, std::set<int>> plate_filaments;
    for (const ArrangePolygon &ap : m_selected)
        if (ap.bed_idx >= 0)
            plate_filaments[ap.bed_idx].insert(ap.extrude_ids.begin(), ap.extrude_ids.end());
    for (const auto &[bed, filaments] : plate_filaments)
        pack.sparrow_max_filaments_per_plate = std::max(pack.sparrow_max_filaments_per_plate, int(filaments.size()));
    // Each update reloads the scene, so big plates refresh less often.
    const auto interval = std::chrono::milliseconds(std::clamp<long long>(5ll * (long long) m_live_aps.size(), 125, 1000));
    pack.on_layout = [this, alive, last, interval](const std::vector<arrangement::ArrangeParams::Pose> &poses) {
        const auto now = std::chrono::steady_clock::now();
        if (now - *last < interval)
            return;
        *last = now;
        wxGetApp().CallAfter([this, alive, poses] {
            if (alive->load())
                apply_live(poses);
        });
    };
    arrangement::arrange(selected, m_unselected, bedpts, pack);
    // Stock places sparrow's leftovers around tower placeholders too, so they get the same retry.
    if (!ctl.was_canceled() && !only_on_partplate && !wipe_tower_is_mandatory())
        retry_unfit_single_color_items(selected, m_unselected, bedpts, params);

    // Fewer parts left out wins, then fewer plates. A stop falls back to the stock result.
    const int packed = score(selected, m_unselected), packed_unplaced = unplaced(selected);
    BOOST_LOG_TRIVIAL(info) << "arrange: " << "pack tighter: stock " << m_stock_score << " / " << m_stock_unplaced << " unplaced, sparrow " << packed << " / " << packed_unplaced;
    if (!ctl.was_canceled() && (packed_unplaced < m_stock_unplaced || (packed_unplaced == m_stock_unplaced && packed < m_stock_score))) {
        m_packed_score    = packed;
        m_packed_unplaced = packed_unplaced;
        m_stock_selected  = std::move(m_selected);
        m_selected        = std::move(selected);
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

    // Keep the parts on IDEX/IQEX plates in their primary zones; see ImexArranger.
    const ImexArranger imex(m_imex, params, bedpts);
    bedpts = imex.bed_shape();
    imex.add_keep_outs(m_unselected);
    // The packer knows nothing about IDEX/IQEX zones; imex.finish() below relies on the stock arranger.
    if (imex.active())
        m_pack_eligible = false;

    bool   enable_wrapping = global_config.option<ConfigOptionBool>("enable_wrapping_detection")->value;
    partplate_list.preprocess_exclude_areas(params.excluded_regions, global_config, enable_wrapping, 1, scale_(1));

    BOOST_LOG_TRIVIAL(debug) << "arrange bedpts:" << bedpts[0].transpose() << ", " << bedpts[1].transpose() << ", " << bedpts[2].transpose() << ", " << bedpts[3].transpose();

    params.stopcondition = [&ctl]() { return ctl.was_canceled(); };

    m_progress_value = 0;
    const double progress_items = double(status_range());
    auto report_progress = [this, &ctl](double fraction, const std::string &str) {
        // Keep all sources monotone, including a stock-arranger retry. Reserve
        // completion for the final status after results have been processed.
        m_progress_value = std::max(m_progress_value, int(std::clamp(fraction, 0., 0.99) * 100));
        // TRN Progress status. %1% is the name of the object being arranged.
        ctl.update_status(m_progress_value, GUI::format(_u8L("Arranging %1%"), str));
    };
    params.progressind = [report_progress, progress_items](unsigned num_finished, std::string str = "") {
        report_progress(num_finished / progress_items, str);
    };
    params.progress_fraction = [report_progress](double fraction, std::string str) {
        report_progress(fraction, str);
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

    // Input for the pack tighter pass, copied after the inflation is set.
    ArrangePolygons pack_selected;
    if (m_pack_eligible)
        pack_selected = m_selected;

    const ArrangePolygons before = imex.active() ? m_selected : ArrangePolygons();
    arrangement::arrange(m_selected, m_unselected, bedpts, params);
    if (!ctl.was_canceled())
        imex.finish(m_selected, before, m_unselected, params);

    // Parts squeezed out by a tower placeholder get a second chance on tower-free plates.
    if (!ctl.was_canceled() && !only_on_partplate && !params.is_seq_print && !params.is_belt && !wipe_tower_is_mandatory())
        retry_unfit_single_color_items(m_selected, m_unselected, bedpts, params);

    m_stock_score    = score(m_selected, m_unselected);
    m_stock_unplaced = unplaced(m_selected);
    if (m_pack_eligible && !ctl.was_canceled() && predict_better(m_stock_score, std::abs(Polygon(bedpts).area())))
        pack_tighter(ctl, pack_selected, bedpts);

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
        m_packing ? done_message(ctl.was_canceled()) :
        ctl.was_canceled() ? _u8L("Arranging canceled.") :
        we_have_unpackable_items ? _u8L("Arranging complete, but some items were not able to be arranged. Reduce spacing and try again.") : _u8L("Arranging done."));
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

void ArrangeJob::restore_live()
{
    // Nothing stops an edit during the job, so skip instances deleted since prepare().
    std::set<const ModelInstance *> alive;
    for (const ModelObject *mo : m_plater->model().objects)
        alive.insert(mo->instances.begin(), mo->instances.end());
    for (auto &[instance, trafo] : m_live_restore)
        if (alive.count(instance))
            instance->set_transformation(trafo);

    // Prime towers too: applying a layout can move them (new plates, exclusion zones).
    PartPlateList &plates = m_plater->get_partplate_list();
    DynamicConfig &proj_cfg = wxGetApp().preset_bundle->project_config;
    auto          *wtx      = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x");
    auto          *wty      = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y");
    if (!wtx || !wty || m_live_tower_x.size() != m_live_tower_y.size())
        return;
    auto &positions = m_plater->model().wipe_tower.positions;
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        if (i < int(m_live_tower_x.size()) && i < int(wtx->values.size()) && i < int(wty->values.size())) {
            wtx->values[i] = m_live_tower_x[i];
            wty->values[i] = m_live_tower_y[i];
        } else
            plates.set_default_wipe_tower_pos_for_plate(i); // a plate added since prepare()
        if (i < int(positions.size()))
            positions[i] = i < int(m_live_tower_pos.size()) ? m_live_tower_pos[i] : Vec2d(wtx->values[i], wty->values[i]);
    }
}

// Main thread. Re-applies from the saved transformations, because ArrangePolygon::apply()
// adds its rotation to the instance's current one.
void ArrangeJob::apply_live(const std::vector<arrangement::ArrangeParams::Pose> &poses)
{
    if (!m_live_open || poses.size() != m_live_aps.size())
        return;
    m_live_used = true;
    restore_live();
    PartPlateList &plate_list = m_plater->get_partplate_list();

    // Sparrow fills plates from the first one, so its plates go in front and the stock
    // plates, with the parts it has not taken yet, follow.
    int sparrow_plates = 0;
    for (size_t i = 0; i < poses.size(); ++i)
        if (!m_live_aps[i].is_virt_object)
            sparrow_plates = std::max(sparrow_plates, poses[i].bed_idx + 1);

    // Assign every plate first: a new plate can reflow the grid that positions depend on.
    std::vector<ArrangePolygon> live, waiting;
    for (size_t i = 0; i < poses.size(); ++i) {
        ArrangePolygon ap = m_live_aps[i];
        if (ap.is_virt_object)
            continue;
        arrangement::ArrangeParams::Pose pose = poses[i];
        if (pose.bed_idx < 0 && !only_on_partplate && i < m_live_stock.size() && m_live_stock[i].bed_idx >= 0) {
            pose = m_live_stock[i];
            pose.bed_idx += sparrow_plates;
        }
        if (pose.bed_idx < 0) {
            waiting.push_back(ap); // not placed yet: wait outside in the original layout
            continue;
        }
        ap.bed_idx     = pose.bed_idx;
        ap.translation = pose.translation;
        ap.rotation    = pose.rotation;
        // Creates plates as needed; finalize() recycles any left empty.
        if (only_on_partplate)
            plate_list.postprocess_bed_index_for_current_plate(ap);
        else
            plate_list.postprocess_bed_index_for_selected(ap);
        live.push_back(ap);
    }
    for (ArrangePolygon &ap : waiting)
        ap.bed_idx = plate_list.get_plate_count();
    live.insert(live.end(), waiting.begin(), waiting.end());
    for (ArrangePolygon &ap : live) {
        plate_list.postprocess_arrange_polygon(ap, true);
        ap.apply();
    }
    m_plater->canvas3D()->reload_scene(true);
}

void ArrangeJob::apply_result(ArrangePolygons &selected, ArrangePolygons &unselected, ArrangePolygons &locked, ArrangePolygons &unprintable)
{
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
    for (ArrangePolygon& ap : selected) {
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
    for (ArrangePolygon& ap : unselected)
    {
        if (ap.is_virt_object)
            continue;

        //BBS: partplate postprocess
        if (!only_on_partplate)
            plate_list.postprocess_bed_index_for_unselected(ap);

        beds = std::max(ap.bed_idx, beds);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":arrange unselected %4%: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y)) % ap.name;
    }

    for (ArrangePolygon& ap : locked) {
        beds = std::max(ap.bed_idx, beds);

        plate_list.postprocess_arrange_polygon(ap, false);

        ap.apply();
    }

    // Apply the arrange result to all selected objects
    for (ArrangePolygon& ap : selected) {
        //BBS: partplate postprocess
        plate_list.postprocess_arrange_polygon(ap, true);

        ap.apply();
    }

    // Apply the arrange result to unselected objects(due to the sukodu-style column changes, the position of unselected may also be modified)
    for (ArrangePolygon& ap : unselected)
    {
        if (ap.is_virt_object)
            continue;

        //BBS: partplate postprocess
        plate_list.postprocess_arrange_polygon(ap, false);

        ap.apply();
    }

    // Move the unprintable items to the last virtual bed.
    // Note ap.apply() moves relatively according to bed_idx, so we need to subtract the orignal bed_idx
    for (ArrangePolygon& ap : unprintable) {
        ap.bed_idx = beds + 1;
        plate_list.postprocess_arrange_polygon(ap, true);

        ap.apply();
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":arrange m_unprintable: name: %4%, bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y)) % ap.name;
    }

    m_plater->update();

    //BBS: reload all objects due to arrange. Plates are never recycled here: arranging
    // must not remove a plate the user created, even when the arrange left it empty.
    if (only_on_partplate) {
        plate_list.rebuild_plates_after_arrangement(false, true, current_plate_index);
    }
    else {
        plate_list.rebuild_plates_after_arrangement(false, true);
    }

    // unlock the plates we just locked
    for (int i : m_uncompatible_plates)
        plate_list.get_plate(i)->lock(false);

    // BBS: update slice context and gcode result.
    m_plater->update_slicing_context_to_current_partplate();

    wxGetApp().obj_list()->reload_all_plates();

    m_plater->update();
}

void ArrangeJob::finalize(bool canceled, std::exception_ptr &eptr) {
    // Drop the live preview: the result applies relative to the original transformations.
    if (m_live_alive)
        m_live_alive->store(false);
    if (m_live_used)
        restore_live();

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

    // A cancel while the packer runs keeps the stock result, which m_selected still holds.
    if ((canceled && !m_packing) || eptr) {
        if (m_live_used) {
            // The preview may have added plates; recycle the empty ones.
            if (!only_on_partplate)
                m_plater->get_partplate_list().rebuild_plates_after_arrangement(true, true);
            m_plater->update();
        }
        return;
    }

    // Nothing could be placed: leave the plates and the instances exactly as they were.
    // Rebuilding the plates would only re-home the instances by their bounds, which moves
    // a too large item to whichever plate it happens to reach over.
    const bool nothing_arranged = !m_selected.empty() &&
        std::all_of(m_selected.begin(), m_selected.end(), [](const ArrangePolygon &ap) { return !ap.is_arranged(); });
    if (nothing_arranged) {
        m_plater->get_notification_manager()->close_notification_of_type(NotificationType::ArrangeOngoing);
        m_plater->m_arrange_running.store(false);
        return;
    }

    // The stock result first, as its own undo step: undo goes packed -> stock -> before.
    // Not with locked or skipped plates: applying unlocks those, and a second apply would
    // then map beds onto them.
    // Nor when the user deleted a part meanwhile: restore_live() could not bring it back.
    bool instances_intact = true;
    {
        std::set<const ModelInstance *> present;
        for (const ModelObject *mo : m_plater->model().objects)
            present.insert(mo->instances.begin(), mo->instances.end());
        for (const auto &entry : m_live_restore)
            instances_intact = instances_intact && present.count(entry.first);
    }
    if (m_packed_score >= 0 && instances_intact && m_locked.empty() && m_uncompatible_plates.empty()) {
        ArrangePolygons unselected = m_unselected, locked = m_locked, unprintable = m_unprintable;
        apply_result(m_stock_selected, unselected, locked, unprintable);
        m_plater->take_snapshot(_u8L("Pack tighter"));
        restore_live();
    }
    apply_result(m_selected, m_unselected, m_locked, m_unprintable);

    {
        Slic3r::LifecycleEventContext ctx;
        ctx.code = Slic3r::LifecycleEvtCode::Ok;
        ctx.msg = "arranged";
        Slic3r::fire_lifecycle_event(Slic3r::LifecycleEvent::ObjectTransformed, ctx);
    }

    if (!m_unarranged.empty()) {
        std::set<std::string> names;
        for (ModelInstance *mi : m_unarranged)
            names.insert(mi->get_object()->name);

        m_plater->get_notification_manager()->push_notification(GUI::format(
            _L("Arrangement ignored the following objects which can't fit into a single bed:\n%s"),
            concat_strings(names, "\n")));
    }
    m_plater->get_notification_manager()->close_notification_of_type(NotificationType::ArrangeOngoing);
    if (m_packing)
        m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
            NotificationManager::NotificationLevel::RegularNotificationLevel, done_message(canceled));

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

    auto [object_skirt_offset, object_skirt_witdh] = print.object_skirt_offset();

    params.clearance_height_to_rod             = print_config.extruder_clearance_height_to_rod.value;
    params.clearance_height_to_lid             = print_config.extruder_clearance_height_to_lid.value;
    params.clearance_radius                    = print_config.extruder_clearance_radius.value + object_skirt_offset * 2;
    params.object_skirt_offset                 = object_skirt_offset;
    params.printable_height                    = print_config.printable_height.value;
    params.allow_rotations                     = settings.enable_rotation;
    params.sparrow_time_limit_s                = settings.arrange_sparrow_time;
    params.nozzle_height                       = print_config.nozzle_height.value;
    params.align_center                        = print_config.best_object_pos.value;
    params.allow_multi_materials_on_same_plate = settings.allow_multi_materials_on_same_plate;
    params.avoid_extrusion_cali_region         = settings.avoid_extrusion_cali_region;
    params.is_seq_print                        = settings.is_seq_print;
    params.min_obj_distance                    = scaled(settings.distance);
    params.align_to_y_axis                     = settings.align_to_y_axis;
    if (print_config.belt_printer.value) {
        // Parts print in belt order: the belt runs across the gantry's tilt axis, a
        // rotation about X prints toward +Y and one about Y toward -X (see
        // BeltTransform), a negative angle flips that, and a tilted layer reaches
        // cot(angle) * height past a part's far edge.
        const BeltRotationAxis axis   = print_config.belt_slice_rotation.value;
        const double           angle  = print_config.belt_slice_rotation_angle.value;
        const bool             tilted = (axis == BeltRotationAxis::X || axis == BeltRotationAxis::Y) && std::abs(angle) > EPSILON;
        params.is_belt         = true;
        params.belt_axis       = axis == BeltRotationAxis::Y ? 0 : 1;
        params.belt_reversed   = tilted && ((axis == BeltRotationAxis::Y) != (angle < 0.));
        params.belt_tilt_slope = tilted ? float(1. / std::tan(Geometry::deg2rad(std::clamp(std::abs(angle), 5., 90.)))) : 0.f;
    }

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

    // Decided after is_seq_print is final, so the outline flag and the "packer
    // skipped" toast in prepare() agree with what the pack tighter pass runs.
    params.prime_tower_always = wipe_tower_is_mandatory();
    params.use_sparrow = arrangement::sparrow_available() && settings.arrange_use_sparrow && !params.is_seq_print && !params.is_belt;
    // Set before the ArrangePolygons are collected. The flag stays set until the
    // next arrange, so any get_arrange_polygon() caller in between sees it too.
    arrangement::use_true_outline.store(params.use_sparrow, std::memory_order_relaxed);
    return params;
}

}} // namespace Slic3r::GUI
