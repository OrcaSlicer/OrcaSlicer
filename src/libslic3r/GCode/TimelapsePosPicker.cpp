#include "ClipperUtils.hpp"
#include "TimelapsePosPicker.hpp"
#include "Layer.hpp"

constexpr int FILTER_THRESHOLD = 5;
constexpr int MAX_CANDIDATE_SIZE = 5;

namespace Slic3r {
    void TimelapsePosPicker::init(const Print* print_, const Point& plate_offset)
    {
        reset();
        m_plate_offset = plate_offset;
        print = print_;

        m_nozzle_height_to_rod = print_->config().extruder_clearance_height_to_rod;
        m_nozzle_clearance_radius = print_->config().extruder_clearance_radius;
        if (print_->config().nozzle_diameter.size() > 1 && print_->config().extruder_printable_height.size() > 1) {
            m_extruder_height_gap = std::abs(print_->config().extruder_printable_height.values[0] - print_->config().extruder_printable_height.values[1]);
            m_liftable_extruder_id = print_->config().extruder_printable_height.values[0] < print_->config().extruder_printable_height.values[1] ? 0 : 1;
        }
        m_print_seq = print_->config().print_sequence.value;
        m_based_on_all_layer = print_->config().timelapse_type == TimelapseType::tlSmooth;

        construct_printable_area_by_printer();
    }

    void TimelapsePosPicker::reset()
    {
        print = nullptr;
        m_bed_polygon.clear();
        m_extruder_printable_area.clear();
        m_all_layer_pos = std::nullopt;
        bbox_cache.clear();

        m_print_seq = PrintSequence::ByObject;
        m_nozzle_height_to_rod = 0;
        m_nozzle_clearance_radius = 0;
        m_liftable_extruder_id = std::nullopt;
        m_extruder_height_gap = std::nullopt;
        m_based_on_all_layer = false;
    }

    /**
     * @brief Retrieves a list of print objects based on the provided optional set of printed objects.
     * 
     * If the optional set of printed objects is provided, it converts the set into a vector.
     * Otherwise, it retrieves all objects from the print instance.
     */
    std::vector<const PrintObject*> TimelapsePosPicker::get_object_list(const std::optional<std::vector<const PrintObject*>>& printed_objects)
    {
        std::vector<const PrintObject*> object_list;
        if (printed_objects.has_value()) {
            object_list = std::vector<const PrintObject*>(printed_objects->begin(), printed_objects->end());
        }
        else {
            object_list = std::vector<const PrintObject*>(print->objects().begin(), print->objects().end());
        }
        return object_list;
    }

    /**
     * @brief Constructs the printable area based on printer configuration.
     *
     * This function initializes the bed polygon, excludes specific areas, accounts for wipe towers,
     * and calculates the printable area for each extruder.
     */
    void TimelapsePosPicker::construct_printable_area_by_printer()
    {
        auto config = print->config();
        size_t extruder_count = config.nozzle_diameter.size();
        m_extruder_printable_area.clear();
        m_extruder_printable_area.resize(extruder_count);

        for (size_t idx = 0; idx < config.printable_area.values.size(); ++idx)
            m_bed_polygon.points.emplace_back(coord_t(scale_(config.printable_area.values[idx].x())), coord_t(scale_(config.printable_area.values[idx].y())));

        auto bed_bbox = get_extents(m_bed_polygon);
        m_plate_height = unscale_(bed_bbox.max.y());
        m_plate_width = unscale_(bed_bbox.max.x());

        Polygon bed_exclude_area;
        for (size_t idx = 0; idx < config.bed_exclude_area.values.size(); ++idx)
            bed_exclude_area.points.emplace_back(coord_t(scale_(config.bed_exclude_area.values[idx].x())), coord_t(scale_(config.bed_exclude_area.values[idx].y())));

        Point base_wp_pt = print->get_fake_wipe_tower().pos.cast<coord_t>();
        base_wp_pt = Point{ scale_(base_wp_pt.x()),scale_(base_wp_pt.y()) };

        auto transform_wt_pt = [base_wp_pt](const Point& pt) -> Point {
            Point out =pt;
            out += base_wp_pt;
            return out;
            };
        auto wt_box = print->wipe_tower_data().bbx;
        Polygon wipe_tower_area{
            {transform_wt_pt({scale_(wt_box.min.x()),scale_(wt_box.min.y())})},
            {transform_wt_pt({scale_(wt_box.max.x()),scale_(wt_box.min.y())})},
            {transform_wt_pt({scale_(wt_box.max.x()),scale_(wt_box.max.y())})},
            {transform_wt_pt({scale_(wt_box.min.x()),scale_(wt_box.max.y())})}
        };
        wipe_tower_area = expand_object_projection(wipe_tower_area, m_print_seq == PrintSequence::ByObject);

        for (size_t idx = 0; idx < extruder_count; ++idx) {
            ExPolygons printable_area = diff_ex(diff(m_bed_polygon, bed_exclude_area), { wipe_tower_area });
            if (idx < config.extruder_printable_area.size()) {
                Polygon extruder_printable_area;
                for (size_t j = 0; j < config.extruder_printable_area.values[idx].size(); ++j)
                    extruder_printable_area.points.emplace_back(coord_t(scale_(config.extruder_printable_area.values[idx][j].x())), coord_t(scale_(config.extruder_printable_area.values[idx][j].y())));
                printable_area = intersection_ex(printable_area, Polygons{ extruder_printable_area });
            }
            m_extruder_printable_area[idx] = printable_area;
        }
    }

    /**
     * @brief Collects object slice data within a specified height range for a given layer.
     *
     * @param layer The layer for which slices are being collected.
     * @param height_range The height range to consider for collecting slices.
     * @param object_list List of print objects to process.
     * @param by_object Decides the expand length of polygon
     * @return ExPolygons representing the collected slice data.
     */
    ExPolygons TimelapsePosPicker::collect_object_slices_data(const Layer* layer, float height_range, const std::vector<const PrintObject*>& object_list, bool by_object)
    {
        auto range_intersect = [](int left1, int right1, int left2, int right2) {
            if (left1 <= left2 && left2 <= right1)
                return true;
            if (left2 <= left1 && left1 <= right2)
                return true;
            return false;
            };
        ExPolygons ret;
        float z_target = layer->print_z;
        float z_low = height_range < 0 ? layer->print_z + height_range : layer->print_z;
        float z_high = height_range < 0 ? layer->print_z : layer->print_z + height_range;
        if (z_low <= 0)
            return to_expolygons({ m_bed_polygon });

        for (auto& obj : object_list) {
            for (auto& instance : obj->instances()) {
                auto instance_bbox = get_real_instance_bbox(instance);
                bool higher_than_curr_layer = (layer->object() == obj)  ?  false : instance_bbox.max.z() > z_target;
                if(range_intersect(instance_bbox.min.z(), instance_bbox.max.z(), z_low, z_high)){
                    ExPolygon expoly;
                    expoly.contour = {
                        {scale_(instance_bbox.min.x()), scale_(instance_bbox.min.y())},
                        {scale_(instance_bbox.max.x()), scale_(instance_bbox.min.y())},
                        {scale_(instance_bbox.max.x()), scale_(instance_bbox.max.y())},
                        {scale_(instance_bbox.min.x()), scale_(instance_bbox.max.y())}
                    };
                    expoly.contour = expand_object_projection(expoly.contour, by_object, higher_than_curr_layer);
                    ret.emplace_back(std::move(expoly));
                }
            }
        }
        ret = union_ex(ret);
        return ret;
    }


    Polygons TimelapsePosPicker::collect_limit_areas_for_camera(const std::vector<const PrintObject*>& object_list)
    {
        Polygons ret;
        for (auto& obj : object_list)
            ret.emplace_back(get_limit_area_for_camera(obj));
        ret = union_(ret);
        return ret;
    }

    // scaled data
    Polygons TimelapsePosPicker::collect_limit_areas_for_rod(const std::vector<const PrintObject*>& object_list, const PosPickCtx& ctx)
    {
        double rod_limit_height = m_nozzle_height_to_rod + ctx.curr_layer->print_z;
        std::vector<const PrintObject*> rod_collision_candidates;
        for(auto& obj : object_list){
            if(ctx.printed_objects && obj == ctx.printed_objects->back())
                continue;
            auto bbox = get_real_instance_bbox(obj->instances().front());
            if(bbox.max.z() >= rod_limit_height)
                rod_collision_candidates.push_back(obj);
        }

        if (rod_collision_candidates.empty())
            return {};


        std::vector<BoundingBoxf3> collision_obj_bboxs;
        for (auto obj : rod_collision_candidates) {
            collision_obj_bboxs.emplace_back(
                expand_object_bbox(
                    get_real_instance_bbox(obj->instances().front()),
                    m_print_seq == PrintSequence::ByObject
                )
            );
        }

        std::sort(collision_obj_bboxs.begin(), collision_obj_bboxs.end(), [&](const auto& lbbox, const auto& rbbox) {
            if (lbbox.min.y() == rbbox.min.y())
                return lbbox.max.y() < rbbox.max.y();
            return lbbox.min.y() < rbbox.min.y();
            });

        std::vector<std::pair<int,int>> object_y_ranges = {{0,0}};
        for(auto& bbox : collision_obj_bboxs){
            if( object_y_ranges.back().second >= bbox.min.y())
                object_y_ranges.back().second = bbox.max.y();
            else
                object_y_ranges.emplace_back(bbox.min.y(), bbox.max.y());
        }

        if (object_y_ranges.back().second < m_plate_height)
            object_y_ranges.emplace_back(m_plate_height, m_plate_height);

        int lower_y_pos = -1, upper_y_pos =-1;
        Point unscaled_curr_pos = {unscale_(ctx.curr_pos.x())-m_plate_offset.x(), unscale_(ctx.curr_pos.y()) - m_plate_offset.y()};

        for (size_t idx = 1; idx < object_y_ranges.size(); ++idx) {
            if (unscaled_curr_pos.y() >= object_y_ranges[idx - 1].second && unscaled_curr_pos.y() <= object_y_ranges[idx].first) {
                lower_y_pos = object_y_ranges[idx - 1].second;
                upper_y_pos = object_y_ranges[idx].first;
                break;
            }
        }

        if(lower_y_pos == -1 && upper_y_pos == -1)
            return { m_bed_polygon };

        Polygons ret;

        ret.emplace_back(
            Polygon{
                Point{scale_(0), scale_(0)},
                Point{scale_(m_plate_width), scale_(0)},
                Point{scale_(m_plate_width), scale_(lower_y_pos)},
                Point{scale_(0), scale_(lower_y_pos)}
            }
        );

        ret.emplace_back(
            Polygon{
                Point{scale_(0), scale_(upper_y_pos)},
                Point{scale_(m_plate_width), scale_(upper_y_pos)},
                Point{scale_(m_plate_width), scale_(m_plate_height)},
                Point{scale_(0), scale_(m_plate_height)}
            }
        );
        return ret;
    }



    // expand the object expolygon by safe distance, scaled data
    Polygon TimelapsePosPicker::expand_object_projection(const Polygon &poly, bool by_object, bool higher_than_curr)
    {
        float radius = 0;
        if (by_object) {
            if (higher_than_curr) {
                radius = scale_(print->config().extruder_clearance_radius.value);
            }else{
                radius = scale_(print->config().extruder_clearance_radius.value / 2);
            }
        }
        else
            radius = scale_(print->config().extruder_clearance_radius.value / 2);

        // the input poly is bounding box, so we get the first offseted polygon is ok
        auto ret = offset(poly, radius);
        if (ret.empty())
            return {};
        return ret[0];
    }

    // unscaled data
    BoundingBoxf3 TimelapsePosPicker::expand_object_bbox(const BoundingBoxf3& bbox, bool by_object)
    {
        float radius = 0;
        if (by_object)
            radius = print->config().extruder_clearance_radius.value;
        else
            radius = print->config().extruder_clearance_radius.value / 2;

        BoundingBoxf3 ret = bbox;
        ret.min.x() -= radius;
        ret.min.y() -= radius;
        ret.max.x() += radius;
        ret.max.y() += radius;

        return ret;
    }


    double TimelapsePosPicker::get_raft_height(const PrintObject* obj)
    {
        if (!obj || !obj->has_raft())
            return 0;
        auto slice_params = obj->slicing_parameters();
        int base_raft_layers = slice_params.base_raft_layers;
        double base_raft_height = slice_params.base_raft_layer_height;
        int interface_raft_layers = slice_params.interface_raft_layers;
        double interface_raft_height = slice_params.interface_raft_layer_height;
        double contact_raft_layer_height = slice_params.contact_raft_layer_height;

        double ret = print->config().initial_layer_print_height;
        if (base_raft_layers - 1 > 0)
            ret += (base_raft_layers - 1) * base_raft_height;
        if (interface_raft_layers - 1 > 0)
            ret += (interface_raft_layers - 1) * interface_raft_height;
        if (obj->config().raft_layers > 1)
            ret += contact_raft_layer_height;

        return ret + slice_params.gap_raft_object;
    }

    // get the real instance bounding box, remove the plate offset and add raft height , unscaled data
    BoundingBoxf3 TimelapsePosPicker::get_real_instance_bbox(const PrintInstance& instance)
    {
        auto iter = bbox_cache.find(&instance);
        if (iter != bbox_cache.end())
            return iter->second;

        auto bbox = instance.get_bounding_box();
        double raft_height =get_raft_height(instance.print_object);
        bbox.max.z() += raft_height;
        // remove plate offset
        bbox.min.x() -= m_plate_offset.x();
        bbox.max.x() -= m_plate_offset.x();
        bbox.min.y() -= m_plate_offset.y();
        bbox.max.y() -= m_plate_offset.y();

        bbox_cache[&instance] = bbox;

        return bbox;
    }

    Polygon TimelapsePosPicker::get_limit_area_for_camera(const PrintObject* obj)
    {
        if (!obj)
            return {};
        auto bbox = get_real_instance_bbox(obj->instances().front());
        float radius = m_nozzle_clearance_radius / 2;

        auto offset_bbox = bbox.inflated(sqrt(2) * radius);
        // Constrain the coordinates to the first quadrant.
        Polygon ret = {
            DefaultCameraPos,
            Point{std::max(scale_(offset_bbox.max.x()),0.),std::max(scale_(offset_bbox.min.y()),0.)},
            Point{std::max(scale_(offset_bbox.max.x()),0.),std::max(scale_(offset_bbox.max.y()),0.)},
            Point{std::max(scale_(offset_bbox.min.x()),0.),std::max(scale_(offset_bbox.max.y()),0.)}
        };
        return ret;
    }

    /**
     * @brief Selects the nearest position within the given safe areas relative to the current position.
     *
     * This function determines the closest point in the safe areas to the provided current position.
     * If the current position is already inside a safe area, it returns the current position.
     * If no safe areas are defined, return default timelapse position.
     *
     * @param curr_pos The reference point representing the current position.
     * @param safe_areas A collection of extended polygons defining the safe areas.
     * @return Point The nearest point within the safe areas or the default timelapse position if no safe areas exist.
     */
    Point pick_pos_internal(const Point& curr_pos, const ExPolygons& safe_areas, const ExPolygons& path_collision_area, bool detect_path_collision)
    {
        struct CandidatePoint
        {
            double dist;
            Point point;
            bool operator<(const CandidatePoint& other) const {
                return dist < other.dist;
            }
        };

        if (std::any_of(safe_areas.begin(), safe_areas.end(), [&curr_pos](const ExPolygon& p) { return p.contains(curr_pos); }))
            return curr_pos;

        if (safe_areas.empty())
            return DefaultTimelapsePos;

        std::priority_queue<CandidatePoint> max_heap;

        const double candidate_point_segment = scale_(5), weight_of_camera=1./3.;
        auto penaltyFunc = [&weight_of_camera](const Point &curr_post, const Point &CameraPos, const Point &candidatet) -> double {
            // move distance + Camera occlusion penalty function
            double ret_pen = (curr_post - candidatet).cwiseAbs().sum() - weight_of_camera * (CameraPos - candidatet).cwiseAbs().sum();
            return ret_pen;
        };

        for (const auto& expoly : safe_areas) {
            Polygons polys = to_polygons(expoly);
            for (auto& poly : polys) {
                for (size_t idx = 0; idx < poly.points.size(); ++idx) {
                    double             best_penalty   = std::numeric_limits<double>::max();
                    Point              best_candidate = DefaultTimelapsePos; // the best candidate form current line
                    //std::vector<Point> candidate_source;
                    if ((poly.points[idx] - poly.points[next_idx_modulo(idx, poly.points)]).cwiseAbs().sum() < candidate_point_segment) {
                        best_candidate = poly.points[idx]; // only check the start point if the line is short
                        best_penalty   = penaltyFunc(curr_pos, DefaultCameraPos, best_candidate);
                    }else{
                        Point direct_of_line = poly.points[next_idx_modulo(idx, poly.points)] - poly.points[idx];
                        double length_L1      = direct_of_line.cwiseAbs().sum();
                        int    num_steps      = static_cast<int>(length_L1 / candidate_point_segment); // for long line use 5mm segmentation to check
                        // devide by length_L1 instead of steps, prevent lose accuracy for the step length
                        direct_of_line.x()    = static_cast<coord_t>(static_cast<double> (direct_of_line.x()) * candidate_point_segment / length_L1);
                        direct_of_line.y()    = static_cast<coord_t>(static_cast<double> (direct_of_line.y()) * candidate_point_segment / length_L1);
                        Point candidate;
                        for (int line_seg_i = 0; line_seg_i <= num_steps; ++line_seg_i) {
                            candidate=poly.points[idx] + direct_of_line * line_seg_i;
                            double dist = penaltyFunc(curr_pos, DefaultCameraPos, candidate);
                            if (dist < best_penalty) {
                                best_penalty   = dist;
                                best_candidate = candidate;
                            }//only push the best point into heap for the whole line
                        }
                    }
                    max_heap.push({best_penalty, best_candidate});
                    if (max_heap.size() > MAX_CANDIDATE_SIZE) max_heap.pop();
                }
            }
        }

        std::vector<Point> top_candidates;
        while (!max_heap.empty()) {
            top_candidates.push_back(max_heap.top().point);
            max_heap.pop();
        }
        std::reverse(top_candidates.begin(), top_candidates.end());

        for (auto& p : top_candidates) {
            if (!detect_path_collision)
                return p;

            Polyline path(curr_pos, p);

            if (intersection_pl({path}, path_collision_area).empty())
                return p;
        }

        return DefaultTimelapsePos;
    }

    Point TimelapsePosPicker::pick_pos(const PosPickCtx& ctx)
    {
        Point res;
        if (m_based_on_all_layer)
            res = pick_pos_for_all_layer(ctx);
        else
            res = pick_pos_for_curr_layer(ctx);

        return { unscale_(res.x()), unscale_(res.y()) };
    }

    // get center point of curr object, scaled data
    Point TimelapsePosPicker::get_object_center(const PrintObject* obj)
    {
        if (!obj)
            return {};
        // in bambu studio, each object only has one instance
        const auto& instance = obj->instances().front();
        auto instance_bbox = get_real_instance_bbox(instance);
        Point min_p{ instance_bbox.min.x(),instance_bbox.min.y() };
        Point max_p{ instance_bbox.max.x(),instance_bbox.max.y() };

        return { scale_((min_p.x() + max_p.x()) / 2),scale_((min_p.y() + max_p.y()) / 2) };
    }

    // scaled data
    Point TimelapsePosPicker::pick_nearest_object_center(const Point& curr_pos, const  std::vector<const PrintObject*>& object_list)
    {
        if (object_list.empty())
            return {};
        const PrintObject* ptr = object_list.front();
        double distance = std::numeric_limits<double>::max();
        for (auto& obj : object_list) {
            Point obj_center = get_object_center(obj);
            double dist = (obj_center - curr_pos).cast<double>().norm();
            if (distance > dist) {
                distance = dist;
                ptr = obj;
            }
        }
        return get_object_center(ptr);
    }

    // scaled data
    Point TimelapsePosPicker::pick_pos_for_curr_layer(const PosPickCtx& ctx)
    {
        float height_gap = 0;
        if (ctx.curr_extruder_id != ctx.picture_extruder_id) {
            if (m_liftable_extruder_id.has_value() && ctx.picture_extruder_id != m_liftable_extruder_id && m_extruder_height_gap.has_value())
                height_gap = -*m_extruder_height_gap;
        }

        bool by_object = m_print_seq == PrintSequence::ByObject;
        std::vector<const PrintObject*> object_list = get_object_list(ctx.printed_objects);

        ExPolygons layer_slices = collect_object_slices_data(ctx.curr_layer, height_gap, object_list, by_object);
        Polygons camera_limit_areas = collect_limit_areas_for_camera(object_list);
        Polygons rod_limit_areas;
        if (by_object) {
            rod_limit_areas = collect_limit_areas_for_rod(object_list, ctx);
        }
        ExPolygons unplacable_area = union_ex(union_ex(layer_slices, camera_limit_areas), rod_limit_areas);
        ExPolygons extruder_printable_area = m_extruder_printable_area[ctx.picture_extruder_id];

        ExPolygons safe_area = diff_ex(extruder_printable_area, unplacable_area);
        safe_area = opening_ex(safe_area, scale_(FILTER_THRESHOLD));

        Point center_p;
        if (by_object && ctx.printed_objects && !ctx.printed_objects->empty())
            center_p = get_object_center(ctx.printed_objects->back());
        else
            center_p = get_objects_center(object_list);

        ExPolygons path_collision_area;
        if (by_object) {
            auto object_without_curr = ctx.printed_objects;
            if (object_without_curr && !object_without_curr->empty())
                object_without_curr->pop_back();

            ExPolygons layer_slices_without_curr = collect_object_slices_data(ctx.curr_layer, height_gap, get_object_list(object_without_curr), by_object);
            path_collision_area = union_ex(layer_slices_without_curr, rod_limit_areas);
        }

        return pick_pos_internal(center_p, safe_area,path_collision_area, by_object);
    }

    /**
     * @brief Calculates the center of multiple objects.
     *
     * This function computes the average center of all instances of the provided objects.
     *
     * @param object_list A vector of pointers to PrintObject instances.
     * @return Point The average center of all objects.Scaled data
     */
    Point TimelapsePosPicker::get_objects_center(const std::vector<const PrintObject*>& object_list)
    {
        if (object_list.empty())
            return Point(0,0);
        double sum_x = 0.0;
        double sum_y = 0.0;
        size_t total_instances = 0;
        for (auto& obj : object_list) {
            for (auto& instance : obj->instances()) {
                const auto& bbox = get_real_instance_bbox(instance);
                Point min_p{ bbox.min.x(),bbox.min.y() };
                Point max_p{ bbox.max.x(),bbox.max.y() };
                double center_x = (min_p.x() + max_p.x()) / 2.f;
                double center_y = (min_p.y() + max_p.y()) / 2.f;
                sum_x += center_x;
                sum_y += center_y;
                total_instances += 1;
            }
        }
        return Point{ coord_t(scale_(sum_x / total_instances)),coord_t(scale_(sum_y / total_instances)) };
    }

    Point TimelapsePosPicker::pick_pos_for_all_layer(const PosPickCtx& ctx)
    {
        bool by_object = m_print_seq == PrintSequence::ByObject;
        if (by_object)
            return DefaultTimelapsePos;

        float height_gap = 0;
        if (ctx.curr_extruder_id != ctx.picture_extruder_id) {
            if (m_liftable_extruder_id.has_value() && ctx.picture_extruder_id != m_liftable_extruder_id && m_extruder_height_gap.has_value())
                height_gap = *m_extruder_height_gap;
        }
        if (ctx.curr_layer->print_z < height_gap)
            return DefaultTimelapsePos;
        if (m_all_layer_pos)
            return *m_all_layer_pos;

        Polygons object_projections;

        auto object_list = get_object_list(std::nullopt);

        for (auto& obj : object_list) {
            for (auto& instance : obj->instances()) {
                const auto& bbox = get_real_instance_bbox(instance);
                Point min_p{ scale_(bbox.min.x()),scale_(bbox.min.y()) };
                Point max_p{ scale_(bbox.max.x()),scale_(bbox.max.y()) };
                Polygon obj_proj{ { min_p.x(),min_p.y() },
                    { max_p.x(),min_p.y() },
                    { max_p.x(),max_p.y() },
                    { min_p.x(),max_p.y() }
                };
                object_projections.emplace_back(expand_object_projection(obj_proj, by_object));
            }
        };


        object_projections = union_(object_projections);
        Polygons camera_limit_areas = collect_limit_areas_for_camera(object_list);
        Polygons unplacable_area = union_(object_projections, camera_limit_areas);

        ExPolygons extruder_printable_area;
        if (m_extruder_printable_area.size() > 1)
            extruder_printable_area = intersection_ex(m_extruder_printable_area[0], m_extruder_printable_area[1]);
        else if (m_extruder_printable_area.size() == 1)
            extruder_printable_area = m_extruder_printable_area.front();

        ExPolygons safe_area = diff_ex(extruder_printable_area, unplacable_area);
        safe_area = opening_ex(safe_area, scale_(FILTER_THRESHOLD));

        Point starting_pos = get_objects_center(object_list);

        m_all_layer_pos = pick_pos_internal(starting_pos, safe_area, {}, by_object);
        return *m_all_layer_pos;
    }

}